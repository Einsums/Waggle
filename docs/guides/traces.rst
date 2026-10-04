..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

======
Traces
======

The report and the viewer say what each zone cost in all.
A trace keeps every call: each zone as a span on its thread's timeline, for the questions totals cannot answer, such as when a thread stalled, which threads waited on which, or whether the GPU sat idle while the host prepared its next submission.

.. code-block:: console

    $ WAGGLE_TRACE=run.pftrace ./myprogram

The file is a `Perfetto <https://perfetto.dev>`_ trace.
Open it at `ui.perfetto.dev <https://ui.perfetto.dev>`_ with *Open trace file*; the page reads it in the browser.

What it holds
=============

Zones
    Every zone, on its thread's track, nested as it ran, under the thread's name.
    A zone's category is its library (its domain), its source location is its call site, and its arguments are its annotations (the last value for each key) and, with the ``counters`` source, how much each counter rose while it was open.

Device work
    A track for each device queue, holding the work with the times the device measured.
    An arrow runs to each from a ``submit`` instant inside the zone that submitted it.

Live bytes
    The allocation track's curve, as a counter under the process.

Energy
    With the ``energy`` source, each thread's energy so far, in millijoules, as a counter under the thread.

Writing it
==========

The consumer thread writes the trace as it drains the rings, so recording costs a thread nothing more than it did.
Each thread's names, call sites and argument names are written once and referred to by number after, and its times as the step from the one before, which keeps a zone to about 32 bytes: a program recording a million zones a second writes about 32 MB a second.
The file is written in pieces of a megabyte, and at least every second.

The trace starts when the setting is applied, so set it in the environment, or configure it before the program records; zones before that are not in it.
``{pid}`` in the path becomes the process id, so the processes of one run, MPI ranks say, each write their own:

.. code-block:: console

    $ WAGGLE_TRACE=rank-{pid}.pftrace mpirun -n 4 ./myprogram

Setting ``trace`` to another path while the program runs ends one file and starts the next; setting it empty ends it.

Asking it questions
===================

Perfetto's trace processor answers SQL about a trace, from its own shell or from Python (``pip install perfetto``):

.. code-block:: python

    from perfetto.trace_processor import TraceProcessor

    tp = TraceProcessor(trace="run.pftrace")
    for row in tp.query(
        "select name, count(*) as calls, sum(dur) / 1e6 as ms "
        "from slice group by name order by ms desc limit 10"
    ):
        print(f"{row.name:30} {row.calls:8} {row.ms:10.3f} ms")

Zones are the ``slice`` table, annotations the ``args`` table under ``debug.``, and the counter tracks the ``counter`` table.

What to know
============

* A zone still open when the program ends stays open in the trace; Perfetto draws it to the trace's end.
* A zone whose end was lost, its event dropped by a full ring, ends where the loss came to light, with the argument ``end_lost``.
* Times are on ``std::chrono::steady_clock`` (``CLOCK_MONOTONIC`` on Linux, the clock the trace declares).
* A thread is shown with its kernel's thread id, as other tools show it.
