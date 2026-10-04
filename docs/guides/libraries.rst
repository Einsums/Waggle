..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=========================
Instrumenting a library
=========================

Waggle is built for libraries that profile together: Einsums, Nectar and a program using both record into one collector, and their zones form one tree.
This guide is what a library does so that works.

One collector per process
=========================

The collector is a shared library, ``libwaggle``, behind a C interface.
Every library in the process links the same one, so a zone Einsums opens inside a zone your program opened is its child, on the same thread, in the same tree.

Link Waggle as a shared library, never statically into your own: a static copy in each library would give each its own collector and split the tree.
If a second copy is loaded anyway (a Python extension that bundles its own, say), it finds the first, switches itself off and says so, and the active collector records which libraries' zones are missing.
Reports and the viewer show that warning; see :doc:`/concepts/architecture`.

Domains
=======

Give your library a domain, its name in reports and the viewer, by writing ``WAGGLE_DEFINE_DOMAIN`` once inside its namespace:

.. code-block:: cpp

    namespace mylib {
    WAGGLE_DEFINE_DOMAIN("mylib")
    }

Every zone written inside ``namespace mylib`` (or a namespace nested in it) then belongs to ``mylib``.
The lookup happens where the zone is written, so a zone in one of your public headers belongs to your library even when the header is compiled into someone else's code.

A domain can be switched off while the program runs, which mutes that library's zones and leaves everyone else's:

.. code-block:: cpp

    waggle::set_domain_enabled("mylib", false);

or from the environment, before the program starts: ``WAGGLE_DISABLE_DOMAINS=mylib,otherlib``.
A switched-off zone costs one load and a branch.

Saying who you are
==================

Call ``waggle::init`` once when your library starts, and ``waggle::finalize`` when it shuts down:

.. code-block:: cpp

    waggle::init({.name = "mylib", .version = MYLIB_VERSION, .git_commit = MYLIB_GIT_COMMIT,
                  .git_branch = MYLIB_GIT_BRANCH, .git_dirty = MYLIB_GIT_DIRTY, .build_type = "release"});
    // ...
    waggle::finalize("mylib");

The session then lists your library with its version and build, which matters when comparing runs.
Each ``init`` is counted; the report and session file are written when the last library calls ``finalize``, or at exit if some never do.

Settings from your library
==========================

A library that has its own options can pass them to Waggle with ``waggle::configure``, the way Einsums maps ``--einsums:profile:*``.
Pass only what the user actually set: libraries share one collector, so the first library to set a setting explicitly keeps it, and a later, different value is reported and not applied.
A default your library merely assumes would otherwise take the setting from everyone after it.
See :doc:`configuration`.

What to time
============

* Time what a user would ask about: a public operation, a phase of an algorithm, a call into another library.
  Leave the inner loop to a sampling profiler.
* A zone costs about 6 ns when recorded; one that opens a hundred million times adds a second.
  Put internals under ``WAGGLE_ZONE_DETAIL``, which compiles out unless ``WAGGLE_DETAIL`` is defined.
* Name zones by what they do; put parameters in annotations (``WAGGLE_ANNOTATE("n", n)``) or a cached formatted name (``WAGGLE_ZONE("gemm {}x{}", m, n)``) when the variants matter in the tree.
  Each distinct name is its own node; a parent keeps at most 256 distinct children before the rest fold into ``(other)``.

Answering the viewer
====================

A library can register request handlers the viewer calls (Einsums answers ``get_compute_graphs``), add sections to session files, and publish its own messages; with a viewer plugin, those show as panels.
See :doc:`plugins`.
