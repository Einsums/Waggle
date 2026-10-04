..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

=======
C types
=======

The types ``<Waggle/Waggle.h>`` declares, which the :doc:`C API <api/c/index>` passes.

.. cpp:type:: waggle_snapshot

    A copy of the aggregated trees, owned by the caller until ``waggle_snapshot_release``.
    Recording continues while it is read.

.. cpp:type:: waggle_node

    One zone in a snapshot, valid until its snapshot is released.

.. cpp:type:: waggle_reply

    What a request handler writes its answer into, with ``waggle_reply_set``.

.. cpp:type:: waggle_handler_fn = void (*)(void *user, char const *params, size_t length, waggle_reply *reply)

    A request handler: receives the request's params (a JSON object) and answers through ``reply``.
    Called from the collector's thread.

.. cpp:type:: waggle_release_fn = void (*)(void *user)

    Called once on ``user`` when the collector no longer needs it; may be null.

.. cpp:type:: waggle_diagnostic_fn = void (*)(void *user, int level, char const *message, size_t length)

    Receives the profiler's own messages: level 0 debug, 1 info, 2 warning, 3 error.

.. cpp:type:: waggle_pair_fn = void (*)(void *user, char const *key, size_t key_length, char const *value, size_t value_length)

    Receives one key and value.

.. cpp:type:: waggle_numeric_fn = void (*)(void *user, char const *key, size_t key_length, double total, double min, double max, uint64_t count)

    Receives one numeric annotation: its key, and the total, smallest, largest and count of the values given it.
