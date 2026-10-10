# Roadmap

Honest status. "Implemented" means the code path exists and is expected to work
on a supported Windows with a supported SDK. "Stubbed" means the interface exists
and the user gets a clear message rather than a silent failure. "Not started"
means there is no code at all.

Nothing here is a promise about a date. The ordering reflects dependency, not
priority: later items are harder or need the earlier ones to be testable.

## Implemented

- Window frame, ribbon host, classic toolbar + menu fallback
- RichEdit 4.1 editing surface with selection bar, zoom, three wrap modes
- Character formatting: face, size, bold, italic, underline (several styles),
  strikethrough, sub/superscript, text colour, highlight colour
- Paragraph formatting: alignment (four), bullets, indentation, line spacing
  (single / 1.5 / double / exact)
- RTF and plain-text open and save, with BOM detection on load and a
  write-to-temp-then-rename save path
- Legacy `.wri` read-only
- Find / Replace (modeless common dialogs, wrap reporting in the status bar),
  Replace All, Go To Page (dry-run `EM_FORMATRANGE` pagination)
- Print with page setup, margin handling, and the `StartPage`/`EndPage` discipline
- Print preview (paginated `EM_FORMATRANGE` into a DIB, page turning, zoom)
- Ruler with unit-aware ticks, margin bands, and draggable indent markers
- OLE: picture and file-object embedding via `IRichEditOle`, file drag-and-drop
- Registry settings, MRU list, window placement restore with multi-monitor
  validation
- Keyboard accelerators for all common operations
- DPI: Per-Monitor V2 with a Windows 7 floor; `WM_DPICHANGED` relayout

## Stubbed (interface present, implementation intentionally absent)

- **DOCX codec.** Needs a ZIP reader and an OOXML writer. Detection already
  works; `FormatIsImplemented` returns false and the UI says so.
- **ODT codec.** Same shape as DOCX. ODT detection is reliable because the ODF
  spec requires `mimetype` to be the first, uncompressed entry.
- **"Create New" in Insert Object.** `OleUIInsertObject`'s structure layout could
  not be verified without the SDK headers in front of the author. The
  from-file path works; create-new does not exist.

## Not started

- **WordPad-compatible search IFilter** (the `WordpadFilter.dll` equivalent).
  An `IFilter` COM server registered for RTF so Windows Search can index
  LetterPad documents. Self-contained and additive; no existing code changes.
- **Ribbon iconography.** The markup declares no `LargeImages`/`SmallImages`, so
  the ribbon renders text-only buttons. This is visible immediately and is the
  single largest gap between LetterPad and the reference UI. It needs a 16x16
  and 32x32 image strip per command and the corresponding `Image` attributes.
- **Ribbon "Create New" object gallery and Paste Special gallery entries.**
- **Localisation.** The string table is the only localisation surface and it has
  one language. Ribbon labels live in `Ribbon.xml`, which UICC can localise via
  a separate resource file; that path is not wired.
- **DDE server / shell single-instance.** Opening a second document from
  Explorer currently launches a second process. WordPad chains to a running
  instance via DDE.
- **Hyperlink insertion UI.** Clicking an existing link works (`EN_LINK`);
  creating one does not.
- **Tables.** RichEdit 4.1 has no table support at all, so this is not a
  LetterPad limitation to fix but a control limitation to work around or accept.

## Known divergences from the reference behaviour

Recorded so that they are decisions rather than accidents. Each was chosen
deliberately; changing any of them is a small, contained edit.

1. **Replace All is not one undo step.** `EM_REPLACESEL` per replacement means
   Ctrl+Z walks them back individually. Making it atomic requires a custom undo
   unit, which RichEdit does not expose.
2. **Mixed selections render as "not set" on the ribbon font box.** RichEdit has
   no three-valued formatting query, and deriving the mixed state needs a second
   per-character pass on every `UpdateProperty`. The ribbon shows SET or NOTSET
   only.
3. **Print preview paginates from page 1 on every page turn.** `EM_FORMATRANGE`
   has no "start at page N" entry point. Caching the per-page character offsets
   would fix it; the cache is not implemented.
4. **Plain text saves as UTF-16LE with a BOM.** WordPad's default "Text Document"
   is ANSI. LetterPad chose a lossless encoding over byte-compatibility; a
   document containing a character outside the ANSI codepage would otherwise
   silently corrupt on save.
5. **`.doc` (Word 97-2003 compound binary) is not readable.** RichEdit does not
   support it. It is reported as an unsupported format rather than misdetected
   as RTF.

## Testing

There is no automated harness. `tests/manual-checklist.md` is the test suite;
it is written for side-by-side comparison against a real WordPad install, and
entries should be added whenever a behaviour is pinned down. An automated
subset is possible (round-trip RTF through `EM_STREAMOUT`/`EM_STREAMIN` and diff)
and is the most valuable first automation.
