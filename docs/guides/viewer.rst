..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

==========
The viewer
==========

``waggle`` is a terminal viewer: it connects to running programs, opens saved sessions, and replays recorded streams.
It needs `Textual <https://textual.textualize.io>`_.

.. waggle-cli:: viewer

Sessions and threads
====================

Each program the viewer connects to, and each session file it opens, is a session tab; each thread is a tab inside it.
A connection that drops is retried; a program found on the network is let go once it stops answering.

The tree
========

A thread's tab is its call tree: one row per zone path, with

* ``%`` and ``% parent``: the zone's inclusive time as a share of its thread's, and of its parent's;
* ``excl(ms)``, ``incl(ms)``: time in the zone alone, and with its children;
* ``count``, ``mean(ms)``: calls, and exclusive time per call;
* ``alloc``, ``peak``: memory recorded in the zone;
* ``anom``: marked when the zone's slowest or fastest call lies more than two standard deviations from its mean.

Rows expand and collapse; ``s`` cycles the sort; ``a`` shows one row per zone name across the tree; ``u`` shows, under each zone, the zones that called it.
``/`` filters by a regular expression, keeping the ancestors of every match.
``d`` captures a baseline: the tree then shows each zone's change since.

The detail panel under the tree describes the selected zone: its statistics, a sparkline of its recent exclusive time, its annotations, and its children's shares.

Panels
======

Each panel toggles with its key.

Hotspots (``H``)
    The zones with the most exclusive time, across all threads.

Flame graph (``f``)
    The selected thread's tree as an icicle chart, roots on top, width by inclusive time; click a bar to zoom into it, Escape to zoom out.

Timeline (``g``)
    Profiled CPU time per wall second, and live tracked memory, over the last two minutes.

Roofline (``o``)
    Zones annotated with ``flops`` and ``bytes_read`` / ``bytes_written``, placed against a roofline.

Gantt chart (``G``)
    Recent zones on each thread, and device work on a row per device queue (see :doc:`device-work`).

Allocation track (``m``)
    Live bytes over time, in the same columns as the Gantt chart, and the largest blocks still allocated, with the zone that allocated each (see :doc:`memory`).

Device work (``k``)
    GPU time by queue and name, with the host zone that submitted it.

Hardware counters (``M``)
    The selected zone's counters, IPC and a classification, when the ``counters`` source records them (see :doc:`sources`).

Disassembly (``A``) and source (``V``)
    The selected zone's function, disassembled from the binaries on this machine, and its source around the zone.

Log (``L``)
    The program's log and printed output, if it sends them; ``l`` cycles the level shown.

The Gantt chart and the allocation track share one window of time: ``]`` and ``[`` zoom in and out, ``{`` and ``}`` move earlier and later, ``0`` shows everything, and the mouse wheel zooms about the time under the pointer.
Zoomed in on a live program, the window follows the newest data until you move it away; moving back to the end follows again.

The status bar shows the program, dropped events, the state of each source asked for, and collectors switched off; a source that records nothing, or a missing collector, is also reported once per session as a warning.

Keys
====

Generated from the viewer's own key table:

.. waggle-keymap::

Recording and replay
====================

``R`` (or ``--record FILE``) records the stream the viewer receives to a ``.jsonl`` file; ``--replay FILE`` plays it back, with ``+`` and ``-`` changing the speed.
A recording is the closest thing to watching the run again, in a viewer that may not have been there for it.
