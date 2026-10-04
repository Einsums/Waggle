..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

========
Settings
========

Every setting by name (as ``waggle_config_set``, ``waggle.configure`` and ``waggle.setting`` take it), its environment variable, and its default.
:doc:`/guides/configuration` explains how they combine.

.. list-table::
    :header-rows: 1
    :widths: 22 30 14 34

    * - Name
      - Environment
      - Default
      - Meaning
    * - ``record``
      - ``WAGGLE_DISABLE`` (inverted)
      - ``true``
      - Record zones and annotations at all.
    * - ``report``
      - ``WAGGLE_REPORT``
      - ``true``
      - Write the text report when the program ends, if it opened a zone.
    * - ``report_file``
      - ``WAGGLE_REPORT_FILE``
      - ``profile.txt``
      - Where the report goes.
    * - ``report_append``
      - ``WAGGLE_REPORT_APPEND``
      - ``false``
      - Append the report to the file rather than replace it.
    * - ``report_detailed``
      - ``WAGGLE_REPORT_DETAILED``
      - ``false``
      - Add each zone's minimum, maximum, mean and counters to the report.
    * - ``save``
      - ``WAGGLE_SAVE``
      - empty
      - Write the session to this file when the program ends, appending to the sessions already in it.
    * - ``server``
      - ``WAGGLE_SERVER``
      - ``false``
      - Serve the session to viewers while the program runs.
    * - ``port``
      - ``WAGGLE_PORT``
      - ``19216``
      - The server's port.
    * - ``wait_for_viewer``
      - ``WAGGLE_WAIT_FOR_VIEWER``
      - ``false``
      - Hold the program until a viewer connects.
    * - ``max_distinct_children``
      - ``WAGGLE_MAX_DISTINCT_CHILDREN``
      - ``256``
      - Distinct zone names one parent keeps before the rest fold into ``(other)``; 0 for no limit.
    * - ``disabled_domains``
      - ``WAGGLE_DISABLE_DOMAINS``
      - empty
      - Libraries whose zones are not recorded, comma-separated.
    * - ``sources``
      - ``WAGGLE_SOURCES``
      - empty
      - Optional instruments to turn on, comma-separated: ``counters``, ``openmp``, ``signposts``.

Booleans accept ``1``, ``true``, ``on``, ``yes`` and ``0``, ``false``, ``off``, ``no``.

Not settings
============

``WAGGLE_DISABLE`` and ``WAGGLE_DETAIL`` are also *preprocessor* macros: defined before ``<Waggle/Waggle.hpp>``, the first compiles a translation unit's zones out and the second compiles its ``WAGGLE_ZONE_DETAIL`` zones in.
