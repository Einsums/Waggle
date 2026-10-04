..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

===================
Profiling C++ code
===================

Include ``<Waggle/Waggle.hpp>`` and put a zone where you want time measured:

.. code-block:: cpp

    #include <Waggle/Waggle.hpp>

    namespace mylib {
    WAGGLE_DEFINE_DOMAIN("mylib")

    void solve(int n) {
        WAGGLE_ZONE("solve n={}", n);
        WAGGLE_ANNOTATE("method", "cg");
        for (int it = 0; it < n; ++it) {
            WAGGLE_ZONE("iteration");
            // ...
        }
    }
    } // namespace mylib

Run the program with a report asked for, and Waggle writes one when the program ends:

.. code-block:: console

    $ WAGGLE_REPORT=1 ./myprogram
    $ cat profile.txt

The report is a call tree per thread with each zone's count and inclusive and exclusive time; ``WAGGLE_REPORT_DETAILED=1`` adds minimum, maximum and mean.
A report is written by default, at the end of a program that opened at least one zone; ``WAGGLE_REPORT=0`` turns it off.
:doc:`/guides/configuration` lists every setting.

Zones
=====

``WAGGLE_ZONE(name, args...)``
    Times the rest of the enclosing scope.
    The name is a `fmt <https://fmt.dev>`_ format string and must be a literal; with arguments, the formatted name is cached per call site and argument value, so a name repeated in a loop is formatted once.
    It expands to two declarations, so write it as a statement.

``WAGGLE_ZONE_FUNC()``
    A zone named after the enclosing function.

``WAGGLE_ZONE_DYNAMIC(expr)``
    A zone whose name is computed at run time from any string expression.
    It interns the name under a lock on every entry, so prefer ``WAGGLE_ZONE`` with format arguments.

``WAGGLE_ZONE_DETAIL(name, args...)``, ``WAGGLE_ZONE_DETAIL_FUNC()``
    Zones a library keeps for profiling its own internals.
    They expand to nothing unless ``WAGGLE_DETAIL`` is defined (``waggle_instrument(target DETAIL)`` in CMake).

Zones nest: a zone opened inside another is its child in the tree.
Zones of the same name under the same parent merge into one node, which counts the calls and sums their time.

Annotations and memory
======================

``WAGGLE_ANNOTATE(key, value)`` attaches a string, integer or floating-point value to the innermost open zone; a numeric annotation is summarized as minimum, maximum, mean and last.
The key must be a literal; the value is evaluated only while recording.
``WAGGLE_ANNOTATE_DIMS(key, dims)`` attaches each element of a sequence as ``key.0``, ``key.1``, and so on.

``WAGGLE_MEM_ALLOC(bytes)`` and ``WAGGLE_MEM_FREE(bytes)`` record memory against the open zone; the ``_AT(address, bytes)`` forms also give the block's address, so the viewer can list it until it is freed.
:doc:`/guides/memory` explains what is tracked.

Domains
=======

``WAGGLE_DEFINE_DOMAIN("mylib")``, written once inside your library's namespace, makes every zone written in that namespace belong to the domain ``mylib``.
It works by name lookup from where the zone is written, so a zone in a header belongs to its library wherever the header is compiled.
Domains let reports and the viewer tell libraries apart, and let a program switch one library's zones off while it runs; see :doc:`/guides/libraries`.

Telling Waggle who you are
==========================

A library that uses Waggle says so, which puts its name, version and build in the session and lets the last library to finish write the outputs:

.. code-block:: cpp

    waggle::init({.name = "mylib", .version = "1.2.0", .git_commit = MYLIB_GIT_COMMIT});
    // ...
    waggle::finalize("mylib");

Neither is required: a program that never calls them still records, and still gets its report at exit.

Compiling zones out
===================

Define ``WAGGLE_DISABLE`` before including the header, or use ``waggle_instrument(target DISABLE)``, and every macro expands to nothing.
The library still links Waggle and shares the process's collector, so other libraries' zones are unaffected.
A library whose public headers contain zones sets ``WAGGLE_DISABLE`` or ``WAGGLE_DETAIL`` from a header of its own, so its users' translation units agree with it.

Next
====

* :doc:`viewer`: watch the zones live.
* :doc:`/guides/libraries`: what a library should do so it profiles well beside others.
* :doc:`/reference/api/cpp/index`: every function and macro.
