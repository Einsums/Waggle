..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

===========
Device work
===========

GPU work runs asynchronously: a host zone around a launch times the launch, not the work.
Waggle records device work once it is done, with the times the device itself measured, on a track per device queue.

Device work does not nest in the host's call tree, since it runs whenever the device gets to it.
Each piece keeps the name of the host zone that submitted it, so the two can be read together: the viewer's Gantt chart shows device queues as rows under the host threads (``G``), and the device panel (``k``) lists device time by name with the zone that submitted it.

Metal
=====

From Objective-C++, ``<Waggle/Metal.h>`` records a command buffer:

.. code-block:: objc

    #include <Waggle/Metal.h>

    id<MTLCommandBuffer> buffer = [queue commandBuffer];
    [kernel encodeToCommandBuffer:buffer ...];
    WAGGLE_METAL_ZONE(buffer, "mps gemm");   // before commit
    [buffer commit];

``WAGGLE_METAL_ZONE`` must come before ``commit``, which is when Metal takes completion handlers.
The buffer's ``GPUStartTime`` and ``GPUEndTime`` are recorded on the track ``Metal: <device name>`` when it completes; a buffer that failed records nothing.
The zone belongs to the domain where it is written and follows that domain's switch.
With ``WAGGLE_DISABLE`` defined it expands to nothing.

A good pattern puts a host zone around the whole call and a device zone on its buffer, so the host's share (copies, encoding, the wait) and the GPU's show side by side.
Einsums does this for its MPS GEMM and GEMV: ``mps gemm`` on the host, ``mps gemm (gpu)`` on the device.

Other devices: the C interface
==============================

CUDA, HIP and any other device use the C interface; Metal's helper is built on it.

.. code-block:: c

    /* Where the work is submitted, inside the zone submitting it: */
    uint64_t const token = waggle_device_submit();
    launch_kernel(stream);
    record_completion(stream, token);   /* however the device reports completion */

    /* When it is done, on any thread: */
    waggle_device_span(track, site, 0, start_ns, end_ns, token);

``track``
    The device queue, as a string id from ``waggle_intern``: ``"CUDA: H100 / stream 3"``.
``site``, ``name_id``
    What the work is, as for a zone.
``start_ns``, ``end_ns``
    The device's times, converted to the host's steady clock: ``waggle_steady_ns()`` gives that clock's now, so sample it beside the device's own clock and keep the difference.
``token``
    The submission's token, which names the host zone that submitted the work; 0 for none.

A span may be reported from any thread, before or after its submission is read: the collector matches them.

Planned
=======

CUDA and HIP helpers like Metal's, and per-kernel records from CUPTI and rocprofiler-sdk as source plugins, which would also capture library kernels nobody wrapped.
