#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# `waggle_docs` builds the HTML documentation into <build>/docs/html. The pages are copied into the
# build tree, the C and C++ reference is generated beside them from the headers by apiary, and
# Sphinx runs with the built waggle package on the path for the Python reference. Warnings are
# errors and every cross-reference must resolve.
#
# apiary_gen_cpp_docs.py renders the references, one for the C header and one for the C++ headers,
# with the compile flags of the build's own apiary command for waggle._core. Metal.h is
# Objective-C++, which a C++ parse cannot read; the device work guide documents it.

if(NOT WAGGLE_BUILD_PYTHON)
  message(FATAL_ERROR "WAGGLE_BUILD_DOCS needs WAGGLE_BUILD_PYTHON: the references come from the Python package and from apiary")
endif()
find_program(WAGGLE_SPHINX_BUILD NAMES sphinx-build REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_docs_source ${CMAKE_CURRENT_BINARY_DIR}/source)
set(_docs_html ${CMAKE_CURRENT_BINARY_DIR}/html)
set(_docs_api ${CMAKE_CURRENT_BINARY_DIR}/api)

# Everything both references pass the generator; each adds which headers, and its titles.
set(_gen_cpp_docs
    ${Python3_EXECUTABLE}
    ${APIARY_SCRIPTS_DIR}/apiary_gen_cpp_docs.py
    --source-dir
    ${PROJECT_SOURCE_DIR}
    --build-dir
    ${PROJECT_BINARY_DIR}
    --tool
    $<TARGET_FILE:apiary::apiary>
    --layout
    entity
    --flags-from
    waggle_register_core
    --module-name
    waggle
)

add_custom_target(
  waggle_docs
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${_docs_source}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR} ${_docs_source}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${_docs_source}/CMakeLists.txt
  COMMAND
    ${_gen_cpp_docs} --out-dir ${_docs_api}/c --include-dir Waggle/C=include --header-glob *.h --exclude-header
    Waggle/Metal.h --index-label-template api_c --index-title-template "C API" --generated-from "the C header"
  COMMAND
    ${_gen_cpp_docs} --out-dir ${_docs_api}/cpp --include-dir Waggle/CPP=include --header-glob *.hpp
    --index-label-template api_cpp --index-title-template "C++ API" --generated-from "the C++ headers"
  # Each reference is one module, so its pages are the whole of it.
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${_docs_api}/c/rst/Waggle/C ${_docs_source}/reference/api/c
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${_docs_api}/cpp/rst/Waggle/CPP ${_docs_source}/reference/api/cpp
  COMMAND
    ${CMAKE_COMMAND} -E env PYTHONPATH=${_waggle_python_dir} WAGGLE_VERSION=${PROJECT_VERSION} ${WAGGLE_SPHINX_BUILD} -E -W -n
    --keep-going -q -b html ${_docs_source} ${_docs_html}
  DEPENDS waggle_core
  COMMENT "Building the documentation into ${_docs_html}"
  VERBATIM
)
