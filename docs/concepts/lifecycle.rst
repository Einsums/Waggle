..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=========
Lifecycle
=========

Starting
========

The collector starts on first use: the first zone, setting or call into the C interface.
It reads the ``WAGGLE_*`` environment then, and starts its consumer thread, and the server if asked for.
A thread registers its ring the first time it records.

``waggle::init`` counts a library as a user and records its name and build; it is optional.

Finishing
=========

The outputs (the report and the session file) are written once, by whichever comes first:

* the last ``waggle::finalize`` of the libraries that called ``init``;
* the exit hook, for a program that never finalized.

The collector registers the exit hook with the process's exit handlers when it first records, and the Python package registers it again with Python's ``atexit``.
The header registers nothing and defines nothing a compiler must emit, so a file compiled for another processor (a SIMD kernel built once per instruction set) can include it without leaving code behind that the linker might pick for every caller.
It writes the outputs while the program's threads still run, which matters on Windows, where a process's other threads are stopped before DLLs unload.
At exit, a program that never opened a zone writes no report.

After the collector is gone
===========================

Code still runs after the collector is destroyed at exit: other libraries' static destructors, and runtimes shutting down their own threads (libomp reports each worker's last task from its exit handler).
From then on every entry point of the C interface does nothing, and every switch a site cached reads off, so late zones are dropped rather than reaching freed memory.

Resetting
=========

``waggle::reset`` clears what has been recorded, keeping the zones open at that moment, which are timed from the reset.
The allocation track's curve starts again, but blocks still allocated stay listed; device work is summed afresh.
