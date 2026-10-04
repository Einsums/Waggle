..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

======
Memory
======

Waggle does not intercept ``malloc``: a library records the allocations it wants seen, which keeps the cost where you put it and the data about what matters.

.. code-block:: cpp

    void *block = allocate(bytes);
    WAGGLE_MEM_ALLOC_AT(block, bytes);
    // ...
    WAGGLE_MEM_FREE_AT(block, bytes);
    release(block);

From C, ``waggle_mem_alloc(address, bytes)`` and ``waggle_mem_free(address, bytes)``; from Python, ``waggle.mem_alloc(bytes, address)`` and ``waggle.mem_free(bytes, address)``.

The size is given again at the free: a block freed on another thread than the one that allocated it can be processed first, so the collector does not look the size up.
``WAGGLE_MEM_ALLOC(bytes)`` and ``WAGGLE_MEM_FREE(bytes)``, without an address, move the totals but cannot list the block.

What is kept
============

Per zone
    Allocations and frees, bytes allocated and freed, and the most the zone held at once, counting what was recorded in the zone itself and not in its children: the tree's ``alloc`` and ``peak`` columns.

The allocation track
    Live bytes after every allocation and free, process-wide, kept for the last 16384 changes; and every block allocated with an address and not yet freed (up to 65536), with its size, time, thread and the zone that allocated it.

The viewer's ``m`` panel draws the live bytes as a curve, in the same columns as the Gantt chart, and lists the largest blocks still live at the window's end, largest first.
A block that ownership passed to another object keeps the zone that allocated it, as long as the new owner records its free with the same address.

Allocations recorded without an address, and those made while the table of live blocks was full, are counted on the curve; the panel says how many it cannot list.

Device memory
=============

Record a device buffer without its address: device addresses are in another space, where they could collide with a host block's.
