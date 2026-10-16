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

energy
======

The energy each zone used, on macOS, and the share of it on the efficiency cores.

.. code-block:: console

    $ WAGGLE_SOURCES=energy WAGGLE_REPORT_DETAILED=1 ./myprogram

The figure is XNU's per-thread estimate from the CPU's power model, in nanojoules: good for comparing zones, and work on performance and efficiency cores, but not a meter's reading.
The kernel updates it every few milliseconds, not continuously, so Waggle samples it rather than reading it per zone: as the consumer drains, it reads every recording thread's energy, and credits each change to the zone that was open on that thread when it was read.
A zone pays nothing for it; the consumer pays one read, about 160 ns, per thread per pass.

What that means for the numbers:

* A zone's energy is *exclusive*, as its time is: a parent's own energy excludes its children's.
* A zone of about 100 ms or more gets a figure that is its own.
  Shorter zones get their share over many calls, as a sampling profiler's would, rather than an exact figure each.
* Energy a zone used in its last few milliseconds can arrive in the zone after it, once the kernel adds it.
* A thread that waits uses next to nothing, so a zone that sleeps gets next to nothing.

Every thread that records is read, whenever the source was asked for.
Linux has no per-thread energy (its RAPL measures a whole processor package), and a virtual Mac estimates none; the source says so.

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

roofline
========

Each zone's floating-point operations and an estimate of the bytes it moved to and from DRAM, on Linux, from hardware counters read as the zone opens and closes.

.. code-block:: console

    $ WAGGLE_SOURCES=roofline WAGGLE_SAVE=run.json ./myprogram
    $ waggle cpu run.json

``waggle cpu`` gives each counted zone its GFLOP/s, its estimated GB/s and its arithmetic intensity (FLOPs per byte), over its inclusive time: the counters include what the zone's children did.

* **FLOPs** come from Intel's ``FP_ARITH_INST_RETIRED`` events, one for scalar instructions and one for each width of packed ones, weighted by the operations each does (a 512-bit FMA of doubles is 16) and summed as they are read, so a zone's count is exact.
  ``roofline_precision`` picks double (``fp64``, the default) or single (``fp32``).
  Intel cores from Broadwell on are known, servers and client parts without efficiency cores; the events of AMD and Arm cores wait to be checked on real machines, and until then the source says it is unavailable there.
* **Bytes** are last-level cache misses times 64, per thread: an estimate, exact in whom it credits but low for streaming code, as lines the hardware prefetches are not misses.
  A zone that streams 512 MB counts roughly half of it.

With a calibration, each counted zone also gets what its numbers mean against the machine:

.. code-block:: console

    $ waggle calibrate          # once per kind of machine, in an exclusive job
    $ waggle cpu run.json

* **Share of peak**: its FLOP rate against one core's measured peak, as a zone's counters are its own thread's: how busy the arithmetic units of the cores it ran on were.
* With the ``work.flops`` and ``work.bytes`` annotations a library gives an operation (the least FLOPs and DRAM bytes its problem needs, per call), **redundant FLOPs** (counted over needed, above 1 for padding, recomputation or masked lanes), **byte efficiency** (needed over counted bytes, 1 when each operand crossed DRAM once), **gap to ideal** (its time over the larger of its FLOPs at the machine's peak and its bytes at the machine's bandwidth: the most it could speed up) and whether its work is **compute or memory bound**.

``waggle calibrate`` runs ``waggle-calibrate``, built with Waggle, which runs fused multiply-adds at the widest width the CPU has on every core at once, then a STREAM triad on each socket, and keeps the peaks in the machine file.
Peaks are measured rather than taken from a data sheet, as turbo and wide-vector frequency limits make those wrong by 20% or more; so it needs the machine to itself.
On a shared login node, ``--threads 2`` checks it works, and the result says it is partial.
One calibration serves every node with the same CPU.

The source shares each thread's counter group with ``counters``: with both asked for, a zone carries cycles and instructions beside the FLOPs and bytes, and not the counters source's cache and branch misses.
A group holds cycles, instructions and five more events; where the PMU cannot hold them at once (four general counters per thread with hyper-threading on), it is split in two, which take turns, and the counts are scaled by the time each ran, as ``perf`` does.
The status then says so, as short zones carry more error.
The counters are chosen before any thread opens them, so ask for the source in the environment, or configure Waggle before threads record.
Reading costs a zone what ``counters`` does, a system call as it opens and as it closes.

sampling
========

Where each zone's CPU time goes, by instruction address, on Linux: the kernel interrupts every thread of the process about a thousand times a second, records where it was, and each sample is credited to the zone open on its thread at that moment.

.. code-block:: console

    $ WAGGLE_SOURCES=sampling WAGGLE_SAVE=run.json ./myprogram
    $ waggle cpu run.json

The samples come from ``perf_event_open``: hardware cycles at ``sampling_rate`` (997 Hz) per thread, user space only, which ``kernel.perf_event_paranoid`` 2 allows; on a machine without hardware counters, as many virtual machines are, the software CPU clock instead.
Each thread's samples go into a 64 KB ring of its own, which the consumer reads as it drains; a sample is stamped with ``CLOCK_MONOTONIC``, the clock zones are on, so it lands in the right zone without the recording thread doing anything.

What that means for the numbers:

* A zone's samples are *exclusive*, as its time is: a parent's own samples exclude its children's.
* Every thread is sampled, not only those that record zones, because time inside a BLAS library runs on pool threads that never open one.
  Threads are found as they record and by a scan of ``/proc/self/task`` every second, so a thread that lives under a second and never records can be missed.
  A sample on a thread with no zone open is in ``(no zone)``.
* A zone with fewer than 200 samples has a share a longer run could move; ``waggle cpu`` marks it.
  The fix is a longer run, not a faster rate.
* Samples lost to a full ring are counted, and the status gives the count.

With the source on, each thread writes a zone's opening as it happens, so the consumer knows which zones are open when it reads a sample; a zone with nothing inside then costs two events rather than one.
The consumer's own thread is not sampled.
``waggle cpu`` ranks zones by their share of the samples, each with its hottest source lines.
``--zone NAME --lines`` lists every line and function of one zone with samples, ``--zone NAME --asm`` the machine code of its hottest function with the samples on each instruction (``--line N`` picks the function holding line N and marks its instructions), and ``--zone NAME`` alone its samples by address.
The viewer's detail panel shows a saved session's hot lines too.

.. code-block:: console

    $ waggle cpu run.json --zone contract --lines
    contract: 1923 samples

    samples %   samples  line
         90.2      1735  src/Contract.cpp:212  pack_b
          9.7       187  src/Contract.cpp:240  pack_a

The session saves raw addresses and the program's loaded files; the names are found when you look, by ``llvm-symbolizer`` when it is on the path and ``addr2line`` otherwise, so recording pays nothing for them.
Build with line information (``-g``, or CMake's ``RelWithDebInfo``) for lines; without it, functions only.
A line inside inlined code is the innermost one, where the time went, and also names the function and line it was inlined into.
Each file is checked to be the one that ran, by its build id, or by its size and time when it was linked without one: a rebuilt file is reported, not read, as its lines would be wrong.
Code with no file, the kernel's vDSO (``clock_gettime``), is named by its module alone.

Time in code without line information, a BLAS library or the vDSO, says little by itself: "38% in ``mkl_blas_avx512_dgemm_kernel``".
So each sample also records its call chain, and a sample in such code is credited to its *caller line*: the innermost frame of the chain in code with line information, which is the line of yours that called into the library.
``waggle cpu`` lists caller lines after a zone's hot lines, each with the function it called.
Call chains come from frame pointers: build your code with ``-fno-omit-frame-pointer`` (the default on Fedora and Ubuntu), and the line that called a library is found however the library was built.
A library built without them can make the chain skip a frame inside it, so the function named as called may be one the library called in turn; the line that called is still yours.
``sampling_callchain=false`` keeps only where each sample was.
The source reports ``unavailable`` on macOS and Windows, and where the kernel or a container's seccomp profile refuses ``perf_event_open``, saying what to change.

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
