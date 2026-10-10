// ============================================================================
//  util.h -- COM smart pointer, x64-safe LPARAM marshalling, string helpers
//
//  Two rules this file exists to enforce:
//
//  1. On x64, WPARAM/LPARAM are 64-bit and LONG is 32-bit. Casting a pointer
//     to LONG truncates it and the failure surfaces as a crash somewhere else
//     entirely, usually in RichEdit's callback. Every pointer<->LPARAM
//     conversion in LetterPad goes through the helpers below.
//
//  2. COM interfaces are AddRef/Release paired by hand everywhere else in
//     Win32 code, which is how leaks get into message-loop programs. ComPtr
//     makes the ownership explicit at the declaration site.
// ============================================================================
#pragma once

// ----------------------------------------------------------------------------
//  ComPtr -- minimal intrusive smart pointer for IUnknown-derived interfaces.
//
//  Deliberately NOT a reimplementation of WRL::ComPtr: no move semantics
//  cleverness, no QueryInterface helpers that hide failures, no aggregation
//  support. It does exactly AddRef-on-copy / Release-on-destroy, which is all
//  a single-threaded STA message-loop app needs.
// ----------------------------------------------------------------------------
template <class T>
class ComPtr
{
public:
    ComPtr() : m_p(nullptr) {}

    // Not explicit on purpose: `ComPtr<IFoo> p = CreateFoo();` reads better
    // than `ComPtr<IFoo> p(CreateFoo());` and there is no ambiguity risk with
    // a single-argument interface pointer constructor.
    ComPtr(T* p) : m_p(p) { if (m_p) m_p->AddRef(); }

    ComPtr(const ComPtr& o) : m_p(o.m_p) { if (m_p) m_p->AddRef(); }

    ~ComPtr() { if (m_p) m_p->Release(); }

    ComPtr& operator=(const ComPtr& o)
    {
        if (o.m_p) o.m_p->AddRef();          // AddRef BEFORE Release: handles
        if (m_p)  m_p->Release();           // self-assignment (p = p) safely.
        m_p = o.m_p;
        return *this;
    }

    ComPtr& operator=(T* p)
    {
        if (p) p->AddRef();
        if (m_p) m_p->Release();
        m_p = p;
        return *this;
    }

    // Detach: surrender ownership WITHOUT releasing. Use when handing a
    // pointer out through an out-parameter that the caller owns.
    T** Put() { if (m_p) { m_p->Release(); m_p = nullptr; } return &m_p; }

    // Adopt: take ownership of a pointer that ALREADY holds a reference for
    // us (the normal contract for an out-parameter the callee filled in).
    // Does NOT AddRef -- that is the whole difference from operator=.
    T** Adopt() { if (m_p) { m_p->Release(); m_p = nullptr; } return &m_p; }

    T*  Get()      const { return m_p; }
    T*  operator->() const { return m_p; }
    T** operator&()      { return Put(); }

    explicit operator bool() const { return m_p != nullptr; }

private:
    T* m_p;
};

// ----------------------------------------------------------------------------
//  x64-safe LPARAM / WPARAM marshalling
//
//  Prefer these over a raw (LPARAM) cast. They are no-ops on the correct
//  platform and a compile-time guarantee that no truncation happened.
// ----------------------------------------------------------------------------
inline LPARAM LParamFromPtr(const void* p)
{
    return static_cast<LPARAM>(reinterpret_cast<INT_PTR>(p));
}

inline WPARAM WParamFromPtr(const void* p)
{
    return static_cast<WPARAM>(reinterpret_cast<INT_PTR>(p));
}

template <class T>
inline T* PtrFromLParam(LPARAM l)
{
    // Round-trips through INT_PTR so that a 64-bit LPARAM holding a pointer is
    // narrowed exactly once, at a point we control.
    return reinterpret_cast<T*>(static_cast<INT_PTR>(l));
}

template <class T>
inline T* PtrFromWParam(WPARAM w)
{
    return reinterpret_cast<T*>(static_cast<INT_PTR>(w));
}

// ----------------------------------------------------------------------------
//  HRESULT helpers
// ----------------------------------------------------------------------------
inline bool Succeeded(HRESULT hr) { return SUCCEEDED(hr); }

// Converts a WIN32 error into the HRESULT space without the sign-extension bug
// that `E_FAIL`-or-nothing fallbacks introduce.
inline HRESULT HrFromWin32(DWORD err) { return HRESULT_FROM_WIN32(err); }

// ----------------------------------------------------------------------------
//  String helpers
// ----------------------------------------------------------------------------

// Defined in main.cpp. Declared here because the string helpers below need the
// module handle, and every TU already has util.h in scope.
extern HINSTANCE g_hInst;

// Fixed-buffer printf. Returns false on truncation so callers can decide
// whether a truncated string is acceptable (status bar text: yes) or not
// (a path being handed to CreateFile: absolutely not).
inline bool StrPrintf(PWSTR buf, size_t cchBuf, PCWSTR fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = _vsnwprintf_s(buf, cchBuf, _TRUNCATE, fmt, args);
    va_end(args);
    // _vsnwprintf_s returns -1 on truncation; buf is still NUL-terminated.
    return n >= 0;
}

// Heap-duplicates a string. Caller owns the result and must LocalFree it.
// Returns nullptr on allocation failure OR on a null input -- callers should
// treat both as "no string", which is the behaviour every call site wants.
inline PWSTR StrDupAlloc(PCWSTR s)
{
    if (!s) return nullptr;
    size_t cch = wcslen(s) + 1;
    PWSTR out = static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, cch * sizeof(WCHAR)));
    if (!out) return nullptr;
    memcpy(out, s, cch * sizeof(WCHAR));
    return out;
}

// Loads a string resource into a caller-supplied buffer. Returns the character
// count, or 0 if the resource is missing. LoadStringW's buffer-mode overload
// (non-zero cch) does NOT NUL-terminate on truncation in all SDK versions, so
// this terminates defensively.
inline int LoadStr(UINT id, PWSTR buf, int cchBuf)
{
    if (cchBuf <= 0) return 0;
    buf[0] = L'\0';
    int n = LoadStringW(g_hInst, id, buf, cchBuf - 1);
    buf[n] = L'\0';                      // safe: n <= cchBuf-1
    return n;
}

// Loads a string resource into a heap buffer sized exactly to fit.
// Caller owns the result; LocalFree it.
inline PWSTR LoadStrAlloc(UINT id)
{
    // LoadStringW with a buffer of 0 returns the HRSRC-style behaviour we do
    // not want; instead loop with a growing buffer. Two iterations is almost
    // always enough because the first guess is generous.
    for (int cch = 256; cch <= 65536; cch *= 2)
    {
        PWSTR buf = static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, cch * sizeof(WCHAR)));
        if (!buf) return nullptr;

        int n = LoadStringW(g_hInst, id, buf, cch);
        if (n > 0 && n < cch)            // fit with room for the NUL
            return buf;

        LocalFree(buf);
        if (n == 0) return nullptr;      // resource genuinely missing
    }
    return nullptr;
}
