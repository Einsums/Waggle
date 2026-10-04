..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

==========
Installing
==========

Waggle is a shared library, ``libwaggle``, with C and C++ headers, a CMake package, and a Python package, ``waggle``, holding the viewer and the Python profiling API.

From conda-forge
================

Waggle will be distributed on conda-forge, and only there; the package is not published yet.
Until it is, build from source as below, or let your project build Waggle as part of its own build (see `Using it from CMake`_).

From source
===========

Waggle builds with CMake 3.24 or later and a C++20 compiler.
Its one required dependency is `fmt <https://fmt.dev>`_ 12; Catch2 is needed for the tests.
The repository's ``environment.yml`` lists everything a full build uses, including the documentation's tools:

.. code-block:: console

    git clone https://github.com/Einsums/Waggle.git
    cd Waggle
    conda env create -f environment.yml
    conda activate waggle-dev
    cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWAGGLE_BUILD_PYTHON=ON
    cmake --build build
    cmake --install build --prefix <prefix>

The options:

``WAGGLE_BUILD_PYTHON``
    Build ``waggle._core``, the compiled module the Python profiling API records through.
    It needs `apiary <https://github.com/Einsums/Apiary>`_, which generates the module from a header, and pybind11.
    On Linux apiary also needs clang's builtin headers from a clang matching its LLVM (``conda install "clang 23.*"`` for apiary 1.1).

``WAGGLE_BUILD_TESTS``
    Build the tests, which ``ctest`` runs; on by default when Waggle is the top-level project.

``WAGGLE_BUILD_DOCS``
    Add the ``waggle_docs`` target, which builds these pages; see :doc:`/development/index`.

``WAGGLE_PYTHON_STAGING_DIR``, ``WAGGLE_PYTHON_INSTALL_DIR``
    Where the ``waggle`` package is copied in the build tree, and where it installs.
    A project that builds Waggle beside its own Python package points both at its own, so the two import together.

Using it from CMake
===================

An installed Waggle is found as a CMake package:

.. code-block:: cmake

    find_package(Waggle 0.1 REQUIRED)
    target_link_libraries(mylib PRIVATE Waggle::waggle)

or, to choose which of a target's zones it records, with the ``waggle_instrument`` helper the package also provides:

.. code-block:: cmake

    waggle_instrument(mylib)            # links Waggle::waggle
    waggle_instrument(mylib DETAIL)     # also records WAGGLE_ZONE_DETAIL zones
    waggle_instrument(mylib DISABLE)    # compiles the target's zones out

A project can also build Waggle as part of its own build, which is how Einsums takes it:

.. code-block:: cmake

    include(FetchContent)
    FetchContent_Declare(
      Waggle
      GIT_REPOSITORY https://github.com/Einsums/Waggle.git
      GIT_TAG <commit>
      FIND_PACKAGE_ARGS 0.1 CONFIG
    )
    FetchContent_MakeAvailable(Waggle)

``FIND_PACKAGE_ARGS`` takes an installed Waggle when there is one, so a process whose libraries come from one installation shares one collector; see :doc:`/concepts/architecture`.

Platforms
=========

Waggle builds and is tested on Linux, macOS and Windows; see :doc:`/guides/platforms` for what differs between them.
