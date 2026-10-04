#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# Waggle's tests link the collector and Catch2's main, nothing else, so they build and run without
# any library that uses Waggle. `waggle_tests` builds them all.

set(_waggle_unit_tests
    Annotations
    Consumer
    DeviceSpans
    Domains
    Producer
    RingBuffer
    Server
    Settings
    Sites
    Snapshot
    Sources
    StringTable
    Trace
)

add_custom_target(waggle_tests)

foreach(_name IN LISTS _waggle_unit_tests)
  set(_target waggle_${_name}_test)
  add_executable(${_target} unit/${_name}.cpp)
  # Waggle::internal: the tests reach the collector's private headers.
  target_link_libraries(${_target} PRIVATE Waggle::internal Catch2::Catch2WithMain)
  if(WIN32)
    target_link_libraries(${_target} PRIVATE ws2_32) # Sockets.hpp
  endif()
  add_dependencies(waggle_tests ${_target})

  add_test(NAME ${WAGGLE_TEST_PREFIX}${_name} COMMAND ${_target})
  # No report at exit: it would bury the test output.
  set_tests_properties(${WAGGLE_TEST_PREFIX}${_name} PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300)
endforeach()

# Which thread is called "main" depends on which records first, so this runs in a process of its own.
add_test(NAME ${WAGGLE_TEST_PREFIX}MainThreadName COMMAND waggle_Producer_test "[main-thread]")
set_tests_properties(${WAGGLE_TEST_PREFIX}MainThreadName PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300 SKIP_RETURN_CODE 4)

# Hardware counters: the backends directly, then the source with it asked for in the environment.
add_executable(waggle_Counters_test unit/Counters.cpp)
target_link_libraries(waggle_Counters_test PRIVATE Waggle::internal Catch2::Catch2WithMain)
add_dependencies(waggle_tests waggle_Counters_test)
add_test(NAME ${WAGGLE_TEST_PREFIX}Counters COMMAND waggle_Counters_test "[backend]")
# A virtual machine with no counters skips the backend test (Catch2's exit code 4), as CI's Macs do.
set_tests_properties(${WAGGLE_TEST_PREFIX}Counters PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300 SKIP_RETURN_CODE 4)
add_test(NAME ${WAGGLE_TEST_PREFIX}CountersSource COMMAND waggle_Counters_test "[source]")
set_tests_properties(${WAGGLE_TEST_PREFIX}CountersSource PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false;WAGGLE_SOURCES=counters" TIMEOUT 300)

# The Perfetto trace, read by Perfetto's own trace_processor (the perfetto package, which downloads
# it on first use). WAGGLE_REQUIRE_PERFETTO_TEST makes its absence an error, as CI does.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import perfetto.trace_processor"
    RESULT_VARIABLE _waggle_no_perfetto
    OUTPUT_QUIET ERROR_QUIET
  )
endif()
if(Python3_Interpreter_FOUND AND _waggle_no_perfetto EQUAL 0)
  add_test(NAME ${WAGGLE_TEST_PREFIX}Perfetto COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/perfetto_trace.py
                                                    $<TARGET_FILE:waggle_Trace_test>
  )
  set_tests_properties(${WAGGLE_TEST_PREFIX}Perfetto PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "perfetto: [0-9]+ zones as expected")
elseif(WAGGLE_REQUIRE_PERFETTO_TEST)
  message(FATAL_ERROR "Waggle: the Perfetto test needs Python with the perfetto package (pip install perfetto)")
else()
  message(STATUS "Waggle: the Perfetto test needs Python with the perfetto package; not run")
endif()

# The energy source, asked for in the environment; where there is no per-thread energy it checks
# the source says why, and skips (Catch2's exit code 4).
add_executable(waggle_Energy_test unit/Energy.cpp)
target_link_libraries(waggle_Energy_test PRIVATE Waggle::internal Catch2::Catch2WithMain)
add_dependencies(waggle_tests waggle_Energy_test)
add_test(NAME ${WAGGLE_TEST_PREFIX}Energy COMMAND waggle_Energy_test)
set_tests_properties(${WAGGLE_TEST_PREFIX}Energy PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false;WAGGLE_SOURCES=energy" TIMEOUT 300 SKIP_RETURN_CODE 4)

# The OpenMP source against a runtime with OMPT: LLVM's libomp, which also serves GCC-compiled code
# through its GOMP entry points. So the test compiles with the compiler's OpenMP flag but links
# libomp itself, never GCC's libgomp, which has no OMPT. MSVC's runtime has none either.
find_package(OpenMP COMPONENTS CXX)
find_library(WAGGLE_LIBOMP NAMES omp libomp)
if(OpenMP_CXX_FOUND AND WAGGLE_LIBOMP AND NOT MSVC)
  add_executable(waggle_Ompt_test unit/Ompt.cpp)
  separate_arguments(_waggle_omp_flags NATIVE_COMMAND "${OpenMP_CXX_FLAGS}")
  target_compile_options(waggle_Ompt_test PRIVATE ${_waggle_omp_flags})
  target_include_directories(waggle_Ompt_test PRIVATE ${OpenMP_CXX_INCLUDE_DIRS})
  target_link_libraries(waggle_Ompt_test PRIVATE Waggle::internal Catch2::Catch2WithMain ${WAGGLE_LIBOMP})
  add_dependencies(waggle_tests waggle_Ompt_test)
  add_test(NAME ${WAGGLE_TEST_PREFIX}Ompt COMMAND waggle_Ompt_test "[active]")
  set_tests_properties(${WAGGLE_TEST_PREFIX}Ompt PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false;WAGGLE_SOURCES=openmp" TIMEOUT 300)
  add_test(NAME ${WAGGLE_TEST_PREFIX}OmptMissed COMMAND waggle_Ompt_test "[missed]")
  set_tests_properties(${WAGGLE_TEST_PREFIX}OmptMissed PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300)
elseif(WAGGLE_REQUIRE_OMPT_TEST)
  # CI asks for the test, so a missing runtime fails there rather than leaving it unbuilt.
  message(FATAL_ERROR "The OpenMP source's test needs OpenMP and LLVM's libomp (found OpenMP: ${OpenMP_CXX_FOUND}, libomp: ${WAGGLE_LIBOMP})")
else()
  message(STATUS "Waggle: the OpenMP source's test needs OpenMP and LLVM's libomp; not built")
endif()

# The signposts source, recorded with Instruments' command line, where there is one.
if(APPLE)
  add_executable(waggle_SignpostsProgram unit/SignpostsProgram.cpp)
  target_link_libraries(waggle_SignpostsProgram PRIVATE Waggle::waggle)
  add_dependencies(waggle_tests waggle_SignpostsProgram)
  execute_process(
    COMMAND xcrun -f xctrace
    RESULT_VARIABLE _waggle_no_xctrace
    OUTPUT_QUIET ERROR_QUIET
  )
  find_package(Python3 COMPONENTS Interpreter)
  if(_waggle_no_xctrace EQUAL 0 AND Python3_Interpreter_FOUND)
    add_test(NAME ${WAGGLE_TEST_PREFIX}Signposts COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/signposts.py
                                                         $<TARGET_FILE:waggle_SignpostsProgram>
    )
    # The script's own verdict, so a script that checks nothing (or is empty) cannot pass.
    set_tests_properties(
      ${WAGGLE_TEST_PREFIX}Signposts PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "signposts: [0-9]+ intervals as expected"
    )
  else()
    message(STATUS "Waggle: the signposts test needs xctrace (Xcode) and Python; not run")
  endif()
endif()

# Metal device zones on the machine's GPU; a runner without one skips.
if(APPLE)
  enable_language(OBJCXX)
  add_executable(waggle_Metal_test unit/Metal.mm)
  target_compile_options(waggle_Metal_test PRIVATE -fobjc-arc)
  target_link_libraries(waggle_Metal_test PRIVATE Waggle::internal Catch2::Catch2WithMain "-framework Metal" "-framework Foundation")
  add_dependencies(waggle_tests waggle_Metal_test)
  add_test(NAME ${WAGGLE_TEST_PREFIX}Metal COMMAND waggle_Metal_test)
  set_tests_properties(${WAGGLE_TEST_PREFIX}Metal PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300 SKIP_RETURN_CODE 4)
endif()

# The C interface from C: the header must compile as strict C, without the C++ layer.
enable_language(C)
add_executable(waggle_CApi_test unit/CApi.c)
target_link_libraries(waggle_CApi_test PRIVATE Waggle::waggle)
set_target_properties(waggle_CApi_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(waggle_CApi_test PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
add_dependencies(waggle_tests waggle_CApi_test)
add_test(NAME ${WAGGLE_TEST_PREFIX}CApi COMMAND waggle_CApi_test)
set_tests_properties(${WAGGLE_TEST_PREFIX}CApi PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300)

# A plain program, not a Catch2 case: what it checks happens after main returns.
add_executable(waggle_ExitWithoutFinalize_test unit/ExitWithoutFinalize.cpp)
target_link_libraries(waggle_ExitWithoutFinalize_test PRIVATE Waggle::waggle)
if(WIN32)
  target_link_libraries(waggle_ExitWithoutFinalize_test PRIVATE ws2_32) # Sockets.hpp
endif()
add_dependencies(waggle_tests waggle_ExitWithoutFinalize_test)
# It also asks, through the environment, for the outputs a finalize would write; they must be
# written at exit all the same. Old ones are removed first, and checked once the program has ended.
set(_exit_report ${CMAKE_CURRENT_BINARY_DIR}/exit_without_finalize_report.txt)
set(_exit_session ${CMAKE_CURRENT_BINARY_DIR}/exit_without_finalize_session.json)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeClean COMMAND ${CMAKE_COMMAND} -E rm -f ${_exit_report} ${_exit_session})
set_tests_properties(${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeClean PROPERTIES FIXTURES_SETUP waggle_exit_clean)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutFinalize COMMAND waggle_ExitWithoutFinalize_test)
set_tests_properties(
  ${WAGGLE_TEST_PREFIX}ExitWithoutFinalize
  PROPERTIES ENVIRONMENT "WAGGLE_REPORT=true;WAGGLE_REPORT_FILE=${_exit_report};WAGGLE_SAVE=${_exit_session}"
             # What the exit path could not do, it says on stderr; fail on it, so the output shows.
             FAIL_REGULAR_EXPRESSION "waggle: could not"
             FIXTURES_REQUIRED waggle_exit_clean
             FIXTURES_SETUP waggle_exit_outputs
             TIMEOUT 300
)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeReport
         COMMAND ${CMAKE_COMMAND} -DFILE=${_exit_report} "-DEXPECT=exit: left for the drain" -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_contains.cmake
)
set_tests_properties(${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeReport PROPERTIES FIXTURES_REQUIRED waggle_exit_outputs)
# And a program that never opens a zone writes no report at exit, though reports are on by default.
set(_unused_report ${CMAKE_CURRENT_BINARY_DIR}/exit_without_zones_report.txt)
add_executable(waggle_ExitWithoutZones_test unit/ExitWithoutZones.cpp)
target_link_libraries(waggle_ExitWithoutZones_test PRIVATE Waggle::waggle)
add_dependencies(waggle_tests waggle_ExitWithoutZones_test)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutZonesClean COMMAND ${CMAKE_COMMAND} -E rm -f ${_unused_report})
set_tests_properties(${WAGGLE_TEST_PREFIX}ExitWithoutZonesClean PROPERTIES FIXTURES_SETUP waggle_unused_clean)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutZones COMMAND waggle_ExitWithoutZones_test)
set_tests_properties(
  ${WAGGLE_TEST_PREFIX}ExitWithoutZones PROPERTIES ENVIRONMENT "WAGGLE_REPORT_FILE=${_unused_report}" FIXTURES_REQUIRED
                                                   waggle_unused_clean FIXTURES_SETUP waggle_unused_ran TIMEOUT 300
)
add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutZonesNoReport COMMAND ${CMAKE_COMMAND} -DFILE=${_unused_report} -P
                                                                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_absent.cmake
)
set_tests_properties(${WAGGLE_TEST_PREFIX}ExitWithoutZonesNoReport PROPERTIES FIXTURES_REQUIRED waggle_unused_ran)

add_test(NAME ${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeSession
         COMMAND ${CMAKE_COMMAND} -DFILE=${_exit_session} "-DEXPECT=exit: left for the drain" -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_contains.cmake
)
set_tests_properties(${WAGGLE_TEST_PREFIX}ExitWithoutFinalizeSession PROPERTIES FIXTURES_REQUIRED waggle_exit_outputs)

# A second, separately built copy of the collector, for the test that loads two at once. Its sources
# and settings are the collector's own; only the file differs.
get_target_property(_waggle_sources waggle SOURCES)
list(TRANSFORM _waggle_sources PREPEND ${PROJECT_SOURCE_DIR}/)
add_library(waggle_second_copy SHARED EXCLUDE_FROM_ALL ${_waggle_sources})
target_compile_features(waggle_second_copy PUBLIC cxx_std_20)
target_include_directories(waggle_second_copy PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(waggle_second_copy PRIVATE WAGGLE_EXPORTS)
target_link_libraries(waggle_second_copy PRIVATE fmt::fmt Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(waggle_second_copy PRIVATE ws2_32 psapi)
endif()
target_compile_options(waggle_second_copy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
set_target_properties(waggle_second_copy PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_executable(waggle_Duplicates_test unit/Duplicates.cpp)
target_link_libraries(waggle_Duplicates_test PRIVATE Waggle::internal Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
target_compile_definitions(waggle_Duplicates_test PRIVATE WAGGLE_SECOND_COPY_PATH="$<TARGET_FILE:waggle_second_copy>")
add_dependencies(waggle_Duplicates_test waggle_second_copy)
add_dependencies(waggle_tests waggle_Duplicates_test)
add_test(NAME ${WAGGLE_TEST_PREFIX}Duplicates COMMAND waggle_Duplicates_test)
set_tests_properties(${WAGGLE_TEST_PREFIX}Duplicates PROPERTIES ENVIRONMENT "WAGGLE_REPORT=false" TIMEOUT 300)

# The viewer's tests, run from the source tree. They need pytest; the app's need Textual too, and
# skip themselves without it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import pytest"
    RESULT_VARIABLE _waggle_no_pytest
    OUTPUT_QUIET ERROR_QUIET
  )
  if(_waggle_no_pytest EQUAL 0)
    # With waggle._core built, the staged package, which has it; otherwise the sources.
    set(_waggle_python_test_env "")
    if(WAGGLE_BUILD_PYTHON)
      set(_waggle_test_python_path "${_waggle_python_dir}")
      set(_waggle_python_test_env ";WAGGLE_REQUIRE_CORE=1")
    else()
      set(_waggle_test_python_path "${PROJECT_SOURCE_DIR}/python")
    endif()
    add_test(NAME ${WAGGLE_TEST_PREFIX}ViewerPython COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
                                                            ${CMAKE_CURRENT_SOURCE_DIR}/python
    )
    set_tests_properties(
      ${WAGGLE_TEST_PREFIX}ViewerPython PROPERTIES ENVIRONMENT "PYTHONPATH=${_waggle_test_python_path}${_waggle_python_test_env}" LABELS PYTHON TIMEOUT 600
    )
  else()
    message(STATUS "Waggle: pytest not found; the viewer's tests are not registered")
  endif()
endif()

# What one zone costs, whole and in pieces. Not a test: run it by hand, paired against another build,
# on a quiet machine on mains power.
add_executable(waggle_ZoneCost_benchmark EXCLUDE_FROM_ALL performance/ZoneCost.cpp)
target_link_libraries(waggle_ZoneCost_benchmark PRIVATE Waggle::internal Catch2::Catch2WithMain)
add_custom_target(waggle_benchmarks DEPENDS waggle_ZoneCost_benchmark)

# Windows has no run path: each test finds waggle.dll through PATH.
if(WIN32)
  get_property(
    _waggle_tests
    DIRECTORY
    PROPERTY TESTS
  )
  foreach(_test IN LISTS _waggle_tests)
    set_property(
      TEST ${_test}
      APPEND
      PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:waggle>"
    )
  endforeach()
endif()
