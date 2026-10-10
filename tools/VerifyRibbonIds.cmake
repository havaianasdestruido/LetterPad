# ============================================================================
#  VerifyRibbonIds.cmake
#
#  Checks that the hand-written src/ribbon/RibbonIDs.h and the UICC-generated
#  header agree on every ID.
#
#  WHY THIS EXISTS
#  Ribbon.xml carries explicit Id="N" attributes and RibbonIDs.h repeats them as
#  #defines. UICC's generated header is the ground truth for what the compiler
#  actually saw. If the two disagree, a ribbon control is bound to the wrong
#  handler, and the symptom is a button that does something else entirely -- one
#  of the least diagnosable failures in ribbon programming, because nothing
#  errors at build time.
#
#  Invoked from CMakeLists.txt as:
#    cmake -DCOMMITTED=<path> -DGENERATED=<path> -P VerifyRibbonIds.cmake
#
#  Value drift and IDs missing from the markup are FATAL: a control bound to the
#  wrong handler fails silently at runtime, so a green build that hides it is
#  worse than a red one. IDs UICC emitted that the committed headers do not know
#  about are reported informationally, because UICC legitimately produces helper
#  symbols (gallery internals) that no handler needs.
# ============================================================================

if(NOT EXISTS "${COMMITTED}")
  message(FATAL_ERROR "VerifyRibbonIds: committed header not found: ${COMMITTED}")
endif()

# The generated header only exists after UICC has run. If it is missing, the
# ribbon was skipped and there is nothing to verify.
if(NOT EXISTS "${GENERATED}")
  message(STATUS "VerifyRibbonIds: no generated header; ribbon skipped, nothing to verify")
  return()
endif()

file(STRINGS "${COMMITTED}" committed_lines REGEX "#define[ \t]+ID_[A-Z0-9_]+[ \t]+[0-9]+")
file(STRINGS "${GENERATED}" generated_lines REGEX "#define[ \t]+ID_[A-Z0-9_]+[ \t]+[0-9]+")

# Parse "<name> <value>" pairs from each file into flat lists, then into a map.
function(parse_ids lines out_map_name out_count_name)
  set(_map "")
  set(_count 0)
  foreach(line IN LISTS lines)
    string(REGEX MATCH "#define[ \t]+([A-Z0-9_]+)[ \t]+([0-9]+)" _m "${line}")
    if(_m)
      list(APPEND _map "${CMAKE_MATCH_1}=${CMAKE_MATCH_2}")
      math(EXPR _count "${_count}+1")
    endif()
  endforeach()
  set(${out_map_name} "${_map}" PARENT_SCOPE)
  set(${out_count_name} "${_count}" PARENT_SCOPE)
endfunction()

parse_ids("${committed_lines}" committed_map committed_count)

# The element IDs live in RibbonIDs.h but every ID_CMD_* handler binding lives in
# resource.h. Scanning only the former would miss exactly the drift that matters
# -- a command bound to the wrong handler -- so the command header is parsed too
# and merged into the same map.
if(COMMITTED_CMDS AND EXISTS "${COMMITTED_CMDS}")
  file(STRINGS "${COMMITTED_CMDS}" cmd_lines REGEX "#define[ \t]+ID_[A-Z0-9_]+[ \t]+[0-9]+")
  parse_ids("${cmd_lines}" cmd_map cmd_count)
  list(APPEND committed_map ${cmd_map})
  math(EXPR committed_count "${committed_count}+${cmd_count}")
else()
  message(WARNING "VerifyRibbonIds: COMMITTED_CMDS not supplied; command IDs are NOT being checked")
endif()

parse_ids("${generated_lines}" generated_map generated_count)

message(STATUS "VerifyRibbonIds: committed=${committed_count} generated=${generated_count}")

set(_drift 0)
set(_report "")

# Every ID in the committed header must appear in the generated one with the
# same value. The reverse direction is checked separately, because UICC can emit
# symbols the committed header does not and needs not carry.
foreach(entry IN LISTS committed_map)
  string(REGEX MATCH "^([A-Za-z0-9_]+)=([0-9]+)$" _m "${entry}")
  if(NOT _m)
    continue()
  endif()
  set(_name "${CMAKE_MATCH_1}")
  set(_val  "${CMAKE_MATCH_2}")

  list(FIND committed_map "${_name}=${_val}" _i)   # sanity: present in source list

  set(_found 0)
  foreach(g IN LISTS generated_map)
    string(REGEX MATCH "^(${_name})=([0-9]+)$" _gm "${g}")
    if(_gm)
      set(_found 1)
      set(_gval "${CMAKE_MATCH_2}")
      if(NOT _gval STREQUAL _val)
        set(_drift 1)
        list(APPEND _report "  VALUE DRIFT  ${_name}: committed=${_val} generated=${_gval}")
      endif()
      break()
    endif()
  endforeach()

  if(NOT _found)
    # Present in RibbonIDs.h but UICC never saw it. This is the dangerous case:
    # a #define with no matching markup means a handler branch that can never
    # fire, usually because the control was renamed or deleted in Ribbon.xml.
    set(_drift 1)
    list(APPEND _report "  MISSING IN MARKUP  ${_name}=${_val} is defined in RibbonIDs.h but UICC produced no such ID")
  endif()
endforeach()

# IDs UICC produced that the committed header does not know about. Usually a new
# control added to Ribbon.xml without updating RibbonIDs.h; the control then
# works but cannot be handled, so its handler would have to hard-code a literal.
foreach(g IN LISTS generated_map)
  string(REGEX MATCH "^([A-Za-z0-9_]+)=([0-9]+)$" _gm "${g}")
  if(_gm)
    set(_gname "${CMAKE_MATCH_1}")
    set(_gval  "${CMAKE_MATCH_2}")
    set(_found 0)
    foreach(c IN LISTS committed_map)
      if(c STREQUAL "${_gname}=${_gval}")
        set(_found 1)
        break()
      endif()
    endforeach()
    if(NOT _found)
      list(APPEND _report "  UNHANDLED IN CODE  ${_gname}=${_gval} appears in the generated header but not in RibbonIDs.h")
    endif()
  endif()
endforeach()

# Drift is fatal. A ribbon control bound to the wrong handler produces a button
# that does something else entirely, with no compile or runtime error, so a
# green build that hides it is worse than a red one. The unhandled-symbol report
# below stays informational: UICC legitimately emits symbols (gallery internals)
# that the committed headers need not carry.
if(_drift)
  message(FATAL_ERROR "Ribbon ID drift detected between committed headers and the UICC output:\n${_report}")
else()
  message(STATUS "VerifyRibbonIds: all committed ribbon IDs match the UICC output")
endif()

if(_report AND NOT _drift)
  # Unhandled-but-not-drifted: informational only.
  foreach(line IN LISTS _report)
    message(STATUS "VerifyRibbonIds:${line}")
  endforeach()
endif()
