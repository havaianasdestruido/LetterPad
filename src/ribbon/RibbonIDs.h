// ============================================================================
//  RibbonIDs.h -- command and element IDs for src/ribbon/Ribbon.xml
//
//  THIS FILE IS HAND-WRITTEN AND COMMITTED. It is the source of truth.
//
//  UICC also generates a header when it compiles Ribbon.xml, but that header is
//  NOT included anywhere. Every Command element in Ribbon.xml carries an
//  explicit Id="N" and Symbol="ID_..." attribute, so UICC emits exactly the
//  defines below and nothing else. The CMake target `verify-ribbon-ids` diffs
//  the generated header against this file and fails the build on drift.
//
//  Why bother hand-writing a file the tool can generate? Because UICC assigns
//  IDs sequentially in document order when Id= is omitted. Adding one control
//  near the top of the markup then renumbers every control below it, which
//  silently breaks every switch statement in RibbonHandler.cpp and invalidates
//  any persisted ribbon customisation. Explicit IDs make the numbering stable
//  for the lifetime of the project.
//
//  ID 0 and ID 1 are reserved by the ribbon framework. Never use them.
// ============================================================================
#pragma once

#include "../resource.h"   // command IDs (ID_CMD_*) shared with menu/accel/toolbar

// ----------------------------------------------------------------------------
//  Ribbon element IDs -- tabs, groups, root, quick-access toolbar.
//  These are NOT commands; they identify layout containers that
//  IUICommandHandler::UpdateProperty is also called for.
// ----------------------------------------------------------------------------
#define ID_RIBBON_ROOT              13
#define ID_RIBBON_QAT               14

// The application menu button (the round "LetterPad" button, top-left).
// It is a layout element that also receives Execute notifications, so it lives
// in the element range rather than with ID_CMD_*.
#define ID_CMD_APPMENU              2

#define ID_TAB_HOME                 3
#define ID_TAB_INSERT               4
#define ID_TAB_VIEW                 5

#define ID_GROUP_CLIPBOARD          6
#define ID_GROUP_FONT               7
#define ID_GROUP_PARAGRAPH          8
#define ID_GROUP_INSERT             9
#define ID_GROUP_EDITING            10
#define ID_GROUP_ZOOM               11
#define ID_GROUP_SHOWHIDE           12

// ----------------------------------------------------------------------------
//  Sanity check. IDs 0 and 1 are reserved by the ribbon framework; UICC will
//  reject markup that assigns them, but the failure is a confusing error about
//  an unnamed command rather than about the number. Catch it here instead.
// ----------------------------------------------------------------------------
static_assert(ID_CMD_NEW      >= 2 && ID_CMD_APPMENU >= 2,
              "Ribbon IDs 0 and 1 are reserved by UICC");
static_assert(ID_CMD_LINESPACING_SINGLE != ID_CMD_LINESPACING_ONEHALF,
              "Duplicate line-spacing command ID");
