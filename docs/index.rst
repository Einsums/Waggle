..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

======
Waggle
======

|waggle| is a low-overhead tracing profiler for C, C++ and Python libraries.
Every library in a process records into one shared collector, so their zones form a single call tree, and a terminal viewer shows that tree live while the program runs.

A zone costs a few nanoseconds when it is recorded, and less than one when recording is off, so libraries can leave their instrumentation in released code.
Optional *sources* add what nobody wrote by hand: hardware counters, each zone's energy, OpenMP parallel regions, and zones in Apple's Instruments.
Device work (Metal command buffers today) is recorded with the times the GPU measured, on a timeline beside the host threads that submitted it.
Every zone can also go to a Perfetto trace, to see each call on a timeline in Perfetto's UI.

Waggle came out of `Einsums <https://github.com/Einsums/Einsums>`_, whose profiler it was, so that Einsums, Nectar and other libraries can profile one program together.

.. grid:: 2
    :gutter: 3

    .. grid-item-card:: Getting started
        :link: getting-started/index
        :link-type: doc

        Install Waggle, put zones in C++, C or Python code, and watch them in the viewer.

    .. grid-item-card:: Guides
        :link: guides/index
        :link-type: doc

        Instrumenting a library, configuration, the viewer, sources, device work, memory, sessions and plugins.

    .. grid-item-card:: Concepts
        :link: concepts/index
        :link-type: doc

        How the collector works, what a zone costs and why, clocks, and what happens at exit.

    .. grid-item-card:: Reference
        :link: reference/index
        :link-type: doc

        The C, C++ and Python APIs, every setting, the command line, and the formats Waggle writes.

.. toctree::
    :hidden:
    :maxdepth: 2

    getting-started/index
    guides/index
    concepts/index
    reference/index
    development/index
