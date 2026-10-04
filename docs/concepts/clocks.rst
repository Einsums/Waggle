..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

======
Clocks
======

Events are stamped with the CPU's own counter, read raw, and converted to time by the consumer, keeping ``steady_clock::now()`` (about 14 ns on macOS) off the hot path:

* **arm64** (outside MSVC): the generic timer's virtual count, ``cntvct_el0``, at the frequency ``cntfrq_el0`` reports (1 GHz on an M4, 24 MHz on M1 to M3).
* **x86-64 with an invariant TSC**: ``rdtsc``, whose frequency is measured once against ``steady_clock``.
* **Anything else**: ``steady_clock`` itself.

Times in reports and the viewer are milliseconds since the program started.

Device times
============

Device work arrives with the device's own times, converted by whoever measured them to the host's steady clock (``waggle_steady_ns``), and placed on the same timeline as the host's zones.
Metal's command buffer times are already on the host's clock; the Metal helper converts them by sampling both clocks.
CUDA and HIP events would be converted the same way, from a pair of readings taken together.

Across processes
================

Ticks are comparable only within one process.
Sessions from different processes are compared by their trees, not their timelines.
