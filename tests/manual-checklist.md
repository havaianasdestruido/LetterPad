# Manual checklist

LetterPad's editing surface is RichEdit's and its chrome is the Windows Ribbon's,
so most verification is behavioural: run LetterPad and a real Windows 7 WordPad
side by side and compare. This list is ordered roughly by how likely each item
is to catch a real defect.

Add an entry whenever you pin down a behaviour. An entry with a date and a
"differs because" note is worth more than a passing check.

## Startup and window

- [ ] Launch with no arguments opens a blank document titled `Document - LetterPad`
- [ ] Launch with a `.rtf` path opens it
- [ ] Launch with a nonexistent path reports an open error and still opens blank
- [ ] Launch with `/p <file>` prints and exits without showing a window
- [ ] Launch with `/?` shows usage and exits 0
- [ ] Window restores its previous position and maximised state
- [ ] After unplugging a monitor, a window that was on it opens on-screen
- [ ] Minimum size is enforced; the edit area never collapses to zero height
- [ ] Ribbon minimises and restores; layout follows the height change
- [ ] Ctrl+wheel zooms; the status-bar percentage tracks it
- [ ] DPI change (move between monitors) relayouts without blur on Win10+

## Editing

- [ ] Typing uses the default font (Calibri 11pt on a fresh profile)
- [ ] Bold/Italic/Underline/Strike/Sub/Superscript toggle on selection and at
      the caret with no selection
- [ ] An underline followed by a font-colour change keeps the underline
- [ ] Font face and size from the ribbon font box apply to the selection
- [ ] Text colour and highlight colour apply and round-trip through RTF
- [ ] The four alignments work; the ruler's marker positions update
- [ ] Bullets add a hanging indent; removing bullets restores the text position
      but leaves any pre-existing manual indent alone
- [ ] Line spacing 1.0 / 1.5 / 2.0 visibly differ
- [ ] Undo and Redo each reverse exactly one step
- [ ] Cut/Copy/Paste and Paste Special behave on text and on an embedded picture

## Wrap modes

- [ ] No wrap shows a horizontal scrollbar and does not wrap long lines
- [ ] Wrap to window reflows on resize
- [ ] Wrap to ruler keeps a fixed measure regardless of window width
- [ ] Switching modes at runtime updates the scrollbars immediately
      (this is the `SWP_FRAMECHANGED` case; a regression here looks like the
      style changed but nothing visibly did)

## File I/O

- [ ] Save an RTF with a picture; reopen; the picture is present
- [ ] Save plain text; reopen in Notepad; content is correct
- [ ] Open a UTF-16LE `.txt` with a BOM; characters are correct
- [ ] Open a zero-byte `.rtf`; opens blank, no error
- [ ] Open a `.docx`; get the clear "unsupported format" message, not a crash
- [ ] Save As offers the current format preselected
- [ ] Saving a file that was read-only clears the attribute and succeeds
- [ ] Cancel out of Save As on a dirty document: window stays open, still dirty
- [ ] A crash mid-save never leaves a truncated document (rename is atomic)
- [ ] MRU lists the nine most recent, most recent first, deduplicated

## Find / Replace / Go To

- [ ] Find opens with the current selection pre-filled
- [ ] Find Next advances past the current match each time
- [ ] Find at the end reports completion in the status bar, no modal box
- [ ] Replace replaces the current match and advances
- [ ] Replace All counts matches in the status bar and leaves the caret put
- [ ] Match case and whole word both respected
- [ ] Go To page N selects the top of page N, within print-layout tolerance

## Print

- [ ] Page setup margins in the dialog match what prints
- [ ] Landscape prints landscape
- [ ] A multi-page document prints every page; the last page is not dropped
- [ ] An image taller than the printable area prints on its own page and the
      job terminates rather than hanging the spooler
- [ ] Preview shows the same page breaks as the printout
- [ ] Preview page-turn and zoom work; closing releases memory (task manager)

## Ruler

- [ ] Tick labels match the chosen unit (inches show 1, 2, 3; cm show 10, 20)
- [ ] Margin bands render at the page-setup margins
- [ ] Dragging the lower marker moves the whole paragraph
- [ ] Dragging the upper marker produces a hanging indent on the first line only
- [ ] Markers snap to 1/16 inch
- [ ] Hiding and showing the ruler relayouts the edit area

## Objects

- [ ] Insert a PNG; it appears inline at the caret
- [ ] The object is resizable by its handles
- [ ] Saving and reopening the RTF preserves the object
- [ ] Dragging a file from Explorer onto the document embeds it
- [ ] Dragging several files embeds exactly one, with no error

## Settings

- [ ] Options dialog Cancel discards everything, including the preview font
- [ ] Changing the default font affects a new document, not the open one
- [ ] Units change the ruler and the page-setup dialog together
- [ ] Settings survive a restart; a corrupt registry value falls back to
      defaults rather than failing to start
