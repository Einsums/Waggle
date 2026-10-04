..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=====================
Profiling Python code
=====================

``import waggle`` gives Python the same zones, recorded into the same collector as the C and C++ libraries the program loads, so a Python zone around a call into a C++ library is the parent of that library's zones.

.. code-block:: python

    import waggle

    SOLVE = waggle.Zone("solve")          # the site is registered once

    def solve(n):
        with SOLVE:
            waggle.annotate("n", n)
            ...

    @waggle.profile
    def build_fock(density):
        ...

    with waggle.zone("setup"):            # a Zone looked up by its name
        ...

    print(waggle.snapshot().find("solve").call_count)

* :class:`waggle.Zone` registers its site when it is made, so entering it costs a call into the collector and nothing else; keep frequently entered zones as module-level ``Zone`` objects.
* :func:`waggle.zone` finds or makes the ``Zone`` for a name, convenient for code entered rarely.
* :func:`waggle.profile` records every call of a function as a zone named after it.
  It refuses coroutine and generator functions: a zone must close before zones opened after it on the same thread, and one held open across an ``await`` or a ``yield`` would not.
* :func:`waggle.snapshot` copies what has been recorded, while recording continues.

Python zones belong to the domain ``python`` unless given another (``waggle.Zone("solve", domain="mylib")``).

The compiled module
===================

Recording goes through ``waggle._core``, a compiled module over the C interface.
:func:`waggle.available` says whether it is present; without it, zones record nothing and cost one check.
Settings, the report and the server are reached through the same module: ``waggle.configure(...)``, ``waggle.print_report()``, ``waggle.start_server(port)``; :doc:`/reference/python` lists them all.

Next
====

:doc:`viewer` shows a running Python program's zones live, the same way as a C++ program's.
