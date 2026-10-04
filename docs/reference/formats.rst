..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=======
Formats
=======

The live stream
===============

The server writes JSON lines over TCP: one object per line, each with a ``type``.

``meta``
    Once, on connect: the process (``pid``, ``hostname``, ``executable``, ``executable_path``, ``start_time``), the libraries that called ``init`` (``clients``, each with ``name``, ``version``, ``git_commit``, ``git_branch``, ``git_dirty``, ``build_type``), collector copies switched off (``duplicates``, each with ``path`` and ``abi``), request handlers (``handlers``), sources and their states (``sources``, each with ``name``, ``state``, ``detail``), and the counters zones carry (``counters``).

``snapshot``
    The call tree of every thread: ``threads`` maps a thread id to its ``name`` and ``children``, each a zone with ``name``, ``call_count``, ``exclusive_ms``, ``inclusive_ms``, ``exclusive_min_ms``, ``exclusive_max_ms``, ``stddev_ms``, ``file``, ``line``, ``function``, ``annotations``, ``counters``, ``histogram``, ``memory``, ``energy`` (``nj`` and ``e_core_nj``, with the energy source) and its own ``children``.
    Also ``seq``, ``dropped`` (events lost to full rings), and the current ``handlers`` and ``sources``, which can change after ``meta``.

``timeline``
    Recent zones for the Gantt chart, ``events``: each with ``tid``, ``name``, ``start_ms`` and ``end_ms``; device work has ``track`` in place of ``tid``.

``memory``
    The allocation track: ``samples`` as ``[t_ms, live_bytes]`` pairs, the sequence number ``seq`` of the last, ``live_bytes``, ``untracked``, and ``live``, the largest blocks still allocated (``address``, ``bytes``, ``t_ms``, ``tid``, ``zone``).
    The first after connecting holds the whole curve; later ones only what is new.
    Samples are consecutive, so a viewer drops those it already has by ``seq``.

``devices``
    Device work by ``track`` and ``name``: ``count``, ``total_ms``, ``min_ms``, ``max_ms`` and the ``submitter`` zone; sent when it changes.

``log``, ``output``
    A library's log messages (``level``, ``timestamp``, ``file``, ``line``, ``function``, ``message``) and printed output.

``response``
    The answer to a viewer's ``request``: ``{"type": "request", "id": ..., "method": ..., "params": {...}}`` is answered with ``{"type": "response", "id": ..., "data": {...}}``.

Messages a library publishes carry its own ``type``.
All times are milliseconds since the program started.

Session files
=============

A session file is a JSON object, or ``{"sessions": [...]}`` holding several (saving to a file that already has one appends).
Each session holds:

``format``, ``version``
    ``"waggle-session"`` and ``1``.
``label``, ``meta``
    The session's name, and the program as in the stream's ``meta`` message.
``seq``, ``dropped``, ``threads``
    The call trees, as in the stream's ``snapshot`` message.
``memory``, ``devices``
    The allocation track and device work, as in the stream.
``extensions``
    What libraries added, each under its own namespaced key (``einsums.compute_graphs``).

The viewer's own saves nest the trees under ``snapshot`` and add ``bookmarks`` and ``node_history``; both shapes load.

Traces
======

The ``trace`` setting writes a `Perfetto <https://perfetto.dev>`_ trace: a protobuf ``Trace`` of ``TracePacket`` messages, as Perfetto's ``protos/perfetto/trace`` define them.
Each recording thread is a packet sequence of its own, starting with a clock snapshot and its track; its zones are ``TrackEvent`` begins and ends, with interned names, call sites and argument names, and times on an incremental clock of the sequence's own.
Sequence 1 holds the tracks (process, threads, device queues, counters), device work and counters, with absolute ``CLOCK_MONOTONIC`` times.
:doc:`/guides/traces` says what each track holds.

Recordings
==========

``waggle --record`` writes one ``{"_ts": seconds, "_msg": message}`` object per line: each message of the stream with when it arrived, which ``--replay`` uses to pace it.
