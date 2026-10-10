// ============================================================================
//  resource.h -- single ID space for the whole application
//
//  ID RANGES (kept disjoint on purpose):
//
//     2 .. 999    Command IDs. SHARED across the Ribbon, the fallback menu,
//                 the fallback toolbar, and the accelerator table. One value
//                 per user action means one dispatch function handles a
//                 command identically no matter which surface raised it.
//                 See ribbon/RibbonIDs.h for the authoritative ribbon half.
//                 NOTE: 0 and 1 are reserved by UICC and must never be used.
//
//  1000 .. 1099   Icons
//  1100 .. 1199   Dialogs
//  1200 .. 1299   Other named resources (ribbon markup, accelerators, version)
//  2000 .. 2999   String table
//  3000 .. 3099   Dialog control IDs
//
//  Command IDs are deliberately NOT aliased to string IDs. The classic Win32
//  trick of `LoadString(hwnd, cmdId, ...)` to fetch a button caption only works
//  while the two tables stay in lockstep, and they never do once a ribbon is
//  involved -- the ribbon owns its own labels in Ribbon.xml.
// ============================================================================
#pragma once

// ----------------------------------------------------------------------------
//  Version
//
//  Defined here rather than in LetterPad.h because the RC compiler only sees
//  this file. A version macro in a C++ header would leave the binary's version
//  resource and the About dialog free to disagree, and nothing in the build
//  would notice.
// ----------------------------------------------------------------------------
#define LP_VERSION_MAJOR    0
#define LP_VERSION_MINOR    1
#define LP_VERSION_PATCH    0
#define LP_VERSION_BUILD    0

// ----------------------------------------------------------------------------
//  Ribbon element IDs live in ribbon/RibbonIDs.h, not here.
//
//  The split is deliberate. Command IDs (below) are shared by every UI surface
//  and belong in the common space. Ribbon *element* IDs -- tabs, groups, the
//  QAT, the ribbon root -- are meaningful only to UICC and IUIFramework, and
//  keeping them in one file next to Ribbon.xml means the markup and its ID
//  table can be reviewed side by side.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//  File / application-menu commands
// ----------------------------------------------------------------------------
#define ID_CMD_NEW                  20
#define ID_CMD_OPEN                 21
#define ID_CMD_SAVE                 22
#define ID_CMD_SAVEAS               34      // split button parent
#define ID_CMD_SAVEAS_RTF           23
#define ID_CMD_SAVEAS_DOCX          24
#define ID_CMD_SAVEAS_ODT           25
#define ID_CMD_SAVEAS_TXT           26
#define ID_CMD_PRINTMENU            35      // split button parent
#define ID_CMD_PRINT                27
#define ID_CMD_QUICKPRINT           28
#define ID_CMD_PRINTPREVIEW         29
#define ID_CMD_PAGESETUP            30
#define ID_CMD_CLOSE                31
#define ID_CMD_OPTIONS              32
#define ID_CMD_EXIT                 33

// ----------------------------------------------------------------------------
//  Editing commands
// ----------------------------------------------------------------------------
#define ID_CMD_UNDO                 40
#define ID_CMD_REDO                 41
#define ID_CMD_CUT                  42
#define ID_CMD_COPY                 43
#define ID_CMD_PASTE                44
#define ID_CMD_PASTESPECIAL         45
#define ID_CMD_SELECTALL            46
#define ID_CMD_FIND                 47
#define ID_CMD_REPLACE              48
#define ID_CMD_GOTO                 49

// ----------------------------------------------------------------------------
//  Font
//  The ribbon uses a single composite FontControl (ID_CMD_FONTCONTROL) which
//  reports changes as UI_PKEY_* properties rather than as discrete commands.
//  The discrete IDs below drive the fallback toolbar/menu path and the
//  keyboard accelerators. RibbonHandler translates the composite control's
//  property changes into these same IDs so there is one code path.
// ----------------------------------------------------------------------------
#define ID_CMD_FONTCONTROL          60
#define ID_CMD_BOLD                 61
#define ID_CMD_ITALIC               62
#define ID_CMD_UNDERLINE            63
#define ID_CMD_STRIKEOUT            64
#define ID_CMD_SUBSCRIPT            65
#define ID_CMD_SUPERSCRIPT          66
#define ID_CMD_FONTCOLOR            67
#define ID_CMD_HIGHLIGHTCOLOR       68

// ----------------------------------------------------------------------------
//  Paragraph
// ----------------------------------------------------------------------------
#define ID_CMD_BULLETS              70
#define ID_CMD_ALIGN_LEFT           71
#define ID_CMD_ALIGN_CENTER         72
#define ID_CMD_ALIGN_RIGHT          73
#define ID_CMD_ALIGN_JUSTIFY        74
#define ID_CMD_LINESPACING          75
#define ID_CMD_INDENT_INCREASE      76
#define ID_CMD_INDENT_DECREASE      77

// Line-spacing drop-down children. Separate IDs rather than one command with a
// parameter, because a ribbon DropDownButton dispatches each menu entry as its
// own command ID and the fallback menu must be able to do the same.
#define ID_CMD_LINESPACING_SINGLE   110
#define ID_CMD_LINESPACING_ONEHALF  111
#define ID_CMD_LINESPACING_DOUBLE   112
#define ID_CMD_LINESPACING_OPTIONS  113

// ----------------------------------------------------------------------------
//  Insert
// ----------------------------------------------------------------------------
#define ID_CMD_DATETIME             80
#define ID_CMD_OBJECT               81
#define ID_CMD_PICTURE              82

// ----------------------------------------------------------------------------
//  View
// ----------------------------------------------------------------------------
#define ID_CMD_ZOOMIN               90
#define ID_CMD_ZOOMOUT              91
#define ID_CMD_ZOOM100              92
#define ID_CMD_RULER                93
#define ID_CMD_STATUSBAR            94
#define ID_CMD_WRAP_NONE            95
#define ID_CMD_WRAP_WINDOW          96
#define ID_CMD_WRAP_RULER           97

// Help. Not on the ribbon (the original has no Help tab), so it only ever
// arrives from the fallback menu.
#define ID_CMD_ABOUT                98

// ----------------------------------------------------------------------------
//  Icons
// ----------------------------------------------------------------------------
#define IDI_LETTERPAD               1000    // application icon, multi-size
#define IDI_LETTERPAD_SMALL         1001    // 16x16, window caption + taskbar
#define IDI_DOC_GENERIC             1002    // document-type icon (small overlay)

// ----------------------------------------------------------------------------
//  Dialogs
// ----------------------------------------------------------------------------
#define IDD_OPTIONS                 1100
#define IDD_GOTO                    1101
#define IDD_PRINTPREVIEW            1102
#define IDD_ABOUT                   1103

// ----------------------------------------------------------------------------
//  Other named resources
// ----------------------------------------------------------------------------
// RT_APPLICATION_RIBBON is 281. UICC writes the compiled markup to a .bml
// which the RC script references under this ID. IUIFramework::LoadUI reads it.
#define RT_APPLICATION_RIBBON       281
#define IDR_RIBBON                  1200
#define IDR_ACCEL                   1201
#define IDR_MENU_FALLBACK           1202    // used only when Ribbon is absent
#define IDR_VERSION                 1203
#define IDR_RIBBON_BML              1204    // the compiled markup payload

// Bitmaps. Only the fallback toolbar uses one; the ribbon supplies its own
// images from the markup's SmallImages/LargeImages attributes, which are
// currently unset (see docs/ROADMAP.md -- iconography is unfinished).
#define IDB_TOOLBAR                 1300

// ----------------------------------------------------------------------------
//  String table
// ----------------------------------------------------------------------------
#define IDS_APPNAME                 2000
#define IDS_APPDESCRIPTION          2001
#define IDS_UNTITLED                2002
#define IDS_READY                   2003

// File-format names and dialog filter fragments
#define IDS_FMT_RTF                 2010
#define IDS_FMT_DOCX                2011
#define IDS_FMT_ODT                 2012
#define IDS_FMT_TXT                 2013
#define IDS_FMT_WRI                 2014
#define IDS_FILTER_ALLDOCS          2015
#define IDS_FILTER_RTF              2016
#define IDS_FILTER_DOCX             2017
#define IDS_FILTER_ODT              2018
#define IDS_FILTER_TXT              2019
#define IDS_FILTER_ALLFILES         2020

// Errors and confirmations. Each has a matching caption string ending in _CAP.
#define IDS_ERR_OPENFILE            2030
#define IDS_ERR_OPENFILE_CAP        2031
#define IDS_ERR_SAVEFILE            2032
#define IDS_ERR_SAVEFILE_CAP        2033
#define IDS_ERR_UNSAVED             2034
#define IDS_ERR_UNSAVED_CAP         2035
#define IDS_ERR_NORICHEDIT          2036
#define IDS_ERR_NORICHEDIT_CAP      2037
#define IDS_ERR_PRINT               2038
#define IDS_ERR_PRINT_CAP           2039
#define IDS_ERR_CODEC               2040    // DOCX/ODT not yet implemented
#define IDS_ERR_CODEC_CAP           2041
#define IDS_ERR_FINDWRAP            2042    // search reached the document end

// Status-bar and caption fragments
#define IDS_CAPTION_DIRTY           2050
#define IDS_STATUS_POS              2051
#define IDS_STATUS_ZOOM             2052
#define IDS_STATUS_SEARCHDONE       2053

// Options dialog labels that are too long or too localised to inline
#define IDS_OPT_INCHES              2060
#define IDS_OPT_CENTIMETERS         2061
#define IDS_OPT_POINTS              2062
#define IDS_OPT_PICAS               2063

#define IDS_ABOUT_TEXT              2070

// ----------------------------------------------------------------------------
//  Dialog control IDs
// ----------------------------------------------------------------------------
// IDD_OPTIONS
#define IDC_OPT_FONTLIST            3000
#define IDC_OPT_SIZEEDIT            3001
#define IDC_OPT_SIZELIST            3002
#define IDC_OPT_FONTSTYLE           3003
#define IDC_OPT_PREVIEW             3004
#define IDC_OPT_UNITS_INCH          3010
#define IDC_OPT_UNITS_CM            3011
#define IDC_OPT_UNITS_PT            3012
#define IDC_OPT_UNITS_PICA          3013
#define IDC_OPT_WRAP_NONE           3020
#define IDC_OPT_WRAP_WINDOW         3021
#define IDC_OPT_WRAP_RULER          3022
#define IDC_OPT_RULER               3030
#define IDC_OPT_STATUSBAR           3031
#define IDC_OPT_RTFDEFAULT          3032

// IDD_GOTO
#define IDC_GOTO_PAGE               3040

// IDD_ABOUT
// 3071, not 3070: IDC_EDIT already owns 3070. Control IDs in different dialogs
// need not be unique, but this header promises disjoint ranges and keeping that
// promise is what lets a duplicate be found by grep rather than by a misbehaving
// dialog.
#define IDC_ABOUT_TEXT              3071

// IDD_PRINTPREVIEW
#define IDC_PP_PREVIEW              3050
#define IDC_PP_ZOOMIN               3051
#define IDC_PP_ZOOMOUT              3052
#define IDC_PP_NEXTPAGE             3053
#define IDC_PP_PREVPAGE             3054
#define IDC_PP_CLOSE                3055
#define IDC_PP_PAGELABEL            3056

// Zoom trackbar on the status bar (not a dialog, but shares the control space)
#define IDC_ZOOM_TRACKBAR           3060
#define IDC_ZOOM_LABEL              3061

// The RichEdit control's child ID. Non-zero on purpose: EN_CHANGE from a
// RichEdit arrives as WM_COMMAND with the control ID in LOWORD(wParam), and an
// ID of 0 is indistinguishable from a menu command with ID 0.
#define IDC_EDIT                    3070

// ----------------------------------------------------------------------------
//  Status-bar pane indices
// ----------------------------------------------------------------------------
#define STATUSBAR_PANE_MESSAGE      0
#define STATUSBAR_PANE_POSITION     1
#define STATUSBAR_PANE_ZOOM         2   // hosts the trackbar, not text
#define STATUSBAR_PANE_COUNT        3
