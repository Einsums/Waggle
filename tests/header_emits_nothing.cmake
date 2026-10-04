#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# Fails if OBJECT (an object file of tests/unit/HeaderOnly.cpp) defines any symbol, a static
# initializer's included: the public headers must leave nothing behind in a file that includes them.
# Usage: cmake -DNM=<nm> -DOBJECT=<object> -P header_emits_nothing.cmake

execute_process(
  COMMAND ${NM} ${OBJECT}
  OUTPUT_VARIABLE _symbols
  RESULT_VARIABLE _failed
)
if(_failed)
  message(FATAL_ERROR "${NM} could not read ${OBJECT}")
endif()
string(REPLACE "\n" ";" _lines "${_symbols}")
set(_defined)
foreach(_line IN LISTS _lines)
  # "<address> <type> <name>" for a definition, "<spaces> U <name>" for a reference. Assemblers'
  # local labels (ltmp0 on Mach-O) mark sections, not code.
  if(_line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) (.+)$")
    set(_type "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_2}")
    if(NOT _name MATCHES "^(ltmp[0-9]+|l_.*|\\..*)$")
      list(APPEND _defined "  ${_type} ${_name}")
    endif()
  endif()
endforeach()
if(_defined)
  list(JOIN _defined "\n" _shown)
  message(FATAL_ERROR "The public headers left symbols in a file that only includes them:\n${_shown}")
endif()
message(STATUS "header-only object defines nothing")
