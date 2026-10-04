..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

===========
Development
===========

Building and testing
====================

.. code-block:: console

    conda env create -f environment.yml && conda activate waggle-dev
    cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWAGGLE_BUILD_PYTHON=ON -DWAGGLE_BUILD_DOCS=ON
    cmake --build build --target all waggle_tests
    ctest --test-dir build -j4

The tests link the collector and Catch2's main and nothing else.
Some need more than the build: the OpenMP source's test needs LLVM's libomp (``WAGGLE_REQUIRE_OMPT_TEST=ON`` makes its absence an error, as CI does), the Metal test a GPU, the signposts test ``xctrace``, and the mDNS discovery test a responder and ``zeroconf`` (``WAGGLE_REQUIRE_MDNS=1`` makes their absence a failure).
``waggle_ZoneCost_benchmark`` measures what a zone costs; it is a benchmark, not a test, and is built only when asked for.

Formatting is enforced by pre-commit: ``pre-commit install``.

Documentation
=============

``-DWAGGLE_BUILD_DOCS=ON`` adds ``waggle_docs``, which builds these pages into ``build/docs/html``:

.. code-block:: console

    cmake --build build --target waggle_docs

The build copies ``docs/`` into the build tree, generates the C and C++ reference there from the headers (``docs/tools/cpp_reference.py`` runs apiary over them), and runs Sphinx with the built ``waggle`` package on the path for the Python reference.
Warnings are errors, and every cross-reference must resolve.

* The C and C++ reference comes from the headers' doc comments (``///`` and ``/** */``); a declaration's comment is its entry.
* The Python reference comes from the docstrings.
* The viewer's keys and the command line's help are generated from the code (``docs/_ext/waggle_docs.py``), so they cannot drift.
* The settings table is written by hand; a test checks it names every setting and environment variable the collector reads.
* Long pages put one sentence per line, so a change reads as the sentences it changed.

CI builds the documentation on every push and publishes ``main``'s to GitHub Pages.

Adding a source
===============

A source with no dependency is built into the collector:

1. Give it a status in ``src/Sources.cpp`` (``off``, ``active``, or why one asked for records nothing) and add it to ``source_statuses``.
2. Decide when it is read: for each thread as it registers (as ``counters`` and ``signposts`` are, through an atomic that ``apply`` refreshes), or once (as the OpenMP runtime asks for a tool once).
3. Record into a domain of its own, so the domain's switch mutes it.
4. Test it end to end against the real thing it instruments, and require that test in CI, so a missing dependency fails rather than skips.

A source tied to a vendor library (CUPTI, PAPI, PMPI) will be a plugin loaded by name; the loader is to be built with the first one.
