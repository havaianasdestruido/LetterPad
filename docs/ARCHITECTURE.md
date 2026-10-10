# Architecture

LetterPad is a Win32/COM desktop application with no framework, no runtime
dependency beyond the Windows system DLLs, and no third-party code. This
document describes how it is put together and, more importantly, *why* -- most
of the structure exists to avoid a specific class of bug.

## The four windows

A frame (`LetterPadFrame`) hosts four children, laid out top to bottom by
`AppFrame::LayoutChildren`:

```
+--------------------------------------------------------------+
|  Ribbon            (IUIFramework; height owned by the frame- |
|                     work, queried via GetHeight on WM_SIZE)  |
+--------------------------------------------------------------+
|  Ruler             (LetterPadRuler, custom-drawn, optional)  |
+--------------------------------------------------------------+
|                                                              |
|  RichEdit 4.1      (RICHEDIT50W; the document)               |
|                                                              |
+--------------------------------------------------------------+
|  Status bar        (message | Ln/Col | [zoom trackbar] 100%) |
+--------------------------------------------------------------+
```

Two layout decisions are load-bearing:

- **The zoom trackbar is a child of the status bar, not a sibling.** A child
  sends `WM_HSCROLL` to its immediate parent, so a trackbar parented to the
  frame would never reach the status bar and vice versa; a trackbar sibling to
  the status bar gets overpainted every time the status bar invalidates. It is
  parented to the status bar, and a `SetWindowSubclass` hook on the status bar
  forwards `WM_HSCROLL` to the frame.

- **The ribbon's height is queried, never cached.** `IUIFramework::GetHeight`
  changes when the ribbon is minimised, when tabs collapse, and on DPI change.
  A cached value desynchronises the layout on the first minimise.

## The document model is RichEdit

There is no text buffer, no undo stack, no layout engine and no formatting model
in LetterPad. RichEdit 4.1 is all of those, and `EditView` (`src/editview.cpp`)
is a thin typed wrapper over the `EM_*` message set. This is the same
architecture WordPad uses, and it is why the codebase contains no RTF parser, no
pagination algorithm beyond the `EM_FORMATRANGE` loop, and no typography code.

The consequence is a pair of invariants that `editview.cpp` enforces at every
call site:

1. **`cbSize` is stamped on every structure.** RichEdit rejects a `CHARFORMAT2W`
   or `PARAFORMAT2` with a wrong `cbSize` silently -- no error code, no
   exception, the formatting simply does not change.
2. **`dwMask`/`pfMask` and the fields they name are always set together.** A mask
   bit without an initialised field writes stack garbage into the document; a
   field without a mask bit is a silent no-op.

## One command, one switch

Every user action arrives as an `ID_CMD_*` value in `AppFrame::OnCommand`. The
ribbon, the accelerator table, the fallback toolbar and the fallback menu all
produce the same values, so a feature is implemented exactly once. The ID space
is shared on purpose; see `src/resource.h` for the ranges and the reasoning.

## The ribbon's pull model

Nothing pushes state into the ribbon. `IUICommandHandler::UpdateProperty` is
called by the framework whenever it needs a value, and LetterPad answers it from
the live document. After anything changes, `AppFrame::UpdateRibbonState` calls
`InvalidateUI`, which tells the framework its cached answers are stale. Getting
this backwards produces a ribbon that renders the state the document had at
startup. See `src/ribbon.cpp` for the full contract, including the `VT_DECIMAL`
font-size conversion and the deliberate simplification of mixed-selection state.

## Persistence

Settings live under `HKCU\Software\LetterPad` as a mix of `REG_DWORD` values and
opaque `REG_BINARY` blobs, guarded by a format-version number. Blobs are used for
`CHARFORMAT2W`, `PARAFORMAT2` and the `DEVMODE`/`DEVNAMES` pair. The `DEVMODE`
case is the reason the scheme exists at all: its layout is printer-driver
specific and carries driver-private trailing bytes that must round-trip
untouched, so decomposing it into named values is not an option.

The MRU list is `File1`..`File9` under `...\Recent File List`, most recent first,
matching the shell's own MRU convention.

## File formats

`Document_Load`/`Document_Save` pump bytes through an `EDITSTREAM` callback; the
control does the serialisation. Format detection is a content sniff followed by
an extension fallback (`src/codec.cpp`). DOCX and ODT are declared in the
`FileFormat` enum and in the Save As filter, but `FormatIsImplemented` returns
false for them, so the UI reports "LetterPad cannot handle this format yet"
rather than a generic open failure.

## Printing

`EM_FORMATRANGE` is the whole pagination engine. `src/print.cpp` documents the
three failure modes that matter: the mandatory cache release, the no-progress
guard (an object taller than the printable area), and the `StartPage`/`EndPage`
bracketing that resets the viewport origin on every page.

## Ownership rules that the code depends on

- `PageSetup` owns two `HGLOBAL`s. `PageSetup_Copy` deep-copies; a shallow struct
  copy is a double-free waiting to happen.
- `ComPtr` AddRefs on copy and Releases on destroy. `Adopt()` takes an already-
  referenced pointer without AddRef; `Put()` releases first.
- `CommandLineToArgvW` returns pointers into one allocation. `ParseCommandLine`
  copies the one string it keeps and frees the block immediately, so no pointer
  can outlive it.
- The drop target is registered on the edit control's `HWND` and revoked in
  `WM_DESTROY`. `RegisterDragDrop` holds its own reference.
