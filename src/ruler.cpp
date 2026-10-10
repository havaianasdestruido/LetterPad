// ============================================================================
//  ruler.cpp -- the horizontal ruler above the document
//
//  The ruler shows three things: the page's linear scale in the user's chosen
//  unit, the grey "unprintable margin" bands at each end, and two draggable
//  markers for the paragraph's left indent and first-line indent.
//
//  It is drawn with plain GDI -- MoveToEx/LineTo/TextOut/FillRect. No GDI+.
//  GDI+ would need a startup token whose lifetime is process-global and which
//  is not documented as thread-safe, and for tick marks and text it buys
//  nothing. The one thing GDI+ would genuinely help with (anti-aliased marker
//  triangles) is cosmetic.
//
//  PAINTING
//  WM_PAINT builds the whole ruler into a memory DC and blits it once. Drawing
//  straight to the window DC produces visible tick-by-tick painting during a
//  resize, because each GDI call is a separate blit to the screen.
//
//  HIT TESTING / DRAGGING
//  The indent markers are draggable. Dragging is implemented by converting the
//  mouse x into twips and reporting it to the frame, which applies it to the
//  paragraph. The ruler never mutates the document itself: it is a view, and a
//  view that also writes is how the ruler and the document get out of step.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// Marker geometry, in pixels.
constexpr int kMarkerWidth  = 9;
constexpr int kMarkerHeight = 7;

// ---------------------------------------------------------------------------
//  Per-ruler state, hung off GWLP_USERDATA.
//
//  The ruler is a view of the paragraph the caret is in, so its state is pushed
//  in by Ruler_SetMetrics whenever that changes. Keeping it here rather than
//  pulling it from the edit control on every paint is what makes painting cheap
//  enough to do into a memory DC without any measurable cost.
// ---------------------------------------------------------------------------
struct RulerState
{
    UnitSystem units        = kUnitInch;
    LONG       pageWidthTwips = 8 * kTwipsPerInch + kTwipsPerInch / 2;
    LONG       marginLeftTwips  = kTwipsPerInch;
    LONG       marginRightTwips = kTwipsPerInch;
    LONG       startIndentTwips = 0;
    LONG       firstLineTwips   = 0;   // dxOffset, usually negative
    int        textOriginX      = 0;   // where the printable area starts, px
    int        twipsPerPixel    = 15;  // 1440/96 at 100% zoom
    bool       dragging         = false;
    int        dragWhich        = 0;   // 1 = start indent, 2 = first line
};

RulerState* StateOf(HWND h)
{
    return PtrFromLParam<RulerState>(GetWindowLongPtrW(h, GWLP_USER));
}

// Converts a twip offset from the left margin into a ruler x coordinate.
int TwipToX(const RulerState* s, LONG twips)
{
    if (!s || s->twipsPerPixel <= 0) return s ? s->textOriginX : 0;
    return s->textOriginX + (static_cast<int>(twips) / s->twipsPerPixel);
}

LONG XToTwip(const RulerState* s, int x)
{
    if (!s) return 0;
    return static_cast<LONG>((x - s->textOriginX) * s->twipsPerPixel);
}

// ---------------------------------------------------------------------------
//  Tick scale for one unit system
//
//  Everything about the tick grid is derived from two numbers: how many twips
//  one minor tick represents, and how many minor ticks make a major (labelled)
//  tick. Keeping them together in one table per unit is what stops the label
//  arithmetic and the tick arithmetic from drifting apart -- which is the bug
//  this file is most likely to grow, because the two are computed in different
//  functions.
// ---------------------------------------------------------------------------
struct TickScale
{
    LONG twipsPerMinor;   // width of one minor tick, in twips
    int  minorsPerMajor;  // minor ticks between labelled ticks
};

TickScale ScaleForUnit(UnitSystem u)
{
    switch (u)
    {
    case kUnitCm:
        // Minor = 0.5 mm (72 twips), major = 5 mm. Labels appear at whole
        // centimetres, so half the major ticks carry a label and half do not;
        // that is the conventional metric ruler layout.
        return { kTwipsPerInch / 20, 10 };
    case kUnitPoint:
        // Minor = 1 pt, major = 12 pt.
        return { kTwipsPerPt, 12 };
    case kUnitPica:
        // Minor = 2 pt (40 twips), major = 1 pica (6 minors).
        return { kTwipsPerPica / 6, 6 };
    case kUnitInch:
    default:
        // Minor = 1/8 inch, major = 1 inch.
        return { kTwipsPerInch / 8, 8 };
    }
}

// Formats the label for a major tick. `minorIndex` is the tick's position
// counted in minor units from the left margin; converting it to the user's unit
// is the whole job of this function. An empty buffer means "major tick with no
// label", which happens on the metric ruler between whole centimetres.
void FormatTickLabel(UnitSystem u, int minorIndex, const TickScale& sc,
                     PWSTR buf, size_t cch)
{
    buf[0] = L'\0';
    if (minorIndex < 0) return;

    switch (u)
    {
    case kUnitInch:
        // 8 minors per inch, and every major is a whole inch.
        StrPrintf(buf, cch, L"%d", minorIndex / sc.minorsPerMajor);
        break;

    case kUnitCm:
    {
        // minors are 0.5 mm, so millimetres = minorIndex / 2. Label only whole
        // centimetres; a label every 5 mm would overlap itself at any normal zoom.
        int mm = minorIndex / 2;
        if (mm % 10 == 0)
            StrPrintf(buf, cch, L"%d", mm / 10);
        break;
    }

    case kUnitPoint:
        // minors are 1 pt, majors are 12 pt.
        StrPrintf(buf, cch, L"%d", minorIndex);
        break;

    case kUnitPica:
        // 6 minors per pica, majors are whole picas.
        StrPrintf(buf, cch, L"%d", minorIndex / sc.minorsPerMajor);
        break;
    }
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------
void PaintRuler(HWND hwnd, HDC hdcTarget, const RECT& rcClient)
{
    RulerState* s = StateOf(hwnd);
    if (!s) return;

    int cx = rcClient.right - rcClient.left;
    int cy = rcClient.bottom - rcClient.top;
    if (cx <= 0 || cy <= 0) return;

    // ---- double buffer ----------------------------------------------------
    HDC hdc = CreateCompatibleDC(hdcTarget);
    if (!hdc) return;

    HBITMAP hbm = CreateCompatibleBitmap(hdcTarget, cx, cy);
    if (!hbm) { DeleteDC(hdc); return; }
    HBITMAP hOldBm = static_cast<HBITMAP>(SelectObject(hdc, hbm));

    // ---- background --------------------------------------------------------
    // The whole ruler is filled with the button-face colour first, then the
    // printable span is overpainted white. The two tones left at either end are
    // the margin bands: that contrast is what makes the ruler readable, since a
    // uniformly white ruler gives no indication of where the margins are.
    HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH hFace  = CreateSolidBrush(GetSysColor(COLOR_3DFACE));

    FillRect(hdc, &rcClient, hFace);

    int xTextL = TwipToX(s, 0);
    int xTextR = TwipToX(s, s->pageWidthTwips - s->marginLeftTwips - s->marginRightTwips);
    if (xTextR > cx) xTextR = cx;

    RECT rcPrintable = { xTextL, 0, xTextR, cy };
    FillRect(hdc, &rcPrintable, hWhite);

    // ---- ticks -------------------------------------------------------------
    HPEN hPenMinor = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
    HPEN hPenMajor = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
    HPEN hOldPen   = static_cast<HPEN>(SelectObject(hdc, hPenMinor));

    // One table drives both the tick spacing and the labels, so they cannot
    // disagree. See ScaleForUnit.
    TickScale sc = ScaleForUnit(s->units);
    LONG twipsPerMinor = sc.twipsPerMinor;
    if (twipsPerMinor <= 0) twipsPerMinor = kTwipsPerInch / 8;

    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    // Iterate over the printable width in minor units. Bounded by the page
    // width, so a bogus twipsPerPixel cannot make this run away.
    LONG textWidth = s->pageWidthTwips - s->marginLeftTwips - s->marginRightTwips;
    if (textWidth < 0) textWidth = 0;

    for (LONG t = 0; t <= textWidth; t += twipsPerMinor)
    {
        int x = TwipToX(s, t);
        if (x < 0 || x > cx) continue;

        int minorIndex = static_cast<int>(t / twipsPerMinor);
        bool major = (minorIndex % sc.minorsPerMajor) == 0;

        SelectObject(hdc, major ? hPenMajor : hPenMinor);

        // Major ticks reach further up the ruler than minor ones, which is the
        // standard ruler convention and the only thing that makes the scale
        // scannable rather than a uniform comb.
        int h = major ? (cy * 5 / 12) : (cy * 3 / 12);
        MoveToEx(hdc, x, cy - h, nullptr);
        LineTo(hdc, x, cy);

        if (major)
        {
            WCHAR label[16];
            FormatTickLabel(s->units, minorIndex, sc, label, _countof(label));
            if (label[0])
            {
                int cchLabel = static_cast<int>(wcslen(label));
                SIZE sz = {};
                GetTextExtentPoint32W(hdc, label, cchLabel, &sz);
                // Labels are centred on their tick, clamped so the first and
                // last cannot be clipped by the ruler's edges.
                int tx = x - sz.cx / 2;
                if (tx < 1) tx = 1;
                if (tx + sz.cx > cx - 1) tx = cx - 1 - sz.cx;
                TextOutW(hdc, tx, 1, label, cchLabel);
            }
        }
    }

    // ---- indent markers ----------------------------------------------------
    // Two triangles: the upper one is the first-line indent (dxOffset relative
    // to dxStartIndent), the lower one is the left indent itself. They are drawn
    // at different vertical positions so that a negative first-line offset --
    // the hanging indent every bulleted list uses -- is visible as two separate
    // markers rather than one overlapping blob.
    LONG startTwips = s->startIndentTwips;
    LONG firstTwips = s->startIndentTwips + s->firstLineTwips;

    int xStart = TwipToX(s, startTwips);
    int xFirst = TwipToX(s, firstTwips);

    HPEN hPenMark = CreatePen(PS_SOLID, 1, RGB(30, 30, 30));
    SelectObject(hdc, hPenMark);

    // Upper triangle: first-line indent.
    {
        POINT pts[3] = {
            { xFirst,              2 },
            { xFirst - kMarkerWidth / 2, 2 + kMarkerHeight },
            { xFirst + kMarkerWidth / 2, 2 + kMarkerHeight },
        };
        HBRUSH hMark = CreateSolidBrush(RGB(90, 90, 90));
        HBRUSH hOldBr = static_cast<HBRUSH>(SelectObject(hdc, hMark));
        Polygon(hdc, pts, 3);
        SelectObject(hdc, hOldBr);
        DeleteObject(hMark);
    }

    // Lower triangle: left indent.
    {
        POINT pts[3] = {
            { xStart,              cy - 2 },
            { xStart - kMarkerWidth / 2, cy - 2 - kMarkerHeight },
            { xStart + kMarkerWidth / 2, cy - 2 - kMarkerHeight },
        };
        HBRUSH hMark = CreateSolidBrush(RGB(90, 90, 90));
        HBRUSH hOldBr = static_cast<HBRUSH>(SelectObject(hdc, hMark));
        Polygon(hdc, pts, 3);
        SelectObject(hdc, hOldBr);
        DeleteObject(hMark);
    }

    // ---- bottom rule --------------------------------------------------------
    SelectObject(hdc, hPenMajor);
    MoveToEx(hdc, 0, cy - 1, nullptr);
    LineTo(hdc, cx, cy - 1);

    // ---- blit ----------------------------------------------------------------
    BitBlt(hdcTarget, 0, 0, cx, cy, hdc, 0, 0, SRCCOPY);

    // ---- teardown --------------------------------------------------------------
    // Every GDI object created above is selected out before deletion. Deleting a
    // GDI object that is still selected into a DC fails silently and leaks the
    // object for the life of the process, which on a ruler repainted during
    // resize means thousands of leaked pens per second.
    SelectObject(hdc, hOldFont);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBm);
    DeleteObject(hPenMinor);
    DeleteObject(hPenMajor);
    DeleteObject(hPenMark);
    DeleteObject(hWhite);
    DeleteObject(hFace);
    DeleteObject(hbm);
    DeleteDC(hdc);
}

} // anonymous namespace

// ===========================================================================
//  RulerWndProc
// ===========================================================================
LRESULT CALLBACK RulerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCREATE:
    {
        // The ruler has no per-instance data at creation; Ruler_SetMetrics
        // allocates the state on first use so that a ruler that is never
        // measured costs nothing.
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PaintRuler(hwnd, hdc, rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        // PaintRuler fills every pixel, so erasing first is pure flicker.
        return 1;

    case WM_LBUTTONDOWN:
    {
        RulerState* s = StateOf(hwnd);
        if (!s) return 0;

        int x = GET_X_LPARAM(lp);
        int xStart = TwipToX(s, s->startIndentTwips);
        int xFirst = TwipToX(s, s->startIndentTwips + s->firstLineTwips);

        // Whichever marker is closer to the click is the one that gets dragged.
        // Comparing distances rather than testing "is the click inside the
        // triangle" makes the markers easy to grab even when they overlap, which
        // is the common case for a paragraph with no hanging indent.
        int dStart = abs(x - xStart);
        int dFirst = abs(x - xFirst);

        if (min(dStart, dFirst) <= kMarkerWidth)
        {
            s->dragging  = true;
            s->dragWhich = (dFirst <= dStart) ? 2 : 1;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        RulerState* s = StateOf(hwnd);
        if (!s || !s->dragging) return 0;

        int x = GET_X_LPARAM(lp);
        LONG twips = XToTwip(s, x);

        // Clamp to the printable area. Allowing a negative left indent would
        // push text under the margin band, and allowing one past the right
        // margin would leave nothing to lay out in.
        LONG maxTwips = s->pageWidthTwips - s->marginLeftTwips - s->marginRightTwips;
        if (twips < 0) twips = 0;
        if (twips > maxTwips) twips = maxTwips;

        // Snap to 1/16 inch. A free-floating indent is impossible to set to a
        // repeatable value by mouse, and snapping makes the ruler usable for
        // real layout work.
        const LONG kSnap = kTwipsPerInch / 16;
        twips = ((twips + kSnap / 2) / kSnap) * kSnap;

        // Report to the frame. The ruler does not write to the document; see the
        // file header for why.
        HWND hFrame = GetParent(hwnd);
        AppFrame* frame = hFrame ? AppFrame::FromHwnd(hFrame) : nullptr;
        if (frame)
        {
            PARAFORMAT2 pf;
            frame->View().GetParaFormat(pf);
            pf.cbSize = sizeof(pf);

            if (s->dragWhich == 1)
            {
                // Lower marker = the paragraph's left indent. Moves the whole
                // paragraph; dxOffset is left alone so any hanging indent the
                // user set survives the drag.
                pf.dwMask        = PFM_STARTINDENT;
                pf.dxStartIndent = twips;
            }
            else
            {
                // Upper marker = the first line's position. dxOffset is defined
                // as the first line's distance FROM dxStartIndent, so the value
                // to store is the difference between where the user dragged and
                // where the paragraph starts -- not the absolute position.
                pf.dwMask   = PFM_OFFSET;
                pf.dxOffset = twips - pf.dxStartIndent;
            }
            frame->View().SetParaFormat(pf);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        RulerState* s = StateOf(hwnd);
        if (s && s->dragging)
        {
            s->dragging = false;
            s->dragWhich = 0;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // Capture can be taken away by another window (a menu opening, for
        // instance). Clearing the drag state here prevents a stuck drag in which
        // the ruler keeps moving markers with no button held.
        if (RulerState* s = StateOf(hwnd))
        {
            s->dragging  = false;
            s->dragWhich = 0;
        }
        return 0;

    case WM_DESTROY:
        if (RulerState* s = StateOf(hwnd))
        {
            delete s;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ===========================================================================
//  Public API
// ===========================================================================
void Ruler_Invalidate(HWND hRuler)
{
    if (hRuler) InvalidateRect(hRuler, nullptr, FALSE);
}

void Ruler_SetMetrics(HWND hRuler, const AppSettings& s, const RECT& rcText,
                      const PARAFORMAT2& pf, LONG pageWidthTwips)
{
    if (!hRuler) return;

    RulerState* st = StateOf(hRuler);
    if (!st)
    {
        st = new (std::nothrow) RulerState();
        if (!st) return;
        SetWindowLongPtrW(hRuler, GWLP_USERDATA, LParamFromPtr(st));
    }

    st->units          = s.units;
    st->pageWidthTwips = (pageWidthTwips > 0) ? pageWidthTwips
                                              : (8 * kTwipsPerInch + kTwipsPerInch / 2);
    st->marginLeftTwips   = s.page.marginLeft;
    st->marginRightTwips  = s.page.marginRight;
    st->startIndentTwips  = pf.dxStartIndent;
    st->firstLineTwips    = pf.dxOffset;

    // Pixels per twip depends on the ruler's own width and the zoom, because
    // the ruler shows the same page the document is showing. Deriving it from
    // rcText keeps the ruler's scale identical to the edit control's, so a
    // marker dragged to "3 inches" on the ruler really is 3 inches in the text.
    int cxText = rcText.right - rcText.left;
    if (cxText <= 0) cxText = 1;

    int zoomPct = (s.nZoomDen > 0) ? (s.nZoomNum * 100 / s.nZoomDen) : 100;
    if (zoomPct < 10) zoomPct = 10;

    LONG scaledPage = MulDiv(st->pageWidthTwips, zoomPct, 100);
    if (scaledPage <= 0) scaledPage = st->pageWidthTwips;

    // twipsPerPixel is the inverse scale. Integer division here loses sub-pixel
    // precision, which is invisible on a ruler and avoids floating point in the
    // paint path.
    st->twipsPerPixel = (scaledPage + cxText / 2) / cxText;
    if (st->twipsPerPixel < 1) st->twipsPerPixel = 1;

    // The printable area starts at the left margin, which is also scaled.
    st->textOriginX = MulDiv(s.page.marginLeft, zoomPct, 100) / st->twipsPerPixel;

    Ruler_Invalidate(hRuler);
}

LONG Ruler_HitTestMargin(HWND hRuler, int x)
{
    RulerState* s = hRuler ? StateOf(hRuler) : nullptr;
    if (!s) return -1;
    return XToTwip(s, x);
}
