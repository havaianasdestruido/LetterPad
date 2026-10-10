// ============================================================================
//  main.cpp -- process entry, global initialisation, message loop
//
//  Startup order matters and is not arbitrary:
//
//    1. InitCommonControlsEx  -- before any toolbar/status/trackbar exists
//    2. OleInitialize         -- RichEdit's OLE support needs an STA and OLE
//                                initialised before the first RICHEDIT50W
//                                window is created. Doing this later produces
//                                an edit control that can be typed into but
//                                cannot embed objects, with no error reported.
//    3. LoadLibrary(msftedit) -- registers the RICHEDIT50W window class
//    4. Register our classes  -- depends on nothing, but must precede CreateWindow
//    5. Create the frame      -- which creates the ribbon, ruler, edit, status
//    6. Message loop          -- TranslateAccelerator before the frame proc
//
//  Shutdown unwinds in reverse. In particular OleUninitialize must not run
//  while a RichEdit control still exists, or its OLE object table is torn down
//  under it during WM_DESTROY.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

// ----------------------------------------------------------------------------
//  Global definitions
// ----------------------------------------------------------------------------
HINSTANCE g_hInst        = nullptr;
HACCEL    g_hAccel       = nullptr;
HMODULE   g_hModRichEdit = nullptr;
int       g_cFrames      = 0;

// ----------------------------------------------------------------------------
//  ShowFatalError
//
//  One message box helper for every unrecoverable path, so the caption, the
//  icon and the "which product is speaking" question are answered identically
//  everywhere. The HRESULT is appended when non-zero because for COM failures
//  the message text alone is almost never enough to diagnose.
// ----------------------------------------------------------------------------
void ShowFatalError(HWND hOwner, UINT idText, UINT idCaption, HRESULT hr)
{
    WCHAR text[512];
    WCHAR cap[128];
    LoadStr(idText, text, _countof(text));
    LoadStr(idCaption, cap, _countof(cap));

    if (FAILED(hr) && hr != E_FAIL)
    {
        WCHAR tail[64];
        StrPrintf(tail, _countof(tail), L"\n\n(0x%08X)", static_cast<unsigned>(hr));
        wcsncat_s(text, _countof(text), tail, _TRUNCATE);
    }

    MessageBoxW(hOwner, text, cap[0] ? cap : LP_APPNAME, MB_OK | MB_ICONERROR);
}

// ----------------------------------------------------------------------------
//  RegisterFrameClasses
//
//  CS_OWNDC is NOT set on the frame: a frame window is long-lived and holding
//  a DC for the process lifetime is a waste of the DC cache for no benefit.
//  The ruler does want a double-buffered paint, but it allocates a memory DC
//  per WM_PAINT instead, which is cheaper than an owned DC it would rarely use.
// ----------------------------------------------------------------------------
static bool RegisterFrameClasses()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = FrameWndProc;
    wc.cbWndExtra    = 0;
    wc.hInstance     = g_hInst;
    wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_LETTERPAD));
    wc.hIconSm       = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_LETTERPAD_SMALL));
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // White brush, not COLOR_WINDOW+1: the frame's visible background is the
    // page area around the edit control, and WordPad paints that white rather
    // than with the window colour. EraseBackground does the real work; the
    // class brush is only the fallback.
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = LP_FRAME_CLASS;
    if (!RegisterClassExW(&wc))
        return false;

    WNDCLASSEXW wr = {};
    wr.cbSize        = sizeof(wr);
    wr.style         = CS_HREDRAW;
    wr.lpfnWndProc   = RulerWndProc;
    wr.hInstance     = g_hInst;
    wr.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wr.hbrBackground = nullptr;              // we paint every pixel ourselves
    wr.lpszClassName = LP_RULER_CLASS;
    if (!RegisterClassExW(&wr))
        return false;

    return true;
}

// ----------------------------------------------------------------------------
//  ParseCommandLine
//
//  Supported forms, in the order they are checked:
//      LetterPad.exe                     new blank document
//      LetterPad.exe <path>              open <path>
//      LetterPad.exe /p <path>           print <path> and exit
//      LetterPad.exe /?                  usage message and exit
//
//  A bare path that happens to start with '/' is ambiguous and is treated as a
//  switch. That matches CommandLineToArgvW semantics and is not worth special
//  casing -- no realistic document path begins with a slash on Windows.
// ----------------------------------------------------------------------------
struct CommandLine
{
    WCHAR initialFile[MAX_PATH] = {};
    bool  hasFile       = false;
    bool  printAndExit  = false;
    bool  showUsage     = false;
};

// Parses into a caller-owned CommandLine whose path buffer is a fixed member.
// That shape is deliberate: CommandLineToArgvW returns pointers into a single
// LocalAlloc'd block, so a parser that returns `PCWSTR` either has to leak that
// block or hand back a dangling pointer the moment it frees it. Copying the one
// string we care about into our own buffer lets the block be freed right here,
// at the only place that knows it exists.
static CommandLine ParseCommandLine()
{
    CommandLine cl;

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return cl;

    for (int i = 1; i < argc; ++i)
    {
        PCWSTR a = argv[i];
        if (!a || !a[0])
            continue;

        if ((a[0] == L'/' || a[0] == L'-') && a[1] != L'\0')
        {
            // Case-insensitive whole-token match, so "/print" works as well as
            // "/p" and no partial-match table is needed.
            if (_wcsicmp(a + 1, L"p") == 0 || _wcsicmp(a + 1, L"print") == 0)
                cl.printAndExit = true;
            else if (_wcsicmp(a + 1, L"?") == 0 || _wcsicmp(a + 1, L"h") == 0 ||
                     _wcsicmp(a + 1, L"help") == 0)
                cl.showUsage = true;
            // Unknown switches are ignored rather than rejected. The shell
            // passes flags we do not implement (notably /embedded for the OLE
            // verb) and refusing to start would be worse than ignoring them.
        }
        else if (!cl.hasFile)
        {
            wcsncpy_s(cl.initialFile, _countof(cl.initialFile), a, _TRUNCATE);
            cl.hasFile = true;
        }
        // Extra positional arguments beyond the first are dropped. WordPad
        // opens one document per window; the shell launches a second process
        // for a second document.
    }

    LocalFree(argv);
    return cl;
}

static void ShowUsage()
{
    MessageBoxW(nullptr,
        L"LetterPad -- a rich-text word processor\n"
        L"\n"
        L"Usage:\n"
        L"  LetterPad.exe              start with a new blank document\n"
        L"  LetterPad.exe <file>       open <file>\n"
        L"  LetterPad.exe /p <file>    print <file> and exit\n"
        L"  LetterPad.exe /?           show this message\n",
        LP_APPNAME, MB_OK | MB_ICONINFORMATION);
}

// ----------------------------------------------------------------------------
//  wWinMain
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*lpCmdLine*/, int nCmdShow)
{
    g_hInst = hInstance;

    // ---- 1. common controls ---------------------------------------------
    // ICC_STANDARD_CLASSES covers status bar and toolbar. ICC_BAR_CLASSES is
    // redundant with it on modern comctl32 but harmless, and older SDKs needed
    // it spelled out for the rebar.
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    // ---- 2. OLE ----------------------------------------------------------
    // Must precede the first RichEdit creation. See the header comment.
    HRESULT hrOle = OleInitialize(nullptr);
    if (FAILED(hrOle))
    {
        ShowFatalError(nullptr, IDS_ERR_NORICHEDIT, IDS_ERR_NORICHEDIT_CAP, hrOle);
        return 1;
    }

    // ---- 3. RichEdit 4.1 -------------------------------------------------
    // Loaded dynamically rather than linked against an import library so that
    // a missing msftedit.dll is a handled error message instead of a loader
    // failure that kills the process before wWinMain runs. msftedit.dll has
    // shipped with every supported Windows since XP SP1, so in practice this
    // never fails -- but Windows 7 N/E editions and stripped images exist.
    g_hModRichEdit = LoadLibraryW(L"msftedit.dll");
    if (!g_hModRichEdit)
    {
        ShowFatalError(nullptr, IDS_ERR_NORICHEDIT, IDS_ERR_NORICHEDIT_CAP,
                       HrFromWin32(GetLastError()));
        OleUninitialize();
        return 1;
    }

    // ---- 4. accelerators and classes -------------------------------------
    g_hAccel = LoadAcceleratorsW(g_hInst, MAKEINTRESOURCEW(IDR_ACCEL));

    if (!RegisterFrameClasses())
    {
        ShowFatalError(nullptr, IDS_ERR_NORICHEDIT, IDS_ERR_NORICHEDIT_CAP,
                       HrFromWin32(GetLastError()));
        FreeLibrary(g_hModRichEdit);
        OleUninitialize();
        return 1;
    }

    // ---- 5. command line --------------------------------------------------
    CommandLine cl = ParseCommandLine();
    if (cl.showUsage)
    {
        ShowUsage();
        // Deliberately no FreeLibrary on g_hModRichEdit: the process is exiting
        // and unloading a module that registered window classes can race with
        // loader teardown. OleUninitialize IS called because it has real
        // cross-process effects (it can revoke OLE/clipboard registrations).
        OleUninitialize();
        return 0;
    }

    // ---- 6. frame ---------------------------------------------------------
    int exitCode = 0;
    {
        AppFrame frame;
        if (!frame.Create(cl.printAndExit ? SW_HIDE : nCmdShow,
                          cl.hasFile ? cl.initialFile : nullptr))
        {
            exitCode = 1;
        }
        else if (cl.printAndExit)
        {
            // /p prints the document we just opened and exits without entering
            // the message loop, which is what a shell print verb expects.
            if (cl.hasFile)
                PrintDocument(frame, /*quickPrint=*/true);

            // Destroy synchronously, then drain the queue. FileClose only POSTS
            // WM_CLOSE, and with no loop running after this branch the window
            // would never be destroyed: the RichEdit control would still exist
            // when OleUninitialize runs -- exactly the ordering this file's
            // header forbids -- and the settings save performed in WM_CLOSE
            // would never happen at all.
            DestroyWindow(frame.Hwnd());
            MSG quit;
            while (GetMessageW(&quit, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&quit);
                DispatchMessageW(&quit);
            }
        }
        else
        {
            // ---- 7. message loop ------------------------------------------
            // TranslateAccelerator is called against the frame only. Modal
            // dialogs run their own loops and own their own accelerators;
            // routing the common-dialog accelerators through our table would
            // steal keys from the Find/Replace dialog.
            MSG msg = {};
            BOOL fGot;
            while ((fGot = GetMessageW(&msg, nullptr, 0, 0)) != 0)
            {
                if (fGot == -1)
                {
                    // GetMessage failed rather than returning WM_QUIT. Looping
                    // here would spin at 100% CPU, so treat it as an exit.
                    exitCode = 1;
                    break;
                }
                // Modeless common dialogs (Find/Replace) need their keyboard
                // input routed through IsDialogMessage, or Tab and Enter do
                // nothing inside them. This must happen before accelerator
                // translation, because the dialog's own keys (Enter in the
                // "Find what" box) would otherwise be eaten by our table.
                HWND hFindDlg = FindReplace_DialogHwnd();
                if (hFindDlg && IsDialogMessageW(hFindDlg, &msg))
                    continue;

                if (!g_hAccel || !TranslateAcceleratorW(msg.hwnd, g_hAccel, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (fGot == 0)
                exitCode = static_cast<int>(msg.wParam);
        }
        // The frame destructor runs here. Its HWND is already gone by
        // WM_DESTROY, so nothing is left holding OLE references.
    }

    // ---- 8. shutdown ------------------------------------------------------
    // msftedit.dll is intentionally NOT FreeLibrary'd. The RICHEDIT50W window
    // class it registered is process-global, and unloading it during process
    // teardown has a long history of crashing on the way out. Every process
    // that hosts RichEdit leaves the module loaded.
    OleUninitialize();

    return exitCode;
}
