..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=====================
Reports and sessions
=====================

Waggle writes two things when a program ends: a text report, and, when asked, a session file the viewer and the command line read back.
Both are written when the last library calls ``finalize``, or at exit for a program that never does.

The text report
===============

On by default, to ``profile.txt``, when the program opened at least one zone.

``WAGGLE_REPORT=0``
    No report.
``WAGGLE_REPORT_FILE=run.txt``
    Where it goes.
``WAGGLE_REPORT_APPEND=1``
    Append to the file rather than replace it, keeping every run.
``WAGGLE_REPORT_DETAILED=1``
    Add each zone's minimum, maximum and mean, its counters, and its energy.

The report starts with anything that limited what was recorded (a collector copy switched off, a source that records nothing), then gives each thread's call tree, and ends with what the profiler itself cost.
``waggle::print_report()`` prints it at any time; ``waggle::export_json(path)`` writes the aggregated tree as JSON.

Session files
=============

``WAGGLE_SAVE=run.json`` writes the session: the program and the libraries that used Waggle, every thread's tree, the allocation track, device work, and what libraries added (Einsums' compute graphs).
Saving to a file that already holds sessions appends, so repeated runs collect in one file for comparison.
The viewer also saves the session it shows (``S``, or ``ctrl+s`` for several).

Open one in the viewer with ``waggle --load run.json``, or use the command line:

``waggle report``
-----------------

.. waggle-cli:: report

.. code-block:: console

    $ waggle report run.json                    # the hotspots
    $ waggle report run.json --tree --thread main
    $ waggle report run.json --format csv > zones.csv

``waggle diff``
---------------

.. waggle-cli:: diff

``--fail-above PCT`` turns a comparison into a check: it exits 1 when any zone's exclusive time grew by more than ``PCT`` percent, ignoring zones under ``--min-ms``, so a CI job that saves a session per commit fails when something slows down.

Recording a live stream
=======================

``waggle --record stream.jsonl`` records everything the viewer receives; ``waggle --replay stream.jsonl`` plays it back at any speed.
A recording keeps the timeline and the order of events, which a session file, a summary at the end, does not.

:doc:`/reference/formats` describes the session file and the stream.
