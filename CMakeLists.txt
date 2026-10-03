#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# Waggle, the profiler Einsums and other libraries share. A project of its own, so it builds the
# same way here, added by Einsums, as it will from its own repository.

cmake_minimum_required(VERSION 3.24)

project(
  Waggle
  VERSION 0.1.0
  DESCRIPTION "A low-overhead tracing profiler that several libraries in one process share"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT TARGET fmt::fmt)
  find_package(fmt 12...<13 REQUIRED)
endif()
find_package(Threads REQUIRED)

# Always shared, whatever BUILD_SHARED_LIBS says: every library in a process must reach the same
# collector, and a static copy linked into each would give each its own.
add_library(
  waggle SHARED
  src/Abi.cpp
  src/Consumer.cpp
  src/CounterBackend.cpp
  src/Diagnostics.cpp
  src/Duplicates.cpp
  src/Mdns.cpp
  src/Ompt.cpp
  src/Server.cpp
  src/Settings.cpp
  src/Snapshot.cpp
  src/Sources.cpp
  src/Waggle.cpp
)
add_library(Waggle::waggle ALIAS waggle)

target_compile_features(waggle PUBLIC cxx_std_20)
# The zone macros use __VA_OPT__, which MSVC's traditional preprocessor does not know: every
# translation unit that includes the headers needs the conforming one.
target_compile_options(waggle PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
target_include_directories(
  waggle
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(waggle PRIVATE WAGGLE_EXPORTS)
target_link_libraries(waggle PUBLIC fmt::fmt PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(waggle PRIVATE ws2_32 psapi)
endif()
set_target_properties(
  waggle
  PROPERTIES CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
)

include(WaggleInstrument)

# The collector's private headers (rings, events, the consumer, the server), for Waggle's own
# tests. Never installed or exported: everything else reaches the collector through the C
# interface, so that nothing about these layouts becomes something a library depends on.
add_library(waggle_internal INTERFACE)
target_include_directories(waggle_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(waggle_internal INTERFACE waggle)
add_library(Waggle::internal ALIAS waggle_internal)

# ── Python ─────────────────────────────────────────────────────────────────
# The viewer and the report tools are pure Python. waggle._core, the profiler for Python code, is a
# compiled module apiary generates from python/src/Core.hpp; it calls only the C interface.
option(WAGGLE_BUILD_PYTHON "Build waggle._core, the profiler for Python code (needs apiary and pybind11)" OFF)

# A project that builds Waggle beside its own Python package stages Waggle's next to it, so both
# import from the build tree, and installs it there too. Building waggle._core needs a staged
# package to put it in, so it stages one in this build tree unless told where.
set(WAGGLE_PYTHON_STAGING_DIR
    ""
    CACHE PATH "Directory to copy the waggle Python package into at configure time; empty for none"
)
set(WAGGLE_PYTHON_INSTALL_DIR
    ""
    CACHE PATH "Install destination of the waggle Python package, relative to the prefix; empty for none"
)
set(_waggle_python_dir "${WAGGLE_PYTHON_STAGING_DIR}")
if(WAGGLE_BUILD_PYTHON AND NOT _waggle_python_dir)
  set(_waggle_python_dir "${CMAKE_CURRENT_BINARY_DIR}/python")
endif()
file(
  GLOB_RECURSE _waggle_python_files CONFIGURE_DEPENDS
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/python
  ${CMAKE_CURRENT_SOURCE_DIR}/python/waggle/*.py ${CMAKE_CURRENT_SOURCE_DIR}/python/waggle/*.tcss
)
if(_waggle_python_dir)
  # configure_file follows the source's timestamp, so an edit lands at the next build.
  foreach(_file IN LISTS _waggle_python_files)
    configure_file(python/${_file} ${_waggle_python_dir}/${_file} COPYONLY)
  endforeach()
endif()
if(WAGGLE_PYTHON_INSTALL_DIR)
  install(
    DIRECTORY python/waggle
    DESTINATION ${WAGGLE_PYTHON_INSTALL_DIR}
    FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.tcss"
    PATTERN "__pycache__" EXCLUDE
  )
endif()

if(WAGGLE_BUILD_PYTHON)
  if(NOT COMMAND apiary_add_bindings)
    find_package(Apiary 1.1...<2 CONFIG REQUIRED)
  endif()
  if(NOT COMMAND pybind11_add_module)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)
  endif()
  apiary_detect_toolchain(CXX_STANDARD 20)

  # What apiary parses Core.hpp with: Waggle's public headers and the annotation macros.
  add_library(waggle_python_api INTERFACE)
  target_include_directories(waggle_python_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/python/src)
  target_link_libraries(waggle_python_api INTERFACE waggle apiary::annotations)

  set(_waggle_gen "${CMAKE_CURRENT_BINARY_DIR}/python-gen")
  apiary_add_bindings(
    BINDING
    HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/python/src/Core.hpp"
    SOURCE_INCLUDES Core.hpp
    REGISTER_FUNCTION waggle_register_core
    DEPENDS_TARGETS waggle_python_api
    OUTPUT_DIR "${_waggle_gen}"
    OUTPUT_NAME waggle_core
    CXX_STANDARD 20
    OUT_BINDING _waggle_binding
    OUT_STUB _waggle_stub
  )
  apiary_aggregate_extension(
    NAME waggle_core
    MAIN "${CMAKE_CURRENT_SOURCE_DIR}/python/src/module.cpp"
    BINDINGS ${_waggle_binding}
    MODULES core
    REGISTER_PREFIX waggle_register_
    MODULES_HEADER "${_waggle_gen}/include/waggle/Modules.hpp"
    MODULES_INCLUDE_DIR "${_waggle_gen}/include"
  )
  target_link_libraries(waggle_core PRIVATE waggle_python_api)

  # waggle/_core beside the staged package's Python files, under every configuration's name.
  set_target_properties(waggle_core PROPERTIES OUTPUT_NAME _core LIBRARY_OUTPUT_DIRECTORY "${_waggle_python_dir}/waggle")
  foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_config}" _config)
    set_target_properties(waggle_core PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_config} "${_waggle_python_dir}/waggle")
  endforeach()
  if(WIN32)
    # Python does not search PATH for an extension's DLLs: waggle.dll goes beside _core.pyd. A
    # process loads one waggle.dll by name, so a second file of the same name is never a second
    # collector.
    add_custom_command(
      TARGET waggle_core
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:waggle> "${_waggle_python_dir}/waggle"
    )
  endif()

  if(WAGGLE_PYTHON_INSTALL_DIR)
    install(TARGETS waggle_core LIBRARY DESTINATION ${WAGGLE_PYTHON_INSTALL_DIR}/waggle RUNTIME DESTINATION
                                                                                           ${WAGGLE_PYTHON_INSTALL_DIR}/waggle
    )
    file(RELATIVE_PATH _waggle_core_to_lib "${CMAKE_INSTALL_PREFIX}/${WAGGLE_PYTHON_INSTALL_DIR}/waggle"
         "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}"
    )
    if(APPLE)
      set_property(TARGET waggle_core APPEND PROPERTY INSTALL_RPATH "@loader_path/${_waggle_core_to_lib}")
    elseif(WIN32)
      install(FILES $<TARGET_FILE:waggle> DESTINATION ${WAGGLE_PYTHON_INSTALL_DIR}/waggle)
    else()
      set_property(TARGET waggle_core APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/${_waggle_core_to_lib}")
    endif()
    install(
      FILES ${_waggle_stub}
      DESTINATION ${WAGGLE_PYTHON_INSTALL_DIR}/waggle
      RENAME _core.pyi
    )
  endif()
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
# On when Waggle is the project being built; a project that adds Waggle decides for itself, and may
# set a prefix that fits its own test names.
option(WAGGLE_BUILD_TESTS "Build Waggle's tests" ${PROJECT_IS_TOP_LEVEL})
set(WAGGLE_TEST_PREFIX
    "Waggle."
    CACHE STRING "Prefix of every Waggle test's name"
)
if(WAGGLE_BUILD_TESTS)
  if(NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 REQUIRED)
  endif()
  # Some Catch2 builds, conda-forge's for Windows among them, leave out the std::string_view
  # StringMaker; a test printing a string_view then fails to link. Match the build.
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state()
  set(CMAKE_REQUIRED_LIBRARIES Catch2::Catch2)
  set(CMAKE_REQUIRED_QUIET TRUE)
  check_cxx_source_compiles(
    "#include <catch2/catch_tostring.hpp>
     #include <string_view>
     int main() { return static_cast<int>(Catch::Detail::stringify(std::string_view{\"x\"}).size()); }"
    WAGGLE_CATCH2_HAS_STRING_VIEW_STRINGMAKER
  )
  cmake_pop_check_state()
  if(NOT WAGGLE_CATCH2_HAS_STRING_VIEW_STRINGMAKER)
    set_property(TARGET Catch2::Catch2 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS CATCH_CONFIG_NO_CPP17_STRING_VIEW)
  endif()
  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
  endif()
  add_subdirectory(tests)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
install(
  TARGETS waggle
  EXPORT WaggleTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(_waggle_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/Waggle)
install(
  EXPORT WaggleTargets
  NAMESPACE Waggle::
  DESTINATION ${_waggle_cmake_dir}
)
# The build tree is a usable package too, for a project that builds Waggle and exports its own
# build tree.
export(
  EXPORT WaggleTargets
  NAMESPACE Waggle::
  FILE ${CMAKE_CURRENT_BINARY_DIR}/WaggleTargets.cmake
)
configure_file(cmake/WaggleInstrument.cmake ${CMAKE_CURRENT_BINARY_DIR}/WaggleInstrument.cmake COPYONLY)
configure_package_config_file(
  cmake/WaggleConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/WaggleConfig.cmake INSTALL_DESTINATION ${_waggle_cmake_dir}
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/WaggleConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/WaggleConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/WaggleConfigVersion.cmake
              cmake/WaggleInstrument.cmake DESTINATION ${_waggle_cmake_dir}
)
