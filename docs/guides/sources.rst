..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=======
Sources
=======

A *source* is an optional instrument built into the collector: it records zones, or adds to them, without anyone writing them by hand.
Each is off unless the ``sources`` setting names it:

.. code-block:: console

    $ WAGGLE_SOURCES=counters,openmp ./myprogram

Each source records into a domain of its own (``openmp``, for one), so switching that domain off mutes it while the program runs.
And each reports its state, so a source you asked for that records nothing says why rather than staying silent:

``off``
    Not asked for.
``active``
    Recording; the detail names what it attached to.
``waiting``
    Asked for, but what it instruments has not started (no OpenMP runtime yet, no thread recorded since).
``missed``
    Asked for after what it instruments started; the detail says what to change.
``unavailable``
    This platform or machine cannot do it; the detail says why.

The state is in the report, in the viewer's status bar (with one warning per session), and in ``waggle_source_status``, ``waggle::source_status`` and ``waggle.source_status``.

counters
========

Each zone's hardware counters, read as it opens and as it closes:

* **Linux**: cycles, instructions, cache misses and branch misses, through ``perf_event_open``, read as one group in one system call.
  ``kernel.perf_event_paranoid`` must be 2 or less, and a virtual machine often has no hardware counters; the status says which.
* **macOS**: cycles and instructions, and the share of each that ran on the efficiency cores, through XNU's per-thread counts.
  No root is needed.
  Cache and branch misses are configurable counters, which macOS opens only to root.
  A virtual Mac has no counters, and says so.
* **Windows**: none yet.

Reading costs a system call, about 100 ns on an M4, twice per zone: a counted zone costs about 200 ns against a few for an uncounted one.
That suits zones that last microseconds or more.
A thread counts if the source was asked for when it first recorded.

The viewer's ``M`` panel derives IPC and misses per thousand instructions from them, and the report's detailed form lists them per zone.

openmp
======

OpenMP parallel regions, through the runtime's OMPT interface:

``omp parallel: <function>``
    The region, on the thread that encountered it, named after the function it is in.
``omp work: <function>``
    Each thread's share of the region.
``omp barrier wait``, ``omp barrier wait (end of region)``, ...
    Time a thread spent waiting at a barrier, by kind.

Together they show what a region's fork costs, how the work divided, and who waited for whom.
A worker's share ends where it reaches the region's closing barrier, so the idle time between regions is not counted as work.
The primary thread's wait at that barrier is recorded: it is how long the slowest worker kept the region open.

The OpenMP runtime must support OMPT: LLVM's libomp does, and serves GCC-compiled code too; Intel's does; GCC's own libgomp does not.
The runtime asks for a tool once, when it starts, so ask for the source before the program's first OpenMP call, in the environment or by configuring Waggle first.
Worksharing chunks and explicit tasks are not recorded: a loop of many small chunks would multiply the events.

Regions are named from the code address of the call into the runtime, using the module's symbol table, so functions with hidden visibility are named too.
A region that shares nothing with its function and comes last in it can be compiled as a jump into the runtime, and is then named after the function's caller.

signposts
=========

Every zone also emitted as an ``os_signpost`` interval, so Apple's Instruments shows Waggle's zones beside its own CPU, GPU, Metal and memory tracks (macOS only).

Intervals go under the subsystem ``waggle``, one category per library, so each library has a lane of its own.
Record with Instruments' *Logging* template, or from the command line:

.. code-block:: console

    $ WAGGLE_SOURCES=signposts xcrun xctrace record --template Logging --launch -- ./myprogram

An interval's name must be fixed when the program is built, so every interval is named ``zone`` and carries the zone's name as its message.
Switched off, the source costs a zone one branch.
Instruments starts recording just after it launches a program; a zone already open then is not emitted, rather than emitted without its beginning.
On a CI machine, ``xctrace`` records only after ``sudo DevToolsSecurity -enable``, or it waits for an authorization nobody can give.

Planned
=======

Sources tied to vendor libraries (CUPTI and rocprofiler-sdk for GPU kernels, PAPI for more counters, PMPI for MPI) will be plugins, loaded by name when asked for, so the collector itself depends on none of them.
