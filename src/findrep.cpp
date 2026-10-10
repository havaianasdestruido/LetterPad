// ============================================================================
//  findrep.cpp -- modeless Find / Replace dialogs, and Go To Page
//
//  WHY MODELESS
//  FindText/ReplaceText create a MODELESS dialog and report user actions by
//  posting a registered window message to hwndOwner. They do not run a modal
//  loop and they do not return a result. A word processor needs this: the user
//  must be able to click in the document while the Find dialog is open, and a
//  modal dialog would make the document unreachable.
//
//  The consequences are all handled here:
//    - The FINDREPLACEW structure and its string buffers must outlive the call
//      to FindText(). They are allocated once and never freed while a dialog is
//      open, because the dialog keeps pointers into them.
//    - The dialog reports completion via FR_DIALOGTERM, not by returning.
//      State is reset there and only there.
//    - Only one Find/Replace dialog can exist at a time. Creating a second
//      would leave the first orphaned with nowhere to post its messages. The
//      code below activates the existing dialog instead of creating another,
//      which is also what WordPad does.
//
//  THE REGISTERED MESSAGE
//  FINDMSGSTRING and REPLACEMSGSTRING are not constants -- they are names.
//  RegisterWindowMessage returns a value in the 0xC000-0xFFFF range that is
//  unique per session, so the message number must be looked up at runtime and
//  compared, never hard-coded. Both Find and Replace use the SAME message
//  number; the FR_REPLACE / FR_REPLACEALL flags in the structure say which
//  action the user chose.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  Dialog state. One instance for the whole process, for the reasons above.
// ---------------------------------------------------------------------------
struct FindState
{
    FINDREPLACEW fr = {};
    WCHAR find[256]    = {};
    WCHAR replace[256] = {};
    bool  open         = false;
    HWND  hDlg         = nullptr;
    AppFrame* frame    = nullptr;
    UINT  msgFind      = 0;      // == msgReplace; registered once, compared once
};

FindState g_find;

// Resolved lazily. RegisterWindowMessage with the same name always returns the
// same value within a session, so this is idempotent and cheap.
UINT EnsureRegisteredMessage()
{
    if (g_find.msgFind == 0)
        g_find.msgFind = RegisterWindowMessageW(FINDMSGSTRINGW);
    return g_find.msgFind;
}

// Brings an already-open dialog to the front instead of creating a second one.
void ActivateExistingDialog()
{
    if (!g_find.hDlg) return;
    if (IsIconic(g_find.hDlg))
        ShowWindow(g_find.hDlg, SW_RESTORE);
    SetForegroundWindow(g_find.hDlg);
}

// Seeds the Find string from the current selection. Opening Find with the
// selected text already in the box is what makes "find the next occurrence of
// this" a two-keystroke operation, and it is the behaviour users expect.
void SeedFromSelection(AppFrame& frame)
{
    // Do NOT clear g_find.find here. The buffer is only overwritten once the
    // selection has passed every check below, so an empty selection, an
    // over-long one, or a multi-line one preserves the previous needle -- which
    // is what "open Find again with just a caret" must mean. Clearing first
    // would silently discard the user's last search.
    if (!frame.View()) return;

    CHARRANGE sel;
    frame.View().GetSel(sel);
    LONG len = sel.cpMax - sel.cpMin;

    // Bounded: a 200 KB selection must not be copied into a 256-char buffer.
    // Truncating silently is acceptable for a search seed; overflowing is not.
    if (len <= 0 || len >= static_cast<LONG>(_countof(g_find.find)))
        return;

    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    TEXTRANGEW tr = {};
    tr.chrg = sel;
    tr.lpstrText = &buf[0];

    if (SendMessageW(frame.View().Hwnd(), EM_GETTEXTRANGE, 0, LParamFromPtr(&tr)) > 0)
    {
        // Reject multi-line selections. Searching for a string containing a
        // paragraph break is a legitimate thing to want, but the common dialog
        // is single-line and would show a truncated, misleading needle.
        if (!wcschr(buf.c_str(), L'\r') && !wcschr(buf.c_str(), L'\n'))
            wcsncpy_s(g_find.find, _countof(g_find.find), buf.c_str(), _TRUNCATE);
    }
}

// Performs one action from the dialog's flags.
void PerformAction()
{
    AppFrame* frame = g_find.frame;
    if (!frame || !frame->View()) return;

    DWORD flags = g_find.fr.Flags;
    bool matchCase = (flags & FR_MATCHCASE) != 0;
    bool wholeWord = (flags & FR_WHOLEWORD) != 0;
    bool down      = (flags & FR_DOWN) != 0;

    if (flags & FR_REPLACEALL)
    {
        int n = frame->View().ReplaceAll(g_find.find, g_find.replace,
                                         matchCase, wholeWord);
        WCHAR msg[128];
        StrPrintf(msg, _countof(msg),
                  n > 0 ? L"Replaced %d occurrence(s)."
                        : L"No occurrences found to replace.", n);
        frame->SetStatusMessage(msg);
        frame->UpdateCaption();
        frame->UpdateRibbonState();
        return;
    }

    if (flags & FR_REPLACE)
    {
        // "Replace" replaces the current match and then advances, which is why
        // it is a ReplaceOne followed by a Find rather than a single call.
        frame->View().ReplaceOne(g_find.find, g_find.replace, matchCase, wholeWord);
        frame->UpdateCaption();
        // fall through to FR_FINDNEXT: WordPad advances after replacing
    }

    // The common dialog sets exactly ONE action flag per notification, so a
    // Replace click arrives with FR_REPLACE and no FR_FINDNEXT. Advancing after
    // a replace is nonetheless the expected behaviour (it is what WordPad does
    // and what the dialog's own layout implies), so the search block runs for
    // both flags. Without FR_REPLACE here the user would have to click Find Next
    // between every Replace.
    if (flags & (FR_FINDNEXT | FR_REPLACE))
    {
        CHARRANGE found;
        if (frame->View().FindText(g_find.find, matchCase, wholeWord,
                                   /*backwards=*/!down, found))
        {
            frame->SetStatusMessage(L"");
            frame->UpdateStatusBar();
            frame->UpdateRibbonState();
        }
        else
        {
            // Search wrapped and found nothing. Reporting this in the status
            // bar rather than a message box keeps the dialog usable: a modal
            // "not found" box on every failed Find would make searching for
            // something near the end of a document miserable.
            WCHAR msg[192];
            if (LoadStr(IDS_ERR_FINDWRAP, msg, _countof(msg)) <= 0)
                wcsncpy_s(msg, _countof(msg),
                          L"LetterPad has finished searching the document.", _TRUNCATE);
            frame->SetStatusMessage(msg);
        }
    }
}

} // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================

UINT FindReplace_RegisteredMessage() { return g_find.msgFind; }

HWND FindReplace_DialogHwnd() { return g_find.hDlg; }

// Called from FrameWndProc's default branch. Returns true when the message was
// the Find/Replace notification and has been handled.
bool FindReplace_HandleMessage(AppFrame& frame, UINT msg, WPARAM wp, LPARAM lp)
{
    UINT reg = EnsureRegisteredMessage();
    if (reg == 0 || msg != reg)
        return false;

    // lParam is a pointer to the SAME FINDREPLACEW we passed in. Trusting it
    // rather than our own copy is correct and also how the dialog tells us
    // which of possibly several structures this notification refers to.
    FINDREPLACEW* pfr = PtrFromLParam<FINDREPLACEW>(lp);
    if (!pfr) return true;

    g_find.frame = &frame;

    // FR_DIALOGTERM is sent when the dialog is closing. This is the ONLY
    // reliable completion signal: FindText returns immediately and the dialog
    // lives on, so there is no return value to check.
    if (pfr->Flags & FR_DIALOGTERM)
    {
        g_find.open  = false;
        g_find.hDlg  = nullptr;
        g_find.frame = nullptr;
        return true;
    }

    if (pfr->Flags & (FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL))
        PerformAction();

    return true;
}

bool ShowFindDialog(AppFrame& frame, bool replace)
{
    UINT reg = EnsureRegisteredMessage();
    if (reg == 0)
        return false;

    // One dialog at a time. If one is already open, bring it forward. Switching
    // between Find and Replace with one already open is not attempted: the two
    // are different dialogs and swapping means destroying and recreating, which
    // loses the user's typed text and the dialog's position.
    if (g_find.open && g_find.hDlg && IsWindow(g_find.hDlg))
    {
        ActivateExistingDialog();
        return true;
    }

    g_find.frame = &frame;

    ZeroMemory(&g_find.fr, sizeof(g_find.fr));
    g_find.fr.lStructSize  = sizeof(FINDREPLACEW);
    // hwndOwner receives the registered message, so it must be the frame --
    // FrameWndProc is where FindReplace_HandleMessage is called from.
    g_find.fr.hwndOwner    = frame.Hwnd();
    g_find.fr.hInstance    = g_hInst;
    g_find.fr.lpstrFindWhat    = g_find.find;
    g_find.fr.lpstrReplaceWith = g_find.replace;
    g_find.fr.wFindWhatLen     = static_cast<WORD>(_countof(g_find.find) - 1);
    g_find.fr.wReplaceWithLen  = static_cast<WORD>(_countof(g_find.replace) - 1);

    // FR_DOWN: search forwards by default. FR_HIDEUPDOWN is NOT set, so the
    // user can choose direction. The other FR_HIDE* flags are left unset for
    // the same reason.
    g_find.fr.Flags = FR_DOWN;

    // Preserve the previous search terms across invocations. g_find.find is
    // only cleared by SeedFromSelection, and that only overwrites it when there
    // is a usable selection -- so an empty selection keeps the last search.
    SeedFromSelection(frame);

    HWND hDlg = replace ? ReplaceTextW(&g_find.fr) : FindTextW(&g_find.fr);
    if (!hDlg)
    {
        // A null return means the dialog could not be created. CommDlgExtendedError
        // gives the reason; CDERR_FINDRESFAILURE / CDERR_INITIALIZATION are the
        // realistic ones and both indicate a resource problem.
        DWORD err = CommDlgExtendedError();
        WCHAR msg[128];
        StrPrintf(msg, _countof(msg), L"Could not open the %s dialog (error %lu).",
                  replace ? L"Replace" : L"Find", err);
        frame.SetStatusMessage(msg);
        return false;
    }

    g_find.open = true;
    g_find.hDlg = hDlg;
    return true;
}

// ===========================================================================
//  Go To Page
//
//  RichEdit has no page concept while editing -- pagination only exists at
//  print time, when EM_FORMATRANGE walks the document. So "go to page N" has
//  to be implemented by paginating against the current printer metrics and
//  finding the character offset where page N begins.
//
//  That is expensive and only approximate (the on-screen layout and the print
//  layout differ), but it is the same trade-off the original makes. Doing it
//  properly means a dry-run EM_FORMATRANGE pass with wParam == FALSE, which
//  measures without drawing.
// ===========================================================================
bool ShowGoToPage(AppFrame& frame)
{
    if (!frame.View()) return false;

    // The dialog is a tiny custom one (IDD_GOTO), not a common dialog, because
    // there is no standard "go to page" UI in comdlg32.
    struct GoToCtx { AppFrame* frame; int page; bool ok; };
    GoToCtx ctx = { &frame, 1, false };

    struct Dlg
    {
        static INT_PTR CALLBACK Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
        {
            GoToCtx* c = PtrFromLParam<GoToCtx>(GetWindowLongPtrW(hDlg, DWLP_USER));

            switch (msg)
            {
            case WM_INITDIALOG:
            {
                c = PtrFromLParam<GoToCtx>(lp);
                SetWindowLongPtrW(hDlg, DWLP_USER, LParamFromPtr(c));
                // Seed with 1 rather than the current page: the control has no
                // notion of a current page, so there is nothing better to show.
                SetDlgItemInt(hDlg, IDC_GOTO_PAGE, 1, FALSE);
                return TRUE;
            }
            case WM_COMMAND:
                switch (LOWORD(wp))
                {
                case IDOK:
                    if (c)
                    {
                        BOOL translated = FALSE;
                        UINT v = GetDlgItemInt(hDlg, IDC_GOTO_PAGE, &translated, FALSE);
                        c->ok = translated && v >= 1;
                        c->page = static_cast<int>(v);
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
                break;
            }
            return FALSE;
        }
    };

    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_GOTO), frame.Hwnd(),
                        Dlg::Proc, LParamFromPtr(&ctx)) != IDOK || !ctx.ok)
        return false;

    // Dry-run pagination. EM_FORMATRANGE with wParam == FALSE measures the text
    // that would fit in the rectangle without drawing any of it, and returns the
    // character offset where the next page starts -- exactly the information
    // needed to map a page number to a character position.
    //
    // This uses the screen DC as the target so the page numbers correspond to
    // what the user sees, not to a printer they may never use.
    HDC hdc = GetDC(frame.View().Hwnd());
    if (!hdc) return false;

    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    if (dpiX <= 0) dpiX = 96;
    if (dpiY <= 0) dpiY = 96;

    // Page geometry comes from the frame's current page setup, converted from
    // twips to pixels for this DC. Falling back to US Letter (8.5 x 11) when no
    // DEVMODE has been chosen yet, which is the state of a fresh document.
    LONG pageTwipsW = 8 * kTwipsPerInch + kTwipsPerInch / 2;
    LONG pageTwipsH = 11 * kTwipsPerInch;
    PageGeometryTwips(frame, &pageTwipsW, &pageTwipsH);

    // FORMATRANGE rectangles are in TWIPS, and the body rectangle is the page
    // minus the page-setup margins. Passing device pixels here would shrink the
    // page roughly fifteenfold and report absurd page counts; ignoring the
    // margins would make the on-screen page breaks disagree with the printed
    // ones, which is the whole point of Go To.
    const PageSetup& ps = frame.Settings().page;
    RECT rcPage = { 0, 0, pageTwipsW, pageTwipsH };
    RECT rcBody;
    rcBody.left   = ps.marginLeft;
    rcBody.top    = ps.marginTop;
    rcBody.right  = pageTwipsW - ps.marginRight;
    rcBody.bottom = pageTwipsH - ps.marginBottom;
    if (rcBody.right  <= rcBody.left)  rcBody.right  = rcBody.left + 1;
    if (rcBody.bottom <= rcBody.top)   rcBody.bottom = rcBody.top + 1;

    // Real character count, for two reasons: -1 would make the loop guard
    // (0 < -1) false so no page after the first could ever be reached, and the
    // end-of-text comparison below needs the true end to know when the last
    // page has been measured.
    LONG textLen = EditTextLengthExact(frame.View().Hwnd());

    FORMATRANGE fr = {};
    fr.hdc        = hdc;
    fr.hdcTarget  = hdc;
    fr.rc         = rcBody;
    fr.rcPage     = rcPage;
    fr.chrg.cpMin = 0;
    fr.chrg.cpMax = textLen;

    // Walk pages until the requested one begins or the document runs out.
    int  page = 1;
    LONG targetStart = 0;
    bool found = (ctx.page == 1);

    while (!found && fr.chrg.cpMin < fr.chrg.cpMax)
    {
        LONG next = static_cast<LONG>(SendMessageW(frame.View().Hwnd(), EM_FORMATRANGE,
                                                   FALSE, LParamFromPtr(&fr)));
        if (next <= fr.chrg.cpMin)
            break;                  // no progress; stop rather than loop forever

        bool wasLast = (next >= textLen);
        fr.chrg.cpMin = next;
        ++page;

        // EM_FORMATRANGE returns the text length for the final page, so `next`
        // reaching the end means the page just measured was the last one and no
        // page `page` exists. Checking that BEFORE accepting page == ctx.page is
        // what stops a one-page document from "successfully" going to page 2.
        if (wasLast)
            break;

        if (page == ctx.page)
        {
            found = true;
            targetStart = next;
        }
    }

    // wParam FALSE means nothing was drawn, but RichEdit still caches the
    // measurement. EM_FORMATRANGE with a null lParam releases that cache; not
    // doing so leaks it until the control is destroyed.
    SendMessageW(frame.View().Hwnd(), EM_FORMATRANGE, FALSE, 0);
    ReleaseDC(frame.View().Hwnd(), hdc);

    if (!found)
    {
        int totalPages = page - 1;
        if (totalPages < 1) totalPages = 1;
        WCHAR msg[128];
        StrPrintf(msg, _countof(msg),
                  L"The document has only %d page(s).", totalPages);
        frame.SetStatusMessage(msg);
        return false;
    }

    CHARRANGE target = { targetStart, targetStart };
    frame.View().SetSel(target);
    SendMessageW(frame.View().Hwnd(), EM_SCROLLCARET, 0, 0);
    SetFocus(frame.View().Hwnd());
    frame.UpdateStatusBar();
    return true;
}
