// ============================================================================
//  dialogs.cpp -- LetterPad Options and About
//
//  The Options dialog edits a COPY of the settings. It is modal and commits on
//  IDOK, so Cancel genuinely discards every change including the font the user
//  previewed. Committing live would mean an "Apply" button and a much harder
//  question about what happens to a half-configured document.
//
//  FONT ENUMERATION
//  EnumFontFamiliesEx over a screen DC, filtered to the fonts RichEdit can
//  actually render. Two filters matter:
//    - DEVICE_FONTTYPE out: printer-resident fonts cannot be shown on screen,
//      and selecting one produces text that renders in a substitute font but
//      prints in the real one -- a silent mismatch.
//    - duplicate face names out: a font with both regular and italic faces
//      enumerates twice under the same name, and a combo box with "Arial" in it
//      four times looks broken.
//
//  The preview is an SS_OWNERDRAW static painted in WM_DRAWITEM. Using a real
//  edit control for the preview would let the user type in it, which is not what
//  a preview is for.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  Options dialog state
// ---------------------------------------------------------------------------
struct OptionsCtx
{
    AppSettings* settings;    // the draft being edited
    WCHAR        faceName[LF_FACESIZE];
    int          pointSize;   // points, not twips: the UI speaks points
    bool         previewDirty;
};

// Font names collected during enumeration. A std::vector of std::wstring is
// used rather than a fixed array because the number of installed fonts ranges
// from about 20 on a fresh system to several hundred with design tools present.
struct FontCollect
{
    std::vector<std::wstring>* names;
};

int CALLBACK EnumFontProc(const LOGFONTW* lf, const TEXTMETRICW* /*tm*/,
                          DWORD fontType, LPARAM lParam)
{
    FontCollect* fc = PtrFromLParam<FontCollect>(lParam);
    if (!fc || !fc->names || !lf) return 1;      // 1 = continue enumerating

    // Printer-only fonts cannot be displayed, so offering them is offering a
    // choice that silently substitutes.
    if (fontType & DEVICE_FONTTYPE) return 1;

    // lfFaceName is the family name; lfStyleName distinguishes the faces within
    // it. Only the family name goes in the list -- a font picker that lists
    // "Arial Black" separately from "Arial" is not wrong, but it is not what a
    // word processor's font box does either.
    if (lf->lfFaceName[0] == L'\0') return 1;

    // Skip the vertical (@-prefixed) variants of CJK fonts. They are real fonts
    // used for vertical text layout and would double the length of the list for
    // no benefit in a left-to-right editor.
    if (lf->lfFaceName[0] == L'@') return 1;

    // De-duplicate. A linear scan is O(n^2) over the font list, which for a few
    // hundred fonts is a few thousand comparisons during dialog open -- well
    // under a millisecond, and far simpler than maintaining a sorted set.
    const std::wstring name(lf->lfFaceName);
    for (const auto& existing : *fc->names)
    {
        if (_wcsicmp(existing.c_str(), name.c_str()) == 0)
            return 1;
    }

    fc->names->push_back(name);
    return 1;
}

// Sorts case-insensitively so the list reads alphabetically regardless of the
// order the font driver happened to enumerate in.
void SortFontNames(std::vector<std::wstring>& names)
{
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b)
              { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
}

void PopulateFontList(HWND hDlg, const OptionsCtx* ctx)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_OPT_FONTLIST);
    if (!hCombo) return;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    HDC hdc = GetDC(hDlg);
    if (hdc)
    {
        std::vector<std::wstring> names;
        FontCollect fc = { &names };

        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        // An empty face name with DEFAULT_CHARSET enumerates every charset's
        // every family. That is the broad query, and the filters in EnumFontProc
        // are what narrow it to something usable.
        EnumFontFamiliesExW(hdc, &lf, EnumFontProc,
                            LParamFromPtr(&fc), 0);
        ReleaseDC(hDlg, hdc);

        SortFontNames(names);
        for (const auto& n : names)
            SendMessageW(hCombo, CB_ADDSTRING, 0, LParamFromPtr(n.c_str()));
    }

    // Select the current face, or fall back to the first entry. A combo with
    // nothing selected shows a blank edit field, and the user then has no idea
    // what the document's font is.
    int idx = static_cast<int>(SendMessageW(hCombo, CB_FINDSTRINGEXACT,
                                            static_cast<WPARAM>(-1),
                                            LParamFromPtr(ctx->faceName)));
    if (idx == CB_ERR) idx = 0;
    SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);
}

void PopulateSizeList(HWND hDlg, int currentPoints)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_OPT_SIZELIST);
    if (!hCombo) return;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    // The conventional point ladder. It is not linear: the gaps widen as sizes
    // grow, because the perceptual difference between 72 and 73 is nil while
    // the difference between 8 and 9 is obvious.
    static const int kSizes[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18,
                                  20, 22, 24, 26, 28, 36, 48, 72 };
    WCHAR buf[16];
    for (int s : kSizes)
    {
        StrPrintf(buf, _countof(buf), L"%d", s);
        SendMessageW(hCombo, CB_ADDSTRING, 0, LParamFromPtr(buf));
    }

    // The current size is added even if it is not on the ladder, so an 11.5 pt
    // or a 30 pt document does not show a size that is not the document's.
    StrPrintf(buf, _countof(buf), L"%d", currentPoints);
    int idx = static_cast<int>(SendMessageW(hCombo, CB_FINDSTRINGEXACT,
                                            static_cast<WPARAM>(-1),
                                            LParamFromPtr(buf)));
    if (idx == CB_ERR)
    {
        SendMessageW(hCombo, CB_ADDSTRING, 0, LParamFromPtr(buf));
        idx = static_cast<int>(SendMessageW(hCombo, CB_FINDSTRINGEXACT,
                                            static_cast<WPARAM>(-1),
                                            LParamFromPtr(buf)));
    }
    if (idx != CB_ERR)
        SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);

    // The edit field mirrors the selection so the user can also type a size.
    SetDlgItemInt(hDlg, IDC_OPT_SIZEEDIT, currentPoints, FALSE);
}

// Reads the face and size out of the dialog into the context.
bool ReadFontFromDialog(HWND hDlg, OptionsCtx* ctx)
{
    WCHAR face[LF_FACESIZE] = {};
    int n = GetDlgItemTextW(hDlg, IDC_OPT_FONTLIST, face, _countof(face));
    if (n <= 0) return false;
    wcsncpy_s(ctx->faceName, _countof(ctx->faceName), face, _TRUNCATE);

    // Typed values win over the ladder selection, because a user who types "13"
    // into a list of 12 and 14 means 13.
    BOOL translated = FALSE;
    UINT pts = GetDlgItemInt(hDlg, IDC_OPT_SIZEEDIT, &translated, FALSE);
    if (!translated || pts == 0)
    {
        WCHAR sz[16] = {};
        GetDlgItemTextW(hDlg, IDC_OPT_SIZELIST, sz, _countof(sz));
        pts = static_cast<UINT>(_wtoi(sz));
    }

    // RichEdit clamps internally, but an out-of-range size reaching CHARFORMAT2W
    // produces text that renders at the clamp value while the dialog still shows
    // what the user typed. Clamping here keeps the two consistent.
    if (pts < 1)    pts = 1;
    if (pts > 1638) pts = 1638;
    ctx->pointSize = static_cast<int>(pts);

    return true;
}

// ---------------------------------------------------------------------------
//  Preview painting
// ---------------------------------------------------------------------------
void PaintFontPreview(HWND hDlg, DRAWITEMSTRUCT* dis, const OptionsCtx* ctx)
{
    if (!dis || !dis->hDC) return;

    RECT rc = dis->rcItem;
    FillRect(dis->hDC, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    FrameRect(dis->hDC, &rc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    // Built from the dialog's current values, not from ctx->settings, so the
    // preview updates as the user browses the list before committing.
    WCHAR face[LF_FACESIZE] = {};
    if (GetDlgItemTextW(hDlg, IDC_OPT_FONTLIST, face, _countof(face)) <= 0)
        wcsncpy_s(face, _countof(face), ctx->faceName, _TRUNCATE);

    BOOL tr = FALSE;
    UINT pts = GetDlgItemInt(hDlg, IDC_OPT_SIZEEDIT, &tr, FALSE);
    if (!tr || pts == 0) pts = static_cast<UINT>(ctx->pointSize);
    if (pts < 1) pts = 1;
    // Cap the preview size: a 72 pt preview in a small box would show one
    // clipped glyph, which tells the user nothing about the font.
    if (pts > 28) pts = 28;

    HDC hdc = dis->hDC;
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (dpi <= 0) dpi = 96;

    HFONT hFont = CreateFontW(-MulDiv(static_cast<int>(pts), dpi, 72), 0, 0, 0,
                              FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, face);
    if (!hFont) return;

    HFONT hOld = static_cast<HFONT>(SelectObject(hdc, hFont));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    static const WCHAR kSample[] = L"AaBbYyZz 0123";
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, kSample, static_cast<int>(wcslen(kSample)), &sz);

    // Centred, and clamped so a long sample at a large size is truncated rather
    // than drawn outside the preview box.
    int x = rc.left + ((rc.right - rc.left) - sz.cx) / 2;
    int y = rc.top  + ((rc.bottom - rc.top) - sz.cy) / 2;
    if (x < rc.left + 2) x = rc.left + 2;
    if (y < rc.top  + 1) y = rc.top  + 1;

    TextOutW(hdc, x, y, kSample, static_cast<int>(wcslen(kSample)));

    SelectObject(hdc, hOld);
    DeleteObject(hFont);     // created here, so deleted here
}

// ---------------------------------------------------------------------------
//  Dialog procedure
// ---------------------------------------------------------------------------
INT_PTR CALLBACK OptionsDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    OptionsCtx* ctx = PtrFromLParam<OptionsCtx>(GetWindowLongPtrW(hDlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ctx = PtrFromLParam<OptionsCtx>(lp);
        SetWindowLongPtrW(hDlg, DWLP_USER, LParamFromPtr(ctx));

        // Seed the context from the draft settings. yHeight is twips; the
        // dialog works in points because that is what a font size box means to
        // a user.
        wcsncpy_s(ctx->faceName, _countof(ctx->faceName),
                  ctx->settings->defChar.szFaceName, _TRUNCATE);
        ctx->pointSize = ctx->settings->defChar.yHeight / kTwipsPerPt;
        if (ctx->pointSize <= 0) ctx->pointSize = 11;

        PopulateFontList(hDlg, ctx);
        PopulateSizeList(hDlg, ctx->pointSize);

        // Units
        UINT unitIds[] = { IDC_OPT_UNITS_INCH, IDC_OPT_UNITS_CM,
                           IDC_OPT_UNITS_PT,  IDC_OPT_UNITS_PICA };
        int u = ctx->settings->units;
        if (u < 0 || u > 3) u = 0;
        CheckRadioButton(hDlg, unitIds[0], unitIds[3], unitIds[u]);

        // Word wrap
        UINT wrapIds[] = { IDC_OPT_WRAP_NONE, IDC_OPT_WRAP_WINDOW, IDC_OPT_WRAP_RULER };
        int w = ctx->settings->wrap;
        if (w < 0 || w > 2) w = 1;
        CheckRadioButton(hDlg, wrapIds[0], wrapIds[2], wrapIds[w]);

        CheckDlgButton(hDlg, IDC_OPT_RULER,
                       ctx->settings->fShowRuler     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_OPT_STATUSBAR,
                       ctx->settings->fShowStatusBar ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_OPT_RTFDEFAULT,
                       ctx->settings->rtfAsDefault   ? BST_CHECKED : BST_UNCHECKED);

        InvalidateRect(GetDlgItem(hDlg, IDC_OPT_PREVIEW), nullptr, TRUE);
        return TRUE;
    }

    case WM_COMMAND:
    {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        switch (id)
        {
        case IDOK:
        {
            if (!ReadFontFromDialog(hDlg, ctx))
            {
                // No font could be read, which means the combo is empty -- only
                // possible if font enumeration failed entirely. Committing then
                // would write a zero-length face name into the defaults and the
                // next document would open in the system fallback font.
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }

            AppSettings* s = ctx->settings;

            wcsncpy_s(s->defChar.szFaceName, _countof(s->defChar.szFaceName),
                      ctx->faceName, _TRUNCATE);
            s->defChar.yHeight = ctx->pointSize * kTwipsPerPt;
            s->defChar.dwMask |= CFM_FACE | CFM_SIZE;
            s->defChar.cbSize  = sizeof(CHARFORMAT2W);

            if (IsDlgButtonChecked(hDlg, IDC_OPT_UNITS_INCH)  == BST_CHECKED) s->units = kUnitInch;
            else if (IsDlgButtonChecked(hDlg, IDC_OPT_UNITS_CM) == BST_CHECKED) s->units = kUnitCm;
            else if (IsDlgButtonChecked(hDlg, IDC_OPT_UNITS_PT) == BST_CHECKED) s->units = kUnitPoint;
            else if (IsDlgButtonChecked(hDlg, IDC_OPT_UNITS_PICA) == BST_CHECKED) s->units = kUnitPica;

            if (IsDlgButtonChecked(hDlg, IDC_OPT_WRAP_NONE)   == BST_CHECKED) s->wrap = kWrapNone;
            else if (IsDlgButtonChecked(hDlg, IDC_OPT_WRAP_WINDOW) == BST_CHECKED) s->wrap = kWrapWindow;
            else if (IsDlgButtonChecked(hDlg, IDC_OPT_WRAP_RULER)  == BST_CHECKED) s->wrap = kWrapRuler;

            s->fShowRuler     = (IsDlgButtonChecked(hDlg, IDC_OPT_RULER)     == BST_CHECKED);
            s->fShowStatusBar = (IsDlgButtonChecked(hDlg, IDC_OPT_STATUSBAR) == BST_CHECKED);
            s->rtfAsDefault   = (IsDlgButtonChecked(hDlg, IDC_OPT_RTFDEFAULT) == BST_CHECKED);

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            // Nothing to undo: the dialog edited a copy.
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case IDC_OPT_FONTLIST:
        case IDC_OPT_SIZELIST:
            if (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)
            {
                // Mirror the ladder selection into the size edit field so the
                // two cannot disagree, then refresh the preview.
                if (id == IDC_OPT_SIZELIST && code == CBN_SELCHANGE)
                {
                    WCHAR sz[16] = {};
                    GetDlgItemTextW(hDlg, IDC_OPT_SIZELIST, sz, _countof(sz));
                    SetDlgItemTextW(hDlg, IDC_OPT_SIZEEDIT, sz);
                }
                InvalidateRect(GetDlgItem(hDlg, IDC_OPT_PREVIEW), nullptr, TRUE);
                return TRUE;
            }
            break;

        case IDC_OPT_SIZEEDIT:
            if (code == EN_CHANGE)
            {
                InvalidateRect(GetDlgItem(hDlg, IDC_OPT_PREVIEW), nullptr, TRUE);
                return TRUE;
            }
            break;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = PtrFromLParam<DRAWITEMSTRUCT>(lp);
        if (dis && dis->CtlID == IDC_OPT_PREVIEW && ctx)
        {
            PaintFontPreview(hDlg, dis, ctx);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        // The owner-draw preview needs a white background brush, not the dialog
        // grey, or WM_DRAWITEM's FillRect fights the dialog's own erase and the
        // box flickers on every repaint.
        HWND hCtl = reinterpret_cast<HWND>(lp);
        if (GetDlgCtrlID(hCtl) == IDC_OPT_PREVIEW)
        {
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
        }
        break;
    }
    }
    return FALSE;
}

} // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================
INT_PTR ShowOptionsDialog(HWND hOwner, AppSettings& s)
{
    OptionsCtx ctx = {};
    ctx.settings    = &s;
    ctx.pointSize   = s.defChar.yHeight / kTwipsPerPt;
    if (ctx.pointSize <= 0) ctx.pointSize = 11;
    wcsncpy_s(ctx.faceName, _countof(ctx.faceName), s.defChar.szFaceName, _TRUNCATE);

    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_OPTIONS), hOwner,
                           OptionsDlgProc, LParamFromPtr(&ctx));
}

namespace {

INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR text[512];
        WCHAR fmt[512];
        if (LoadStr(IDS_ABOUT_TEXT, fmt, _countof(fmt)) > 0)
            StrPrintf(text, _countof(text), fmt,
                      LP_VERSION_MAJOR, LP_VERSION_MINOR, LP_VERSION_PATCH);
        else
            StrPrintf(text, _countof(text),
                      L"LetterPad %d.%d.%d\n\nAn original rich-text word processor.\n"
                      L"Not affiliated with, derived from, or a reproduction of\n"
                      L"Microsoft WordPad.",
                      LP_VERSION_MAJOR, LP_VERSION_MINOR, LP_VERSION_PATCH);
        SetDlgItemTextW(hDlg, IDC_ABOUT_TEXT, text);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        break;
    }
    (void)lp;
    return FALSE;
}

} // anonymous namespace

INT_PTR ShowAboutDialog(HWND hOwner)
{
    return DialogBoxW(g_hInst, MAKEINTRESOURCEW(IDD_ABOUT), hOwner, AboutDlgProc);
}
