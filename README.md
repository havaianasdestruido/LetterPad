# LetterPad

An original, from-scratch rich-text word processor for Windows that recreates the
look and behaviour of the classic Windows 7 WordPad experience — Ribbon UI, RichEdit
4.1 editing surface, ruler, zoom, RTF round-tripping, and paginated printing.

LetterPad is **independent clean-source code**. It is *not* a decompilation, port,
translation, or derivative of Microsoft WordPad. No Microsoft binaries, disassembly,
debug symbols, or proprietary source were used. Everything here is written against
publicly documented Win32 / COM APIs:

| Capability | Documented API used |
|---|---|
| Editing surface | RichEdit 4.1 — `Msftedit.dll`, `RICHEDIT50W`, `EM_*` messages |
| Ribbon UI | `IUIFramework`, `IUIApplication`, `IUICommandHandler` (`Ribbon.dll`) |
| Rich text persistence | `EM_STREAMIN` / `EM_STREAMOUT` with `SF_RTF` |
| Printing | `PrintDlgEx`, `PageSetupDlg`, `EM_FORMATRANGE` pagination |
| Find / Replace | `FindText` / `ReplaceText` (`comdlg32`), `EM_FINDTEXTEXW` |
| Embedded objects | `IRichEditOle` (`EM_GETOLEINTERFACE`), `OleUIInsertObject` |
| Settings + MRU | Registry (`Software\LetterPad`) |

## Status

This is an active build-out. See [`docs/ROADMAP.md`](docs/ROADMAP.md) for what is
implemented, what is stubbed, and what is deliberately left for later.

- **Working:** window frame, Ribbon host with classic-toolbar fallback, RichEdit
  editing surface, character/paragraph formatting, RTF and plain-text open/save,
  find/replace, page setup, paginated printing, ruler, zoom, wrap modes, registry
  settings, MRU list, OLE picture/object insertion.
- **Stubbed with interfaces in place:** DOCX and OpenDocument codecs.
- **Not started:** WordPad-compatible search IFilter (`WordpadFilter.dll` equivalent).

## Building

LetterPad requires **Windows** with **Visual Studio Build Tools** and the
**Windows SDK**. It cannot be built on Linux or macOS — see
[`docs/BUILD.md`](docs/BUILD.md) for full instructions, including the UICC step
needed to compile the Ribbon markup.

```bat
cmake -B build -A x64
cmake --build build --config Release
```

## Layout

```
src/            C++ sources
src/ribbon/     Ribbon markup (UICC input) and generated command IDs
res/            RC script, icons, dialogs, version resource
docs/           Build, architecture and roadmap notes
```

## Why "LetterPad" and not "WordPad"

The application deliberately ships under its own name and its own iconography.
Recreating a *user interface layout* from public observation and documented APIs
is a normal engineering exercise; shipping someone else's name, icon, and branding
on top of it is not. LetterPad's ribbon layout, group ordering, and defaults mirror
WordPad's so the two feel interchangeable in the hand, but the identity is its own.

## Licence

All original code in this repository is released under the Unlicense — see
[`LICENSE`](LICENSE). That dedication covers work authored here and nothing else.

### A note on this repository's history

An earlier revision of this repository committed three proprietary Microsoft files
under `bin/` (`wordpad.exe`, `WordpadFilter.dll`, `pt-BR/wordpad.exe.mui`). Those
files are copyright Microsoft Corporation, are covered by the Windows EULA, and are
**not** public domain — attaching an Unlicense to a repository cannot dedicate
someone else's proprietary binary. They have been removed from the working
tree on this branch. They remain reachable in earlier commits on `main` until
that branch's history is rewritten (e.g. `git filter-repo --path bin
--invert-paths` followed by a force-push), which is a repository-admin
operation outside this branch's scope. If you forked or cloned before the
working-tree removal, delete your copy and re-clone.
