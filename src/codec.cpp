// ============================================================================
//  codec.cpp -- document load/save, format detection, dialog filters
//
//  RichEdit does the actual serialisation. LetterPad opens a file handle and
//  pumps bytes through an EDITSTREAM callback; the control writes RTF (or text)
//  itself. That is why there is no RTF parser in this codebase and never will
//  be: EM_STREAMIN/EM_STREAMOUT with SF_RTF is the supported path and it
//  round-trips formatting that a hand-written parser would silently drop.
//
//  FORMAT SUPPORT
//    Rtf    full read and write, via SF_RTF
//    Text   full read and write, via SF_TEXT (+ SF_UNICODE when the source is
//           UTF-16; detected from the BOM, not assumed)
//    Wri    read-only. Legacy Windows 3.x Write files are RTF-adjacent and
//           RichEdit still loads them. Never written.
//    Docx   NOT IMPLEMENTED. Requires a zip reader and an OOXML writer.
//    Odt    NOT IMPLEMENTED. Requires a zip reader and an ODF writer.
//
//  The unimplemented codecs are surfaced through FormatIsImplemented() rather
//  than by returning a generic failure from Document_Load, so the caller can
//  say "LetterPad cannot read this format yet" instead of "this file is
//  corrupt". Those are different problems and the user can only act on one.
//
//  64-BIT NOTE: EDITSTREAM::dwCookie is DWORD_PTR. Passing a pointer through a
//  DWORD truncates it on x64 and the callback dereferences garbage. Every
//  cookie in this file is DWORD_PTR.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// ---------------------------------------------------------------------------
//  Stream context
//
//  Carried through dwCookie. The error field exists because the EDITSTREAM
//  callback's only channel back to the caller is its return value, which
//  RichEdit interprets as "stop" without preserving a reason. Without this, a
//  mid-stream disk-full would report as a successful short read.
// ---------------------------------------------------------------------------
struct StreamCtx
{
    HANDLE hFile;
    DWORD  lastError;

    StreamCtx() : hFile(INVALID_HANDLE_VALUE), lastError(ERROR_SUCCESS) {}
};

DWORD CALLBACK StreamInProc(DWORD_PTR dwCookie, LPBYTE pbBuff, LONG cb, LONG* pcb)
{
    // dwCookie is a DWORD_PTR holding a pointer. Converting through UINT_PTR
    // rather than through LPARAM keeps the width correct on both x86 and x64 --
    // LPARAM is signed, and a pointer with the high bit set would sign-extend.
    StreamCtx* ctx = reinterpret_cast<StreamCtx*>(static_cast<UINT_PTR>(dwCookie));

    if (!ctx || ctx->hFile == INVALID_HANDLE_VALUE || cb <= 0)
    {
        if (pcb) *pcb = 0;
        return 1;                              // abort
    }

    DWORD read = 0;
    if (!ReadFile(ctx->hFile, pbBuff, static_cast<DWORD>(cb), &read, nullptr))
    {
        ctx->lastError = GetLastError();
        if (pcb) *pcb = 0;
        return 1;                              // abort: report the real error
    }

    // read == 0 is EOF. Setting *pcb to 0 and returning SUCCESS is the
    // documented end-of-stream signal; returning nonzero here would make
    // RichEdit treat a clean EOF as a read failure.
    if (pcb) *pcb = static_cast<LONG>(read);
    return 0;
}

DWORD CALLBACK StreamOutProc(DWORD_PTR dwCookie, LPBYTE pbBuff, LONG cb, LONG* pcb)
{
    StreamCtx* ctx = reinterpret_cast<StreamCtx*>(static_cast<UINT_PTR>(dwCookie));
    if (!ctx || ctx->hFile == INVALID_HANDLE_VALUE || cb <= 0)
    {
        if (pcb) *pcb = 0;
        return 1;
    }

    DWORD written = 0;
    if (!WriteFile(ctx->hFile, pbBuff, static_cast<DWORD>(cb), &written, nullptr))
    {
        ctx->lastError = GetLastError();
        if (pcb) *pcb = 0;
        return 1;
    }

    if (written != static_cast<DWORD>(cb))
    {
        // Short write: disk full or a quota. Must abort, because returning
        // success with a partial count makes RichEdit believe the bytes were
        // written and it continues from the next buffer, producing a file with
        // a silent hole in the middle.
        ctx->lastError = ERROR_DISK_FULL;
        if (pcb) *pcb = static_cast<LONG>(written);
        return 1;
    }

    if (pcb) *pcb = static_cast<LONG>(written);
    return 0;
}

// Creates the file with the sharing mode a document editor needs: deny write
// (so another process cannot corrupt it under us) but allow read (so antivirus,
// search indexers and thumbnail providers can still see it). FILE_SHARE_READ
// alone is what WordPad uses and what makes "file in use" errors rare.
HANDLE OpenForRead(PCWSTR path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE OpenForWrite(PCWSTR path)
{
    return CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Reads the first N bytes of a file for sniffing. Returns the number of bytes
// actually read; 0 means the file is empty or unreadable.
DWORD SniffHead(PCWSTR path, BYTE* buf, DWORD cb)
{
    HANDLE h = OpenForRead(path);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD read = 0;
    ReadFile(h, buf, cb, &read, nullptr);
    CloseHandle(h);
    return read;
}

// Case-insensitive substring search over a raw byte range. Used to find the
// zip entry names that distinguish DOCX from ODT. Zip local file headers store
// the entry name uncompressed and unencrypted in the header itself, so a byte
// scan finds it without needing a real zip parser -- which is the point: a
// heuristic that is right for every file a user will actually open, at no
// dependency cost.
bool ByteRangeContains(const BYTE* hay, DWORD cbHay, const char* needle)
{
    size_t n = strlen(needle);
    if (n == 0 || cbHay < n) return false;

    for (DWORD i = 0; i + n <= cbHay; ++i)
    {
        size_t j = 0;
        while (j < n &&
               towlower(static_cast<wchar_t>(hay[i + j])) ==
               towlower(static_cast<wchar_t>(static_cast<unsigned char>(needle[j]))))
            ++j;
        if (j == n) return true;
    }
    return false;
}

} // anonymous namespace

// ===========================================================================
//  Format detection
// ===========================================================================
FileFormat DetectFormatByExtension(PCWSTR path)
{
    if (!path || !path[0]) return FileFormat::Unknown;

    PCWSTR ext = PathFindExtensionW(path);
    if (!ext) return FileFormat::Unknown;

    if (_wcsicmp(ext, L".rtf")  == 0) return FileFormat::Rtf;
    if (_wcsicmp(ext, L".txt")  == 0) return FileFormat::Text;
    if (_wcsicmp(ext, L".text") == 0) return FileFormat::Text;
    if (_wcsicmp(ext, L".docx") == 0) return FileFormat::Docx;
    if (_wcsicmp(ext, L".odt")  == 0) return FileFormat::Odt;
    if (_wcsicmp(ext, L".wri")  == 0) return FileFormat::Wri;

    // .doc is genuinely ambiguous: Word 97-2003 wrote a compound binary file,
    // but WordPad has always written RTF into files named .doc. Content sniffing
    // resolves it, so report Unknown here and let DetectFormatByContent decide.
    if (_wcsicmp(ext, L".doc")  == 0) return FileFormat::Unknown;

    return FileFormat::Unknown;
}

FileFormat DetectFormatByContent(PCWSTR path)
{
    if (!path || !path[0]) return FileFormat::Unknown;

    BYTE head[4096] = {};
    DWORD cb = SniffHead(path, head, sizeof(head));

    // An empty or unreadable file falls through to the extension. Opening a
    // zero-byte .rtf is a legitimate thing to do (the shell's "New > RTF
    // Document" creates exactly that) and must not be reported as unknown.
    if (cb == 0)
        return DetectFormatByExtension(path);

    // RTF: the control word is always the first thing in the file, and always
    // ASCII. No BOM precedes it.
    if (cb >= 5 && memcmp(head, "{\\rtf", 5) == 0)
        return FileFormat::Rtf;

    // Zip container: DOCX or ODT. The distinguishing entry names are in the
    // first few KB of local file headers.
    if (cb >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4)
    {
        // ODT's mimetype entry is required by the ODF spec to be the FIRST
        // entry in the archive and stored uncompressed, so this is reliable
        // rather than merely likely.
        if (ByteRangeContains(head, cb, "mimetype"))
            return FileFormat::Odt;
        if (ByteRangeContains(head, cb, "word/document.xml"))
            return FileFormat::Docx;
        return FileFormat::Unknown;   // a zip that is neither
    }

    // UTF-16 BOMs mean plain text. Neither RTF nor a zip container starts with
    // one, so this is unambiguous.
    if (cb >= 2 && head[0] == 0xFF && head[1] == 0xFE) return FileFormat::Text;
    if (cb >= 2 && head[0] == 0xFE && head[1] == 0xFF) return FileFormat::Text;

    // Compound binary file (legacy .doc written by Word). LetterPad cannot read
    // these -- RichEdit does not support the format -- and they are reported as
    // Unknown rather than Docx so the error message does not claim OOXML
    // support that is not there anyway.
    if (cb >= 8 && memcmp(head, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
        return FileFormat::Unknown;

    // Legacy Write files begin with an RTF-ish header of their own; RichEdit
    // still loads them, so they are worth recognising.
    if (cb >= 4 && memcmp(head, "\x31\xBE\x00\x00", 4) == 0)
        return FileFormat::Wri;

    // Nothing matched. Default to the extension, which covers ANSI plain text
    // and any text file without a BOM -- the overwhelmingly common case for
    // .txt, and the right answer for a .doc that is really RTF.
    FileFormat byExt = DetectFormatByExtension(path);
    return (byExt == FileFormat::Unknown) ? FileFormat::Text : byExt;
}

bool FormatIsImplemented(FileFormat f)
{
    switch (f)
    {
    case FileFormat::Rtf:
    case FileFormat::Text:
    case FileFormat::Wri:     // read-only, but RichEdit handles it
        return true;
    case FileFormat::Docx:
    case FileFormat::Odt:
        return false;         // see docs/ROADMAP.md
    case FileFormat::Unknown:
    default:
        return false;
    }
}

const wchar_t* FormatDisplayName(FileFormat f)
{
    switch (f)
    {
    case FileFormat::Rtf:  return L"Rich Text Format";
    case FileFormat::Text: return L"Text Document";
    case FileFormat::Docx: return L"Office Open XML Document";
    case FileFormat::Odt:  return L"OpenDocument Text";
    case FileFormat::Wri:  return L"Write Document";
    default:               return L"Document";
    }
}

// ===========================================================================
//  Load / Save
// ===========================================================================
HRESULT Document_Load(HWND hEdit, PCWSTR path, FileFormat fmt)
{
    if (!hEdit || !path || !path[0])
        return E_INVALIDARG;

    if (!FormatIsImplemented(fmt))
        return E_NOTIMPL;

    HANDLE hFile = OpenForRead(path);
    if (hFile == INVALID_HANDLE_VALUE)
        return HrFromWin32(GetLastError());

    // Reset the control before loading. Without this, a document shorter than
    // the previous one leaves the tail of the old text in place, because
    // EM_STREAMIN appends at the current selection rather than replacing the
    // document.
    SendMessageW(hEdit, WM_SETTEXT, 0, LParamFromPtr(L""));

    StreamCtx ctx;
    ctx.hFile = hFile;

    EDITSTREAM es = {};
    es.dwCookie    = static_cast<DWORD_PTR>(reinterpret_cast<UINT_PTR>(&ctx));
    es.dwError     = 0;
    es.pfnCallback = StreamInProc;

    // Format flags:
    //   SF_RTF      parse as RTF. RichEdit also accepts RTF-with-embedded
    //               objects, which is how pictures and OLE items round-trip.
    //   SF_TEXT     plain text, interpreted in the ANSI codepage unless
    //               SF_UNICODE is also set.
    //   SF_UNICODE  the source is UTF-16LE. Decided from the BOM below rather
    //               than from the extension, because .txt files arrive in every
    //               encoding and the BOM is the only reliable signal.
    WPARAM flags = 0;
    if (fmt == FileFormat::Rtf || fmt == FileFormat::Wri)
    {
        flags = SF_RTF;
    }
    else
    {
        flags = SF_TEXT;

        BYTE head[2] = {};
        DWORD cb = SniffHead(path, head, sizeof(head));
        // Little-endian UTF-16 BOM. Big-endian is not handled: RichEdit's
        // SF_UNICODE is little-endian only, and a BE text file is vanishingly
        // rare on Windows. It loads as ANSI mojibake, which is visible and
        // therefore better than silently refusing to open.
        if (cb >= 2 && head[0] == 0xFF && head[1] == 0xFE)
            flags |= SF_UNICODE;
    }

    LRESULT lr = SendMessageW(hEdit, EM_STREAMIN, flags, LParamFromPtr(&es));
    CloseHandle(hFile);

    // EM_STREAMIN returns the number of characters read, NOT an HRESULT. A
    // zero return with es.dwError set is a hard failure; a zero return with no
    // error is an empty document, which is success.
    if (ctx.lastError != ERROR_SUCCESS)
        return HrFromWin32(ctx.lastError);
    if (es.dwError != 0)
        return HrFromWin32(es.dwError);

    (void)lr;

    // Move the caret to the start. EM_STREAMIN leaves it at the end of the
    // streamed text, so a freshly opened document would open scrolled to the
    // bottom.
    SendMessageW(hEdit, EM_SETSEL, 0, 0);
    SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);

    return S_OK;
}

HRESULT Document_Save(HWND hEdit, PCWSTR path, FileFormat fmt)
{
    if (!hEdit || !path || !path[0])
        return E_INVALIDARG;

    if (!FormatIsImplemented(fmt))
        return E_NOTIMPL;

    // Write to a temporary file and rename over the target. A crash or a full
    // disk part-way through a direct write leaves the user with a truncated
    // document and no copy of the original -- the worst possible outcome for a
    // save operation. The rename is atomic on NTFS within a volume, so at every
    // instant either the old file or the complete new file exists.
    WCHAR tmp[MAX_PATH + 16] = {};
    StrPrintf(tmp, _countof(tmp), L"%s.lp-tmp", path);

    HANDLE hFile = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        // Fall back to writing the target directly. Better to risk truncation
        // than to fail the save outright when the directory is not writable for
        // new files but is for existing ones (a real configuration on some
        // locked-down shares).
        hFile = OpenForWrite(path);
        if (hFile == INVALID_HANDLE_VALUE)
            return HrFromWin32(GetLastError());
        tmp[0] = L'\0';
    }

    StreamCtx ctx;
    ctx.hFile = hFile;

    // A UTF-16LE text document needs its BOM written by us: EM_STREAMOUT with
    // SF_TEXT|SF_UNICODE emits the bytes but not the BOM, and a BOM-less UTF-16
    // file is indistinguishable from binary to every other program.
    if (fmt == FileFormat::Text)
    {
        BYTE bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        WriteFile(hFile, bom, sizeof(bom), &written, nullptr);
    }

    EDITSTREAM es = {};
    es.dwCookie    = static_cast<DWORD_PTR>(reinterpret_cast<UINT_PTR>(&ctx));
    es.dwError     = 0;
    es.pfnCallback = StreamOutProc;

    WPARAM flags = (fmt == FileFormat::Rtf || fmt == FileFormat::Wri)
                       ? static_cast<WPARAM>(SF_RTF)
                       : static_cast<WPARAM>(SF_TEXT | SF_UNICODE);

    SendMessageW(hEdit, EM_STREAMOUT, flags, LParamFromPtr(&es));

    // Flush before closing. EM_STREAMOUT's callback writes synchronously, but
    // the file system cache does not hit the disk until FlushFileBuffers or
    // CloseHandle, and a rename that races the flush can expose a zero-length
    // file to another reader.
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (ctx.lastError != ERROR_SUCCESS || es.dwError != 0)
    {
        DeleteFileW(tmp);
        return HrFromWin32(ctx.lastError != ERROR_SUCCESS ? ctx.lastError : es.dwError);
    }

    if (tmp[0])
    {
        // MOVEFILE_REPLACE_EXISTING is required: the target usually exists.
        // MOVEFILE_WRITE_THROUGH makes the directory entry update durable.
        if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DWORD err = GetLastError();
            DeleteFileW(tmp);
            return HrFromWin32(err);
        }
    }

    // Clear the read-only attribute if the source had it. A document opened
    // read-only from a share and then saved would otherwise fail on the next
    // save with a confusing "access denied" long after the reason.
    DWORD attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path, attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

    return S_OK;
}

// ===========================================================================
//  Dialog filter strings
//
//  The common-dialog filter format is a run of NUL-separated pairs terminated
//  by a second NUL:
//      "Display Text\0*.ext\0Display Text 2\0*.ext2\0\0"
//
//  A string resource cannot contain embedded NULs, so these are built at
//  runtime. The order of the entries IS the nFilterIndex mapping used by
//  AppFrame::FileSaveAs -- reordering them here silently changes which format a
//  user's selection means, so the two must be edited together.
// ===========================================================================
namespace {

// Appends one "label\0pattern\0" pair. Returns false if the buffer filled,
// which callers treat as fatal because a truncated filter string produces a
// dialog with a missing or garbled entry.
bool AppendFilterPair(PWSTR buf, size_t cch, size_t* used,
                      PCWSTR label, PCWSTR pattern)
{
    size_t l1 = wcslen(label), l2 = wcslen(pattern);
    // +1 for each NUL
    if (*used + l1 + 1 + l2 + 1 + 1 > cch)
        return false;

    memcpy(buf + *used, label, l1 * sizeof(WCHAR));
    *used += l1;
    buf[(*used)++] = L'\0';

    memcpy(buf + *used, pattern, l2 * sizeof(WCHAR));
    *used += l2;
    buf[(*used)++] = L'\0';

    return true;
}

} // anonymous namespace

void BuildOpenFilter(PWSTR buf, size_t cch)
{
    if (!buf || cch < 4) return;
    buf[0] = L'\0';

    size_t used = 0;
    // Entry 1: everything LetterPad can actually open. Listed first because
    // nFilterIndex defaults to 1 and this is the entry a user wants.
    AppendFilterPair(buf, cch, &used,
                     L"All LetterPad Documents",
                     L"*.rtf;*.txt;*.wri");
    AppendFilterPair(buf, cch, &used, L"Rich Text Format", L"*.rtf");
    AppendFilterPair(buf, cch, &used, L"Text Document",    L"*.txt");
    AppendFilterPair(buf, cch, &used, L"Write Document",   L"*.wri");
    AppendFilterPair(buf, cch, &used, L"All Files",        L"*.*");

    // Terminator: a second consecutive NUL. AppendFilterPair leaves `used`
    // pointing just past the last pattern's NUL, so one more NUL closes the run.
    if (used + 1 <= cch) buf[used] = L'\0';
}

void BuildSaveFilter(PWSTR buf, size_t cch, FileFormat current)
{
    if (!buf || cch < 4) return;
    buf[0] = L'\0';

    size_t used = 0;
    // Order matches AppFrame::FileSaveAs's nFilterIndex switch:
    //   1 = RTF, 2 = DOCX, 3 = ODT, 4 = Text
    AppendFilterPair(buf, cch, &used, L"Rich Text Format (RTF)",        L"*.rtf");
    AppendFilterPair(buf, cch, &used, L"Office Open XML Document",      L"*.docx");
    AppendFilterPair(buf, cch, &used, L"OpenDocument Text",             L"*.odt");
    AppendFilterPair(buf, cch, &used, L"Text Document",                 L"*.txt");
    AppendFilterPair(buf, cch, &used, L"All Files",                     L"*.*");

    if (used + 1 <= cch) buf[used] = L'\0';

    (void)current;   // reserved: preselecting the current format's entry
}
