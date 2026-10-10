// ============================================================================
//  ruler.cpp -- the horizontal ruler above the document
//
//  THE INDENT MODEL (read this before touching a marker)
//  RichEdit's PARAFORMAT stores two values, and their meaning is the opposite
//  of the intuitive reading:
//
//      dxStartIndent   the position of the FIRST line, from the left margin
//      dxOffset        the position of the SECOND and later lines, RELATIVE TO
//                      dxStartIndent
//
//  So a hanging indent (bullets, numbered lists) is dxOffset > 0: the first line
//  sits at the margin with the bullet and the wrapped lines step right. A
//  paragraph whose first line is indented (prose style) is dxOffset < 0.
//  Treating dxStartIndent as "the paragraph indent" -- the obvious but wrong
//  reading -- swaps the two markers and makes every drag write the other
//  indent. Everything below is written against the correct model.
//
//  SCALE
//  The ruler maps twips to pixels through one rational scale, pxNum/pxDen,
//  where pxNum = dpiX * zoomPercent and pxDen = 1440 * 100. A single rational
//  is used rather than a twips-per-pixel integer because the inverse of the
//  scale is not an integer at most zooms, and an integer twipsPerPixel drifts
//  visibly: at 200% zoom an inch would otherwise occupy half its true width.
//  The same scale feeds the tick grid, the margin bands, the markers and the
//  drag hit-test, so they cannot disagree with each other.
//
//  PAINTING
//  WM_PAINT builds the whole ruler into a memory DC and blits once. Drawing
//  straight to the window DC shows tick-by-tick painting during a resize.
//
//  The ruler is a view. It never writes to the document itself; drags are
//  reported to the frame, which applies them. A view that also edits is how a
//  ruler and a document get out of step.
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
    UnitSystem units           = kUnitInch;
    LONG       pageWidthTwips  = 8 * kTwipsPerInch + kTwipsPerInch / 2;
    LONG       marginLeftTwips  = kTwipsPerInch;
    LONG       marginRightTwips = kTwipsPerInch;
    LONG       firstLineTwips   = 0;   // dxStartIndent
    LONG       laterOffsetTwips = 0;   // dxOffset, relative to firstLineTwips
    int        textOriginX      = 0;   // left margin, in pixels
    LONG       pxNum            = 96;  // pixels-per-twip, as the rational
    LONG       pxDen            = 1440; //   pxNum / pxDen
    bool       dragging         = false;
    int        dragWhich        = 0;   // 1 = left/later marker, 2 = first line
};

RulerState* StateOf(HWND h)
{
    return PtrFromLParam<RulerState>(GetWindowLongPtrW(h, GWLP_USER));
}

int TwipToX(const RulerState* s, LONG twips)
{
    if (!s) return 0;
    return s->textOriginX + MulDiv(twips, s->pxNum, s->pxDen);
}

LONG XToTwip(const RulerState* s, int x)
{
    if (!s) return 0;
    return MulDiv(x - s->textOriginX, s->pxDen, s->pxNum);
}

// Positions of the two markers. The upper triangle is the FIRST line; the lower
// one is the left edge of the second and later lines.
int XFirstLine(const RulerState* s) { return TwipToX(s, s->firstLineTwips); }
int XLaterLines(const RulerState* s)
{
    return TwipToX(s, s->firstLineTwips + s->laterOffsetTwips);
}

// ---------------------------------------------------------------------------
//  Tick scale
//
//  One rational step per unit, in twips per minor tick, expressed as num/den so
//  that metric units -- where a millimetre is 56.69 twips and no integer step is
//  exact -- still land on the right pixels. An integer twips-per-minor for
//  centimetres is how a "1 cm" label ends up an inch away from the last one.
// ---------------------------------------------------------------------------
struct TickScale
{
    LONG num;             // twips per minor tick = num / den
    LONG den;
    int  minorsPerMajor;  // minor ticks between labelled ticks
};

TickScale ScaleForUnit(UnitSystem u)
{
    switch (u)
    {
    case kUnitCm:
        // Minor = 1 mm. 1 mm = 1440/25.4 twips = 14400/254 exactly. Major and
        // labelled every 10 minors, i.e. every centimetre.
        return { 14400, 254, 10 };
    case kUnitPoint:
        // Minor = 1 pt, major = 12 pt.
        return { kTwipsPerPt, 1, 12 };
    case kUnitPica:
        // Minor = 2 pt, major = 1 pica (6 minors).
        return { kTwipsPerPica / 6, 1, 6 };
    case kUnitInch:
    default:
        // Minor = 1/8 inch, major = 1 inch.
        return { kTwipsPerInch / 8, 1, 8 };
    }
}

// Formats the label for a major tick. `minorIndex` counts minor ticks from the
// left margin; converting that to the user's unit is the whole job here. An
// empty buffer means "major tick with no label", which the metric ruler uses
// between whole centimetres.
void FormatTickLabel(UnitSystem u, int minorIndex, PWSTR buf, size_t cch)
{
    buf[0] = L'\0';
    if (minorIndex < 0) return;

    switch (u)
    {
    case kUnitInch:
        StrPrintf(buf, cch, L"%d", minorIndex / 8);
        break;

    case kUnitCm:
        // minors are millimetres; label whole centimetres only. A label every
        // 5 mm would overlap itself at any normal zoom.
        if (minorIndex % 10 == 0)
            StrPrintf(buf, cch, L"%d", minorIndex / 10);
        break;

    case kUnitPoint:
        StrPrintf(buf, cch, L"%d", minorIndex);
        break;

    case kUnitPica:
        StrPrintf(buf, cch, L"%d", minorIndex / 6);
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

    TickScale sc = ScaleForUnit(s->units);

    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT hOldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    LONG textWidth = s->pageWidthTwips - s->marginLeftTwips - s->marginRightTwips;
    if (textWidth < 0) textWidth = 0;

    // Iterate by minor-tick INDEX and derive each position with MulDiv, so a
    // non-integer twip step (the metric case) accumulates no error: tick i is at
    // exactly i * num/den twips, never at the sum of i rounded steps.
    for (int i = 0; ; ++i)
    {
        LONG t = MulDiv(i, sc.num, sc.den);
        if (t > textWidth) break;

        int x = TwipToX(s, t);
        if (x < 0 || x > cx) continue;

        bool major = (i % sc.minorsPerMajor) == 0;

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
            FormatTickLabel(s->units, i, label, _countof(label));
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
    // Upper triangle: the FIRST line (dxStartIndent). Lower triangle: the left
    // edge of the second and later lines (dxStartIndent + dxOffset). Drawing
    // them at different vertical positions is what makes a hanging indent
    // readable as two markers instead of one overlapping blob.
    int xFirst = XFirstLine(s);
    int xLater = XLaterLines(s);

    HPEN hPenMark = CreatePen(PS_SOLID, 1, RGB(30, 30, 30));
    SelectObject(hdc, hPenMark);

    {
        POINT pts[3] = {
            { xFirst,                  2 },
            { xFirst - kMarkerWidth / 2, 2 + kMarkerHeight },
            { xFirst + kMarkerWidth / 2, 2 + kMarkerHeight },
        };
        HBRUSH hMark = CreateSolidBrush(RGB(90, 90, 90));
        HBRUSH hOldBr = static_cast<HBRUSH>(SelectObject(hdc, hMark));
        Polygon(hdc, pts, 3);
        SelectObject(hdc, hOldBr);
        DeleteObject(hMark);
    }

    {
        POINT pts[3] = {
            { xLater,                  cy - 2 },
            { xLater - kMarkerWidth / 2, cy - 2 - kMarkerHeight },
            { xLater + kMarkerWidth / 2, cy - 2 - kMarkerHeight },
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
        int y = GET_Y_LPARAM(lp);
        int xFirst = XFirstLine(s);
        int xLater = XLaterLines(s);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int cy = rc.bottom - rc.top;

        int dFirst = abs(x - xFirst);
        int dLater = abs(x - xLater);

        if (min(dFirst, dLater) <= kMarkerWidth)
        {
            // When the markers overlap -- the common case for a paragraph with
            // no hanging indent -- distance cannot choose between them, so the
            // vertical half of the click does: the upper triangle is the first
            // line, the lower one is the left/later-lines edge.
            bool overlap = abs(xFirst - xLater) <= kMarkerWidth;
            if (overlap)
                s->dragWhich = (y < cy / 2) ? 2 : 1;
            else
                s->dragWhich = (dFirst <= dLater) ? 2 : 1;

            s->dragging = true;
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

        // Clamp to the printable area. Allowing a negative position would push
        // text under the margin band; allowing one past the right margin would
        // leave nothing to lay out in.
        LONG maxTwips = s->pageWidthTwips - s->marginLeftTwips - s->marginRightTwips;
        if (twips < 0) twips = 0;
        if (twips > maxTwips) twips = maxTwips;

        // Snap to 1/16 inch. A free-floating indent is impossible to set to a
        // repeatable value by mouse, and snapping makes the ruler usable for
        // real layout work.
        const LONG kSnap = kTwipsPerInch / 16;
        twips = ((twips + kSnap / 2) / kSnap) * kSnap;

        HWND hFrame = GetParent(hwnd);
        AppFrame* frame = hFrame ? AppFrame::FromHwnd(hFrame) : nullptr;
        if (frame)
        {
            PARAFORMAT2 pf;
            frame->View().GetParaFormat(pf);
            pf.cbSize = sizeof(pf);

            if (s->dragWhich == 1)
            {
                // Lower marker = the left edge of the second and later lines.
                // dxOffset is relative to dxStartIndent, so moving the later
                // lines while leaving the first line exactly where it is means
                // writing the difference.
                pf.dwMask   = PFM_OFFSET;
                pf.dxOffset = twips - pf.dxStartIndent;
            }
            else
            {
                // Upper marker = the first line. Moving it must not drag the
                // later lines with it, so their absolute position (start +
                // offset) is preserved by rewriting both fields.
                LONG laterAbsolute = pf.dxStartIndent + pf.dxOffset;
                pf.dwMask        = PFM_STARTINDENT | PFM_OFFSET;
                pf.dxStartIndent = twips;
                pf.dxOffset      = laterAbsolute - twips;
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
            s->dragging  = false;
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
    // The FULL configured page width, not the edit control's client width. The
    // ruler's job is to show the page measure; deriving it from the client area
    // would make the margin bands move every time the window is resized and
    // would disagree with page setup.
    st->pageWidthTwips = (pageWidthTwips > 0) ? pageWidthTwips
                                              : (8 * kTwipsPerInch + kTwipsPerInch / 2);
    st->marginLeftTwips   = s.page.marginLeft;
    st->marginRightTwips  = s.page.marginRight;
    st->firstLineTwips    = pf.dxStartIndent;
    st->laterOffsetTwips  = pf.dxOffset;

    // ---- scale -------------------------------------------------------------
    // px = twips * dpiX * zoom / (1440 * 100). Higher zoom must produce MORE
    // pixels per twip, so zoom sits in the numerator; an earlier version had it
    // in the denominator via a twips-per-pixel integer, which made 200% zoom
    // render an inch at half its true width.
    HDC hdc = GetDC(hRuler);
    int dpiX = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(hRuler, hdc);
    if (dpiX <= 0) dpiX = 96;

    int zoomPct = (s.nZoomDen > 0) ? (s.nZoomNum * 100 / s.nZoomDen) : 100;
    if (zoomPct < 10)  zoomPct = 10;
    if (zoomPct > 500) zoomPct = 500;

    st->pxNum = static_cast<LONG>(dpiX) * zoomPct;
    st->pxDen = kTwipsPerInch * 100;

    st->textOriginX = MulDiv(s.page.marginLeft, st->pxNum, st->pxDen);

    (void)rcText;    // reserved: a future text-area-only ruler mode

    Ruler_Invalidate(hRuler);
}

LONG Ruler_HitTestMargin(HWND hRuler, int x)
{
    RulerState* s = hRuler ? StateOf(hRuler) : nullptr;
    if (!s) return -1;
    return XToTwip(s, x);
}
