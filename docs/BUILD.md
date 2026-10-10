# Building LetterPad

LetterPad is a Win32/COM desktop application. It must be built **on Windows** —
it links against `Msftedit.dll`, `Ribbon.dll`, `comdlg32`, `ole32` and
the RichEdit 4.1 headers, none of which exist outside the Windows SDK.

## Prerequisites

| Component | Notes |
|---|---|
| Windows 7 SP1 or later | Ribbon requires Windows 7; `Msftedit.dll` is present on all supported SKUs |
| Visual Studio 2022 Build Tools | "Desktop development with C++" workload. MSVC v143 or later |
| Windows SDK 10.0.19041 or later | Provides `richedit.h` (v5.00 structures), `UIRibbon.h`, `uicc.exe` |
| CMake 3.20+ | Bundled with the VS installer |

`uicc.exe` (the **U**ser **I**nterface **C**ommand **C**ompiler) ships with the
Windows SDK and lives at:

```
%ProgramFiles(x86)%\Windows Kits\10\bin\<version>\x64\uicc.exe
```

It is required to compile `src/ribbon/Ribbon.xml` into the binary markup the
Ribbon framework loads at runtime. CMake finds and invokes it automatically.

## Configure and build

```bat
cd LetterPad
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\LetterPad.exe`

For a 32-bit build (closer to the original Win7 WordPad's architecture):

```bat
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

## Opening in Visual Studio directly

```bat
cmake -B build -A x64
start build\LetterPad.sln
```

## The Ribbon build step

`src/ribbon/Ribbon.xml` is the single source of truth for the ribbon layout.
CMake runs:

```bat
uicc src\ribbon\Ribbon.xml build\generated\Ribbon.bml ^
     /header:build\generated\RibbonIDs_gen.h ^
     /res:build\generated\RibbonRes.rc /name:LETTERPAD_RIBBON
```

Every command in `Ribbon.xml` carries an **explicit** `id="..."` attribute. That
matters: UICC otherwise assigns IDs sequentially in document order, which makes
the generated header shift whenever a control is added anywhere above it. With
explicit IDs, `src/ribbon/RibbonIDs.h` (hand-written, committed) stays valid
forever and the generated header is only a cross-check.

CMake adds a post-build verification target that diffs the UICC-generated IDs
against the committed `RibbonIDs.h` and fails the build if they drift.

### If UICC is unavailable

Set `-DLETTERPAD_SKIP_RIBBON=ON`. The app then builds without the ribbon
resource and falls back to the classic toolbar + menu path at runtime
(`Frame::CreateFallbackToolbar`). This is a degraded but fully functional mode,
useful for smoke-testing the editing engine on a machine without the SDK bin path.

## Required system DLLs (at runtime, on the target machine)

All of these ship with Windows. **You do not need to redistribute or supply any
of them** — LetterPad links them dynamically and degrades gracefully when one is
absent:

```
msftedit.dll   RichEdit 4.1 control (RICHEDIT50W). Loaded via LoadLibrary so a
               failure is detectable rather than fatal at process start.
ribbon.dll     IUIFramework. Created via CoCreateInstance(CLSID_UIRibbonFramework);
               if this fails (Windows 7 N/E, or ribbon resource missing) the app
               falls back to the classic toolbar.
comdlg32.dll   Open/Save/Print/PageSetup/Find/Replace common dialogs.
ole32.dll      COM, OLE initialisation, IRichEditOle object embedding.
oledlg.dll     OleUIInsertObject / OleUIChangeSource dialogs.
comctl32.dll   Toolbar, tooltips, rebar (fallback UI + trackbar for zoom).
shell32.dll    Shell file operations, icon extraction, "open containing folder".
shlwapi.dll    Path helpers, StrFormatByteSize.
uxtheme.dll    Visual styles for the status bar and zoom trackbar.
advapi32.dll   Registry persistence for settings and the MRU list.
```

Hyperlink activation goes through `ShellExecute`, so no WinINet/URLMON dependency.
The ruler is drawn with plain GDI (`MoveToEx`/`LineTo`/`TextOut`), not GDI+, which
keeps the dependency list short and avoids a `GdiplusStartup` token lifetime that
is not safe across threads.

The only *build*-time dependency that is not a Windows system DLL is the Windows
SDK's import libraries, which the VS installer provides.

## Compiler and linker settings

CMake sets these explicitly; if you build by hand, replicate them, because several
are load-bearing rather than cosmetic:

```
_UNICODE, UNICODE            All sources are UTF-16; the ANSI variants are unused.
WIN32_LEAN_AND_MEAN          Faster compile; no MFC/WinSock pollution.
_RICHEDIT_VER=0x0500         REQUIRED. Unlocks CHARFORMAT2W/PARAFORMAT2W,
                             EM_SETZOOM, EM_SETTYPOGRAPHYOPTIONS, MSFTEDIT_CLASS.
                             Without it richedit.h gives you the v3 structures
                             and most of this app will not compile.
NTDDI_VERSION=0x06010000     Windows 7 floor.
/utf-8                       Sources are UTF-8; string literals must not be
                             reinterpreted through the system ANSI codepage.
/W4 /permissive-             Strict conformance; catches the classic Win32
                             signedness and pointer-truncation bugs on x64.
SUBSYSTEM:WINDOWS,5.01       (x86) — GUI subsystem, no console.
DPIAWARE                     Manifest declares Per-Monitor V2 with a GDI-scaling
                             fallback, see res/LetterPad.manifest.
```

### The `_RICHEDIT_VER` trap

This is the single most common reason a RichEdit 4.1 project fails to compile.
`richedit.h` gates the v5.00 declarations behind `_RICHEDIT_VER`. If you include
it without the define you will see errors on `CHARFORMAT2W`, `PARAFORMAT2W`,
`EM_SETZOOM` and `MSFTEDIT_CLASS` that look like missing-header problems but are
actually version gates. `src/stdafx.h` defines it before any SDK include; keep it
that way and never include `<richedit.h>` directly from a `.cpp`.

## Running

```bat
build\Release\LetterPad.exe                 # new blank document
build\Release\LetterPad.exe notes.rtf       # open a file
build\Release\LetterPad.exe /p notes.rtf    # print and exit
```

## Testing

There is no test harness yet — LetterPad is a GUI app and the editing surface is
RichEdit's, so most value comes from manual side-by-side comparison.
`tests/manual-checklist.md` lists the behaviours to verify against a real WordPad
install. Add entries as behaviours are pinned down.

## Known build notes

- **Windows 7 targeting:** `IUIFramework::LoadUI` returns
  `E_NOTIMPL`/`HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)` when the ribbon resource
  is missing or corrupt. The fallback path exists for exactly this; check
  `Frame::InitRibbon`'s return value rather than assuming success.
- **x64 pointer truncation:** every `LPARAM` payload in this codebase goes through
  the helpers in `src/util.h`. Never cast a pointer to `LONG`. `/W4` is on to
  enforce this.
- **No GDI+:** the ruler is plain GDI and `gdiplus` is not linked anywhere. If
  GDI+ is ever introduced, note that `GdiplusStartup`'s token is process-global
  and not documented as thread-safe, so ruler painting would have to stay on
  the UI thread.
