#----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
#----------------------------------------------------------------------------------------------

# `waggle_docs` builds the HTML documentation into <build>/docs/html. The pages are copied into the
# build tree, the C and C++ reference is generated beside them from the headers by apiary, and
# Sphinx runs with the built waggle package on the path for the Python reference. Warnings are
# errors and every cross-reference must resolve.

if(NOT WAGGLE_BUILD_PYTHON)
  message(FATAL_ERROR "WAGGLE_BUILD_DOCS needs WAGGLE_BUILD_PYTHON: the references come from the Python package and from apiary")
endif()
find_program(WAGGLE_SPHINX_BUILD NAMES sphinx-build REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_docs_source ${CMAKE_CURRENT_BINARY_DIR}/source)
set(_docs_html ${CMAKE_CURRENT_BINARY_DIR}/html)

add_custom_target(
  waggle_docs
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${_docs_source}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR} ${_docs_source}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${_docs_source}/CMakeLists.txt
  COMMAND
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/cpp_reference.py --source-dir ${PROJECT_SOURCE_DIR} --build-dir
    ${PROJECT_BINARY_DIR} --tool $<TARGET_FILE:apiary::apiary> --scripts ${APIARY_SCRIPTS_DIR} --out-dir
    ${_docs_source}/reference/api
  COMMAND
    ${CMAKE_COMMAND} -E env PYTHONPATH=${_waggle_python_dir} WAGGLE_VERSION=${PROJECT_VERSION} ${WAGGLE_SPHINX_BUILD} -E -W -n
    --keep-going -q -b html ${_docs_source} ${_docs_html}
  DEPENDS waggle_core
  COMMENT "Building the documentation into ${_docs_html}"
  VERBATIM
)
