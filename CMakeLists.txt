# ============================================================================
#  LetterPad -- CMake build
#
#  Windows only. This project links RichEdit 4.1, the Windows Ribbon, and the
#  common dialogs; none of those exist off Windows and there is no portability
#  layer to write, because the entire point of the application is those APIs.
#
#  USAGE
#    cmake -B build -A x64
#    cmake --build build --config Release
#
#  OPTIONS
#    -DLETTERPAD_SKIP_RIBBON=ON   build without the ribbon resource. The app
#                                 falls back to a classic toolbar + menu at
#                                 runtime. Use this on a machine without the
#                                 Windows SDK bin directory on PATH.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(LetterPad
        VERSION   0.1.0
        LANGUAGES CXX
        DESCRIPTION "An original rich-text word processor for Windows")

if(NOT WIN32)
  # Fail loudly and early. Continuing would produce a long list of missing-
  # header errors that look like a broken source tree rather than a wrong OS.
  message(FATAL_ERROR
    "LetterPad targets Windows only. It links msftedit.dll (RichEdit 4.1), "
    "the Windows Ribbon, and the Win32 common dialogs. Configure this project "
    "on Windows with Visual Studio Build Tools and the Windows SDK.")
endif()

# ----------------------------------------------------------------------------
#  Options
# ----------------------------------------------------------------------------
option(LETTERPAD_SKIP_RIBBON
       "Build without the ribbon resource; the app uses the classic toolbar" OFF)

# ----------------------------------------------------------------------------
#  Language standard and global compile settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode everywhere. LetterPad has no ANSI code path and no TCHAR-mapped types;
# defining these makes an accidental A/W mismatch a compile error rather than a
# silent runtime codepage conversion.
add_compile_definitions(UNICODE _UNICODE)

# The two defines that decide whether this project compiles at all.
#
#   _RICHEDIT_VER=0x0500  unlocks CHARFORMAT2W/PARAFORMAT2W, EM_SETZOOM,
#                         EM_SETTARGETDEVICE, EM_SETTYPOGRAPHYOPTIONS and
#                         MSFTEDIT_CLASS. Without it, <richedit.h> exposes only
#                         the v3.00 structures and roughly half the codebase
#                         fails with "undeclared identifier" errors that point
#                         nowhere near the actual cause.
#
#   WIN32_LEAN_AND_MEAN   keeps <windows.h> from pulling in WinSock and MFC
#                         surface that conflicts with the COM headers.
add_compile_definitions(_RICHEDIT_VER=0x0500 WIN32_LEAN_AND_MEAN)

# Windows 7 SP1 is the floor, because IUIFramework is.
add_compile_definitions(
  WINVER=0x0601
  _WIN32_WINNT=0x0601
  NTDDI_VERSION=0x06010000
)

# ----------------------------------------------------------------------------
#  Sources
# ----------------------------------------------------------------------------
set(LETTERPAD_SOURCES
    src/main.cpp
    src/frame.cpp
    src/editview.cpp
    src/codec.cpp
    src/findrep.cpp
    src/print.cpp
    src/ruler.cpp
    src/ribbon.cpp
    src/oleobj.cpp
    src/dialogs.cpp
    src/settings.cpp
    res/LetterPad.rc
)

set(LETTERPAD_HEADERS
    src/stdafx.h
    src/util.h
    src/resource.h
    src/LetterPad.h
    src/ribbon/RibbonIDs.h
    src/ribbon/Ribbon.xml
)

# WIN32 -> /SUBSYSTEM:WINDOWS, so the entry point is wWinMain and no console
# window appears. Omitting this gives a console subsystem GUI app, which flashes
# a black window on every launch.
add_executable(LetterPad WIN32 ${LETTERPAD_SOURCES} ${LETTERPAD_HEADERS})

target_include_directories(LetterPad PRIVATE src)

# ----------------------------------------------------------------------------
#  Compiler flags
#
#  /W4 and /permissive- are on deliberately and should stay on. The bugs they
#  catch in Win32 code are the ones that do not reproduce: pointer truncation on
#  x64, signed/unsigned comparisons against LPARAM, and implicit conversions
#  between the A and W variants of a structure.
# ----------------------------------------------------------------------------
if(MSVC)
  target_compile_options(LetterPad PRIVATE
      /utf-8          # sources are UTF-8; literals must not be reinterpreted
                      # through the system ANSI codepage
      /W4
      /permissive-
      /Zc:__cplusplus # __cplusplus reports the real standard, not 199711L
      /Zc:strictStrings
      /guard:cf       # Control Flow Guard; cheap and standard on Win8+
      /EHsc
      /MP             # parallel compilation; this is a lot of heavy headers
  )

  # /utf-8 sets both source and execution charset. Without it, the non-ASCII
  # characters in the string table (the 1/2 glyph in ruler tick labels) are
  # encoded in the build machine's codepage and render as mojibake on any
  # machine with a different one.
  target_compile_definitions(LetterPad PRIVATE
      _CRT_SECURE_NO_WARNINGS   # wcscpy_s family is used throughout; the
                                # deprecation warnings for the unsafe variants
                                # are noise when they are never called
      NOMINMAX                  # <windows.h> defines min/max macros that break
                                # std::min and std::max in dialogs.cpp
  )

  target_link_options(LetterPad PRIVATE
      /DYNAMICBASE      # ASLR
      /NXCOMPAT         # DEP
      /HIGHENTROPYVA    # 64-bit ASLR; ignored harmlessly on x86
      /CETCOMPAT        # Intel CET shadow stack, Win10 2004+
  )

  # Debug builds get /RTC1 and no optimisation; Release gets /O2 with frame
  # pointer omission left ON (/Oy-). Omitting frame pointers makes a crash dump
  # from a release build nearly impossible to stack-walk, and the performance
  # difference on a UI application is unmeasurable.
  target_compile_options(LetterPad PRIVATE
      $<$<CONFIG:Debug>:/Od /RTC1 /Zi /MDd>
      $<$<CONFIG:Release>:/O2 /Oy- /Zi /MD>
  )
  target_link_options(LetterPad PRIVATE
      $<$<CONFIG:Debug>:/DEBUG>
      $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>
  )
endif()

# ----------------------------------------------------------------------------
#  Link libraries
#
#  Every one of these is a Windows system DLL present on all supported SKUs.
#  Nothing here is a third-party or redistributable dependency, so the built
#  LetterPad.exe is a single file that runs on a clean Windows install.
#
#  uuid.lib is required for the COM interface IIDs (IID_IUIFramework,
#  IID_IRichEditOle, CLSID_UIRibbonFramework). Forgetting it produces a wall of
#  LNK2001 unresolved-external errors that name IIDs rather than functions,
#  which is not obviously a missing library.
# ----------------------------------------------------------------------------
target_link_libraries(LetterPad PRIVATE
    kernel32
    user32
    gdi32
    advapi32        # registry
    shell32         # ShellExecute, DragQueryFile, SHGetFolderPath
    shlwapi         # PathFindFileName, PathFindExtension
    comctl32        # toolbar, status bar, trackbar, tooltips, SetWindowSubclass
    comdlg32        # Open/Save/Print/PageSetup/Find/Replace dialogs
    ole32           # COM, OLE, CoCreateInstance
    oleaut32        # SysAllocString, PROPVARIANT BSTR handling
    oledlg          # OleUIPasteSpecial
    uxtheme         # visual styles queries
    version         # version resource
    uuid            # COM IIDs -- see the note above
)

# ----------------------------------------------------------------------------
#  Ribbon: compile src/ribbon/Ribbon.xml with UICC
#
#  uicc.exe ships in the Windows SDK bin directory, which is NOT on PATH by
#  default. The search below covers the usual install locations; if it comes up
#  empty the build reports exactly what to do rather than failing on a missing
#  command with no explanation.
#
#  The generated Ribbon.bml is written into res/ so that the path in
#  LetterPad.rc ("Ribbon.bml") resolves. rc.exe resolves relative resource paths
#  against the .rc file's own directory, and there is no supported way to add a
#  search path for binary payloads. res/Ribbon.bml is therefore a build output
#  living in the source tree, and it is gitignored.
# ----------------------------------------------------------------------------
set(RIBBON_BML "${CMAKE_CURRENT_SOURCE_DIR}/res/Ribbon.bml")
set(RIBBON_GEN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/RibbonIDs_gen.h")

if(LETTERPAD_SKIP_RIBBON)
  message(STATUS "LETTERPAD_SKIP_RIBBON=ON: building without the ribbon resource")
  target_compile_definitions(LetterPad PRIVATE LETTERPAD_NO_RIBBON)
  # A stale Ribbon.bml from a previous ribbon-enabled build would still be
  # picked up by the RC script if the define were ever removed, so make the
  # skip explicit rather than relying on the #ifndef alone.
else()
  find_program(UICC_EXECUTABLE
      NAMES uicc uicc.exe
      HINTS
        ENV WindowsSdkDir
        "$ENV{ProgramFiles(x86)}/Windows Kits/10/bin"
        "$ENV{ProgramFiles}/Windows Kits/10/bin"
      PATH_SUFFIXES x64 x86 arm64
      DOC "Windows SDK User Interface Command Compiler"
  )

  # find_program with PATH_SUFFIXES does not recurse into the per-version
  # directories (10.0.22621.0 and friends), so glob for them explicitly.
  if(NOT UICC_EXECUTABLE)
    file(GLOB _sdk_bin_dirs LIST_DIRECTORIES true
         "$ENV{ProgramFiles(x86)}/Windows Kits/10/bin/*"
         "$ENV{ProgramFiles}/Windows Kits/10/bin/*")
    foreach(_d IN LISTS _sdk_bin_dirs)
      foreach(_arch x64 x86)
        if(EXISTS "${_d}/${_arch}/uicc.exe")
          set(UICC_EXECUTABLE "${_d}/${_arch}/uicc.exe")
          break()
        endif()
      endforeach()
      if(UICC_EXECUTABLE)
        break()
      endif()
    endforeach()
  endif()

  if(NOT UICC_EXECUTABLE)
    message(WARNING
      "uicc.exe was not found. The Windows Ribbon markup cannot be compiled, so "
      "LetterPad will build WITHOUT the ribbon resource and fall back to the "
      "classic toolbar at runtime.\n"
      "To get the ribbon, install the Windows SDK and either add its bin "
      "directory to PATH or configure with:\n"
      "  cmake -B build -DUICC_EXECUTABLE=\"C:/Program Files (x86)/Windows "
      "Kits/10/bin/10.0.22621.0/x64/uicc.exe\"\n"
      "Or silence this by configuring with -DLETTERPAD_SKIP_RIBBON=ON.")
    target_compile_definitions(LetterPad PRIVATE LETTERPAD_NO_RIBBON)
  else()
    message(STATUS "Found uicc: ${UICC_EXECUTABLE}")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

    # /name: sets the resource name UICC writes into its generated .rc snippet.
    # We do not use that snippet -- LetterPad.rc references the .bml by ID
    # directly -- but /name is required for UICC to emit a valid one.
    add_custom_command(
        OUTPUT  "${RIBBON_BML}" "${RIBBON_GEN_HEADER}"
        COMMAND "${UICC_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ribbon/Ribbon.xml"
                "${RIBBON_BML}"
                "/header:${RIBBON_GEN_HEADER}"
                "/name:LETTERPAD_RIBBON"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ribbon/Ribbon.xml"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ribbon/RibbonIDs.h"
        COMMENT "Compiling Ribbon.xml with uicc"
        VERBATIM
    )

    # The RC file #includes Ribbon.bml, so the markup must be regenerated before
    # the resources are compiled. Attaching the output to the target as a source
    # is what creates that ordering; a separate custom target would need an
    # explicit add_dependencies and is easier to get wrong.
    target_sources(LetterPad PRIVATE "${RIBBON_BML}")

    # -------------------------------------------------------------------------
    #  ID drift check
    #
    #  RibbonIDs.h is hand-written and Ribbon.xml carries explicit Id= values.
    #  The generated header is the ground truth for what UICC actually saw. If
    #  the two disagree, a ribbon control is bound to the wrong handler and the
    #  symptom is a button that does something else entirely -- one of the least
    #  diagnosable failures in ribbon programming.
    # -------------------------------------------------------------------------
    add_custom_target(verify-ribbon-ids ALL
        COMMAND ${CMAKE_COMMAND}
                -DCOMMITTED="${CMAKE_CURRENT_SOURCE_DIR}/src/ribbon/RibbonIDs.h"
                -DCOMMITTED_CMDS="${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h"
                -DGENERATED="${RIBBON_GEN_HEADER}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/VerifyRibbonIds.cmake"
        DEPENDS "${RIBBON_GEN_HEADER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ribbon/RibbonIDs.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/VerifyRibbonIds.cmake"
        COMMENT "Verifying ribbon IDs match RibbonIDs.h"
        VERBATIM
    )
    add_dependencies(verify-ribbon-ids LetterPad)
  endif()
endif()

# ----------------------------------------------------------------------------
#  Output layout
#
#  Flat per-config directories rather than CMake's default of nesting by target,
#  so `build\Release\LetterPad.exe` is the path the documentation promises.
# ----------------------------------------------------------------------------
set_target_properties(LetterPad PROPERTIES
    OUTPUT_NAME "LetterPad"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    # No PDB next to a release exe by default; /Zi + /DEBUG above put them in
    # the same directory, which is what makes a crash dump actionable.
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(LETTERPAD_SKIP_RIBBON OR NOT UICC_EXECUTABLE)
  set(_ribbon_status "SKIPPED (classic toolbar at runtime)")
else()
  set(_ribbon_status "ENABLED (${UICC_EXECUTABLE})")
endif()

message(STATUS "")
message(STATUS "LetterPad ${PROJECT_VERSION}")
message(STATUS "  ribbon resource : ${_ribbon_status}")
message(STATUS "  output          : ${CMAKE_BINARY_DIR}/<config>/LetterPad.exe")
message(STATUS "")
