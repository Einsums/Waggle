..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=======================
Watching a program live
=======================

Start the program with its server on, then the viewer:

.. code-block:: console

    $ WAGGLE_SERVER=1 ./myprogram &
    $ waggle

With no arguments the viewer connects to ``127.0.0.1:19216``, the server's default port, and also finds programs that advertise themselves on the network (see `Finding programs`_).
The program does not wait for the viewer unless ``WAGGLE_WAIT_FOR_VIEWER=1`` asks it to, which keeps a short run from finishing before you have attached.

The viewer needs `Textual <https://textual.textualize.io>`_; finding programs on the network needs ``zeroconf``.

What you see
============

The main view is the call tree of each thread, one tab per thread, refreshed several times a second: each zone's share of its thread's time, its exclusive and inclusive time, calls, mean, and memory.
Below it, panels opened by key show the same data other ways:

* ``H``: the hotspots, the zones with the most exclusive time across all threads.
* ``f``: a flame graph of the selected thread; click a bar to zoom into it.
* ``G``: a Gantt chart of recent zones per thread, with a row for each GPU queue.
* ``m``: the allocation track, live bytes over time and the largest blocks still allocated.
* ``k``: device work by name, with the host zone that submitted it.
* ``g``, ``o``, ``M``, ``A``, ``V``, ``L``: CPU and memory over time, a roofline, hardware counters, disassembly, source, and the program's log.

``?`` shows every key; :doc:`/guides/viewer` explains each panel.

Finding programs
================

A program's server advertises itself over mDNS (Bonjour on macOS, the Avahi daemon on Linux, the DNS-SD service of Windows 10 1809 and later), and the viewer connects to each one it finds.
Where no responder runs, pass the port: ``waggle 19217`` or ``waggle otherhost:19216``.

Without the viewer
==================

``WAGGLE_SAVE=run.json`` saves the session when the program ends; ``waggle --load run.json`` opens it, ``waggle report run.json`` prints its hotspots or tree, and ``waggle diff a.json b.json`` compares two runs.
See :doc:`/guides/sessions`.
