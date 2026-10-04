..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

============
Architecture
============

.. code-block:: text

    thread A ──zone──▶ ring A ─┐
    thread B ──zone──▶ ring B ─┼─▶ consumer thread ──▶ call trees, timeline, ──▶ report, session file,
    thread C ──zone──▶ ring C ─┘                       allocation track,          server ──▶ viewers
                                                       device work

Producers
=========

A zone writes an event into its thread's ring buffer: a single-producer, single-consumer ring of 65536 64-byte events, one cache line each.
No lock, no allocation, no string: a zone's name is a *site* id registered once, and its time is the CPU's own counter, read raw.
A zone with nothing inside it is written as one event when it closes; one that has something inside is written as it opens and as it closes.

If a ring fills (the consumer fell behind a thread producing faster than it drains), new events are dropped and counted; the report and viewer show the count.
Each event carries its nesting depth, so a dropped event costs only the zones it belonged to, not every zone after it.

The consumer
============

One background thread drains every ring, waking every millisecond while there is work and backing off to ten when there is none, or at once when a ring passes half full.
It turns events into an aggregated call tree per thread (zones of one name under one parent merge, with counts, times, histograms, annotations and memory), a timeline of recent zones, the allocation track and the device work.
Readers (the report, snapshots, the server) take a shared lock on what it built.

One collector per process
=========================

Every library in a process records into the same collector, ``libwaggle``, so their zones nest in one tree.
That only works with one copy of the library loaded.
Each copy, as it loads, looks for another already in the process (by a symbol only Waggle exports, across every loaded image); a later copy switches itself off, says so on standard error, and tells the first, which lists it in the session and the report as a library whose zones are missing.

The C interface is the contract between libraries and the collector: plain exported functions, versioned by the library's soname, so libraries built against different minor versions share one collector.

The server
==========

With ``server`` on, the session is served over TCP (127.0.0.1, port 19216 by default) as JSON lines: a ``meta`` message on connect, then a snapshot, the timeline, and what changed in the allocation track and device work, with every pass of the consumer while a viewer is connected.
Viewers send requests the program's handlers answer.
The server advertises itself over mDNS so viewers find it.
:doc:`/reference/formats` gives the messages.
