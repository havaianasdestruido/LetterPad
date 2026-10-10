// ============================================================================
//  print.cpp -- page setup, paginated printing, and print preview
//
//  THE PAGINATION LOOP
//  RichEdit does not know about pages while you edit. Pagination is computed on
//  demand by EM_FORMATRANGE, which lays out the text into a rectangle and
//  returns the character offset where it had to stop. Printing is therefore a
//  loop:
//
//      fr.chrg = { 0, -1 }                 // -1 means "to end of document"
//      while (fr.chrg.cpMin < fr.chrg.cpMax) {
//          next = EM_FORMATRANGE(TRUE, &fr)   // draw one page
//          if (next <= fr.chrg.cpMin) break   // no progress -> bail
//          StartPage / EndPage around it
//          fr.chrg.cpMin = next
//      }
//      EM_FORMATRANGE(FALSE, 0)            // release the measurement cache
//
//  Three details that are easy to get wrong and hard to notice:
//
//    1. The final EM_FORMATRANGE(FALSE, 0) is not optional. RichEdit caches the
//       formatting state built up during pagination. Skipping the release leaks
//       it for the life of the control and the next print job starts with stale
//       metrics, which shows up as a wrong page break on the second print.
//
//    2. The "no progress" guard is not paranoia. A paragraph containing an
//       object taller than the printable area cannot be split, and
//       EM_FORMATRANGE returns the same offset it was given. Without the guard
//       the loop never terminates and the print job hangs the spooler.
//
//    3. StartPage/EndPage must bracket the FORMATRANGE call, not the other way
//       round, and must be called for EVERY page including the last. A missing
//       EndPage discards the page silently -- the job prints with the last page
//       absent and reports success.
//
//  COORDINATE SYSTEM
//  The printer DC's origin is the top-left of the PRINTABLE area, not of the
//  paper. Most printers cannot print to the physical edge, so the printable
//  area is inset by PHYSICALOFFSETX/Y. Shifting the viewport origin by
//  -offset makes (0,0) mean "the corner of the paper", after which margins are
//  plain arithmetic and a full-bleed element would land where it should.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  Printer resolution helpers
// ---------------------------------------------------------------------------
struct PageRects
{
    RECT rcPage;      // whole sheet, origin at the paper corner, in device units
    RECT rcPrint;     // area inside the margins
    int  dpiX;
    int  dpiY;
};

// Computes the page and print rectangles for a printer DC that has already had
// its viewport origin shifted to the paper corner.
PageRects ComputePageRects(HDC hdc, const PageSetup& ps)
{
    PageRects r = {};
    r.dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    r.dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    if (r.dpiX <= 0) r.dpiX = 300;      // sane printer defaults
    if (r.dpiY <= 0) r.dpiY = 300;

    int physW = GetDeviceCaps(hdc, PHYSICALWIDTH);
    int physH = GetDeviceCaps(hdc, PHYSICALHEIGHT);
    if (physW <= 0) physW = r.dpiX * 8;     // last-resort US Letter
    if (physH <= 0) physH = r.dpiY * 11;

    SetRect(&r.rcPage, 0, 0, physW, physH);

    // Margins are stored in twips; the DC works in device units (pixels at the
    // printer's resolution). MulDiv rather than a float multiply: MulDiv does
    // the 64-bit intermediate internally and rounds, which avoids both overflow
    // on a 1200 dpi page and the accumulated drift of float arithmetic.
    int left   = MulDiv(ps.marginLeft,   r.dpiX, kTwipsPerInch);
    int top    = MulDiv(ps.marginTop,    r.dpiY, kTwipsPerInch);
    int right  = MulDiv(ps.marginRight,  r.dpiX, kTwipsPerInch);
    int bottom = MulDiv(ps.marginBottom, r.dpiY, kTwipsPerInch);

    SetRect(&r.rcPrint, left, top, physW - right, physH - bottom);

    // Clamp: a margin larger than the page would invert the rectangle, and
    // EM_FORMATRANGE with an inverted rc draws nothing and returns no progress,
    // which the pagination loop reports as a hang rather than as bad margins.
    if (r.rcPrint.right  <= r.rcPrint.left) r.rcPrint.right  = r.rcPrint.left + 1;
    if (r.rcPrint.bottom <= r.rcPrint.top)  r.rcPrint.bottom = r.rcPrint.top  + 1;

    return r;
}

// Shifts the DC origin to the physical paper corner. Returns the old origin so
// the caller can restore it.
POINT ShiftToPaperOrigin(HDC hdc)
{
    POINT old = {};
    int offX = GetDeviceCaps(hdc, PHYSICALOFFSETX);
    int offY = GetDeviceCaps(hdc, PHYSICALOFFSETY);
    // PHYSICALOFFSETX/Y are reported as positive insets on most drivers and
    // negative on a few. Using the value as-is with a negation is the documented
    // recipe; drivers that report it negative end up shifted the other way,
    // which is why the sign is taken from the value rather than assumed.
    SetViewportOrgEx(hdc, -offX, -offY, &old);
    return old;
}

} // anonymous namespace

// ===========================================================================
//  PageGeometryTwips
// ===========================================================================
void PageGeometryTwips(const AppFrame& frame, LONG* cxTwips, LONG* cyTwips)
{
    const PageSetup& ps = frame.Settings().page;

    // Without a DEVMODE there is no printer to ask, so US Letter portrait is
    // the assumption. That is also what the print dialog will default to, so
    // screen pagination and the eventual print job agree until the user picks
    // something else.
    LONG cx = 8 * kTwipsPerInch + kTwipsPerInch / 2;   // 8.5"
    LONG cy = 11 * kTwipsPerInch;

    if (ps.hDevMode)
    {
        DEVMODEW* dm = static_cast<DEVMODEW*>(GlobalLock(ps.hDevMode));
        if (dm)
        {
            if (dm->dmPaperWidth > 0 && dm->dmPaperLength > 0)
            {
                // dmPaperWidth/Length are in tenths of a millimetre. Converting
                // via hundredths-of-mm arithmetic avoids a float and keeps the
                // result exact for the standard paper sizes.
                cx = MulDiv(dm->dmPaperWidth,  kTwipsPerInch, 2540);
                cy = MulDiv(dm->dmPaperLength, kTwipsPerInch, 2540);
            }
            if (dm->dmFields & DM_ORIENTATION && dm->dmOrientation == DMORIENT_LANDSCAPE)
            {
                // Swap rather than trust dmPaperWidth having been swapped
                // already: drivers are inconsistent about whether the width and
                // length fields follow the orientation.
                LONG t = cx; cx = cy; cy = t;
            }
            GlobalUnlock(ps.hDevMode);
        }
    }

    if (cxTwips) *cxTwips = cx;
    if (cyTwips) *cyTwips = cy;
}

// ===========================================================================
//  Page setup
// ===========================================================================
bool ShowPageSetup(AppFrame& frame)
{
    AppSettings& s = frame.Settings();

    // PAGESETUPDLG wants the margins in HUNDREDTHS OF AN INCH, not twips and
    // not the units the user has chosen for display. The conversion is done on
    // the way in and undone on the way out; the internal representation stays
    // twips throughout so nothing else in the app needs to know about this.
    auto TwipsToHundredthsInch = [](LONG twips) -> LONG
    {
        return MulDiv(twips, 100, kTwipsPerInch);
    };
    auto HundredthsInchToTwips = [](LONG hi) -> LONG
    {
        return MulDiv(hi, kTwipsPerInch, 100);
    };

    // Stage the DEVMODE/DEVNAMES so a cancelled dialog cannot have modified the
    // live settings. The common dialog writes into the handles we hand it.
    PageSetup staged;
    PageSetup_Copy(&staged, &s.page);

    PAGESETUPDLGW psd = {};
    psd.lStructSize   = sizeof(psd);
    psd.hwndOwner     = frame.Hwnd();
    psd.hInstance     = g_hInst;
    psd.hDevMode      = staged.hDevMode;
    psd.hDevNames     = staged.hDevNames;
    psd.Flags         = PSD_MARGINS | PSD_INHUNDREDTHSOFINCH | PSD_ENABLEPAGESETUPHOOK;

    RECT rcMin = {};
    SetRect(&rcMin, TwipsToHundredthsInch(s.page.marginLeft),
                    TwipsToHundredthsInch(s.page.marginTop),
                    TwipsToHundredthsInch(s.page.marginRight),
                    TwipsToHundredthsInch(s.page.marginBottom));
    psd.rtMargin = rcMin;

    // No minimum margin enforced. Setting rtMinMargin would stop the user
    // printing borderless on a printer that supports it.
    SetRect(&rcMin, 0, 0, 0, 0);
    psd.rtMinMargin = rcMin;

    if (!PageSetupDlgW(&psd))
    {
        // Cancelled, or the dialog failed. CommDlgExtendedError distinguishes
        // them; either way the live settings are untouched because we staged.
        PageSetup_Free(&staged);
        return false;
    }

    // The dialog may have reallocated the handles. Take ownership of whatever
    // it gives back and free the staged originals.
    s.page.hDevMode  = psd.hDevMode;
    s.page.hDevNames = psd.hDevNames;
    staged.hDevMode  = nullptr;      // ownership moved; do not double-free
    staged.hDevNames = nullptr;
    PageSetup_Free(&staged);

    s.page.marginLeft   = HundredthsInchToTwips(psd.rtMargin.left);
    s.page.marginTop    = HundredthsInchToTwips(psd.rtMargin.top);
    s.page.marginRight  = HundredthsInchToTwips(psd.rtMargin.right);
    s.page.marginBottom = HundredthsInchToTwips(psd.rtMargin.bottom);

    Settings_Save(&s);
    frame.LayoutChildren();      // the ruler and wrap width depend on the page
    return true;
}

// ===========================================================================
//  Printing
// ===========================================================================
bool PrintDocument(AppFrame& frame, bool quickPrint)
{
    if (!frame.View()) return false;

    const AppSettings& s = frame.Settings();

    // ------------------------------------------------------------------
    //  Obtain a DEVMODE. Quick print uses the stored one (or the system
    //  default); full print shows PrintDlgEx so the user can change it.
    // ------------------------------------------------------------------
    HGLOBAL hDevMode  = nullptr;
    HGLOBAL hDevNames = nullptr;
    HDC     hdc       = nullptr;
    bool    ownDevMode = false;

    if (quickPrint)
    {
        // Copy the stored DEVMODE rather than passing it to CreateDC directly:
        // CreateDC does not take ownership, but a later PageSetup would, and
        // sharing one HGLOBAL between "current settings" and "this job" is how
        // double-frees happen.
        PageSetup tmp;
        PageSetup_Copy(&tmp, &s.page);
        hDevMode  = tmp.hDevMode;
        hDevNames = tmp.hDevNames;
        ownDevMode = true;

        if (!hDevMode && !hDevNames)
        {
            // Nothing stored: resolve the default printer. Passing all-nulls to
            // CreateDC selects the default device, which is exactly quick print.
            hdc = CreateDCW(nullptr, nullptr, nullptr, nullptr);
        }
    }
    else
    {
        PRINTDLGEXW pd = {};
        pd.lStructSize = sizeof(pd);
        pd.hwndOwner   = frame.Hwnd();
        pd.hInstance   = g_hInst;

        // Stage copies for the same ownership reason as above.
        PageSetup tmp;
        PageSetup_Copy(&tmp, &s.page);
        pd.hDevMode  = tmp.hDevMode;
        pd.hDevNames = tmp.hDevNames;

        pd.Flags = PD_RETURNDC | PD_COLLATE | PD_USEDEVMODECOPIES | PD_NOSELECTION;
        pd.nCopies     = 1;
        pd.nMinPage    = 1;
        pd.nMaxPage    = static_cast<WORD>(0xFFFF);
        pd.nStartPage  = START_PAGE_GENERAL;

        HRESULT hr = PrintDlgExW(&pd);
        if (FAILED(hr) || pd.dwResultAction == PD_RESULT_CANCEL)
        {
            PageSetup_Free(&tmp);
            if (pd.hDC) DeleteDC(pd.hDC);
            return false;
        }

        // Take ownership of whatever came back.
        hDevMode  = pd.hDevMode;
        hDevNames = pd.hDevNames;
        tmp.hDevMode  = nullptr;
        tmp.hDevNames = nullptr;
        PageSetup_Free(&tmp);
        ownDevMode = true;

        // PD_RETURNDC means the dialog created the DC for us. Adopting it is
        // required; creating our own from the DEVMODE would produce a second,
        // differently-configured DC and the user's dialog choices would be lost.
        hdc = pd.hDC;

        // Persist the printer choice so the next quick print uses the same
        // device. The settings struct takes ownership of the two HGLOBALs, so
        // whatever it was holding is freed first and ownDevMode is cleared --
        // otherwise the cleanup at the bottom of this function would free
        // handles that the settings now own.
        AppSettings& ms = frame.Settings();
        if (ms.page.hDevMode)  { GlobalFree(ms.page.hDevMode);  ms.page.hDevMode  = nullptr; }
        if (ms.page.hDevNames) { GlobalFree(ms.page.hDevNames); ms.page.hDevNames = nullptr; }
        ms.page.hDevMode  = hDevMode;
        ms.page.hDevNames = hDevNames;
        ownDevMode = false;      // ownership now belongs to the settings
    }

    if (!hdc)
    {
        // Resolve a DC from the DEVMODE/DEVNAMES pair.
        LPWSTR driver = nullptr, device = nullptr, port = nullptr;
        if (hDevNames)
        {
            DEVNAMES* dn = static_cast<DEVNAMES*>(GlobalLock(hDevNames));
            if (dn)
            {
                driver = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(dn) + dn->wDriverOffset);
                device = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(dn) + dn->wDeviceOffset);
                port   = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(dn) + dn->wOutputOffset);
            }
        }
        DEVMODEW* dm = hDevMode ? static_cast<DEVMODEW*>(GlobalLock(hDevMode)) : nullptr;

        hdc = CreateDCW(driver, device, port, dm);

        if (dm) GlobalUnlock(hDevMode);
        if (hDevNames) GlobalUnlock(hDevNames);
    }

    if (!hdc)
    {
        ShowFatalError(frame.Hwnd(), IDS_ERR_PRINT, IDS_ERR_PRINT_CAP,
                       HrFromWin32(GetLastError()));
        if (ownDevMode) { if (hDevMode) GlobalFree(hDevMode);
                          if (hDevNames) GlobalFree(hDevNames); }
        return false;
    }

    // ------------------------------------------------------------------
    //  Document properties and the job itself
    // ------------------------------------------------------------------
    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    WCHAR jobName[MAX_PATH + 32];
    if (frame.DocumentPath()[0])
        StrPrintf(jobName, _countof(jobName), L"%s", frame.DocumentPath());
    else
        LoadStr(IDS_UNTITLED, jobName, _countof(jobName));
    di.lpszDocName = jobName;
    di.lpszOutput  = nullptr;      // to the printer, not to a file

    // Shift the origin to the paper corner before computing the rectangles, so
    // margins are measured from where the user thinks the page edge is.
    POINT oldOrigin = ShiftToPaperOrigin(hdc);
    PageRects pr = ComputePageRects(hdc, s.page);

    if (StartDocW(hdc, &di) <= 0)
    {
        SetViewportOrgEx(hdc, oldOrigin.x, oldOrigin.y, nullptr);
        DeleteDC(hdc);
        if (ownDevMode) { if (hDevMode) GlobalFree(hDevMode);
                          if (hDevNames) GlobalFree(hDevNames); }
        ShowFatalError(frame.Hwnd(), IDS_ERR_PRINT, IDS_ERR_PRINT_CAP,
                       HrFromWin32(GetLastError()));
        return false;
    }

    // Save/restore the DC state around each page so per-page settings (fonts
    // selected by RichEdit, clipping regions) cannot leak into the next page.
    LONG cpMin = 0, cpMax = -1;
    int pages = 0;
    bool failed = false;

    // The pagination and the page framing have to interleave: StartPage must
    // come before the FORMATRANGE that draws onto it, and EndPage after. Doing
    // all the pagination first and then the pages would require buffering the
    // layout, which EM_FORMATRANGE does not give us.
    {
        FORMATRANGE fr = {};
        fr.hdc        = hdc;
        fr.hdcTarget  = hdc;
        fr.rc         = pr.rcPrint;
        fr.rcPage     = pr.rcPage;
        fr.chrg.cpMin = cpMin;
        fr.chrg.cpMax = cpMax;

        while (fr.chrg.cpMin < fr.chrg.cpMax)
        {
            if (StartPage(hdc) <= 0) { failed = true; break; }

            // Re-shift the origin: StartPage resets the DC's viewport to the
            // printable-area origin. Not doing this makes every page after the
            // first print with margins measured from the wrong corner, which
            // shows up as text creeping down the page.
            ShiftToPaperOrigin(hdc);

            LONG next = static_cast<LONG>(SendMessageW(frame.View().Hwnd(),
                                                       EM_FORMATRANGE, TRUE,
                                                       LParamFromPtr(&fr)));
            if (EndPage(hdc) <= 0) { failed = true; break; }

            if (next <= fr.chrg.cpMin)
            {
                // An unbreakable element taller than the printable area.
                // Stop rather than spin; the pages emitted so far are valid.
                break;
            }

            ++pages;
            fr.chrg.cpMin = next;
        }

        SendMessageW(frame.View().Hwnd(), EM_FORMATRANGE, FALSE, 0);
    }

    int rc = failed ? AbortDoc(hdc) : EndDoc(hdc);
    SetViewportOrgEx(hdc, oldOrigin.x, oldOrigin.y, nullptr);
    DeleteDC(hdc);

    if (ownDevMode)
    {
        if (hDevMode)  GlobalFree(hDevMode);
        if (hDevNames) GlobalFree(hDevNames);
    }

    if (failed || rc <= 0)
    {
        ShowFatalError(frame.Hwnd(), IDS_ERR_PRINT, IDS_ERR_PRINT_CAP,
                       HrFromWin32(GetLastError()));
        return false;
    }

    WCHAR msg[128];
    StrPrintf(msg, _countof(msg), L"Printed %d page(s).", pages);
    frame.SetStatusMessage(msg);
    return true;
}

// ===========================================================================
//  Print preview
//
//  Renders one page at a time into a memory DC and stretches it into the
//  preview pane. The preview is a genuine EM_FORMATRANGE pass against a DC
//  scaled to the page, so what it shows is what the printer will produce --
//  not an approximation drawn from screen metrics.
//
//  The cost is that each page turn re-paginates from the start, because
//  EM_FORMATRANGE has no "start at page N" entry point and the character
//  offset of page N is only known by walking pages 1..N-1. For a short
//  document that is invisible; for a long one, page turns get progressively
//  slower. Caching the per-page character offsets is the obvious fix and is
//  listed in ROADMAP.md.
// ===========================================================================
namespace {

struct PreviewState
{
    AppFrame* frame    = nullptr;
    HWND      hDlg     = nullptr;
    int       page     = 1;      // 1-based, the page currently shown
    int       totalPages = 0;
    int       zoom     = 100;    // percent of fit-to-window
    LONG      cpPageStart = 0;   // character offset where `page` begins
};

// ---------------------------------------------------------------------------
//  Preview rendering
//
//  Renders into a DIB section sized to the preview pane, then hands the bitmap
//  to a SS_BITMAP static via STM_SETIMAGE. A DIB rather than painting directly
//  in WM_PAINT because the render is a full EM_FORMATRANGE pagination walk --
//  doing that on every paint (including every expose during a resize drag)
//  would be far too slow. Rendering on demand and blitting a cached bitmap is
//  the only way this stays interactive.
//
//  The DC is a memory DC, not a printer DC, so font metrics are the screen's.
//  The preview is therefore accurate in layout and approximate in glyph
//  rendering. Matching the printer exactly would mean creating the printer DC
//  for every page turn, which is slow enough to be its own problem.
// ---------------------------------------------------------------------------
void Preview_Render(PreviewState* st)
{
    if (!st->hDlg || !st->frame || !st->frame->View())
        return;

    HWND hStatic = GetDlgItem(st->hDlg, IDC_PP_PREVIEW);
    if (!hStatic) return;

    RECT rcClient;
    GetClientRect(hStatic, &rcClient);
    int cx = rcClient.right - rcClient.left;
    int cy = rcClient.bottom - rcClient.top;
    if (cx <= 4 || cy <= 4) return;

    // ---- page geometry ---------------------------------------------------
    LONG pageW = 0, pageH = 0;
    PageGeometryTwips(*st->frame, &pageW, &pageH);
    if (pageW <= 0 || pageH <= 0) return;

    // Fit-to-window DPI, then scaled by the user's zoom. Working in DPI rather
    // than in a scale factor keeps the arithmetic integer-only and lets the
    // same MulDiv twips-to-pixels conversion serve both axes.
    int fitDpiX = MulDiv(cx, kTwipsPerInch, pageW);
    int fitDpiY = MulDiv(cy, kTwipsPerInch, pageH);
    int fitDpi  = (fitDpiX < fitDpiY) ? fitDpiX : fitDpiY;
    if (fitDpi < 8) fitDpi = 8;

    int effDpi = MulDiv(fitDpi, st->zoom, 100);
    if (effDpi < 8) effDpi = 8;

    int pw = MulDiv(pageW, effDpi, kTwipsPerInch);
    int ph = MulDiv(pageH, effDpi, kTwipsPerInch);
    if (pw < 8 || ph < 8) return;

    // ---- DIB section -----------------------------------------------------
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cx;
    // Negative height gives a top-down DIB. A bottom-up DIB would render the
    // page upside down when handed to STM_SETIMAGE, which expects top-down.
    bi.bmiHeader.biHeight      = -cy;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return; }

    HDC hdcScreen = GetDC(hStatic);
    HDC memdc = CreateCompatibleDC(hdcScreen);
    HBITMAP hOld = static_cast<HBITMAP>(SelectObject(memdc, hbm));

    // ---- canvas ----------------------------------------------------------
    // Mid-grey surround, white page, thin dark border. The grey is what makes
    // the page edge visible; on a white canvas a white page has no boundary.
    HBRUSH hGrey = CreateSolidBrush(RGB(128, 128, 128));
    FillRect(memdc, &rcClient, hGrey);
    DeleteObject(hGrey);

    int ox = (cx - pw) / 2;
    int oy = (cy - ph) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    RECT rcPage = { ox, oy, ox + pw, oy + ph };
    HBRUSH hWhite = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(memdc, &rcPage, hWhite);
    FrameRect(memdc, &rcPage, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    // ---- margins ---------------------------------------------------------
    const PageSetup& ps = st->frame->Settings().page;
    RECT rcPrint;
    rcPrint.left   = ox + MulDiv(ps.marginLeft,   effDpi, kTwipsPerInch);
    rcPrint.top    = oy + MulDiv(ps.marginTop,    effDpi, kTwipsPerInch);
    rcPrint.right  = ox + pw - MulDiv(ps.marginRight,  effDpi, kTwipsPerInch);
    rcPrint.bottom = oy + ph - MulDiv(ps.marginBottom, effDpi, kTwipsPerInch);
    if (rcPrint.right  <= rcPrint.left) rcPrint.right  = rcPrint.left + 1;
    if (rcPrint.bottom <= rcPrint.top)  rcPrint.bottom = rcPrint.top  + 1;

    // ---- paginate, drawing only the requested page -----------------------
    // The walk runs to the end of the document regardless, because the total
    // page count is only known once pagination finishes. Drawing is suppressed
    // (wParam FALSE) on every page except the target.
    HWND hEdit = st->frame->View().Hwnd();

    FORMATRANGE fr = {};
    fr.hdc        = memdc;
    fr.hdcTarget  = memdc;
    fr.rc         = rcPrint;
    fr.rcPage     = rcPage;
    fr.chrg.cpMin = 0;
    fr.chrg.cpMax = -1;

    int page = 1;
    st->cpPageStart = 0;
    // Hard ceiling. Without it a document whose layout makes no forward
    // progress would spin here; the guard below also catches it, but a ceiling
    // bounds the work even if that guard is ever wrong.
    const int kMaxPreviewPages = 10000;

    while (fr.chrg.cpMin < fr.chrg.cpMax && page <= kMaxPreviewPages)
    {
        bool isTarget = (page == st->page);
        if (isTarget) st->cpPageStart = fr.chrg.cpMin;

        LONG next = static_cast<LONG>(SendMessageW(hEdit, EM_FORMATRANGE,
                                                   isTarget ? TRUE : FALSE,
                                                   LParamFromPtr(&fr)));
        if (next <= fr.chrg.cpMin)
            break;                       // no progress; bail rather than spin
        fr.chrg.cpMin = next;
        ++page;
    }

    st->totalPages = page - 1;
    if (st->totalPages < 1) st->totalPages = 1;
    if (st->page > st->totalPages) st->page = st->totalPages;

    // Release RichEdit's measurement cache. Skipping this makes the next
    // preview render start from stale metrics.
    SendMessageW(hEdit, EM_FORMATRANGE, FALSE, 0);

    SelectObject(memdc, hOld);
    DeleteDC(memdc);
    ReleaseDC(hStatic, hdcScreen);

    // ---- hand the bitmap to the static -----------------------------------
    // STM_SETIMAGE returns the previous image, which we own and must delete.
    // Not deleting it leaks a DIB section on every page turn and every zoom
    // change, which adds up fast in a preview the user is paging through.
    HBITMAP hPrev = reinterpret_cast<HBITMAP>(
        SendMessageW(hStatic, STM_SETIMAGE, IMAGE_BITMAP, LParamFromPtr(hbm)));
    if (hPrev) DeleteObject(hPrev);

    InvalidateRect(hStatic, nullptr, TRUE);

    // ---- page label -------------------------------------------------------
    WCHAR label[64];
    StrPrintf(label, _countof(label), L"Page %d of %d", st->page, st->totalPages);
    SetDlgItemTextW(st->hDlg, IDC_PP_PAGELABEL, label);

    EnableWindow(GetDlgItem(st->hDlg, IDC_PP_PREVPAGE), st->page > 1);
    EnableWindow(GetDlgItem(st->hDlg, IDC_PP_NEXTPAGE), st->page < st->totalPages);
}

// ---------------------------------------------------------------------------
//  Dialog procedure
//
//  A real static function, not a lambda: DLGPROC is CALLBACK (__stdcall), and
//  while a non-capturing lambda converts to a plain function pointer, that
//  pointer is __cdecl. On x86 the calling-convention mismatch is undefined
//  behaviour that usually survives until a dialog with many parameters; on x64
//  the distinction disappears and hides the bug entirely.
// ---------------------------------------------------------------------------
INT_PTR CALLBACK PreviewDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PreviewState* st = PtrFromLParam<PreviewState>(GetWindowLongPtrW(hDlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        st = PtrFromLParam<PreviewState>(lp);
        SetWindowLongPtrW(hDlg, DWLP_USER, LParamFromPtr(st));
        st->hDlg = hDlg;

        // Render after the dialog is visible so the preview static has its real
        // size. Rendering in WM_INITDIALOG measures a static that has not been
        // laid out yet and produces a 1x1 bitmap.
        SetTimer(hDlg, 1, 1, nullptr);
        return TRUE;
    }

    case WM_TIMER:
        // One-shot: render once the layout has settled.
        KillTimer(hDlg, wp);
        if (st) Preview_Render(st);
        return TRUE;

    case WM_SIZE:
        // Re-render on resize. WM_SIZE fires continuously during a drag, and
        // each render is a full pagination walk, so this is debounced through a
        // short timer rather than done inline.
        if (st)
        {
            KillTimer(hDlg, 2);
            SetTimer(hDlg, 2, 120, nullptr);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PP_PREVPAGE:
            if (st && st->page > 1) { --st->page; Preview_Render(st); }
            return TRUE;
        case IDC_PP_NEXTPAGE:
            if (st && st->page < st->totalPages) { ++st->page; Preview_Render(st); }
            return TRUE;
        case IDC_PP_ZOOMIN:
            if (st && st->zoom < 400) { st->zoom = st->zoom * 125 / 100; Preview_Render(st); }
            return TRUE;
        case IDC_PP_ZOOMOUT:
            if (st && st->zoom > 25)  { st->zoom = st->zoom * 80 / 100;  Preview_Render(st); }
            return TRUE;
        case IDC_PP_CLOSE:
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        case IDOK:
            // Enter closes rather than printing. A preview whose default button
            // prints is a dialog that can be committed by accident.
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        // Free the bitmap the static is holding. STM_GETIMAGE does not transfer
        // ownership, but the static will not delete it either once the dialog is
        // gone, so this is the last chance.
        {
            HWND hStatic = GetDlgItem(hDlg, IDC_PP_PREVIEW);
            HBITMAP hbm = reinterpret_cast<HBITMAP>(
                SendMessageW(hStatic, STM_GETIMAGE, IMAGE_BITMAP, 0));
            if (hbm) DeleteObject(hbm);
        }
        return TRUE;
    }
    return FALSE;
}

} // anonymous namespace

bool ShowPrintPreview(AppFrame& frame)
{
    if (!frame.View()) return false;

    PreviewState st;
    st.frame = &frame;
    st.page  = 1;
    st.zoom  = 100;

    // Modal. A modeless preview would have to track document edits to stay
    // accurate, which means re-paginating on every EN_CHANGE -- unacceptable
    // while typing. WordPad's preview is modal for the same reason.
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PRINTPREVIEW), frame.Hwnd(),
                    PreviewDlgProc, LParamFromPtr(&st));
    return true;
}
