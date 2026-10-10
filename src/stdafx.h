// ============================================================================
//  stdafx.h -- precompiled header
//
//  Every translation unit in LetterPad includes this first and includes
//  NOTHING from the Windows SDK directly. That is deliberate, for one reason:
//  the macro block below must be evaluated before <richedit.h> is parsed, and
//  <richedit.h> must be parsed before any RichEdit 4.1 identifier is used.
//  A stray #include <richedit.h> in a .cpp file silently downgrades the whole
//  program to the v3.00 structures and produces errors that look nothing like
//  the actual cause.
//
//  _RICHEDIT_VER = 0x0500 is the load-bearing define in this file. It unlocks:
//      CHARFORMAT2W / PARAFORMAT2W   -- the extended formatting structures
//      EM_SETZOOM / EM_GETZOOM       -- the zoom slider
//      EM_SETTYPOGRAPHYOPTIONS       -- advanced typography (kerning, ligatures)
//      EM_SETTARGETDEVICE            -- "wrap to ruler" at an explicit width
//      MSFTEDIT_CLASS                -- the RICHEDIT50W window class name
// ============================================================================
#pragma once

// ---- target platform -------------------------------------------------------
// Literals rather than the NTDDI_WIN7 symbols: those symbols live in
// <sdkddkver.h>, and including it here to then use it to gate <windows.h>
// would be circular. Windows 7 SP1 is the floor because IUIFramework is.
#ifndef WINVER
#define WINVER         0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT   0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION  0x06010000
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// All sources are UTF-16. The ANSI entry points and TCHAR-mapped types are
// never used; defining these means an accidental A/W mismatch is a compile
// error rather than a silent codepage conversion at runtime.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// MUST precede <richedit.h>. See the header comment above.
#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER  0x0500
#endif

// ---- core SDK --------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>          // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <ole2.h>              // MUST precede <richole.h>
#include <oleidl.h>
#include <oleauto.h>           // SysAllocString; ole2.h does not pull it in
#include <propidl.h>           // PROPERTYKEY, IsEqualPropertyKey
#include <richedit.h>
#include <richole.h>           // IRichEditOle, REOBJECT
#include <commctrl.h>
#include <commdlg.h>
#include <cderr.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <dlgs.h>
#include <oledlg.h>            // OleUIPasteSpecial

// Windows Ribbon. MUST come after <ole2.h>: UIRibbon.h is a COM header and
// expects IUnknown/IStream to be in scope. UIRibbonPropertyKeys.h carries the
// UI_PKEY_* PROPERTYKEY definitions that UIRibbon.h itself does not; including
// only the former compiles the interfaces but leaves every UI_PKEY_Font*
// reference undefined.
#include <UIRibbon.h>
#include <UIRibbonPropertyKeys.h>

// ---- COM / ATL-free smart pointer -----------------------------------------
// The Windows SDK ships WRL's ComPtr in <wrl/client.h>, but its availability
// and namespace have moved between SDK releases. util.h carries a 40-line
// equivalent instead so the build does not depend on that churn.
#include <new>
#include <cstdlib>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// ---- project headers -------------------------------------------------------
#include "util.h"
#include "resource.h"
