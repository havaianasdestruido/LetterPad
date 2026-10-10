// ============================================================================
//  settings.cpp -- registry persistence for AppSettings, PageSetup and the MRU
//
//  Everything lives under HKCU\Software\LetterPad. Structures are stored as
//  opaque REG_BINARY blobs guarded by a format-version DWORD.
//
//  Why blobs rather than one registry value per field:
//    - DEVMODE cannot be decomposed. Its layout is printer-driver-specific and
//      it carries driver-private extra bytes at the end. Splitting it into
//      named fields would lose those bytes and silently change the printer.
//      Persisting it whole is what WordPad does and the only correct option.
//    - CHARFORMAT2W / PARAFORMAT2 have ~20 fields each, most of which are only
//      meaningful together with a dwMask/pfMask bit. Writing them as individual
//      values means maintaining a mask-to-value correspondence in two places.
//
//  The cost is that a blob is opaque to anyone editing the registry by hand,
//  and that a struct layout change invalidates saved settings. The version
//  DWORD handles the second problem: on mismatch, defaults are used and the key
//  is rewritten on next save. Nothing here is security-relevant, so "fall back
//  to defaults" is always the right recovery.
//
//  All reads are best-effort and all writes are best-effort. A read failure
//  yields a default; a write failure is ignored. A word processor that refuses
//  to start because HKCU is unavailable would be worse than one that forgets
//  your window position.
// ============================================================================
#include "stdafx.h"
#include "LetterPad.h"

namespace {

// Bumped whenever the binary layout of a persisted struct changes.
constexpr DWORD kSettingsVersion = 1;

// ---------------------------------------------------------------------------
//  Registry primitives
//
//  Each returns false and leaves the output untouched on any failure, so a
//  missing value is indistinguishable from a corrupt one. That is the
//  behaviour we want: both mean "use the default".
// ---------------------------------------------------------------------------
bool ReadDword(HKEY hKey, PCWSTR name, DWORD* out)
{
    DWORD type = 0, cb = sizeof(DWORD), value = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &cb) != ERROR_SUCCESS)
        return false;
    if (type != REG_DWORD)
        return false;
    *out = value;
    return true;
}

bool WriteDword(HKEY hKey, PCWSTR name, DWORD value)
{
    return RegSetValueExW(hKey, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value),
                          sizeof(value)) == ERROR_SUCCESS;
}

// Reads a REG_BINARY blob into a fixed-size struct. Rejects size mismatches:
// a blob larger than the struct would overflow it, and a smaller one means the
// struct was only partially written, which is worse than not written at all
// because the unread tail keeps whatever garbage was on the stack.
template <class T>
bool ReadBinaryStruct(HKEY hKey, PCWSTR name, T* out)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
        return false;
    if (type != REG_BINARY || cb != sizeof(T))
        return false;

    T tmp;                       // stage in a local so *out is untouched on failure
    if (RegQueryValueExW(hKey, name, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&tmp), &cb) != ERROR_SUCCESS)
        return false;

    *out = tmp;
    return true;
}

template <class T>
bool WriteBinaryStruct(HKEY hKey, PCWSTR name, const T& value)
{
    return RegSetValueExW(hKey, name, 0, REG_BINARY,
                          reinterpret_cast<const BYTE*>(&value),
                          sizeof(value)) == ERROR_SUCCESS;
}

// Reads a REG_BINARY blob into a fresh HGLOBAL. Caller owns the result and must
// GlobalFree it. Returns null on any failure, including a zero-length blob.
HGLOBAL ReadBinaryToHGlobal(HKEY hKey, PCWSTR name)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
        return nullptr;
    if (type != REG_BINARY || cb == 0)
        return nullptr;

    // GHND zero-initialises, which matters because a DEVMODE's driver-extra
    // region is read by the spooler and must not contain stack garbage if the
    // stored blob happens to be shorter than the allocation.
    HGLOBAL h = GlobalAlloc(GHND, cb);
    if (!h)
        return nullptr;

    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return nullptr; }

    DWORD read = cb;
    DWORD err = RegQueryValueExW(hKey, name, nullptr, nullptr,
                                 static_cast<BYTE*>(p), &read);
    GlobalUnlock(h);

    if (err != ERROR_SUCCESS || read != cb)
    {
        GlobalFree(h);
        return nullptr;
    }
    return h;
}

bool WriteHGlobalToBinary(HKEY hKey, PCWSTR name, HGLOBAL h)
{
    if (!h) return false;
    SIZE_T cb = GlobalSize(h);
    if (cb == 0) return false;

    const void* p = GlobalLock(h);
    if (!p) return false;

    DWORD err = RegSetValueExW(hKey, name, 0, REG_BINARY,
                               static_cast<const BYTE*>(p), static_cast<DWORD>(cb));
    GlobalUnlock(h);
    return err == ERROR_SUCCESS;
}

// Opens HKCU\Software\LetterPad, creating it if necessary.
HKEY OpenSettingsKey(bool fWrite)
{
    HKEY hKey = nullptr;
    DWORD disp = 0;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, LP_REGISTRY_KEY, 0, nullptr,
                             REG_OPTION_NON_VOLATILE,
                             fWrite ? (KEY_READ | KEY_WRITE) : KEY_READ,
                             nullptr, &hKey, &disp);
    if (r != ERROR_SUCCESS)
        return nullptr;
    return hKey;
}

// ---------------------------------------------------------------------------
//  PageSetup persistence
//
//  Margins are stored as four DWORDs (readable, and editable by hand) while the
//  DEVMODE/DEVNAMES pair is stored as blobs (not decomposable, see header).
//  Splitting it this way is a small inconsistency in an otherwise all-blob
//  scheme, and it is worth it: margins are the one part a user might plausibly
//  want to fix from regedit when the dialog will not open.
// ---------------------------------------------------------------------------
void SavePageSetup(HKEY hKey, const PageSetup& ps)
{
    WriteDword(hKey, L"MarginLeft",   static_cast<DWORD>(ps.marginLeft));
    WriteDword(hKey, L"MarginTop",    static_cast<DWORD>(ps.marginTop));
    WriteDword(hKey, L"MarginRight",  static_cast<DWORD>(ps.marginRight));
    WriteDword(hKey, L"MarginBottom", static_cast<DWORD>(ps.marginBottom));

    WriteHGlobalToBinary(hKey, L"DevMode",  ps.hDevMode);
    WriteHGlobalToBinary(hKey, L"DevNames", ps.hDevNames);
}

void LoadPageSetup(HKEY hKey, PageSetup* ps)
{
    DWORD v = 0;
    if (ReadDword(hKey, L"MarginLeft", &v))   ps->marginLeft   = static_cast<LONG>(v);
    if (ReadDword(hKey, L"MarginTop", &v))    ps->marginTop    = static_cast<LONG>(v);
    if (ReadDword(hKey, L"MarginRight", &v))  ps->marginRight  = static_cast<LONG>(v);
    if (ReadDword(hKey, L"MarginBottom", &v)) ps->marginBottom = static_cast<LONG>(v);

    // Freed defensively: PageSetup's constructor leaves these null, but
    // LoadPageSetup is also reachable on a struct that already holds a
    // previous document's DEVMODE.
    if (ps->hDevMode)  { GlobalFree(ps->hDevMode);  ps->hDevMode  = nullptr; }
    if (ps->hDevNames) { GlobalFree(ps->hDevNames); ps->hDevNames = nullptr; }

    ps->hDevMode  = ReadBinaryToHGlobal(hKey, L"DevMode");
    ps->hDevNames = ReadBinaryToHGlobal(hKey, L"DevNames");
}

} // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================

void PageSetup_Free(PageSetup* ps)
{
    if (!ps) return;
    if (ps->hDevMode)  { GlobalFree(ps->hDevMode);  ps->hDevMode  = nullptr; }
    if (ps->hDevNames) { GlobalFree(ps->hDevNames); ps->hDevNames = nullptr; }
}

// Deep-copies the two HGLOBALs. A shallow struct copy would leave two owners
// of one allocation and the second PageSetup_Free would be a double-free.
BOOL PageSetup_Copy(PageSetup* dst, const PageSetup* src)
{
    if (!dst || !src) return FALSE;

    // Release whatever dst held first, then copy fields. On allocation failure
    // we leave the HGLOBAL null rather than pointing at a freed block: a null
    // DEVMODE means "use the default printer", which degrades correctly.
    PageSetup_Free(dst);

    dst->marginLeft   = src->marginLeft;
    dst->marginTop    = src->marginTop;
    dst->marginRight  = src->marginRight;
    dst->marginBottom = src->marginBottom;

    if (src->hDevMode)
    {
        SIZE_T cb = GlobalSize(src->hDevMode);
        const void* p = GlobalLock(src->hDevMode);
        if (p && cb)
        {
            dst->hDevMode = GlobalAlloc(GHND, cb);
            void* q = dst->hDevMode ? GlobalLock(dst->hDevMode) : nullptr;
            if (q) { memcpy(q, p, cb); GlobalUnlock(dst->hDevMode); }
        }
        if (p) GlobalUnlock(src->hDevMode);
    }

    if (src->hDevNames)
    {
        SIZE_T cb = GlobalSize(src->hDevNames);
        const void* p = GlobalLock(src->hDevNames);
        if (p && cb)
        {
            dst->hDevNames = GlobalAlloc(GHND, cb);
            void* q = dst->hDevNames ? GlobalLock(dst->hDevNames) : nullptr;
            if (q) { memcpy(q, p, cb); GlobalUnlock(dst->hDevNames); }
        }
        if (p) GlobalUnlock(src->hDevNames);
    }
    return TRUE;
}

void Settings_ApplyDefaults(AppSettings* s)
{
    if (!s) return;

    // Character defaults matching the classic WordPad new-document appearance:
    // Calibri 11pt black on automatic background.
    //
    // yHeight is in TWIPS, not points -- 11pt * 20 = 220. Getting this wrong
    // by a factor of 20 is the most common RichEdit bug and produces text that
    // is either microscopic or enormous.
    ZeroMemory(&s->defChar, sizeof(s->defChar));
    s->defChar.cbSize          = sizeof(CHARFORMAT2W);
    s->defChar.dwMask          = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET |
                                 CFM_LCID | CFM_UNDERLINETYPE;
    s->defChar.dwEffects       = 0;
    s->defChar.yHeight         = 11 * kTwipsPerPt;      // 220 twips = 11pt
    s->defChar.crTextColor     = RGB(0, 0, 0);
    s->defChar.bCharSet        = DEFAULT_CHARSET;
    s->defChar.bPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    s->defChar.bUnderlineType  = CFU_UNDERLINENONE;
    s->defChar.wWeight         = FW_NORMAL;
    s->defChar.sSpacing        = 0;
    s->defChar.lcid            = LOCALE_USER_DEFAULT;
    wcscpy_s(s->defChar.szFaceName, _countof(s->defChar.szFaceName), L"Calibri");

    // Paragraph defaults: left-aligned, single-spaced, no indent, bullets off.
    ZeroMemory(&s->defPara, sizeof(s->defPara));
    s->defPara.cbSize        = sizeof(PARAFORMAT2);
    s->defPara.dwMask        = PFM_ALIGNMENT | PFM_SPACEBEFORE | PFM_SPACEAFTER |
                               PFM_LINESPACING | PFM_STARTINDENT | PFM_OFFSET |
                               PFM_RIGHTINDENT | PFM_TABSTOPS;
    s->defPara.wAlignment    = PFA_LEFT;
    s->defPara.dySpaceBefore = 0;
    s->defPara.dySpaceAfter  = 0;
    // dyLineSpacing is only honoured when bLineSpacingRule says so. Rule 0 is
    // "single spacing, ignore dyLineSpacing". Setting the field without the
    // rule is a no-op that looks like a bug.
    s->defPara.bLineSpacingRule = 0;
    s->defPara.dyLineSpacing    = 20;
    s->defPara.dxStartIndent = 0;
    s->defPara.dxOffset      = 0;
    s->defPara.dxRightIndent = 0;
    s->defPara.cTabCount     = 0;
    s->defPara.wNumbering    = PFN_BULLET_NONE;
    s->defPara.wReserved     = 0;

    s->wrap           = kWrapWindow;
    s->units          = kUnitInch;
    s->fShowRuler     = TRUE;
    s->fShowStatusBar = TRUE;
    s->rtfAsDefault   = TRUE;
    s->nZoomNum       = 100;
    s->nZoomDen       = 100;
    s->nWindowShow    = SW_SHOWNORMAL;
    s->fMaximised     = FALSE;

    // Centred default window. Not a hard-coded rectangle: a fixed 100,100
    // origin is off-screen on some multi-monitor layouts and in remote
    // sessions where the virtual desktop does not include the primary origin.
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    int w  = cx * 3 / 4;
    int h  = cy * 3 / 4;
    SetRect(&s->rcWindow, (cx - w) / 2, (cy - h) / 2, (cx + w) / 2, (cy + h) / 2);

    PageSetup_Free(&s->page);
    s->page.marginLeft   = kTwipsPerInch;
    s->page.marginTop    = kTwipsPerInch;
    s->page.marginRight  = kTwipsPerInch;
    s->page.marginBottom = kTwipsPerInch;
}

void Settings_Load(AppSettings* s)
{
    if (!s) return;

    // Start from defaults so every field has a sane value even when the
    // registry is empty, partially written, or unreadable.
    Settings_ApplyDefaults(s);

    HKEY hKey = OpenSettingsKey(/*fWrite=*/false);
    if (!hKey)
        return;

    DWORD v = 0;
    BOOL  fVersionOk = ReadDword(hKey, L"SettingsVersion", &v) && v == kSettingsVersion;

    if (fVersionOk)
    {
        // View settings are independent of the persisted struct layout, so
        // they are read even when the binary blobs below are skipped.
        if (ReadDword(hKey, L"Wrap", &v) && v <= static_cast<DWORD>(kWrapRuler))
            s->wrap = static_cast<WrapMode>(v);
        if (ReadDword(hKey, L"Units", &v) && v <= static_cast<DWORD>(kUnitPica))
            s->units = static_cast<UnitSystem>(v);
        if (ReadDword(hKey, L"ShowRuler", &v))     s->fShowRuler     = v ? TRUE : FALSE;
        if (ReadDword(hKey, L"ShowStatusBar", &v)) s->fShowStatusBar = v ? TRUE : FALSE;
        if (ReadDword(hKey, L"RtfAsDefault", &v))  s->rtfAsDefault   = v ? TRUE : FALSE;

        DWORD num = 0, den = 0;
        if (ReadDword(hKey, L"ZoomNum", &num) && ReadDword(hKey, L"ZoomDen", &den) &&
            num >= 10 && num <= 500 && den > 0 && den <= 500)
        {
            // Range-checked because EM_SETZOOM with a zero denominator is a
            // division by zero inside RichEdit, and a stored 0 is reachable
            // from a truncated write.
            s->nZoomNum = static_cast<int>(num);
            s->nZoomDen = static_cast<int>(den);
        }

        if (ReadDword(hKey, L"Maximized", &v)) s->fMaximised = v ? TRUE : FALSE;

        RECT rc;
        if (ReadBinaryStruct(hKey, L"FrameRect", &rc))
        {
            // Validate against the virtual screen before trusting it. A rect
            // saved on a monitor that has since been disconnected would create
            // a window the user cannot reach or close.
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            RECT inter;
            if (IntersectRect(&inter, &rc, &(RECT{vx, vy, vx + vw, vy + vh})) &&
                (inter.right - inter.left) >= 80 && (inter.bottom - inter.top) >= 60)
            {
                s->rcWindow = rc;
            }
        }

        // Struct blobs are only loaded when the version matches: their layout
        // is exactly what the version number describes.
        ReadBinaryStruct(hKey, L"DefaultChar", &s->defChar);
        ReadBinaryStruct(hKey, L"DefaultPara", &s->defPara);

        // cbSize is re-stamped after loading rather than trusted. A blob
        // written by a build with a different structure size would carry the
        // wrong cbSize, and RichEdit rejects the whole call when cbSize is
        // wrong -- silently, returning FALSE with no error code.
        s->defChar.cbSize = sizeof(CHARFORMAT2W);
        s->defPara.cbSize = sizeof(PARAFORMAT2);

        LoadPageSetup(hKey, &s->page);
    }

    RegCloseKey(hKey);
}

void Settings_Save(const AppSettings* s)
{
    if (!s) return;

    HKEY hKey = OpenSettingsKey(/*fWrite=*/true);
    if (!hKey)
        return;

    WriteDword(hKey, L"SettingsVersion", kSettingsVersion);

    WriteDword(hKey, L"Wrap",          static_cast<DWORD>(s->wrap));
    WriteDword(hKey, L"Units",         static_cast<DWORD>(s->units));
    WriteDword(hKey, L"ShowRuler",     s->fShowRuler     ? 1 : 0);
    WriteDword(hKey, L"ShowStatusBar", s->fShowStatusBar ? 1 : 0);
    WriteDword(hKey, L"RtfAsDefault",  s->rtfAsDefault   ? 1 : 0);
    WriteDword(hKey, L"ZoomNum",       static_cast<DWORD>(s->nZoomNum));
    WriteDword(hKey, L"ZoomDen",       static_cast<DWORD>(s->nZoomDen));
    WriteDword(hKey, L"Maximized",     s->fMaximised ? 1 : 0);

    WriteBinaryStruct(hKey, L"FrameRect",   s->rcWindow);
    WriteBinaryStruct(hKey, L"DefaultChar", s->defChar);
    WriteBinaryStruct(hKey, L"DefaultPara", s->defPara);
    SavePageSetup(hKey, s->page);

    RegCloseKey(hKey);
}

// ===========================================================================
//  Most-recently-used list
//
//  Stored as "File1".."File9" under the Recent File List subkey, most recent
//  first. That numbering convention matches the shell's own MRU keys, so the
//  subkey is legible to anything that already understands one.
//
//  Mru_Add inserts at position 1 and shifts the rest down, dropping the entry
//  that falls off the end. It also de-duplicates: an existing entry is removed
//  before being re-inserted at the top, so opening the same file twice does not
//  consume two slots.
// ===========================================================================
void Mru_Add(PCWSTR path)
{
    if (!path || !path[0])
        return;

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LP_MRU_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    // Read the existing list into a local array so the shift-down below does
    // not read a value it has just overwritten.
    WCHAR list[kMruMax][MAX_PATH] = {};
    int   count = 0;
    for (int i = 0; i < kMruMax; ++i)
    {
        WCHAR name[16];
        StrPrintf(name, _countof(name), L"File%d", i + 1);

        WCHAR value[MAX_PATH] = {};
        DWORD cb = sizeof(value), type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(value), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && value[0])
        {
            wcsncpy_s(list[count], MAX_PATH, value, _TRUNCATE);
            ++count;
        }
    }

    // Drop any existing occurrence of this path (case-insensitive: Windows
    // paths are case-insensitive, and treating "C:\A.rtf" and "c:\a.rtf" as
    // two entries would waste a slot).
    for (int i = 0; i < count; )
    {
        if (_wcsicmp(list[i], path) == 0)
        {
            for (int j = i; j + 1 < count; ++j)
                wcscpy_s(list[j], MAX_PATH, list[j + 1]);
            --count;
        }
        else ++i;
    }

    // Shift down, keeping at most kMruMax-1 old entries so the new one fits.
    if (count > kMruMax - 1) count = kMruMax - 1;
    for (int i = count - 1; i >= 0; --i)
        wcscpy_s(list[i + 1], MAX_PATH, list[i]);

    wcscpy_s(list[0], MAX_PATH, path);
    ++count;

    // Write back. Entries beyond the list length are deleted rather than left
    // stale: a shrunk list must not keep showing files the user closed.
    for (int i = 0; i < kMruMax; ++i)
    {
        WCHAR name[16];
        StrPrintf(name, _countof(name), L"File%d", i + 1);
        if (i < count)
            RegSetValueExW(hKey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(list[i]),
                           static_cast<DWORD>((wcslen(list[i]) + 1) * sizeof(WCHAR)));
        else
            RegDeleteValueW(hKey, name);
    }

    RegCloseKey(hKey);
}

int Mru_GetList(PWSTR paths[kMruMax])
{
    if (!paths) return 0;
    for (int i = 0; i < kMruMax; ++i) paths[i][0] = L'\0';

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LP_MRU_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return 0;

    int count = 0;
    for (int i = 0; i < kMruMax; ++i)
    {
        WCHAR name[16];
        StrPrintf(name, _countof(name), L"File%d", i + 1);

        WCHAR value[MAX_PATH] = {};
        DWORD cb = sizeof(value), type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(value), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && value[0])
        {
            wcsncpy_s(paths[count], MAX_PATH, value, _TRUNCATE);
            ++count;
        }
    }

    RegCloseKey(hKey);
    return count;
}

void Mru_Clear()
{
    // DeleteValue on the whole subkey rather than looping: it removes the
    // values and the key in one call, and the key is recreated on next Mru_Add.
    RegDeleteKeyW(HKEY_CURRENT_USER, LP_MRU_KEY);
}
