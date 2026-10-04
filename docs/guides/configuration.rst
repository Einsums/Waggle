..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=============
Configuration
=============

Every setting has a default, can come from the environment, and can be set by a library or program while it runs.
:doc:`/reference/settings` lists them all; this page explains how they combine.

Who decides
===========

Settings come in three layers, highest first:

1. **Set explicitly** by a library or program, through ``waggle::configure`` (C++), ``waggle_config_set`` (C) or ``waggle.configure`` (Python).
2. **The environment**: the ``WAGGLE_*`` variables, read when the collector starts.
3. **The defaults**.

Libraries share one collector, so when two set the same setting to different values, the first keeps it.
The second is reported as a diagnostic, never applied, and no library's choice changes under it.
A library should therefore pass only what its user actually set, not its own defaults.

``waggle::override_settings`` sets a value whoever set it before; it exists for tests and tools that must restore a value, not for libraries.

From the environment
====================

.. code-block:: console

    $ WAGGLE_SERVER=1 WAGGLE_SOURCES=counters,openmp WAGGLE_REPORT_FILE=run.txt ./myprogram

Booleans accept ``1``, ``true``, ``on``, ``yes`` and ``0``, ``false``, ``off``, ``no``, in any case.
A value that does not parse is skipped with a diagnostic.

From code
=========

.. code-block:: cpp

    waggle::configure({.server = true, .port = 19300, .sources = "counters"});
    auto const s = waggle::settings();      // the settings in force

.. code-block:: python

    import waggle
    waggle.configure({"server": "true", "port": "19300"})   # names and values as text
    waggle.setting("port")                                   # "19300"

``waggle_config_set`` and Python's ``configure`` return how many settings they refused because another library set them first; C++'s ``configure`` reports each refusal as a diagnostic.

Settings that act once
======================

Most settings act as soon as they change.
Three are read at a particular moment, and the source's status says so when a request comes too late:

* ``sources=openmp``: the OpenMP runtime asks for a tool once, when it starts.
* ``sources=counters`` and ``sources=signposts``: a thread counts, or emits, if the source was asked for when that thread first recorded.

Set these in the environment, or configure Waggle before the program's first OpenMP call and before it starts threads.

Diagnostics
===========

Waggle reports its own problems (a port it could not bind, a refused setting, a duplicate collector) as diagnostics, on standard error by default.
A library with a logger routes them there with ``waggle::set_diagnostic_handler``.
