// ============================================================================
//  LetterPad.h -- shared declarations for the whole application
//
//  ARCHITECTURE IN ONE PARAGRAPH
//  -----------------------------
//  LetterPad is one window class (`LetterPadFrame`) hosting four children: a
//  ribbon (IUIFramework, top), a ruler (custom-drawn, below the ribbon), a
//  RichEdit 4.1 control (`RICHEDIT50W`, the client area), and a status bar
//  with an embedded zoom trackbar (bottom). There is no separate document
//  model and no undo stack of our own: RichEdit *is* the document. It owns the
//  text, the formatting, the undo/redo history and the OLE object table, and
//  LetterPad drives it entirely through the documented EM_* message set. That
//  is the same architecture WordPad uses, and it is the reason this codebase
//  has no text-buffer code, no layout code and no pagination code of its own
//  beyond the EM_FORMATRANGE loop in print.cpp.
//
//  One document per window, no MDI. Command dispatch is unified: the ribbon,
//  the fallback toolbar, the fallback menu and the accelerator table all emit
//  the same ID_CMD_* values into the same AppFrame::OnCommand switch.
// ============================================================================
#pragma once

#include "stdafx.h"
#include "ribbon/RibbonIDs.h"

// ----------------------------------------------------------------------------
//  Version. The canonical definitions live in resource.h so that the RC script
//  and the C++ code cannot disagree about what version was built.
// ----------------------------------------------------------------------------
#include "resource.h"   // already included by stdafx.h; idempotent

#define LP_APPNAME          L"LetterPad"
#define LP_FRAME_CLASS      L"LetterPadFrame"
#define LP_RULER_CLASS      L"LetterPadRuler"
#define LP_REGISTRY_KEY     L"Software\\LetterPad"
#define LP_MRU_KEY          L"Software\\LetterPad\\Recent File List"

// ----------------------------------------------------------------------------
//  Globals (defined in main.cpp)
// ----------------------------------------------------------------------------
extern HINSTANCE g_hInst;
extern HACCEL    g_hAccel;

// Msftedit.dll handle. Loaded once at startup and never unloaded: the
// RICHEDIT50W window class is registered against it, and unloading the module
// while an instance of that class exists is a guaranteed crash.
extern HMODULE   g_hModRichEdit;

// Number of live frames. LetterPad is multi-instance within one process only
// in the sense that /p and the shell verb can chain; in practice one window per
// process, like WordPad. Kept for the Exit command, which must close them all.
extern int       g_cFrames;

// ----------------------------------------------------------------------------
//  Measurement units
//
//  RichEdit works in TWIPS everywhere: margins, indents, EM_SETTARGETDEVICE,
//  PARAFORMAT2 dy* fields. 1 twip = 1/20 point = 1/1440 inch.
//  LetterPad stores nothing but twips and converts only at the UI boundary,
//  which is why the ruler, the options dialog and page setup all take a
//  UnitSystem parameter rather than having three private unit conventions.
// ----------------------------------------------------------------------------
constexpr int kTwipsPerInch = 1440;
constexpr int kTwipsPerCm   = 567;    // 1440 / 2.54, truncated like WordPad
constexpr int kTwipsPerPt   = 20;
constexpr int kTwipsPerPica = 240;    // 12 points

enum UnitSystem
{
    kUnitInch  = 0,
    kUnitCm    = 1,
    kUnitPoint = 2,
    kUnitPica  = 3,
};

// Twips in one of the user's chosen units. kTwipsPerCm is the truncated value
// above rather than 566.93, which is why a 2.54 cm margin and a 1 inch margin
// differ by one twip. That is a real, visible discrepancy in the original too;
// reproducing it is cheaper than explaining it.
inline int TwipsPerUnit(UnitSystem u)
{
    switch (u)
    {
    case kUnitCm:    return kTwipsPerCm;
    case kUnitPoint: return kTwipsPerPt;
    case kUnitPica:  return kTwipsPerPica;
    case kUnitInch:
    default:         return kTwipsPerInch;
    }
}

inline const wchar_t* UnitSuffix(UnitSystem u)
{
    switch (u)
    {
    case kUnitCm:    return L" cm";
    case kUnitPoint: return L" pt";
    case kUnitPica:  return L" pi";
    case kUnitInch:
    default:         return L" in";
    }
}

// ----------------------------------------------------------------------------
//  Word wrap
//
//  Implemented three different ways because RichEdit exposes three different
//  mechanisms and each mode needs a different one:
//
//    kWrapNone    ES_AUTOHSCROLL style set, horizontal scrollbar shown, and
//                 EM_SETTARGETDEVICE(0, 0) so the device is "as wide as needed".
//    kWrapWindow  ES_AUTOHSCROLL cleared, no horizontal scrollbar, and
//                 EM_SETTARGETDEVICE(0, 0) so RichEdit wraps to the client width.
//    kWrapRuler   ES_AUTOHSCROLL cleared, no horizontal scrollbar, and
//                 EM_SETTARGETDEVICE(0, widthInTwips) so RichEdit wraps to an
//                 explicit width independent of how wide the window is.
//
//  The style bit has to change at runtime, so SetWindowLongPtr + SetWindowPos
//  with SWP_FRAMECHANGED. Forgetting SWP_FRAMECHANGED is the classic bug here:
//  the style changes but the scrollbars do not.
// ----------------------------------------------------------------------------
enum WrapMode
{
    kWrapNone   = 0,
    kWrapWindow = 1,
    kWrapRuler  = 2,
};

// ----------------------------------------------------------------------------
//  Page setup
//
//  hDevMode and hDevNames are owned by the PageSetup and freed by
//  PageSetup_Free. They are persisted to the registry as opaque binary blobs,
//  which is both what WordPad does and the only robust option: a DEVMODE's
//  layout is driver-specific and its driver-extra bytes must round-trip
//  untouched or the printer selection silently changes.
// ----------------------------------------------------------------------------
struct PageSetup
{
    HGLOBAL hDevMode;       // may be null: fall back to the default printer
    HGLOBAL hDevNames;      // may be null: same

    LONG    marginLeft;     // twips
    LONG    marginTop;
    LONG    marginRight;
    LONG    marginBottom;

    PageSetup()
        : hDevMode(nullptr), hDevNames(nullptr)
        , marginLeft(kTwipsPerInch), marginTop(kTwipsPerInch)
        , marginRight(kTwipsPerInch), marginBottom(kTwipsPerInch)
    {}
};

void PageSetup_Free(PageSetup* ps);
BOOL PageSetup_Copy(PageSetup* dst, const PageSetup* src);

// ----------------------------------------------------------------------------
//  Application settings, persisted under HKCU\Software\LetterPad
// ----------------------------------------------------------------------------
struct AppSettings
{
    // Window
    RECT      rcWindow;         // restored position, screen coords
    int       nWindowShow;      // SW_* for the next creation
    BOOL      fMaximised;

    // View
    WrapMode  wrap;
    UnitSystem units;
    BOOL      fShowRuler;
    BOOL      fShowStatusBar;
    int       nZoomNum;         // zoom as a numerator/denominator pair, so
    int       nZoomDen;         // 110/100 is exactly 110% with no rounding

    // Default formatting applied to a brand-new document
    CHARFORMAT2W  defChar;
    PARAFORMAT2   defPara;

    // Page
    PageSetup page;

    // When true, "Save" on an untitled document offers Rich Text Format as the
    // default filter entry. When false it offers the format of whatever was
    // opened last. WordPad's behaviour here is to remember RTF as the default,
    // which is what this reproduces.
    BOOL rtfAsDefault;

    AppSettings()
        : nWindowShow(SW_SHOWNORMAL)
        , fMaximised(FALSE)
        , wrap(kWrapWindow)
        , units(kUnitInch)
        , fShowRuler(TRUE)
        , fShowStatusBar(TRUE)
        , nZoomNum(100), nZoomDen(100)
        , rtfAsDefault(TRUE)
    {
        SetRect(&rcWindow, 100, 100, 100 + 800, 100 + 600);
        ZeroMemory(&defChar, sizeof(defChar));
        ZeroMemory(&defPara, sizeof(defPara));
    }
};

// settings.cpp
void  Settings_Load(AppSettings* s);
void  Settings_Save(const AppSettings* s);
void  Settings_ApplyDefaults(AppSettings* s);

// Most-recently-used file list. Stored as "File1".."File9" under the MRU key,
// matching the shell's own MRU convention so that if a user has ever used a
// jump list against this key the numbering is familiar.
constexpr int kMruMax = 9;
void  Mru_Add(PCWSTR path);
int   Mru_GetList(PWSTR paths[kMruMax]);   // returns count, most recent first
void  Mru_Clear();

// ----------------------------------------------------------------------------
//  File formats
// ----------------------------------------------------------------------------
enum class FileFormat
{
    Unknown = 0,
    Rtf,            // Rich Text Format. The default.
    Text,           // Plain text.
    Docx,           // Office Open XML. Codec not yet implemented.
    Odt,            // OpenDocument Text. Codec not yet implemented.
    Wri,            // Legacy Windows 3.x Write. Read-only, via RichEdit.
};

// codec.cpp
FileFormat DetectFormatByExtension(PCWSTR path);
FileFormat DetectFormatByContent(PCWSTR path);   // sniffs; falls back to extension
bool       FormatIsImplemented(FileFormat f);
HRESULT    Document_Load(HWND hEdit, PCWSTR path, FileFormat fmt);
HRESULT    Document_Save(HWND hEdit, PCWSTR path, FileFormat fmt);
const wchar_t* FormatDisplayName(FileFormat f);

// The open/save filter strings are built at runtime rather than being a single
// RC string literal, because the filter format needs embedded NULs and an RC
// string cannot contain them.
void BuildOpenFilter(PWSTR buf, size_t cch);
void BuildSaveFilter(PWSTR buf, size_t cch, FileFormat current);

// ----------------------------------------------------------------------------
//  EditView -- thin typed wrapper over the RichEdit HWND
//
//  Not a document model. Every method is one or two SendMessage calls. The
//  class exists so that call sites read `view.SetBold(TRUE)` instead of
//  hand-building a CHARFORMAT2W with the right cbSize and dwMask at every one
//  of the ~20 places that need to, which is where the real bugs live: a
//  forgotten cbSize makes RichEdit reject the whole structure silently.
// ----------------------------------------------------------------------------
class AppFrame;

class EditView
{
public:
    void   Attach(HWND hEdit, AppFrame* frame);
    HWND   Hwnd() const { return m_hEdit; }
    operator bool() const { return m_hEdit != nullptr; }

    // -- selection -------------------------------------------------------
    void   GetSel(CHARRANGE& r) const;
    void   SetSel(const CHARRANGE& r) const;
    void   SelectAll() const;
    bool   HasSelection() const;

    // -- formatting ------------------------------------------------------
    // GetCharFormat returns the formatting of the selection, or of the
    // insertion point when the selection is empty. pfAll is set for the
    // caller's benefit: it says whether the value is uniform across the
    // selection, which is what decides whether a toolbar toggle button
    // should render as checked, unchecked, or indeterminate.
    void   GetCharFormat(CHARFORMAT2W& cf) const;
    void   GetParaFormat(PARAFORMAT2& pf) const;
    bool   SetCharFormat(const CHARFORMAT2W& cf) const;   // SCF_SELECTION
    bool   SetParaFormat(const PARAFORMAT2& pf) const;

    // Discrete toggles. Each reads current state, flips it, writes it back --
    // a "set to absolute value" call would be wrong when the selection spans
    // mixed formatting, because half the selection would silently not change.
    void   ToggleBold();
    void   ToggleItalic();
    void   ToggleUnderline();
    void   ToggleStrikeout();
    void   ToggleSubscript();
    void   ToggleSuperscript();
    void   SetFontColor(COLORREF cr);
    void   SetHighlightColor(COLORREF cr);
    void   SetFontFace(PCWSTR face, LONG yHeightTwips);

    void   SetAlignment(WORD pfmStyle);       // PFA_LEFT / CENTER / RIGHT / JUSTIFY
    void   ToggleBullets();
    // PARAFORMAT2's line spacing is a (rule, value) pair, not a single number.
    // Rules 0/1/2 are single / one-and-a-half / double and IGNORE dyTwips
    // entirely; rules 3/4/5 interpret dyTwips (exact, at-least, and points).
    // Collapsing that into one parameter is how "1.5 spacing" silently becomes
    // "1.5 twips" -- i.e. lines stacked on top of each other.
    void   SetLineSpacing(BYTE rule, LONG dyTwips);
    void   AdjustIndent(LONG deltaTwips);

    // -- zoom ------------------------------------------------------------
    void   GetZoom(int& num, int& den) const;
    void   SetZoom(int num, int den);
    void   ZoomIn();
    void   ZoomOut();
    void   ZoomReset();

    // -- wrap / target device -------------------------------------------
    void   ApplyWrapMode(WrapMode wrap, LONG rulerWidthTwips);

    // -- modification flag ----------------------------------------------
    // RichEdit maintains its own modify flag; use it rather than tracking
    // EN_CHANGE ourselves, or undo/redo will desynchronise the dirty state.
    bool   IsModified() const;
    void   SetModified(bool f) const;

    // -- search ----------------------------------------------------------
    bool   FindText(PCWSTR needle, bool matchCase, bool wholeWord,
                    bool backwards, CHARRANGE& found) const;
    bool   ReplaceOne(PCWSTR find, PCWSTR replace, bool matchCase, bool wholeWord);
    int    ReplaceAll(PCWSTR find, PCWSTR replace, bool matchCase, bool wholeWord);

    // -- undo ------------------------------------------------------------
    void   Undo();
    void   Redo();

    // -- clipboard -------------------------------------------------------
    bool   CanPaste() const;
    void   Cut(); void Copy(); void Paste(); void PasteSpecial();

    // -- OLE -------------------------------------------------------------
    HRESULT InsertPicture(PCWSTR path);
    HRESULT InsertObject(HWND hOwner);

    // -- geometry --------------------------------------------------------
    // Text width available for layout, in twips, honouring the current wrap
    // mode. The ruler and the print paginator both need this and must agree.
    LONG   GetTextWidthTwips() const;

private:
    HWND     m_hEdit  = nullptr;
    AppFrame* m_frame = nullptr;
};

// ----------------------------------------------------------------------------
//  AppFrame -- the main window
// ----------------------------------------------------------------------------
class AppFrame
{
public:
    // Creates the window and everything in it. Returns false if the frame
    // could not be created at all; a ribbon failure is NOT fatal and sets
    // m_fRibbon to false instead, falling back to the classic toolbar.
    bool Create(int nCmdShow, PCWSTR initialFile);

    static AppFrame* FromHwnd(HWND h);

    HWND     Hwnd()   const { return m_hFrame; }
    EditView& View()        { return m_view; }
    const EditView& View() const { return m_view; }

    // Read-only access to the live settings. Exposed because the print
    // paginator, the ruler and Go-To-Page all need the page geometry, and each
    // of them reading it a different way is how the on-screen page and the
    // printed page end up disagreeing.
    const AppSettings& Settings() const { return m_settings; }
    AppSettings&       Settings()       { return m_settings; }

    // -- document --------------------------------------------------------
    bool FileNew();
    bool FileOpen(PCWSTR path = nullptr);
    bool FileSave();
    bool FileSaveAs(FileFormat force = FileFormat::Unknown);
    bool FileClose();                       // returns false if the user cancelled
    bool ConfirmSaveIfDirty();              // true = OK to proceed

    void   SetDocumentPath(PCWSTR path);    // null clears it
    PCWSTR DocumentPath() const { return m_path; }
    void   UpdateCaption();
    void   UpdateRibbonState();             // refresh every enabled/checked state
    void   UpdateStatusBar();

    // -- layout ----------------------------------------------------------
    void   LayoutChildren();
    void   ApplySettings(const AppSettings& s);
    void   SaveSettingsTo(AppSettings& s) const;

    // -- status bar ------------------------------------------------------
    void   SetStatusMessage(PCWSTR text);
    void   SetZoomPercent(int percent);

    // -- command dispatch ------------------------------------------------
    // Single entry point for every user action from every surface. The window
    // procedure itself is the free function FrameWndProc below, not a member:
    // it needs to run before any AppFrame exists (WM_NCCREATE) and after the
    // last one is gone (the final WM_DESTROY), which a member function cannot
    // express.
    void   OnCommand(UINT id);

    // -- message-handling hooks -------------------------------------------
    // These are public because FrameWndProc and the two subclass procs are free
    // functions, not members. They could be private with `friend` declarations,
    // but the subclass procs live in an anonymous namespace inside frame.cpp and
    // befriending those is more machinery than it is worth. They are the frame's
    // message-handling contract and are only ever called from a window proc.
    void   SetHwndForProc(HWND h) { m_hFrame = h; }   // WM_NCCREATE, before Create() assigns
    void   OnEditChange();
    void   OnEditSelChange();
    void   OnZoomTrackbar(int pos);
    bool   OnEraseBackground(HDC hdc);
    // Re-reads the live zoom out of the control and mirrors it into the
    // settings, the trackbar and the wrap mode. Needed after any zoom change
    // that did not originate from the trackbar itself.
    void   SyncZoomFromView();

    // -- ribbon ----------------------------------------------------------
    bool        RibbonActive() const { return m_fRibbon; }
    IUIFramework* Framework()  const { return m_pFramework.Get(); }
    void        SetRibbonMinimised(bool f);

private:
    bool CreateChildren();
    bool InitRibbon();
    void CreateFallbackToolbar();
    void CreateStatusBar();
    bool CreateEditView();
    bool CreateRuler();
    void ApplyWrapMode();
    void ShowOptionsDialog();
    void ShowGoToDialog();
    void ShowAboutDialog();
    void InsertDateTime();
    void OnInsertPicture();

    HWND  m_hFrame     = nullptr;
    HWND  m_hRuler     = nullptr;
    HWND  m_hStatus    = nullptr;
    HWND  m_hZoomTrack = nullptr;
    HWND  m_hToolbar   = nullptr;   // fallback only
    EditView m_view;

    ComPtr<IUIFramework>   m_pFramework;
    ComPtr<IUIApplication> m_pApplication;
    bool  m_fRibbon        = false;

    WCHAR m_path[MAX_PATH] = {};    // full path, or empty for an untitled doc
    WCHAR m_title[MAX_PATH] = {};   // display name only

    int   m_cyRibbon       = 0;     // ribbon height, queried on WM_SIZE
    int   m_cyRuler        = 0;
    int   m_cyStatus       = 0;
    AppSettings m_settings;
    bool  m_fSuspendDirty  = false; // suppress dirty tracking during programmatic loads
};

// ----------------------------------------------------------------------------
//  frame.cpp -- window procedure entry, registered by main.cpp
// ----------------------------------------------------------------------------
LRESULT CALLBACK FrameWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK RulerWndProc(HWND, UINT, WPARAM, LPARAM);

// ----------------------------------------------------------------------------
//  Free functions implemented across the other translation units
// ----------------------------------------------------------------------------
// print.cpp
bool PrintDocument(AppFrame& frame, bool quickPrint);
bool ShowPageSetup(AppFrame& frame);
bool ShowPrintPreview(AppFrame& frame);
// Resolves the current page size in TWIPS, from the frame's DEVMODE if one has
// been chosen and from US Letter otherwise. Every caller that needs page
// geometry goes through this so that screen pagination, print pagination and
// the ruler cannot disagree about how wide a page is.
void PageGeometryTwips(const AppFrame& frame, LONG* cxTwips, LONG* cyTwips);

// findrep.cpp
bool ShowFindDialog(AppFrame& frame, bool replace);
bool ShowGoToPage(AppFrame& frame);
// Handles the registered FINDMSGSTRING notification. Called from FrameWndProc's
// default branch, which is the only place that can see messages outside the
// known set. Returns true when the message was consumed.
bool FindReplace_HandleMessage(AppFrame& frame, UINT msg, WPARAM wp, LPARAM lp);
// The runtime message number for FINDMSGSTRING. Exposed so FrameWndProc can
// compare against it in its default branch; RegisterWindowMessage assigns a
// value in 0xC000..0xFFFF that is stable per session but not a compile-time
// constant, so it can never be a case label. Returns 0 until first use.
UINT FindReplace_RegisteredMessage();

// dialogs.cpp
INT_PTR ShowOptionsDialog(HWND hOwner, AppSettings& s);
INT_PTR ShowAboutDialog(HWND hOwner);

// ribbon.cpp -- creates the IUIApplication/IUICommandHandler pair
HRESULT RibbonApp_Create(IUIFramework* fw, AppFrame* frame, IUIApplication** ppApp);

// oleobj.cpp -- OLE drag/drop registration and object insertion helpers.
// The drop target is registered against the edit control's HWND and must be
// revoked before that HWND is destroyed; RegisterDragDrop on a window that is
// going away leaves the OLE drop-target table holding a dangling HWND.
void Ole_RegisterDropTarget(HWND hEdit);
void Ole_RevokeDropTarget(HWND hEdit);

// ruler.cpp
void Ruler_Invalidate(HWND hRuler);
void Ruler_SetMetrics(HWND hRuler, const AppSettings& s, const RECT& rcText,
                      const PARAFORMAT2& pf, LONG pageWidthTwips);
LONG Ruler_HitTestMargin(HWND hRuler, int x);

// main.cpp
void ShowFatalError(HWND hOwner, UINT idText, UINT idCaption, HRESULT hr);
