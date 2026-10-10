// ============================================================================
//  editview.cpp -- EditView: the typed wrapper over the RichEdit 4.1 control
//
//  Every method here is one or two SendMessage calls. There is no text buffer,
//  no layout engine and no formatting model in LetterPad; RichEdit is all of
//  those, and this file is purely the boundary between "what the user asked
//  for" and "which EM_* message that corresponds to".
//
//  TWO INVARIANTS THAT CAUSE 90% OF RICHEDIT BUGS
//
//  1. cbSize must be set on every structure passed in. CHARFORMAT2W.cbSize must
//     be sizeof(CHARFORMAT2W) and PARAFORMAT2.cbSize must be sizeof(PARAFORMAT2).
//     If cbSize is wrong RichEdit rejects the entire call and returns FALSE with
//     no error code, no exception and no message. The formatting simply does not
//     change. Every structure in this file is filled through a helper that
//     stamps cbSize first.
//
//  2. dwMask / pfMask must name every field you intend to be honoured. Setting
//     crTextColor without CFM_COLOR is a silent no-op. Worse, a mask bit set
//     without the corresponding field initialised writes stack garbage into the
//     document. Fields and mask bits are always set as a pair below.
//
//  UNITS: every length in this file is TWIPS unless the name says otherwise.
//  Font sizes in CHARFORMAT2W.yHeight are twips (11pt = 220), NOT points and
//  NOT pixels. Off by 20x is the single most common RichEdit mistake.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

// ----------------------------------------------------------------------------
//  Message-number fallbacks
//
//  These are all gated behind _RICHEDIT_VER in the SDK headers. stdafx.h sets
//  it to 0x0500 so they should always be present; the guards are here because
//  a few SDK releases have moved individual defines between version gates and
//  the resulting "EM_SETZOOM undeclared identifier" error gives no hint that
//  the cause is a version macro three files away.
// ----------------------------------------------------------------------------
#ifndef EM_SETZOOM
#define EM_SETZOOM              (WM_USER + 224)
#endif
#ifndef EM_GETZOOM
#define EM_GETZOOM              (WM_USER + 223)
#endif
#ifndef EM_SETTARGETDEVICE
#define EM_SETTARGETDEVICE      (WM_USER + 72)
#endif
#ifndef EM_SETTYPOGRAPHYOPTIONS
#define EM_SETTYPOGRAPHYOPTIONS (WM_USER + 228)
#endif
#ifndef EM_CANPASTE
#define EM_CANPASTE             (WM_USER + 50)
#endif
#ifndef EM_REDO
#define EM_REDO                 (WM_USER + 84)
#endif
#ifndef TO_ADVANCEDTYPOGRAPHY
#define TO_ADVANCEDTYPOGRAPHY   0x0001
#endif
#ifndef MSFTEDIT_CLASS
#define MSFTEDIT_CLASS          L"RICHEDIT50W"
#endif

// ----------------------------------------------------------------------------
//  Zoom steps. RichEdit accepts an arbitrary numerator/denominator pair, but
//  an arbitrary pair produces an arbitrary-feeling control. These are the same
//  discrete stops a word processor zoom slider uses; jumping between them is
//  what makes the slider feel detented rather than slippery.
// ----------------------------------------------------------------------------
static const int kZoomStops[] = { 10, 15, 20, 25, 33, 50, 67, 75, 80, 90, 100,
                                  110, 120, 125, 133, 150, 175, 200, 250, 300,
                                  400, 500 };
static const int kZoomStopCount = static_cast<int>(_countof(kZoomStops));

// ===========================================================================
//  Construction
// ===========================================================================
void EditView::Attach(HWND hEdit, AppFrame* frame)
{
    m_hEdit = hEdit;
    m_frame = frame;
}

// ===========================================================================
//  Selection
// ===========================================================================
void EditView::GetSel(CHARRANGE& r) const
{
    if (!m_hEdit) { r.cpMin = r.cpMax = 0; return; }
    // EM_EXGETSEL returns void; r is filled in place. A zeroed struct is the
    // correct "no selection" answer and is also what an empty document yields.
    r.cpMin = r.cpMax = 0;
    SendMessageW(m_hEdit, EM_EXGETSEL, 0, LParamFromPtr(&r));
}

void EditView::SetSel(const CHARRANGE& r) const
{
    if (!m_hEdit) return;
    CHARRANGE local = r;    // EM_EXSETSEL may clamp, and takes a non-const ptr
    SendMessageW(m_hEdit, EM_EXSETSEL, 0, LParamFromPtr(&local));
}

void EditView::SelectAll() const
{
    if (!m_hEdit) return;
    // cpMax = -1 means "to the end of the text" for EM_EXSETSEL. Using the
    // actual text length also works but needs a WM_GETTEXTLENGTH round trip.
    CHARRANGE r = { 0, -1 };
    SendMessageW(m_hEdit, EM_EXSETSEL, 0, LParamFromPtr(&r));
}

bool EditView::HasSelection() const
{
    CHARRANGE r;
    GetSel(r);
    return r.cpMin != r.cpMax;
}

// ===========================================================================
//  Formatting -- raw structure access
//
//  These are the two methods every discrete formatting operation funnels
//  through, which is why cbSize is stamped here rather than at 20 call sites.
// ===========================================================================
void EditView::GetCharFormat(CHARFORMAT2W& cf) const
{
    // Zero first. GetCharFormat only fills the fields named by the mask it
    // writes back; anything left from a previous call would be indistinguishable
    // from a real value, and callers that check dwMask before reading a field
    // would read stale data.
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    if (!m_hEdit) return;

    // SCF_SELECTION returns the formatting of the selection, or -- when the
    // selection is empty -- the formatting that will apply to the next typed
    // character. That second behaviour is exactly what a toolbar needs in order
    // to show the state of the insertion point.
    SendMessageW(m_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, LParamFromPtr(&cf));
}

void EditView::GetParaFormat(PARAFORMAT2& pf) const
{
    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    if (!m_hEdit) return;
    SendMessageW(m_hEdit, EM_GETPARAFORMAT, 0, LParamFromPtr(&pf));
}

bool EditView::SetCharFormat(const CHARFORMAT2W& cf) const
{
    if (!m_hEdit) return false;
    // Copy-and-stamp: the caller's struct may have been built without cbSize
    // (easy to forget) or with the wrong one, and RichEdit's rejection is
    // completely silent. Stamping here makes it impossible to get wrong.
    CHARFORMAT2W local = cf;
    local.cbSize = sizeof(CHARFORMAT2W);
    return SendMessageW(m_hEdit, EM_SETCHARFORMAT, SCF_SELECTION,
                        LParamFromPtr(&local)) != 0;
}

bool EditView::SetParaFormat(const PARAFORMAT2& pf) const
{
    if (!m_hEdit) return false;
    PARAFORMAT2 local = pf;
    local.cbSize = sizeof(PARAFORMAT2);
    return SendMessageW(m_hEdit, EM_SETPARAFORMAT, 0, LParamFromPtr(&local)) != 0;
}

// ---------------------------------------------------------------------------
//  Toggle helper
//
//  Read the current state, flip it, write it back. NOT "set to a fixed value":
//  when the selection spans mixed formatting, RichEdit reports the effect bit
//  as clear, and a naive "set bold" would make everything bold while the user
//  pressed a button that visibly looked like "bold is on". Read-flip-write
//  gives the intuitive result in both the uniform and mixed cases.
// ---------------------------------------------------------------------------
void EditView::ToggleBold()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize    = sizeof(cf);
    cf.dwMask    = CFM_BOLD;
    cf.dwEffects = (cf.dwEffects & CFE_BOLD) ? 0 : CFE_BOLD;
    SetCharFormat(cf);
}

void EditView::ToggleItalic()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize    = sizeof(cf);
    cf.dwMask    = CFM_ITALIC;
    cf.dwEffects = (cf.dwEffects & CFE_ITALIC) ? 0 : CFE_ITALIC;
    SetCharFormat(cf);
}

void EditView::ToggleUnderline()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize = sizeof(cf);
    // CFM_UNDERLINE and CFM_UNDERLINETYPE must be set together. Setting only
    // CFM_UNDERLINE on a RichEdit 4.1 control leaves bUnderlineType untouched,
    // which produces an underline whose style is whatever the previous
    // character had -- usually single, but not reliably.
    cf.dwMask = CFM_UNDERLINE | CFM_UNDERLINETYPE;
    if (cf.dwEffects & CFE_UNDERLINE)
    {
        cf.dwEffects      &= ~CFE_UNDERLINE;
        cf.bUnderlineType  = CFU_UNDERLINENONE;
    }
    else
    {
        cf.dwEffects      |= CFE_UNDERLINE;
        cf.bUnderlineType  = CFU_UNDERLINE;      // plain single underline
    }
    SetCharFormat(cf);
}

void EditView::ToggleStrikeout()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize    = sizeof(cf);
    cf.dwMask    = CFM_STRIKEOUT;
    cf.dwEffects = (cf.dwEffects & CFE_STRIKEOUT) ? 0 : CFE_STRIKEOUT;
    SetCharFormat(cf);
}

// Subscript and superscript are mutually exclusive: CFE_SUBSCRIPT and
// CFE_SUPERSCRIPT cannot both be set, and setting one without clearing the
// other leaves RichEdit in an undefined rendering state. CFM_OFFSET is the
// mask bit; the effect bits are what actually select which one.
void EditView::ToggleSubscript()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_OFFSET;
    if (cf.dwEffects & CFE_SUBSCRIPT)
        cf.dwEffects &= ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT);
    else
        cf.dwEffects = (cf.dwEffects & ~CFE_SUPERSCRIPT) | CFE_SUBSCRIPT;
    SetCharFormat(cf);
}

void EditView::ToggleSuperscript()
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_OFFSET;
    if (cf.dwEffects & CFE_SUPERSCRIPT)
        cf.dwEffects &= ~(CFE_SUBSCRIPT | CFE_SUPERSCRIPT);
    else
        cf.dwEffects = (cf.dwEffects & ~CFE_SUBSCRIPT) | CFE_SUPERSCRIPT;
    SetCharFormat(cf);
}

void EditView::SetFontColor(COLORREF cr)
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize     = sizeof(cf);
    cf.dwMask     = CFM_COLOR;
    cf.crTextColor = cr;
    // CFM_COLOR alone. dwEffects is deliberately NOT touched here: reusing the
    // struct we just read and writing it back with a broad mask would clobber
    // bold/italic state the caller did not ask about.
    SetCharFormat(cf);
}

void EditView::SetHighlightColor(COLORREF cr)
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize     = sizeof(cf);
    // CFM_BACKCOLOR is the RichEdit 4.1 spelling of text highlight. (CFMBackColor
    // in the older structures is a different, incompatible field.)
    cf.dwMask     = CFM_BACKCOLOR;
    cf.crBackColor = cr;
    SetCharFormat(cf);
}

void EditView::SetFontFace(PCWSTR face, LONG yHeightTwips)
{
    CHARFORMAT2W cf;
    GetCharFormat(cf);
    cf.cbSize = sizeof(cf);

    // Build the mask from what was actually supplied. Passing CFM_SIZE with
    // yHeight == 0 sets the font size to zero, which renders as an empty line
    // with the correct paragraph metrics -- visually "the text disappeared".
    DWORD mask = CFM_FACE;
    if (yHeightTwips > 0)
    {
        mask |= CFM_SIZE;
        cf.yHeight = yHeightTwips;
    }
    cf.dwMask = mask;

    if (face && face[0])
        wcsncpy_s(cf.szFaceName, _countof(cf.szFaceName), face, _TRUNCATE);

    SetCharFormat(cf);
}

// ===========================================================================
//  Paragraph formatting
// ===========================================================================
void EditView::SetAlignment(WORD alignment)
{
    PARAFORMAT2 pf;
    GetParaFormat(pf);
    pf.cbSize     = sizeof(pf);
    pf.dwMask     = PFM_ALIGNMENT;
    pf.wAlignment = alignment;
    SetParaFormat(pf);
}

// Bullets in RichEdit are drawn in the paragraph's hanging-indent region. If
// dxOffset is zero the bullet lands on top of the first character, so turning
// bullets on has to set a hanging indent as part of the same operation. WordPad
// does the same thing; doing only the PFM_NUMBERING half looks like the bullet
// feature is broken.
void EditView::ToggleBullets()
{
    PARAFORMAT2 pf;
    GetParaFormat(pf);

    bool on = (pf.wNumbering == PFN_BULLET);

    pf.cbSize     = sizeof(pf);
    // PFM_STARTINDENT is deliberately absent: neither branch moves the first
    // line, and naming a field in the mask is what makes RichEdit write it.
    pf.dwMask     = PFM_NUMBERING | PFM_OFFSET;

    if (on)
    {
        // Bullets currently ON: switching them off.
        pf.wNumbering = PFN_BULLET_NONE;
        // Remove the hanging indent along with the bullet, otherwise the
        // paragraph keeps a step that nothing on screen explains. A NEGATIVE
        // offset is a prose-style first-line indent, was not created by
        // bullets, and must survive their removal.
        if (pf.dxOffset > 0)
            pf.dxOffset = 0;
    }
    else
    {
        // Bullets currently OFF: switching them on.
        pf.wNumbering = PFN_BULLET;
        // A bulleted paragraph needs a hanging indent: the bullet glyph sits on
        // the first line at the paragraph's left edge and wrapped lines step
        // right of it. In RichEdit's model that is a POSITIVE dxOffset -- later
        // lines relative to the first -- with dxStartIndent untouched. Writing
        // -288 here (as an earlier draft did) produces the mirror image: bullet
        // out in the margin, wrapped lines at the left edge.
        if (pf.dxOffset <= 0)
            pf.dxOffset = 288;      // 0.2 inch; round in inches so ruler ticks align
    }
    SetParaFormat(pf);
}

void EditView::SetLineSpacing(BYTE rule, LONG dyTwips)
{
    PARAFORMAT2 pf;
    GetParaFormat(pf);
    pf.cbSize           = sizeof(pf);
    pf.dwMask           = PFM_LINESPACING;
    pf.bLineSpacingRule = rule;
    // Only meaningful for rules 3/4/5. For 0/1/2 RichEdit ignores it, but we
    // still write a sane value so the structure round-trips predictably.
    pf.dyLineSpacing    = (rule >= 3) ? dyTwips : 20;
    SetParaFormat(pf);
}

// Adjusts the left indent by a delta, clamped so the text cannot be pushed off
// the left edge of the page. dxStartIndent is the paragraph's distance from the
// left margin; dxOffset is the first line's distance from dxStartIndent (usually
// negative for a hanging indent) and is left untouched.
void EditView::AdjustIndent(LONG deltaTwips)
{
    PARAFORMAT2 pf;
    GetParaFormat(pf);

    LONG next = pf.dxStartIndent + deltaTwips;
    if (next < 0) next = 0;
    // Upper bound at 6 inches. Arbitrary but necessary: without it, repeatedly
    // pressing "decrease indent" on a paragraph with dxOffset < 0 can wrap
    // dxStartIndent negative and then very large positive on overflow.
    const LONG kMaxIndent = 6 * kTwipsPerInch;
    if (next > kMaxIndent) next = kMaxIndent;

    pf.cbSize        = sizeof(pf);
    pf.dwMask        = PFM_STARTINDENT;
    pf.dxStartIndent = next;
    SetParaFormat(pf);
}

// ===========================================================================
//  Zoom
// ===========================================================================
void EditView::GetZoom(int& num, int& den) const
{
    num = 100; den = 100;
    if (!m_hEdit) return;

    WPARAM n = 0, d = 0;
    // EM_GETZOOM returns nonzero on success and fills both. If it returns zero
    // the control has no zoom state, and 100/100 is the correct default.
    if (SendMessageW(m_hEdit, EM_GETZOOM, WParamFromPtr(&n), LParamFromPtr(&d)))
    {
        if (n > 0 && d > 0) { num = static_cast<int>(n); den = static_cast<int>(d); }
    }
}

void EditView::SetZoom(int num, int den)
{
    if (!m_hEdit) return;
    if (den <= 0) den = 100;               // guard the divide-by-zero
    if (num <= 0) num = 100;
    SendMessageW(m_hEdit, EM_SETZOOM, static_cast<WPARAM>(num), static_cast<LPARAM>(den));
}

// Steps to the nearest stop strictly greater/less than the current percentage.
// Landing exactly ON a stop moves one further, which is what a detented slider
// does and what makes repeated clicks feel predictable.
void EditView::ZoomIn()
{
    int num, den;
    GetZoom(num, den);
    int cur = (den > 0) ? (num * 100 / den) : 100;

    for (int i = 0; i < kZoomStopCount; ++i)
    {
        if (kZoomStops[i] > cur) { SetZoom(kZoomStops[i], 100); return; }
    }
    SetZoom(kZoomStops[kZoomStopCount - 1], 100);
}

void EditView::ZoomOut()
{
    int num, den;
    GetZoom(num, den);
    int cur = (den > 0) ? (num * 100 / den) : 100;

    for (int i = kZoomStopCount - 1; i >= 0; --i)
    {
        if (kZoomStops[i] < cur) { SetZoom(kZoomStops[i], 100); return; }
    }
    SetZoom(kZoomStops[0], 100);
}

void EditView::ZoomReset()
{
    SetZoom(100, 100);
}

// ===========================================================================
//  Word wrap
//
//  Three mechanisms, because RichEdit exposes three:
//
//    none    ES_AUTOHSCROLL set  + horizontal scrollbar + target device 0
//    window  ES_AUTOHSCROLL clear, no hscroll           + target device 0
//    ruler   ES_AUTOHSCROLL clear, no hscroll           + target device = width
//
//  ES_AUTOHSCROLL is a CREATE-time style for most controls, but RichEdit
//  honours changing it at runtime -- provided SetWindowPos is called with
//  SWP_FRAMECHANGED so the non-client area (and therefore the scrollbars) is
//  recalculated. Without that flag the style changes and nothing visibly does.
// ===========================================================================
void EditView::ApplyWrapMode(WrapMode wrap, LONG rulerWidthTwips)
{
    if (!m_hEdit) return;

    LONG_PTR style = GetWindowLongPtrW(m_hEdit, GWL_STYLE);
    bool wantHScroll = (wrap == kWrapNone);

    if (wantHScroll)
    {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }
    else
    {
        style &= ~(WS_HSCROLL | ES_AUTOHSCROLL);
    }

    // ES_AUTOVSCROLL and the vertical scrollbar stay in all three modes: a
    // document always scrolls vertically.
    style |= WS_VSCROLL | ES_AUTOVSCROLL | ES_MULTILINE;

    SetWindowLongPtrW(m_hEdit, GWL_STYLE, style);
    SetWindowPos(m_hEdit, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Target device: 0 tells RichEdit to wrap to the client width (or not wrap,
    // when ES_AUTOHSCROLL is set). A nonzero value is an explicit width in twips.
    LONG target = (wrap == kWrapRuler && rulerWidthTwips > 0) ? rulerWidthTwips : 0;
    SendMessageW(m_hEdit, EM_SETTARGETDEVICE, 0, static_cast<LPARAM>(target));
}

// ===========================================================================
//  Modification flag
//
//  RichEdit maintains its own modify flag and clears it on EM_SETMODIFY(FALSE).
//  Tracking dirtiness via EN_CHANGE instead would desynchronise the moment an
//  undo or redo reverted the document to its saved state -- our counter would
//  say dirty, RichEdit would say clean, and the caption would lie.
// ===========================================================================
bool EditView::IsModified() const
{
    if (!m_hEdit) return false;
    return SendMessageW(m_hEdit, EM_GETMODIFY, 0, 0) != 0;
}

void EditView::SetModified(bool f) const
{
    if (!m_hEdit) return;
    SendMessageW(m_hEdit, EM_SETMODIFY, f ? TRUE : FALSE, 0);
}

// ===========================================================================
//  Search
// ===========================================================================
bool EditView::FindText(PCWSTR needle, bool matchCase, bool wholeWord,
                        bool backwards, CHARRANGE& found) const
{
    found.cpMin = found.cpMax = 0;
    if (!m_hEdit || !needle || !needle[0])
        return false;

    CHARRANGE sel;
    GetSel(sel);

    FINDTEXTEXW ft = {};
    ft.lpstrText = needle;

    WPARAM flags = 0;
    if (matchCase)  flags |= FR_MATCHCASE;
    if (wholeWord)  flags |= FR_WHOLEWORD;

    if (backwards)
    {
        // No FR_DOWN. The search starts at cpMin and runs towards the
        // beginning of the document. Starting from cpMin rather than cpMax is
        // what makes repeated "find previous" walk backwards one match at a
        // time instead of rediscovering the same match forever.
        ft.chrg.cpMin = sel.cpMin;
        ft.chrg.cpMax = 0;
    }
    else
    {
        // Start after the current selection so that a repeated "find next"
        // advances. When the selection is empty cpMin == cpMax and this
        // searches from the insertion point, which is correct.
        flags |= FR_DOWN;
        ft.chrg.cpMin = sel.cpMax;
        ft.chrg.cpMax = -1;      // -1 = to end of document
    }

    LONG pos = static_cast<LONG>(SendMessageW(m_hEdit, EM_FINDTEXTEXW, flags,
                                              LParamFromPtr(&ft)));
    if (pos == -1)
        return false;

    // EM_FINDTEXTEXW fills ft.chrgText with the matched range, which for a
    // whole-word search may differ from the range we asked about.
    found = ft.chrgText;

    // Selecting the match is what scrolls it into view; EM_SCROLLCARET then
    // guarantees it is visible rather than merely selected off-screen.
    SetSel(found);
    SendMessageW(m_hEdit, EM_SCROLLCARET, 0, 0);
    return true;
}

bool EditView::ReplaceOne(PCWSTR find, PCWSTR replace, bool matchCase, bool wholeWord)
{
    if (!m_hEdit || !find || !find[0])
        return false;

    // "Replace" in a word processor replaces the CURRENT match, which is the
    // one already selected by the preceding Find. If the selection is not a
    // match for `find` -- because the user moved the caret between pressing
    // Find and pressing Replace -- fall back to finding the next one instead of
    // silently replacing whatever happens to be selected.
    CHARRANGE sel;
    GetSel(sel);

    bool selectionMatches = false;
    if (sel.cpMax > sel.cpMin)
    {
        LONG len = sel.cpMax - sel.cpMin;
        if (len == static_cast<LONG>(wcslen(find)))
        {
            // Compare the selected text against the needle, honouring the
            // dialog's case setting. EM_GETTEXTRANGE needs a writable buffer.
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            TEXTRANGEW tr = {};
            tr.chrg = sel;
            tr.lpstrText = &buf[0];
            if (SendMessageW(m_hEdit, EM_GETTEXTRANGE, 0, LParamFromPtr(&tr)) > 0)
            {
                selectionMatches = matchCase
                    ? (wcscmp(&buf[0], find) == 0)
                    : (_wcsicmp(&buf[0], find) == 0);
            }
        }
    }

    if (!selectionMatches)
    {
        CHARRANGE found;
        if (!FindText(find, matchCase, wholeWord, /*backwards=*/false, found))
            return false;
        GetSel(sel);
    }

    // Replace via EM_REPLACESEL rather than by selecting and sending WM_PASTE:
    // EM_REPLACESEL inserts as a single undoable step, so one Ctrl+Z reverses
    // the whole replacement. A select-then-paste sequence is two steps and the
    // user has to press undo twice to get their text back.
    SendMessageW(m_hEdit, EM_REPLACESEL, TRUE,
                 LParamFromPtr(replace ? replace : L""));
    return true;
}

int EditView::ReplaceAll(PCWSTR find, PCWSTR replace, bool matchCase, bool wholeWord)
{
    if (!m_hEdit || !find || !find[0])
        return 0;

    // Remember where the user was, and restore it afterwards. Leaving the caret
    // at the final replacement makes "replace all" look like it also scrolled
    // the document to the end, which is disorienting.
    CHARRANGE original;
    GetSel(original);

    int count = 0;
    LONG searchFrom = 0;

    // The loop re-reads the match range each iteration rather than advancing by
    // wcslen(find), because a replacement of a different length shifts every
    // subsequent character position. Advancing by a stale offset skips matches
    // or replaces the wrong text.
    for (;;)
    {
        FINDTEXTEXW ft = {};
        ft.lpstrText = find;
        ft.chrg.cpMin = searchFrom;
        ft.chrg.cpMax = -1;

        WPARAM flags = FR_DOWN;
        if (matchCase) flags |= FR_MATCHCASE;
        if (wholeWord) flags |= FR_WHOLEWORD;

        LONG pos = static_cast<LONG>(SendMessageW(m_hEdit, EM_FINDTEXTEXW, flags,
                                                  LParamFromPtr(&ft)));
        if (pos == -1)
            break;

        SendMessageW(m_hEdit, EM_EXSETSEL, 0, LParamFromPtr(&ft.chrgText));
        SendMessageW(m_hEdit, EM_REPLACESEL, TRUE,
                     LParamFromPtr(replace ? replace : L""));
        ++count;

        LONG replaceLen = replace ? static_cast<LONG>(wcslen(replace)) : 0;
        searchFrom = ft.chrgText.cpMin + replaceLen;

        // Guard against a pathological case: replacing "a" with "a" would never
        // advance and would loop forever on the first match.
        if (searchFrom <= pos)
            searchFrom = pos + 1;
    }

    if (count > 0)
    {
        SetSel(original);
        // One undo step for the whole operation is not achievable through
        // EM_REPLACESEL, so the modify flag is left set and undo will walk the
        // replacements back one at a time. Documented in ROADMAP.md as a known
        // divergence from the original.
    }
    return count;
}

// ===========================================================================
//  Exact text length
//
//  EM_GETTEXTLENGTHEX with GTL_PRECISE returns RichEdit's own character count.
//  WM_GETTEXTLENGTH is NOT equivalent: for a RichEdit control it can report the
//  length with CRLF line endings where the control stores a single CR, so the
//  number is larger than the range EM_FORMATRANGE paginates over. Using it as a
//  pagination bound lets a loop believe there is one more page than there is.
// ===========================================================================
LONG EditTextLengthExact(HWND hEdit)
{
    if (!hEdit) return 0;

    GETTEXTLENGTHEX gtl = {};
    gtl.flags    = GTL_NUMCHARS | GTL_PRECISE;
    gtl.codepage = 1200;      // UTF-16: count characters, not encoded bytes

    LRESULT lr = SendMessageW(hEdit, EM_GETTEXTLENGTHEX, 0, LParamFromPtr(&gtl));
    return (lr >= 0) ? static_cast<LONG>(lr) : 0;
}

// ===========================================================================
//  Undo / redo
// ===========================================================================
void EditView::Undo()
{
    if (!m_hEdit) return;
    // WM_UNDO is the standard message and RichEdit honours it. EM_UNDO is an
    // alias in some SDK headers; WM_UNDO is in <winuser.h> and always exists.
    SendMessageW(m_hEdit, WM_UNDO, 0, 0);
}

void EditView::Redo()
{
    if (!m_hEdit) return;
    SendMessageW(m_hEdit, EM_REDO, 0, 0);
}

// ===========================================================================
//  Clipboard
// ===========================================================================
bool EditView::CanPaste() const
{
    if (!m_hEdit) return false;
    // EM_CANPASTE with wParam 0 asks "can you paste anything currently on the
    // clipboard", which is the right question for enabling a Paste button.
    // Passing CF_TEXT or CF_UNICODETEXT asks a narrower one and reports FALSE
    // for a perfectly pasteable picture or OLE object.
    return SendMessageW(m_hEdit, EM_CANPASTE, 0, 0) != 0;
}

void EditView::Cut()  { if (m_hEdit) SendMessageW(m_hEdit, WM_CUT,   0, 0); }
void EditView::Copy() { if (m_hEdit) SendMessageW(m_hEdit, WM_COPY,  0, 0); }
void EditView::Paste(){ if (m_hEdit) SendMessageW(m_hEdit, WM_PASTE, 0, 0); }

void EditView::PasteSpecial()
{
    if (!m_hEdit) return;

    // OleUIPasteSpecial needs the IRichEditOle interface, which is obtained
    // through EM_GETOLEINTERFACE. That message returns an IUnknown that must be
    // QueryInterface'd for IRichEditOle and then Released -- it is NOT already
    // an IRichEditOle, despite the message name.
    ComPtr<IUnknown>      pUnk;
    ComPtr<IRichEditOle>  pOle;

    SendMessageW(m_hEdit, EM_GETOLEINTERFACE, 0, LParamFromPtr(pUnk.Adopt()));
    if (!pUnk) return;

    if (FAILED(pUnk->QueryInterface(IID_IRichEditOle,
                                    reinterpret_cast<void**>(pOle.Adopt()))))
        return;

    OLEUIPASTESPECIALW ps = {};
    ps.cbStruct    = sizeof(ps);
    // dwFlags left at 0: the flag constants for this dialog have moved between
    // oledlg.h revisions and 0 selects the default "show the list, let the user
    // pick a format and paste" behaviour, which is the only mode we want.
    ps.dwFlags     = 0;
    ps.hWndOwner   = GetParent(m_hEdit);      // the frame, not the edit control
    ps.lpSrcDataObj = nullptr;                // paste from the clipboard
    ps.reOle       = pOle.Get();              // RichEdit's OLE site

    // Field-name note: `reOle` is how <oledlg.h> spells the IRichEditOle member
    // of OLEUIPASTESPECIAL. If a future SDK renames it the compiler will point
    // at this line specifically, which is the reason it is set by name here
    // rather than folded into the aggregate initialiser above.

    if (OleUIPasteSpecialW(&ps) == IDOK)
    {
        // The dialog performs the insertion itself through reOle, so there is
        // nothing left to do except mark the document dirty -- RichEdit's own
        // modify flag is not updated by an OLE insertion performed this way.
        SetModified(true);
    }
}

// ===========================================================================
//  Geometry
// ===========================================================================
LONG EditView::GetTextWidthTwips() const
{
    if (!m_hEdit) return 6 * kTwipsPerInch;   // letter width minus 1" margins

    RECT rc;
    GetClientRect(m_hEdit, &rc);
    int cxPx = rc.right - rc.left;
    if (cxPx <= 0) return 6 * kTwipsPerInch;

    // Pixels to twips needs the device DPI. RichEdit already accounts for DPI
    // internally; the conversion here is only for EM_SETTARGETDEVICE, which
    // takes an absolute width in twips regardless of display scaling.
    HDC hdc = GetDC(m_hEdit);
    int dpiX = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(m_hEdit, hdc);
    if (dpiX <= 0) dpiX = 96;

    // The selection bar (the gutter RichEdit draws on the left when
    // ECO_SELECTIONBAR is set) consumes client width that is not available for
    // text. GetDC's client rect includes it; subtracting an approximation keeps
    // the wrap width honest. The real gutter width is not exposed by any
    // message, so this uses the same constant RichEdit does.
    const int kSelectionBarPx = MulDiv(8, dpiX, 96);

    cxPx -= kSelectionBarPx;
    if (cxPx < 16) cxPx = 16;

    return static_cast<LONG>(MulDiv(cxPx, kTwipsPerInch, dpiX));
}
