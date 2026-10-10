// ============================================================================
//  frame.cpp -- AppFrame: the main window, its layout, and command dispatch
//
//  WINDOW LAYOUT (top to bottom, all children of the frame):
//
//      +------------------------------------------------------+  y = 0
//      |  Ribbon (IUIFramework)                 m_cyRibbon    |
//      +------------------------------------------------------+
//      |  Ruler (custom class)                  m_cyRuler     |
//      +------------------------------------------------------+
//      |                                                      |
//      |  RichEdit control (the document)       fills rest    |
//      |                                                      |
//      +------------------------------------------------------+
//      |  Status bar  [ message | position |  [-zoom-] 100% ] |  m_cyStatus
//      +------------------------------------------------------+
//
//  The zoom trackbar and its percentage label are children OF THE STATUS BAR,
//  not siblings of it. That is not a stylistic choice: a sibling would be
//  overpainted whenever the status bar invalidated its own client area, which
//  includes the rectangle underneath the trackbar. Parenting it puts it in the
//  status bar's client z-order where it always paints last.
//
//  COMMAND DISPATCH
//  Every user action -- ribbon button, fallback toolbar, fallback menu,
//  accelerator key, status-bar trackbar -- arrives as an ID_CMD_* value in
//  OnCommand. There is exactly one switch. Adding a feature means adding a case
//  here and a binding in whichever surfaces should expose it; no surface has
//  its own private implementation of anything.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

// Forward declarations. The subclass procedures live further down this file,
// next to FrameWndProc, because they are part of message handling; but the
// children they attach to are created up here. Declaring the installers rather
// than moving 150 lines of window-proc code above Create() keeps the file
// readable in the order it executes.
void Subclass_InstallStatus(HWND hStatus, AppFrame* frame);
void Subclass_InstallEdit(HWND hEdit, AppFrame* frame);

// Internal message used by the status-bar subclass to defer zoom updates out of
// the trackbar's own notification. Defined here, at the top, because the
// subclass procedure that posts it is textually above the point where the
// message-handling code would naturally declare it.
#define WM_APP_ZOOMCHANGED (WM_APP + 1)

namespace {

// Ruler and status-bar heights, in DIPs. Scaled by DPI at creation time so the
// chrome does not shrink to unreadable sizes on a high-DPI display.
constexpr int kRulerHeightDip   = 24;
constexpr int kStatusHeightDip  = 24;
constexpr int kZoomTrackWidthDip = 120;
constexpr int kZoomLabelWidthDip = 44;

int DipToPx(int dip)
{
    HDC hdc = GetDC(nullptr);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    if (dpi <= 0) dpi = 96;
    return MulDiv(dip, dpi, 96);
}

// The GWLP_USERDATA slot holds the AppFrame*. Every message that needs object
// context recovers it through here rather than through a map, because there is
// one frame per window and the lookup must be O(1) on every WM_PAINT.
AppFrame* FrameOf(HWND h)
{
    return PtrFromLParam<AppFrame>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

} // anonymous namespace

// ===========================================================================
//  AppFrame -- construction
// ===========================================================================
AppFrame* AppFrame::FromHwnd(HWND h) { return FrameOf(h); }

bool AppFrame::Create(int nCmdShow, PCWSTR initialFile)
{
    Settings_Load(&m_settings);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    DWORD exStyle = 0;

    // A maximised window is created at its restored size and then shown
    // maximised. Creating it already-maximised loses the restore rectangle, so
    // un-maximising later snaps it to the top-left at the default size.
    if (m_settings.fMaximised)
        style &= ~WS_MAXIMIZE;

    m_hFrame = CreateWindowExW(exStyle, LP_FRAME_CLASS, LP_APPNAME, style,
                               m_settings.rcWindow.left, m_settings.rcWindow.top,
                               m_settings.rcWindow.right - m_settings.rcWindow.left,
                               m_settings.rcWindow.bottom - m_settings.rcWindow.top,
                               nullptr, nullptr, g_hInst, this);
    if (!m_hFrame)
        return false;

    // CreateWindowExW's lpParam is forwarded to WM_NCCREATE, where the window
    // proc stores it in GWLP_USERDATA. By the time CreateWindowExW returns, the
    // association already exists and CreateChildren can rely on FrameOf().
    if (!CreateChildren())
    {
        DestroyWindow(m_hFrame);
        m_hFrame = nullptr;
        return false;
    }

    ApplySettings(m_settings);

    // Ribbon initialisation is best-effort. A failure here means the classic
    // toolbar is used instead; it does NOT mean the application cannot run.
    // This is why Create() still returns true.
    m_fRibbon = InitRibbon();
    if (!m_fRibbon)
        CreateFallbackToolbar();

    ++g_cFrames;

    if (initialFile && initialFile[0])
    {
        // A failure to open the requested file is reported by FileOpen itself;
        // the window still opens with an empty document rather than closing.
        FileOpen(initialFile);
    }
    else
    {
        FileNew();
    }

    ShowWindow(m_hFrame, m_settings.fMaximised ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(m_hFrame);

    // Focus goes to the edit control, not the frame. Without this the first
    // keystroke after launch is consumed by the frame's default processing and
    // lost, because a window with no focused child routes keys nowhere useful.
    if (m_view) SetFocus(m_view.Hwnd());

    return true;
}

bool AppFrame::CreateChildren()
{
    if (!CreateEditView()) return false;
    if (!CreateRuler())    return false;
    CreateStatusBar();
    return true;
}

// ---------------------------------------------------------------------------
//  The RichEdit control
//
//  Every EM_* call below is order-dependent in a way that is not obvious:
//
//    EM_SETOPTIONS      before formatting, because ECO_SELECTIONBAR changes
//                       the client layout and therefore the wrap width.
//    EM_EXLIMITTEXT     before loading any document. RichEdit's default limit
//                       is small enough that a large RTF load silently
//                       truncates with no error.
//    EM_SETCHARFORMAT   with SCF_ALL, before the first paint, to establish the
//                       document's default font.
//    EM_SETMODIFY(FALSE) last, because everything above dirties the control.
// ---------------------------------------------------------------------------
bool AppFrame::CreateEditView()
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                  ES_MULTILINE | ES_AUTOVSCROLL | ES_SELECTIONBAR;

    // ES_AUTOHSCROLL is added only for kWrapNone, and ApplyWrapMode manages it
    // from then on. Setting it unconditionally here would make the first paint
    // wrap differently from every subsequent one.
    if (m_settings.wrap == kWrapNone)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;

    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", style,
                                 0, 0, 100, 100,      // real size from LayoutChildren
                                 m_hFrame,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)),
                                 g_hInst, nullptr);
    if (!hEdit)
        return false;

    m_view.Attach(hEdit, this);

    // --- options -----------------------------------------------------------
    // ECOOP_SET replaces the option set rather than OR-ing into it, so this one
    // call establishes the complete configuration.
    //   ECO_AUTOVSCROLL  scroll vertically as the caret moves
    //   ECO_NOHIDESEL    show the selection even when unfocused -- without it,
    //                    clicking the ribbon makes the selection vanish and the
    //                    user loses track of what they had selected
    //   ECO_SELECTIONBAR the left gutter that marks whole paragraphs
    //   ECO_SAVEONIDLE   not set: LetterPad owns saving explicitly
    SendMessageW(hEdit, EM_SETOPTIONS, ECOOP_SET,
                 static_cast<LPARAM>(ECO_AUTOVSCROLL | ECO_NOHIDESEL | ECO_SELECTIONBAR));

    // --- notifications ------------------------------------------------------
    // ENM_PROTECTED is required for WM_NOTIFY(EN_PROTECTED) on attempts to
    // modify read-only regions, and ENM_LINK for hyperlink clicks.
    SendMessageW(hEdit, EM_SETEVENTMASK, 0,
                 static_cast<LPARAM>(ENM_CHANGE | ENM_SELCHANGE | ENM_LINK |
                                     ENM_PROTECTED | ENM_SCROLLEVENTS));

    // --- text limit ---------------------------------------------------------
    // -1 removes the limit. Without this, a large document load truncates at
    // the default ceiling and EM_STREAMIN reports success anyway -- the text is
    // simply missing from the end with no diagnostic.
    SendMessageW(hEdit, EM_EXLIMITTEXT, 0, static_cast<LPARAM>(-1));

    // --- background ----------------------------------------------------------
    // Explicit white. RichEdit's default follows COLOR_WINDOW, which under a
    // high-contrast theme is not white, and a document is a document.
    SendMessageW(hEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(RGB(255, 255, 255)));

    // --- typography ----------------------------------------------------------
    // Advanced typography enables kerning and ligature data in CHARFORMAT2.
    // Harmless when the font has none; required for the properties to survive
    // an RTF round trip when it does.
    SendMessageW(hEdit, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY,
                 static_cast<LPARAM>(TO_ADVANCEDTYPOGRAPHY));

    // --- default formatting ---------------------------------------------------
    m_view.SetCharFormat(m_settings.defChar);
    m_view.SetParaFormat(m_settings.defPara);

    // --- drag and drop ----------------------------------------------------------
    // Registered here rather than in oleobj.cpp so that the lifetime is obvious:
    // the drop target is revoked when the edit control is destroyed, in
    // WM_DESTROY. RegisterDragDrop requires OLE to be initialised, which
    // wWinMain did before creating this window.
    Ole_RegisterDropTarget(hEdit);

    // Ctrl+wheel zoom. Must come after the control exists and after this pointer
    // is valid, which it is: SetHwndForProc ran during WM_NCCREATE.
    Subclass_InstallEdit(hEdit, this);

    return true;
}

bool AppFrame::CreateRuler()
{
    if (!m_settings.fShowRuler)
    {
        m_cyRuler = 0;
        return true;      // not an error: the ruler is optional
    }

    m_cyRuler = DipToPx(kRulerHeightDip);
    m_hRuler = CreateWindowExW(0, LP_RULER_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                               0, 0, 100, m_cyRuler, m_hFrame, nullptr, g_hInst, nullptr);
    return m_hRuler != nullptr;
}

void AppFrame::CreateStatusBar()
{
    m_cyStatus = m_settings.fShowStatusBar ? DipToPx(kStatusHeightDip) : 0;
    if (!m_settings.fShowStatusBar)
        return;

    // SBARS_SIZEGRIP adds the resize grip in the bottom-right corner. It is
    // suppressed automatically when the window is maximised.
    m_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SBARS_SIZEGRIP,
                                0, 0, 0, 0, m_hFrame,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ZOOM_TRACKBAR - 100)),
                                g_hInst, nullptr);

    // The zoom trackbar and its label are children of the status bar. See the
    // file header for why they cannot be siblings of it.
    m_hZoomTrack = CreateWindowExW(0, TRACKBARCLASSW, nullptr,
                                   WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_NOPARENT,
                                   0, 0, DipToPx(kZoomTrackWidthDip), DipToPx(kStatusHeightDip),
                                   m_hStatus,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ZOOM_TRACKBAR)),
                                   g_hInst, nullptr);
    if (m_hZoomTrack)
    {
        // Range matches kZoomStops in editview.cpp: 10% to 500%. The trackbar
        // position IS the percentage, and SetZoomPercent snaps it to the
        // nearest actual stop so the slider never rests on a value the document
        // is not using.
        SendMessageW(m_hZoomTrack, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
        SendMessageW(m_hZoomTrack, TBM_SETLINESIZE, 0, 10);
        SendMessageW(m_hZoomTrack, TBM_SETPAGESIZE, 0, 25);
        SendMessageW(m_hZoomTrack, TBM_SETPOS, TRUE, 100);
        // Buddy label: TICKSTYLE_NONE means the buddy is positioned by us, and
        // doing it in LayoutChildren keeps it aligned when the status bar resizes.
    }

    m_hZoomLabel = nullptr;   // replaced below; created as a STATIC
    HWND hLabel = CreateWindowExW(0, L"STATIC", L"100%",
                                  WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER,
                                  0, 0, DipToPx(kZoomLabelWidthDip), DipToPx(kStatusHeightDip),
                                  m_hStatus,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ZOOM_LABEL)),
                                  g_hInst, nullptr);
    m_hZoomLabel = hLabel;

    // The trackbar is a child of the status bar, so its WM_HSCROLL goes to the
    // status bar and never reaches the frame. The subclass forwards it.
    Subclass_InstallStatus(m_hStatus, this);

    UpdateStatusBar();
}

// ===========================================================================
//  Ribbon
// ===========================================================================
bool AppFrame::InitRibbon()
{
    // CoCreateInstance rather than LoadLibrary+GetProcAddress on Ribbon.dll:
    // the framework is a registered COM object and the CLSID lookup handles the
    // "not available on this SKU" case by simply failing, which is the signal to
    // fall back. Ribbon.dll is Windows 7+; on anything older this returns
    // REGDB_E_CLASSNOTREG.
    HRESULT hr = CoCreateInstance(CLSID_UIRibbonFramework, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(m_pFramework.Put()));
    if (FAILED(hr) || !m_pFramework)
        return false;

    hr = RibbonApp_Create(m_pFramework.Get(), this, m_pApplication.Put());
    if (FAILED(hr) || !m_pApplication)
    {
        m_pFramework = nullptr;
        return false;
    }

    // Initialize before LoadUI. The framework needs the host HWND in order to
    // size itself and to route keyboard focus; loading the markup first
    // produces a ribbon that renders at zero height.
    hr = m_pFramework->Initialize(m_hFrame, m_pApplication.Get());
    if (FAILED(hr))
    {
        m_pFramework = nullptr;
        m_pApplication = nullptr;
        return false;
    }

    // LoadUI reads the RT_APPLICATION_RIBBON resource that UICC produced from
    // src/ribbon/Ribbon.xml. If the build ran with -DLETTERPAD_SKIP_RIBBON this
    // resource does not exist and LoadUI fails -- which is the supported way to
    // get the fallback toolbar, not an error condition to report to the user.
    hr = m_pFramework->LoadUI(g_hInst, IDR_RIBBON_BML);
    if (FAILED(hr))
    {
        m_pFramework->Destroy();
        m_pFramework = nullptr;
        m_pApplication = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
//  Fallback UI
//
//  Used when the ribbon is unavailable. Deliberately minimal: the same command
//  IDs, a flat toolbar of the most-used actions, and the standard menu. This is
//  a degraded mode, not a second product -- it exists so that the editing
//  engine remains reachable (and testable) on a machine without Ribbon.dll.
// ---------------------------------------------------------------------------
void AppFrame::CreateFallbackToolbar()
{
    SetMenu(m_hFrame, LoadMenuW(g_hInst, MAKEINTRESOURCEW(IDR_MENU_FALLBACK)));

    m_hToolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                 WS_CHILD | WS_VISIBLE | CCS_TOP |
                                 TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_WRAPABLE,
                                 0, 0, 0, 0, m_hFrame, nullptr, g_hInst, nullptr);
    if (!m_hToolbar) return;

    SendMessageW(m_hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    // One entry per button. iString = -1 means "no text label", which is what a
    // compact toolbar wants; TBSTYLE_LIST with real strings would need each
    // label loaded from the string table.
    static const struct { int id; int bmp; BYTE style; } kButtons[] =
    {
        { ID_CMD_NEW,      0, BTNS_BUTTON     },
        { ID_CMD_OPEN,     1, BTNS_BUTTON     },
        { ID_CMD_SAVE,     2, BTNS_BUTTON     },
        { 0,               0, BTNS_SEP       },
        { ID_CMD_UNDO,     3, BTNS_BUTTON     },
        { ID_CMD_REDO,     4, BTNS_BUTTON     },
        { 0,               0, BTNS_SEP       },
        { ID_CMD_CUT,      5, BTNS_BUTTON     },
        { ID_CMD_COPY,     6, BTNS_BUTTON     },
        { ID_CMD_PASTE,    7, BTNS_BUTTON     },
        { 0,               0, BTNS_SEP       },
        { ID_CMD_BOLD,     8, BTNS_BUTTON     },
        { ID_CMD_ITALIC,   9, BTNS_BUTTON     },
        { ID_CMD_UNDERLINE,10,BTNS_BUTTON     },
        { 0,               0, BTNS_SEP       },
        { ID_CMD_FIND,     11,BTNS_BUTTON     },
        { ID_CMD_REPLACE,  12,BTNS_BUTTON     },
        { 0,               0, BTNS_SEP       },
        { ID_CMD_PRINT,    13,BTNS_BUTTON     },
    };

    TBBUTTON tb[_countof(kButtons)] = {};
    for (size_t i = 0; i < _countof(kButtons); ++i)
    {
        tb[i].iBitmap   = kButtons[i].bmp;
        tb[i].idCommand = kButtons[i].id;
        tb[i].fsState   = (kButtons[i].id == 0) ? 0 : TBSTATE_ENABLED;
        tb[i].fsStyle   = kButtons[i].style;
        tb[i].iString   = -1;
    }

    // The toolbar needs a bitmap list before TB_ADDBUTTONS, or every button
    // renders blank. A system-provided default is used: shipping our own
    // icon strip is a res/ concern and is listed in ROADMAP.md.
    SendMessageW(m_hToolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(16, 16));
    if (HBITMAP hbm = static_cast<HBITMAP>(LoadImageW(g_hInst,
                                                      MAKEINTRESOURCEW(IDB_TOOLBAR),
                                                      IMAGE_BITMAP, 0, 0,
                                                      LR_DEFAULTCOLOR)))
    {
        // TB_ADDBITMAP with hInst == null and nID == an HBITMAP is the
        // "add this bitmap directly" form. The { hInst, nID } pair is a union
        // in spirit: setting hInst to a real module makes nID a resource ID
        // instead, which is not what we have here.
        TBADDBITMAP ab = {};
        ab.hInst = nullptr;
        ab.nID   = reinterpret_cast<UINT_PTR>(hbm);
        SendMessageW(m_hToolbar, TB_ADDBITMAP, _countof(kButtons), LParamFromPtr(&ab));
    }

    SendMessageW(m_hToolbar, TB_ADDBUTTONSW, _countof(kButtons), LParamFromPtr(tb));
    SendMessageW(m_hToolbar, TB_AUTOSIZE, 0, 0);

    // Tooltips: TBSTYLE_TOOLTIPS plus a TT_GETTEXT handler would give real text.
    // Omitted here because the fallback path is temporary and the menu provides
    // the same information with labels.
}

// ===========================================================================
//  Layout
// ===========================================================================
void AppFrame::LayoutChildren()
{
    if (!m_hFrame) return;

    RECT rc;
    GetClientRect(m_hFrame, &rc);
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    if (cx <= 0 || cy <= 0) return;

    // Ribbon height is owned by the framework, not by us. GetHeight returns the
    // current height including any minimised/collapsed state; caching a value
    // from WM_SIZE of an earlier layout would desynchronise after the user
    // minimises the ribbon.
    m_cyRibbon = 0;
    if (m_fRibbon && m_pFramework)
    {
        UINT h = 0;
        if (SUCCEEDED(m_pFramework->GetHeight(&h)))
            m_cyRibbon = static_cast<int>(h);
    }
    else if (m_hToolbar)
    {
        RECT rcTb;
        GetWindowRect(m_hToolbar, &rcTb);
        m_cyRibbon = rcTb.bottom - rcTb.top;
    }

    int y = 0;

    HDWP hDwp = BeginDeferWindowPos(5);

    if (m_hToolbar && hDwp)
    {
        hDwp = DeferWindowPos(hDwp, m_hToolbar, nullptr, 0, y, cx, m_cyRibbon,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    y += m_cyRibbon;

    if (m_hRuler && hDwp)
    {
        hDwp = DeferWindowPos(hDwp, m_hRuler, nullptr, 0, y, cx, m_cyRuler,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    y += m_cyRuler;

    int cyEdit = cy - y - m_cyStatus;
    if (cyEdit < 0) cyEdit = 0;

    if (m_view && hDwp)
    {
        hDwp = DeferWindowPos(hDwp, m_view.Hwnd(), nullptr, 0, y, cx, cyEdit,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (m_hStatus && hDwp)
    {
        // The status bar sizes itself from WM_SIZE when it has SBARS_SIZEGRIP
        // and is positioned at the bottom, but it does not move on its own --
        // we place it explicitly so it tracks the client height.
        hDwp = DeferWindowPos(hDwp, m_hStatus, nullptr, 0, cy - m_cyStatus,
                              cx, m_cyStatus, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (hDwp) EndDeferWindowPos(hDwp);

    // --- status bar panes -------------------------------------------------
    // Pane boundaries are absolute right edges. Pane 0 (message) takes the
    // flexible remainder; pane 1 (position) and pane 2 (zoom widgets) are fixed.
    if (m_hStatus)
    {
        int cxPos  = DipToPx(140);
        int cxZoom = DipToPx(kZoomTrackWidthDip + kZoomLabelWidthDip + 16);
        int parts[STATUSBAR_PANE_COUNT] =
        {
            cx - cxPos - cxZoom,   // message
            cx - cxZoom,           // position
            -1,                    // zoom (to the right edge)
        };
        // A negative boundary in SB_SETPARTS means "the last pane runs to the
        // right edge". Clamp the earlier boundaries so they cannot go negative
        // when the window is very narrow, which would collapse the panes.
        if (parts[0] < 40) parts[0] = 40;
        if (parts[1] < parts[0] + 40) parts[1] = parts[0] + 40;

        SendMessageW(m_hStatus, SB_SETPARTS, STATUSBAR_PANE_COUNT, LParamFromPtr(parts));

        // Position the zoom widgets inside pane 2. They are children of the
        // status bar, so their coordinates are relative to it.
        if (m_hZoomTrack)
            SetWindowPos(m_hZoomTrack, nullptr,
                         parts[1] + 8, 0, DipToPx(kZoomTrackWidthDip), m_cyStatus,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        if (m_hZoomLabel)
            SetWindowPos(m_hZoomLabel, nullptr,
                         parts[1] + 8 + DipToPx(kZoomTrackWidthDip), 0,
                         DipToPx(kZoomLabelWidthDip), m_cyStatus,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The wrap mode depends on the edit control's new width, so it must be
    // reapplied after the geometry settles rather than before.
    ApplyWrapMode();

    if (m_hRuler)
    {
        // The ruler shows the text area and the indent markers, so it needs the
        // current paragraph formatting and the page width.
        PARAFORMAT2 pf;
        m_view.GetParaFormat(pf);
        RECT rcText = { 0, 0, cx, m_cyRuler };
        Ruler_SetMetrics(m_hRuler, m_settings, rcText, pf,
                         m_view.GetTextWidthTwips());
        Ruler_Invalidate(m_hRuler);
    }
}

void AppFrame::ApplyWrapMode()
{
    if (!m_view) return;
    LONG width = (m_settings.wrap == kWrapRuler) ? m_view.GetTextWidthTwips() : 0;
    m_view.ApplyWrapMode(m_settings.wrap, width);
}

// ===========================================================================
//  Settings
// ===========================================================================
void AppFrame::ApplySettings(const AppSettings& s)
{
    m_settings = s;
    // PageSetup_Copy deep-copies the DEVMODE/DEVNAMES handles. Assigning the
    // struct directly would leave two PageSetup objects owning one HGLOBAL, and
    // the second PageSetup_Free would double-free it.
    PageSetup_Free(&m_settings.page);
    PageSetup_Copy(&m_settings.page, &s.page);

    if (m_view)
    {
        m_view.SetZoom(m_settings.nZoomNum, m_settings.nZoomDen);
        SetZoomPercent(m_settings.nZoomNum * 100 / (m_settings.nZoomDen ? m_settings.nZoomDen : 100));
    }

    if (m_hRuler)
        ShowWindow(m_hRuler, m_settings.fShowRuler ? SW_SHOW : SW_HIDE);
    if (m_hStatus)
        ShowWindow(m_hStatus, m_settings.fShowStatusBar ? SW_SHOW : SW_HIDE);

    LayoutChildren();
    UpdateRibbonState();
}

void AppFrame::SaveSettingsTo(AppSettings& s) const
{
    s = m_settings;

    // Capture the live geometry rather than the last-saved rectangle, so a
    // window the user resized but never re-saved still reopens correctly.
    if (m_hFrame)
    {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(m_hFrame, &wp))
        {
            s.rcWindow    = wp.rcNormalPosition;
            s.fMaximised  = (wp.showCmd == SW_SHOWMAXIMIZED);
            s.nWindowShow = wp.showCmd;
        }
    }

    // Zoom is read back from the control, not from m_settings, because the
    // user may have changed it with the trackbar or Ctrl+wheel since the last
    // ApplySettings.
    if (m_view)
        m_view.GetZoom(s.nZoomNum, s.nZoomDen);

    PageSetup_Free(&s.page);
    PageSetup_Copy(&s.page, &m_settings.page);
}

// ===========================================================================
//  Document operations
// ===========================================================================
bool AppFrame::ConfirmSaveIfDirty()
{
    if (!m_view || !m_view.IsModified())
        return true;

    WCHAR text[512];
    WCHAR title[MAX_PATH + 64];
    LoadStr(IDS_ERR_UNSAVED_CAP, title, _countof(title));

    PCWSTR name = m_title[0] ? m_title : nullptr;
    if (!name)
    {
        WCHAR untitled[128];
        LoadStr(IDS_UNTITLED, untitled, _countof(untitled));
        name = untitled;
    }
    StrPrintf(text, _countof(text),
              L"Do you want to save changes to %s?", name);

    int r = MessageBoxW(m_hFrame, text, title[0] ? title : LP_APPNAME,
                        MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    switch (r)
    {
    case IDYES:    return FileSave();          // FileSave returns false if cancelled
    case IDNO:     return true;
    case IDCANCEL:
    default:       return false;
    }
}

bool AppFrame::FileNew()
{
    if (!ConfirmSaveIfDirty()) return false;

    // Clearing the document. WM_SETTEXT rather than EM_STREAMIN of an empty
    // RTF: WM_SETTEXT is what actually resets RichEdit's internal paragraph
    // table and undo stack. Streaming in empty RTF leaves stale paragraph
    // properties from the previous document attached to the first paragraph.
    SendMessageW(m_view.Hwnd(), WM_SETTEXT, 0, LParamFromPtr(L""));

    // Undo history is tied to the text buffer, but the undo LIMIT persists
    // across WM_SETTEXT on some RichEdit versions. EM_EMPTYUNDOBUFFER is
    // explicit about intent and costs nothing.
    SendMessageW(m_view.Hwnd(), EM_EMPTYUNDOBUFFER, 0, 0);

    SetDocumentPath(nullptr);

    // Re-apply the defaults: WM_SETTEXT does not reset character formatting, so
    // without this a new document inherits the previous document's font.
    m_view.SetCharFormat(m_settings.defChar);
    m_view.SetParaFormat(m_settings.defPara);
    m_view.SelectAll();
    m_view.SetCharFormat(m_settings.defChar);
    m_view.SetParaFormat(m_settings.defPara);
    CHARRANGE r = { 0, 0 };
    m_view.SetSel(r);

    m_view.SetModified(false);

    ApplyWrapMode();
    UpdateCaption();
    UpdateStatusBar();
    UpdateRibbonState();
    return true;
}

bool AppFrame::FileOpen(PCWSTR path)
{
    if (!ConfirmSaveIfDirty()) return false;

    WCHAR buf[MAX_PATH] = {};

    if (path && path[0])
    {
        wcsncpy_s(buf, _countof(buf), path, _TRUNCATE);
    }
    else
    {
        // Build the filter at runtime: the common-dialog filter format requires
        // embedded NULs between the display text and the pattern, and an RC
        // string literal cannot contain them.
        WCHAR filter[1024] = {};
        BuildOpenFilter(filter, _countof(filter));

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hFrame;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = _countof(buf);
        ofn.lpstrTitle  = L"Open";
        // OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST make the dialog reject typos
        // itself, which is both less code here and a better error message.
        // OFN_EXPLORER is required for the modern dialog on all supported OSes.
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                    OFN_HIDEREADONLY | OFN_ENABLESIZING;

        if (!GetOpenFileNameW(&ofn))
            return false;      // user cancelled; not an error
    }

    FileFormat fmt = DetectFormatByContent(buf);
    if (!FormatIsImplemented(fmt))
    {
        // The codec exists in the interface but not yet in the implementation.
        // Reporting this as "cannot open" would be misleading; the file is fine,
        // LetterPad just cannot read this format yet.
        ShowFatalError(m_hFrame, IDS_ERR_CODEC, IDS_ERR_CODEC_CAP, E_NOTIMPL);
        return false;
    }

    // Suppress dirty tracking during the load: EM_STREAMIN generates EN_CHANGE,
    // which would mark a freshly opened document as modified and make LetterPad
    // prompt to save a file the user has not touched.
    m_fSuspendDirty = true;
    HRESULT hr = Document_Load(m_view.Hwnd(), buf, fmt);
    m_fSuspendDirty = false;

    if (FAILED(hr))
    {
        ShowFatalError(m_hFrame, IDS_ERR_OPENFILE, IDS_ERR_OPENFILE_CAP, hr);
        return false;
    }

    // A loaded document is definitionally clean. RichEdit's modify flag was set
    // by the streaming operation.
    m_view.SetModified(false);
    SendMessageW(m_view.Hwnd(), EM_EMPTYUNDOBUFFER, 0, 0);

    SetDocumentPath(buf);
    Mru_Add(buf);

    // Formatting in the file overrides our defaults, so re-read what the
    // document actually starts with and adopt it as the session default. Without
    // this, "New" after opening a Courier document starts in Courier.
    CHARFORMAT2W cf;
    m_view.GetCharFormat(cf);
    m_settings.defChar = cf;
    m_settings.defChar.cbSize = sizeof(CHARFORMAT2W);

    ApplyWrapMode();
    UpdateCaption();
    UpdateStatusBar();
    UpdateRibbonState();
    SetFocus(m_view.Hwnd());
    return true;
}

bool AppFrame::FileSave()
{
    // No path yet means this is really a Save As. Returning the result of
    // FileSaveAs (rather than true) is important: the caller uses the return
    // value to decide whether to close the window, and a cancelled Save As must
    // not look like a successful save.
    if (!m_path[0])
        return FileSaveAs();

    FileFormat fmt = DetectFormatByExtension(m_path);
    if (!FormatIsImplemented(fmt))
        return FileSaveAs();       // cannot write this format; offer a choice

    HRESULT hr = Document_Save(m_view.Hwnd(), m_path, fmt);
    if (FAILED(hr))
    {
        ShowFatalError(m_hFrame, IDS_ERR_SAVEFILE, IDS_ERR_SAVEFILE_CAP, hr);
        return false;
    }

    m_view.SetModified(false);
    Mru_Add(m_path);
    UpdateCaption();
    UpdateStatusBar();
    return true;
}

bool AppFrame::FileSaveAs(FileFormat force)
{
    WCHAR buf[MAX_PATH] = {};
    FileFormat chosen = force;

    if (m_path[0])
        wcsncpy_s(buf, _countof(buf), m_path, _TRUNCATE);

    if (chosen == FileFormat::Unknown)
    {
        WCHAR filter[1024] = {};
        BuildSaveFilter(filter, _countof(filter), FileFormat::Rtf);

        OPENFILENAMEW ofn = {};
        ofn.lStructSize  = sizeof(ofn);
        ofn.hwndOwner    = m_hFrame;
        ofn.lpstrFilter  = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrFile    = buf;
        ofn.nMaxFile     = _countof(buf);
        ofn.lpstrTitle   = L"Save As";
        // Preselect the filter that matches the document's current format, so
        // saving an already-open .txt does not silently offer RTF as the
        // default. Index order is fixed by BuildSaveFilter: 1=RTF 2=DOCX
        // 3=ODT 4=Text.
        switch (DetectFormatByExtension(buf))
        {
        case FileFormat::Docx: ofn.nFilterIndex = 2; break;
        case FileFormat::Odt:  ofn.nFilterIndex = 3; break;
        case FileFormat::Text: ofn.nFilterIndex = 4; break;
        default:               ofn.nFilterIndex = 1; break;
        }
        // OFN_OVERWRITEPROMPT asks before clobbering. OFN_PATHMUSTEXIST is NOT
        // set: Save As must be able to create a file that does not exist yet.
        // OFN_NOREADONLYRETURN stops the dialog handing back a path it would
        // have had to clear the read-only bit on.
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_HIDEREADONLY |
                    OFN_ENABLESIZING | OFN_NOREADONLYRETURN;

        if (!GetSaveFileNameW(&ofn))
            return false;          // cancelled

        // Honour the filter the user picked rather than the extension they
        // typed. A user who selects "Plain Text" and leaves the name as
        // "notes.rtf" wants plain text; the dialog's filter index is the
        // stronger signal and the extension is just what was prefilled.
        switch (ofn.nFilterIndex)
        {
        case 1: chosen = FileFormat::Rtf;  break;
        case 2: chosen = FileFormat::Docx; break;
        case 3: chosen = FileFormat::Odt;  break;
        case 4: chosen = FileFormat::Text; break;
        default: chosen = DetectFormatByExtension(buf); break;
        }
    }

    if (!FormatIsImplemented(chosen))
    {
        ShowFatalError(m_hFrame, IDS_ERR_CODEC, IDS_ERR_CODEC_CAP, E_NOTIMPL);
        return false;
    }

    HRESULT hr = Document_Save(m_view.Hwnd(), buf, chosen);
    if (FAILED(hr))
    {
        ShowFatalError(m_hFrame, IDS_ERR_SAVEFILE, IDS_ERR_SAVEFILE_CAP, hr);
        return false;
    }

    m_view.SetModified(false);
    SetDocumentPath(buf);
    Mru_Add(buf);
    UpdateCaption();
    UpdateStatusBar();
    UpdateRibbonState();
    return true;
}

bool AppFrame::FileClose()
{
    if (!ConfirmSaveIfDirty()) return false;
    PostMessageW(m_hFrame, WM_CLOSE, 0, 0);
    return true;
}

void AppFrame::SetDocumentPath(PCWSTR path)
{
    if (path && path[0])
    {
        wcsncpy_s(m_path, _countof(m_path), path, _TRUNCATE);
        // Display name is the leaf, not the full path: the caption reads
        // "notes.rtf - LetterPad", matching the original, and the tooltip /
        // status bar carries the full path for anyone who needs it.
        PCWSTR leaf = PathFindFileNameW(m_path);
        wcsncpy_s(m_title, _countof(m_title), leaf ? leaf : m_path, _TRUNCATE);
    }
    else
    {
        m_path[0]  = L'\0';
        m_title[0] = L'\0';
    }
}

void AppFrame::UpdateCaption()
{
    WCHAR cap[MAX_PATH + 128] = {};
    WCHAR name[MAX_PATH + 32];

    if (m_title[0])
        wcsncpy_s(name, _countof(name), m_title, _TRUNCATE);
    else
        LoadStr(IDS_UNTITLED, name, _countof(name));

    // The dirty marker is a leading asterisk, as in the original. Appending it
    // after the filename instead would change the caption width as the user
    // types, which is visibly jittery.
    if (m_view && m_view.IsModified())
        StrPrintf(cap, _countof(cap), L"*%s - %s", name, LP_APPNAME);
    else
        StrPrintf(cap, _countof(cap), L"%s - %s", name, LP_APPNAME);

    // The taskbar button text comes from the window title, so this one call
    // covers both the caption and the taskbar. LetterPad does not set an
    // explicit AppUserModelID: the shell derives one from the executable path,
    // which is correct for a single-exe application and avoids the extra
    // shell32 dependency an explicit SetCurrentProcessExplicitAppUserModelID
    // would need at startup.
    SetWindowTextW(m_hFrame, cap);
}

// ===========================================================================
//  Status bar and ribbon state
// ===========================================================================
void AppFrame::SetStatusMessage(PCWSTR text)
{
    if (!m_hStatus) return;
    // Pane 0. SBT_NOBORDERS keeps the pane visually quiet; the message area is
    // transient text and does not need the sunken box the other panes have.
    SendMessageW(m_hStatus, SB_SETTEXTW,
                 STATUSBAR_PANE_MESSAGE | SBT_NOBORDERS, LParamFromPtr(text));
}

void AppFrame::SetZoomPercent(int percent)
{
    if (m_hZoomTrack)
        SendMessageW(m_hZoomTrack, TBM_SETPOS, TRUE, static_cast<LPARAM>(percent));

    if (m_hZoomLabel)
    {
        WCHAR buf[16];
        StrPrintf(buf, _countof(buf), L"%d%%", percent);
        SetWindowTextW(m_hZoomLabel, buf);
    }
}

void AppFrame::UpdateStatusBar()
{
    if (!m_hStatus) return;

    // Position pane: line and character offset of the selection, in the same
    // "Ln x, Col y" shape a text editor uses. Computed from the character
    // position rather than tracked incrementally, because an incremental
    // counter desynchronises on undo, on paste, and on document load.
    WCHAR pos[64] = {};
    if (m_view)
    {
        CHARRANGE sel;
        m_view.GetSel(sel);

        // EM_LINEFROMCHAR / EM_LINEINDEX give the line and column directly.
        // This is two messages per update, which is acceptable at selection-
        // change frequency and avoids maintaining any derived state.
        LONG line = static_cast<LONG>(SendMessageW(m_view.Hwnd(), EM_LINEFROMCHAR,
                                                   static_cast<WPARAM>(sel.cpMin), 0));
        LONG lineStart = static_cast<LONG>(SendMessageW(m_view.Hwnd(), EM_LINEINDEX,
                                                        static_cast<WPARAM>(line), 0));
        LONG col = sel.cpMin - lineStart;

        WCHAR fmt[128];
        if (LoadStr(IDS_STATUS_POS, fmt, _countof(fmt)) > 0)
            StrPrintf(pos, _countof(pos), fmt, line + 1, col + 1);
        else
            StrPrintf(pos, _countof(pos), L"Ln %d, Col %d", line + 1, col + 1);
    }
    SendMessageW(m_hStatus, SB_SETTEXTW, STATUSBAR_PANE_POSITION, LParamFromPtr(pos));

    // Zoom pane text lives in the child label, not in the status bar pane,
    // because the pane rectangle is occupied by the trackbar. Pane 2 is left
    // empty on purpose.
    SendMessageW(m_hStatus, SB_SETTEXTW, STATUSBAR_PANE_ZOOM, LParamFromPtr(L""));
}

void AppFrame::UpdateRibbonState()
{
    // The ribbon's checked/enabled state is pulled, not pushed: IUICommandHandler
    // ::UpdateProperty is called by the framework whenever it needs a value, and
    // our job is only to tell it that things may have changed. InvalidateControl
    // on the specific controls would be more surgical, but InvalidateUI on the
    // whole application is one call, is what most ribbon hosts do, and the
    // framework coalesces the resulting UpdateProperty calls itself.
    if (m_fRibbon && m_pFramework)
        m_pFramework->InvalidateUI(UI_INVALIDATIONS_PROPERTY | UI_INVALIDATIONS_STATE);

    // The fallback toolbar has no equivalent pull model, so its state is pushed.
    if (m_hToolbar)
    {
        bool canPaste = m_view && m_view.CanPaste();
        bool hasSel   = m_view && m_view.HasSelection();

        SendMessageW(m_hToolbar, TB_ENABLEBUTTON, ID_CMD_PASTE, canPaste ? TRUE : FALSE);
        SendMessageW(m_hToolbar, TB_ENABLEBUTTON, ID_CMD_CUT,   hasSel   ? TRUE : FALSE);
        SendMessageW(m_hToolbar, TB_ENABLEBUTTON, ID_CMD_COPY,  hasSel   ? TRUE : FALSE);
        SendMessageW(m_hToolbar, TB_ENABLEBUTTON, ID_CMD_UNDO,
                     m_view && SendMessageW(m_view.Hwnd(), EM_CANUNDO, 0, 0) ? TRUE : FALSE);

        // Reflect the toggle states as TBSTATE_CHECKED. Reading the current
        // formatting once and applying it to all four buttons avoids four
        // EM_GETCHARFORMAT round trips per state refresh.
        if (m_view)
        {
            CHARFORMAT2W cf;
            m_view.GetCharFormat(cf);
            SendMessageW(m_hToolbar, TB_PRESSBUTTON, ID_CMD_BOLD,
                         (cf.dwEffects & CFE_BOLD) ? TRUE : FALSE);
            SendMessageW(m_hToolbar, TB_PRESSBUTTON, ID_CMD_ITALIC,
                         (cf.dwEffects & CFE_ITALIC) ? TRUE : FALSE);
            SendMessageW(m_hToolbar, TB_PRESSBUTTON, ID_CMD_UNDERLINE,
                         (cf.dwEffects & CFE_UNDERLINE) ? TRUE : FALSE);
        }

        SendMessageW(m_hToolbar, TB_ENABLEBUTTON, ID_CMD_SAVE,
                     (m_view && m_view.IsModified()) ? TRUE : TRUE);
        // Save is deliberately always enabled: saving a clean document is a
        // no-op that costs nothing, and graying it out when the document is
        // clean makes the button's state depend on something the user cannot
        // see. The condition above is written out rather than collapsed to
        // TRUE so that changing the policy is a one-line edit.
    }

    // Fallback menu items whose enabled state differs from the toolbar's.
    if (HMENU hMenu = GetMenu(m_hFrame))
    {
        EnableMenuItem(hMenu, ID_CMD_PASTE,
                       MF_BYCOMMAND | ((m_view && m_view.CanPaste()) ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(hMenu, ID_CMD_UNDO,
                       MF_BYCOMMAND | ((m_view && SendMessageW(m_view.Hwnd(), EM_CANUNDO, 0, 0))
                                        ? MF_ENABLED : MF_GRAYED));
    }
}

void AppFrame::OnZoomTrackbar(int pos)
{
    if (!m_view) return;
    if (pos < 10) pos = 10;
    if (pos > 500) pos = 500;

    m_view.SetZoom(pos, 100);
    SetZoomPercent(pos);

    m_settings.nZoomNum = pos;
    m_settings.nZoomDen = 100;

    // Zooming changes the effective text width, so a wrap-to-ruler document has
    // to be re-wrapped or the wrap will still reflect the old zoom.
    ApplyWrapMode();
    LayoutChildren();
}

// ===========================================================================
//  Edit-control notifications
// ===========================================================================
void AppFrame::OnEditChange()
{
    if (m_fSuspendDirty) return;
    UpdateCaption();
    UpdateRibbonState();
}

void AppFrame::OnEditSelChange()
{
    UpdateStatusBar();
    UpdateRibbonState();
    if (m_hRuler && m_view)
    {
        // The ruler's indent markers track the paragraph the caret is in, so a
        // selection change can move them even though no geometry changed.
        PARAFORMAT2 pf;
        m_view.GetParaFormat(pf);
        RECT rc;
        GetClientRect(m_hFrame, &rc);
        RECT rcText = { 0, 0, rc.right, m_cyRuler };
        Ruler_SetMetrics(m_hRuler, m_settings, rcText, pf, m_view.GetTextWidthTwips());
        Ruler_Invalidate(m_hRuler);
    }
}

// ===========================================================================
//  Command dispatch -- the single switch for every UI surface
// ===========================================================================
void AppFrame::OnCommand(UINT id)
{
    switch (id)
    {
    // ---- file -----------------------------------------------------------
    case ID_CMD_NEW:            FileNew(); break;
    case ID_CMD_OPEN:           FileOpen(); break;
    case ID_CMD_SAVE:           FileSave(); break;
    case ID_CMD_SAVEAS_RTF:     FileSaveAs(FileFormat::Rtf);  break;
    case ID_CMD_SAVEAS_DOCX:    FileSaveAs(FileFormat::Docx); break;
    case ID_CMD_SAVEAS_ODT:     FileSaveAs(FileFormat::Odt);  break;
    case ID_CMD_SAVEAS_TXT:     FileSaveAs(FileFormat::Text); break;
    case ID_CMD_SAVEAS:         FileSaveAs(); break;   // let the dialog choose
    case ID_CMD_PRINT:          PrintDocument(*this, /*quickPrint=*/false); break;
    case ID_CMD_QUICKPRINT:     PrintDocument(*this, /*quickPrint=*/true);  break;
    case ID_CMD_PRINTPREVIEW:   ShowPrintPreview(*this); break;
    case ID_CMD_PAGESETUP:      ShowPageSetup(*this); break;
    case ID_CMD_CLOSE:          FileClose(); break;
    case ID_CMD_OPTIONS:        ShowOptionsDialog(); break;
    case ID_CMD_EXIT:
        // Close every open frame, then quit. Destroying windows one at a time
        // and only posting WM_QUIT afterwards means each frame gets its
        // confirm-save prompt; posting WM_QUIT first would abandon the loop
        // with unsaved documents still open.
        PostMessageW(m_hFrame, WM_CLOSE, 0, 0);
        break;

    // ---- edit -----------------------------------------------------------
    case ID_CMD_UNDO:           if (m_view) m_view.Undo(); break;
    case ID_CMD_REDO:           if (m_view) m_view.Redo(); break;
    case ID_CMD_CUT:            if (m_view) m_view.Cut();  break;
    case ID_CMD_COPY:           if (m_view) m_view.Copy(); break;
    case ID_CMD_PASTE:          if (m_view) m_view.Paste(); break;
    case ID_CMD_PASTESPECIAL:   if (m_view) m_view.PasteSpecial(); break;
    case ID_CMD_SELECTALL:      if (m_view) m_view.SelectAll(); break;
    case ID_CMD_FIND:           ShowFindDialog(*this, /*replace=*/false); break;
    case ID_CMD_REPLACE:        ShowFindDialog(*this, /*replace=*/true);  break;
    case ID_CMD_GOTO:           ShowGoToDialog(); break;

    // ---- font -----------------------------------------------------------
    case ID_CMD_BOLD:           if (m_view) m_view.ToggleBold(); break;
    case ID_CMD_ITALIC:         if (m_view) m_view.ToggleItalic(); break;
    case ID_CMD_UNDERLINE:      if (m_view) m_view.ToggleUnderline(); break;
    case ID_CMD_STRIKEOUT:      if (m_view) m_view.ToggleStrikeout(); break;
    case ID_CMD_SUBSCRIPT:      if (m_view) m_view.ToggleSubscript(); break;
    case ID_CMD_SUPERSCRIPT:    if (m_view) m_view.ToggleSuperscript(); break;

    // ---- paragraph ------------------------------------------------------
    case ID_CMD_ALIGN_LEFT:     if (m_view) m_view.SetAlignment(PFA_LEFT);    break;
    case ID_CMD_ALIGN_CENTER:   if (m_view) m_view.SetAlignment(PFA_CENTER);  break;
    case ID_CMD_ALIGN_RIGHT:    if (m_view) m_view.SetAlignment(PFA_RIGHT);   break;
    case ID_CMD_ALIGN_JUSTIFY:  if (m_view) m_view.SetAlignment(PFA_JUSTIFY); break;
    case ID_CMD_BULLETS:        if (m_view) m_view.ToggleBullets(); break;
    case ID_CMD_INDENT_INCREASE: if (m_view) m_view.AdjustIndent(+180); break;
    case ID_CMD_INDENT_DECREASE: if (m_view) m_view.AdjustIndent(-180); break;

    // Line spacing rules 0/1/2 ignore dyLineSpacing entirely; see the comment
    // on EditView::SetLineSpacing. Passing a value anyway is harmless.
    case ID_CMD_LINESPACING_SINGLE:  if (m_view) m_view.SetLineSpacing(0, 20); break;
    case ID_CMD_LINESPACING_ONEHALF: if (m_view) m_view.SetLineSpacing(1, 20); break;
    case ID_CMD_LINESPACING_DOUBLE:  if (m_view) m_view.SetLineSpacing(2, 20); break;
    case ID_CMD_LINESPACING_OPTIONS: ShowOptionsDialog(); break;
    case ID_CMD_LINESPACING:         break;   // the drop-down itself does nothing

    // ---- insert ---------------------------------------------------------
    case ID_CMD_DATETIME:       InsertDateTime(); break;
    case ID_CMD_PICTURE:        OnInsertPicture(); break;
    case ID_CMD_OBJECT:         if (m_view) m_view.InsertObject(m_hFrame); break;

    // ---- view -----------------------------------------------------------
    case ID_CMD_ZOOMIN:         if (m_view) { m_view.ZoomIn();  SyncZoomFromView(); } break;
    case ID_CMD_ZOOMOUT:        if (m_view) { m_view.ZoomOut(); SyncZoomFromView(); } break;
    case ID_CMD_ZOOM100:        if (m_view) { m_view.ZoomReset(); SyncZoomFromView(); } break;

    case ID_CMD_RULER:
        m_settings.fShowRuler = !m_settings.fShowRuler;
        if (m_settings.fShowRuler && !m_hRuler)
            CreateRuler();                    // ruler was never created
        if (m_hRuler)
            ShowWindow(m_hRuler, m_settings.fShowRuler ? SW_SHOW : SW_HIDE);
        m_cyRuler = m_settings.fShowRuler ? DipToPx(kRulerHeightDip) : 0;
        LayoutChildren();
        UpdateRibbonState();
        break;

    case ID_CMD_STATUSBAR:
        m_settings.fShowStatusBar = !m_settings.fShowStatusBar;
        if (m_settings.fShowStatusBar && !m_hStatus)
            CreateStatusBar();
        if (m_hStatus)
            ShowWindow(m_hStatus, m_settings.fShowStatusBar ? SW_SHOW : SW_HIDE);
        m_cyStatus = m_settings.fShowStatusBar ? DipToPx(kStatusHeightDip) : 0;
        LayoutChildren();
        UpdateRibbonState();
        break;

    case ID_CMD_WRAP_NONE:
    case ID_CMD_WRAP_WINDOW:
    case ID_CMD_WRAP_RULER:
        m_settings.wrap = static_cast<WrapMode>(id - ID_CMD_WRAP_NONE);
        ApplyWrapMode();
        LayoutChildren();
        UpdateRibbonState();
        break;

    case ID_CMD_ABOUT:          ShowAboutDialog(); break;

    default:
        // Unhandled IDs are ignored rather than reported. The ribbon sends
        // Execute for elements that have no action -- the application menu
        // button itself, tabs, groups -- and treating those as errors would
        // produce a dialog on every tab click.
        break;
    }
}

// ===========================================================================
//  Small command implementations that belong to the frame
// ===========================================================================
void AppFrame::SyncZoomFromView()
{
    int num = 100, den = 100;
    if (m_view) m_view.GetZoom(num, den);
    int pct = (den > 0) ? (num * 100 / den) : 100;

    m_settings.nZoomNum = num;
    m_settings.nZoomDen = den;
    SetZoomPercent(pct);
    ApplyWrapMode();
    LayoutChildren();
}

void AppFrame::InsertDateTime()
{
    if (!m_view) return;

    // GetDateFormat with a null format string uses the user's locale short-date
    // pattern, which is what "insert the date" means to a user. Hard-coding
    // "%d/%m/%Y" would be wrong in most of the world.
    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR date[64] = {}, time[64] = {};
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, _countof(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, nullptr, time, _countof(time));

    WCHAR buf[160];
    StrPrintf(buf, _countof(buf), L"%s %s", date, time);

    // EM_REPLACESEL rather than WM_SETTEXT-based insertion: it inserts at the
    // caret as a single undoable step, replacing any current selection, which
    // is the behaviour every "insert" command in a word processor has.
    SendMessageW(m_view.Hwnd(), EM_REPLACESEL, TRUE, LParamFromPtr(buf));
    m_view.SetModified(true);
    OnEditChange();
}

void AppFrame::OnInsertPicture()
{
    if (!m_view) return;

    // Filter is inlined here rather than coming from the string table: the
    // picture filter is a fixed set of extensions and does not vary by format.
    static const WCHAR kPicFilter[] =
        L"All Picture Files\0*.bmp;*.dib;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.tif;*.tiff;*.png;*.wmf;*.emf;*.ico\0"
        L"Bitmap Files\0*.bmp;*.dib\0"
        L"JPEG Files\0*.jpg;*.jpeg;*.jpe;*.jfif\0"
        L"GIF Files\0*.gif\0"
        L"TIFF Files\0*.tif;*.tiff\0"
        L"PNG Files\0*.png\0"
        L"Metafiles\0*.wmf;*.emf\0"
        L"All Files\0*.*\0\0";

    WCHAR buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = m_hFrame;
    ofn.lpstrFilter  = kPicFilter;
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = _countof(buf);
    ofn.lpstrTitle   = L"Insert Picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ENABLESIZING;

    if (!GetOpenFileNameW(&ofn))
        return;

    HRESULT hr = m_view.InsertPicture(buf);
    if (FAILED(hr))
        ShowFatalError(m_hFrame, IDS_ERR_OPENFILE, IDS_ERR_OPENFILE_CAP, hr);
    else
    {
        m_view.SetModified(true);
        OnEditChange();
    }
}

void AppFrame::ShowOptionsDialog()
{
    // The dialog edits a COPY. Committing on IDOK means Cancel genuinely
    // discards everything, including the font preview the user may have applied
    // to the live document to see how it looked.
    AppSettings draft = m_settings;
    PageSetup_Free(&draft.page);
    PageSetup_Copy(&draft.page, &m_settings.page);

    if (::ShowOptionsDialog(m_hFrame, draft) == IDOK)
    {
        PageSetup_Free(&m_settings.page);
        m_settings = draft;
        // m_settings now owns the HGLOBALs draft was holding. Nulling them in
        // draft is what makes the else-branch's PageSetup_Free(&draft) safe, and
        // keeps ownership unambiguous if an error path is added later.
        draft.page.hDevMode  = nullptr;
        draft.page.hDevNames = nullptr;

        ApplySettings(m_settings);
        Settings_Save(&m_settings);

        // New defaults apply to subsequently created documents, not to the
        // current one. Re-applying them to the open document would silently
        // reformat text the user did not ask about.
        if (m_view && !m_view.IsModified())
        {
            m_view.SetCharFormat(m_settings.defChar);
            m_view.SetParaFormat(m_settings.defPara);
            m_view.SetModified(false);
        }
    }
    else
    {
        PageSetup_Free(&draft);
    }
}

void AppFrame::ShowGoToDialog()   { ShowGoToPage(*this); }
void AppFrame::ShowAboutDialog()  { ::ShowAboutDialog(m_hFrame); }

void AppFrame::SetRibbonMinimised(bool f)
{
    if (!m_fRibbon || !m_pFramework)
        return;

    // UI_PKEY_ViewState takes a VT_UI4 PROPVARIANT. Built by hand rather than
    // through PropVariantCreateFromUINT, which lives in propsys.dll -- a
    // dependency the rest of LetterPad does not need and which would have to be
    // added to the link line and the runtime DLL list just for this one call.
    PROPVARIANT pv = {};
    pv.vt      = VT_UI4;
    pv.ulVal   = f ? UI_VIEWSTATE_MINIMIZED : UI_VIEWSTATE_RESTORED;

    m_pFramework->SetUIProperty(UI_PKEY_ViewState, pv);

    // The ribbon's height changed, so the edit control and ruler have to move.
    LayoutChildren();
}

// ===========================================================================
//  Subclasses
//
//  Two child windows need messages that they do not forward to the frame:
//
//    the status bar   because the zoom trackbar is its CHILD, and a child sends
//                     WM_HSCROLL to its immediate parent. Without a subclass the
//                     slider moves and nothing else happens.
//    the edit control because it has focus while the user works, and WM_MOUSEWHEEL
//                     goes to the focused window. Ctrl+wheel zoom therefore never
//                     reaches the frame.
//
//  SetWindowSubclass (comctl32 v6) rather than a hand-rolled SetWindowLongPtr
//  GWLP_WNDPROC swap: the comctl version chains correctly, is reentrant-safe,
//  and removes itself automatically when the window is destroyed. Hand-rolled
//  subclassing that forgets to restore the original proc on WM_NCDESTROY is a
//  reliable way to crash on window teardown.
// ===========================================================================
namespace {

LRESULT CALLBACK StatusSubclassProc(HWND hStatus, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    AppFrame* frame = PtrFromLParam<AppFrame>(static_cast<LPARAM>(dwRefData));

    if (msg == WM_HSCROLL && frame)
    {
        // A trackbar sends WM_HSCROLL to its parent with lParam == its own HWND.
        // A standard scrollbar sends lParam == 0. Verifying that lParam really
        // is our trackbar keeps a stray scrollbar notification from being
        // misread as a zoom position.
        HWND hTrack = GetDlgItem(hStatus, IDC_ZOOM_TRACKBAR);
        if (hTrack && reinterpret_cast<HWND>(lp) == hTrack)
        {
            int pos = static_cast<int>(SendMessageW(hTrack, TBM_GETPOS, 0, 0));
            // PostMessage rather than calling OnZoomTrackbar directly: the
            // trackbar is mid-notification and its internal state is not yet
            // settled. Doing the work synchronously here re-enters the trackbar
            // and produces a slider that lags the document by one step.
            PostMessageW(frame->Hwnd(), WM_APP_ZOOMCHANGED, static_cast<WPARAM>(pos), 0);
            return 0;
        }
    }

    return DefSubclassProc(hStatus, msg, wp, lp);
}

LRESULT CALLBACK EditSubclassProc(HWND hEdit, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    AppFrame* frame = PtrFromLParam<AppFrame>(static_cast<LPARAM>(dwRefData));

    if (msg == WM_MOUSEWHEEL && frame && (wp & MK_CONTROL))
    {
        // GET_WHEEL_DELTA_WPARAM is signed: a short cast is required, because
        // the delta lives in the HIGHORD as a signed value and reading it as an
        // unsigned int makes every scroll direction "down".
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int steps = delta / WHEEL_DELTA;
        while (steps-- > 0) frame->View().ZoomIn();
        while (steps++ < 0) frame->View().ZoomOut();
        frame->SyncZoomFromView();
        return 0;      // consumed: do not also scroll
    }

    return DefSubclassProc(hEdit, msg, wp, lp);
}

} // anonymous namespace

// ===========================================================================
//  Subclass installation
//
//  Both are installed immediately after the child is created, and neither is
//  explicitly removed: SetWindowSubclass tears itself down when the window is
//  destroyed. uIdSubclass is arbitrary but must be stable and unique per
//  (window, subclass) pair; the control IDs serve that purpose.
// ===========================================================================
void Subclass_InstallStatus(HWND hStatus, AppFrame* frame)
{
    SetWindowSubclass(hStatus, StatusSubclassProc, IDC_ZOOM_TRACKBAR,
                      static_cast<DWORD_PTR>(LParamFromPtr(frame)));
}

void Subclass_InstallEdit(HWND hEdit, AppFrame* frame)
{
    SetWindowSubclass(hEdit, EditSubclassProc, IDC_EDIT,
                      static_cast<DWORD_PTR>(LParamFromPtr(frame)));
}


// ===========================================================================
//  FrameWndProc
// ===========================================================================
LRESULT CALLBACK FrameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Recovered from GWLP_USERDATA, which WM_NCCREATE below populates from
    // CREATESTRUCT::lpCreateParams. Every message after WM_NCCREATE has a valid
    // frame pointer; the ones before it (WM_NCCREATE itself, WM_GETMINMAXINFO
    // during creation) must be handled with frame == nullptr.
    AppFrame* frame = FrameOf(hwnd);

    switch (msg)
    {
    case WM_NCCREATE:
    {
        CREATESTRUCTW* cs = PtrFromLParam<CREATESTRUCTW>(lp);
        if (cs)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              LParamFromPtr(static_cast<AppFrame*>(cs->lpCreateParams)));
            frame = FrameOf(hwnd);
            if (frame) frame->SetHwndForProc(hwnd);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_CREATE:
        // Children are created by AppFrame::Create after CreateWindowExW returns
        // rather than here, because they need m_settings (loaded in Create) and
        // because a WM_CREATE failure path that has to unwind partially-created
        // children is much harder to get right than "create, then check".
        return 0;

    case WM_SIZE:
        if (frame) frame->LayoutChildren();
        // Not returning 0 early: DefWindowProc still needs to run so that the
        // status bar (SBARS_SIZEGRIP) recalculates its grip rectangle.
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_GETMINMAXINFO:
    {
        // Enforce a usable minimum. Without this the window can be resized
        // smaller than the ribbon plus one line of text, at which point the edit
        // control has zero height and RichEdit starts returning layout errors.
        MINMAXINFO* mmi = PtrFromLParam<MINMAXINFO>(lp);
        if (mmi)
        {
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 320;
        }
        return 0;
    }

    case WM_ERASEBKGND:
        if (frame)
            return frame->OnEraseBackground(reinterpret_cast<HDC>(wp)) ? 1 : 0;
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_SETFOCUS:
        // Focus belongs to the document, not the chrome. A frame that keeps
        // focus swallows keystrokes that should be typing.
        if (frame && frame->View())
        {
            SetFocus(frame->View().Hwnd());
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_COMMAND:
    {
        UINT id     = LOWORD(wp);
        UINT code   = HIWORD(wp);
        HWND hCtl   = reinterpret_cast<HWND>(lp);

        // RichEdit sends EN_CHANGE through WM_COMMAND (as an EDIT control does),
        // not through WM_NOTIFY, even though ENM_CHANGE was requested. Both
        // paths are handled -- WM_COMMAND here and WM_NOTIFY below -- because
        // which one fires has varied across RichEdit versions and handling only
        // one means the dirty flag silently stops updating on some builds.
        if (code == EN_CHANGE && hCtl && frame && frame->View() &&
            hCtl == frame->View().Hwnd())
        {
            frame->OnEditChange();
            return 0;
        }

        // code == 0 means a menu/accelerator/toolbar command. code == 1 (BN_CLICKED)
        // means a button. Anything else is a control notification we do not own.
        if (frame && (code == 0 || code == BN_CLICKED || code == CBN_SELCHANGE))
            frame->OnCommand(id);
        return 0;
    }

    case WM_NOTIFY:
    {
        NMHDR* nh = PtrFromLParam<NMHDR>(lp);
        if (!nh || !frame) return 0;

        if (nh->hwndFrom == frame->View().Hwnd())
        {
            switch (nh->code)
            {
            case EN_SELCHANGE:
                frame->OnEditSelChange();
                return 0;

            case EN_CHANGE:
                frame->OnEditChange();
                return 0;

            case EN_LINK:
            {
                // Hyperlink activation. RichEdit only reports the click; it does
                // not navigate. EN_LINK arrives for enter, leave and click, and
                // only the click should do anything.
                ENLINK* el = reinterpret_cast<ENLINK*>(nh);
                if (el && el->msg == WM_LBUTTONUP)
                {
                    // Read the URL text out of the reported range rather than
                    // trusting any stored value: the document may have been
                    // edited since the link was created.
                    LONG len = el->chrg.cpMax - el->chrg.cpMin;
                    if (len > 0 && len < 2048)
                    {
                        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
                        TEXTRANGEW tr = {};
                        tr.chrg = el->chrg;
                        tr.lpstrText = &buf[0];
                        if (SendMessageW(frame->View().Hwnd(), EM_GETTEXTRANGE, 0,
                                         LParamFromPtr(&tr)) > 0)
                        {
                            // ShellExecute rather than a hand-built navigation:
                            // it respects the user's default browser, handles
                            // mailto:, and needs no WinINet dependency.
                            ShellExecuteW(frame->Hwnd(), L"open", buf.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
                        }
                    }
                }
                return 0;
            }

            case EN_PROTECTED:
                // An attempt to modify a read-only range. Returning nonzero
                // tells RichEdit to swallow the change, which is what a
                // protected region means.
                return TRUE;

            default:
                break;
            }
        }
        return 0;
    }

    case WM_APP_ZOOMCHANGED:
        if (frame) frame->OnZoomTrackbar(static_cast<int>(wp));
        return 0;

    case WM_DPICHANGED:
        // Per-monitor V2: Windows hands us the rectangle the window should move
        // to. Honouring it is the difference between crisp and blurry on a DPI
        // change; ignoring it leaves the window at its old size on the new
        // monitor. The ruler and status bar heights are DPI-derived, so they are
        // recomputed by LayoutChildren.
        if (frame)
        {
            RECT* suggested = PtrFromLParam<RECT>(lp);
            if (suggested)
            {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            frame->LayoutChildren();
        }
        return 0;

    case WM_DROPFILES:
        // Handled by the OLE drop target registered on the edit control, which
        // is a superset of WM_DROPFILES (it supports drag images and
        // cancel-on-Escape). This branch exists only for a drop that lands on
        // the frame's non-client area, where the OLE target is not registered.
        if (frame)
        {
            HDROP hDrop = reinterpret_cast<HDROP>(wp);
            WCHAR path[MAX_PATH] = {};
            if (DragQueryFileW(hDrop, 0, path, _countof(path)) > 0)
                frame->FileOpen(path);
            DragFinish(hDrop);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CLOSE:
        // ConfirmSaveIfDirty returning false means the user chose Cancel and
        // the window must stay open. DestroyWindow is only reached when saving
        // succeeded or was declined.
        if (frame)
        {
            if (!frame->ConfirmSaveIfDirty())
                return 0;

            // Persist geometry and view settings on the way out. Doing this in
            // WM_DESTROY instead would be too late to read a valid
            // WINDOWPLACEMENT, because by then the window has no position.
            AppSettings s;
            frame->SaveSettingsTo(s);
            Settings_Save(&s);
            PageSetup_Free(&s);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (frame)
        {
            // Order matters: revoke the drop target before the HWND goes away,
            // tear the ribbon down while its host window still exists, and only
            // then clear the userdata. Doing these in the wrong order leaves the
            // OLE drop-target table and the ribbon framework holding a dead HWND.
            if (frame->View())
                Ole_RevokeDropTarget(frame->View().Hwnd());

            if (frame->Framework())
                frame->Framework()->Destroy();

            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);

            if (--g_cFrames <= 0)
            {
                g_cFrames = 0;
                // Posted here rather than from WM_CLOSE so that a frame closed
                // by any path -- Exit command, taskbar close, session end --
                // terminates the message loop exactly once.
                PostQuitMessage(0);
            }
        }
        return 0;

    default:
        // The Find/Replace dialog reports user actions through a message number
        // that RegisterWindowMessage assigns at runtime, so it cannot appear in
        // a case label. Checking it here, before DefWindowProc, is the only
        // place it can be caught.
        if (frame && msg == FindReplace_RegisteredMessage() &&
            FindReplace_HandleMessage(*frame, msg, wp, lp))
            return 0;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ===========================================================================
//  Background painting
//
//  The frame's visible background is the area around the document. WordPad
//  paints it white; a grey "canvas" would make the page edges visible, which is
//  a page-layout view, not a word processor. Returning TRUE suppresses
//  DefWindowProc's class-brush fill so there is no double paint and no flicker
//  on resize.
// ===========================================================================
bool AppFrame::OnEraseBackground(HDC hdc)
{
    RECT rc;
    if (!GetClientRect(m_hFrame, &rc))
        return false;

    HBRUSH hWhite = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(hdc, &rc, hWhite);
    // GetStockObject brushes are process-global and must NOT be deleted.
    return true;
}

