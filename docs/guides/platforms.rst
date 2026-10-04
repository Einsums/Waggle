..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=========
Platforms
=========

Waggle builds and is tested on Linux, macOS and Windows; what differs is what each platform offers.

.. list-table::
    :header-rows: 1
    :widths: 28 24 24 24

    * -
      - Linux
      - macOS
      - Windows
    * - Zones, reports, sessions, server
      - yes
      - yes
      - yes
    * - Finding programs (mDNS)
      - Avahi daemon
      - Bonjour
      - Windows 10 1809+
    * - ``counters`` source
      - perf: cycles, instructions, cache and branch misses
      - XNU: cycles and instructions, P/E split
      - not yet
    * - ``openmp`` source
      - LLVM or Intel runtime
      - LLVM runtime
      - not tested
    * - ``signposts`` source
      - no
      - yes
      - no
    * - Device work
      - C interface
      - Metal, C interface
      - C interface

Linux
=====

Hardware counters need ``kernel.perf_event_paranoid`` at 2 or less; Ubuntu sets 4.
Avahi's client library is loaded only when the server starts, so a machine without Avahi runs the server unadvertised.
OpenMP regions need LLVM's libomp (which runs GCC-compiled code too) or Intel's runtime; GCC's libgomp has no OMPT.

macOS
=====

A virtual Mac, such as a CI runner, has no performance counters; the ``counters`` source says so.
Recording signposts from a CI runner needs ``sudo DevToolsSecurity -enable``.

Windows
=======

The library is ``waggle.dll``; programs and the Python module find it beside them or on ``PATH``.
Waggle writes its report and session file from an exit hook installed when the header is included, because Windows stops a process's other threads before DLLs unload.
The DNS-SD functions are looked up at run time, so an older Windows still loads Waggle.
