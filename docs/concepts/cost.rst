..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

====================
What a zone costs
====================

Measured on an Apple M4 by the ``waggle_ZoneCost_benchmark`` target, median of repeated runs:

.. list-table::
    :header-rows: 1
    :widths: 60 40

    * - Zone
      - Cost
    * - Recording off (global or the domain's switch)
      - under 1 ns
    * - Recorded, nothing inside
      - about 6 ns
    * - Recorded, with the ``counters`` source on
      - about 200 ns
    * - Python ``with waggle.Zone(...)``
      - about 190 ns

Why it is cheap
===============

* A site is registered once (a function-local static), so entering a zone passes integers, not strings.
* The switch a zone checks is one combined word per domain (the global switch and the domain's), cached by the site: one load and a branch.
* Time is the CPU's counter read directly (``cntvct_el0`` on arm64, ``rdtsc`` on x86-64), not ``steady_clock``.
* A zone with nothing inside is written as one event when it closes: its opening is held in the thread's channel until something happens inside it.
* The ring is per thread, so writing an event is a store and a relaxed counter update, with no atomic read-modify-write.

What the report says it cost
============================

The report ends with the profiler's own overhead: what one zone's push and pop cost, measured once at start-up, times how many there were.
The report's times include that overhead; for zones of a few microseconds or more it is negligible.

Keeping it cheap
================

* Leave zones in released code; switch them off at run time, or compile them out with ``WAGGLE_DISABLE``.
* Keep fine-grained zones under ``WAGGLE_ZONE_DETAIL``.
* Prefer a literal name with format arguments to ``WAGGLE_ZONE_DYNAMIC``, which interns the name under a lock on every entry.
* Turn on the ``counters`` source for coarse zones only.
