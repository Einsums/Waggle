..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

==========
Python API
==========

.. py:currentmodule:: waggle

Profiling
=========

.. autoclass:: Zone
    :members:

.. autofunction:: zone

.. autofunction:: profile

.. autofunction:: annotate

.. autofunction:: annotate_dims

.. autofunction:: mem_alloc

.. autofunction:: mem_free

.. autofunction:: set_thread_name

.. autofunction:: available

Switches and settings
=====================

.. autofunction:: enabled

.. autofunction:: set_enabled

.. autofunction:: domain_enabled

.. autofunction:: set_domain_enabled

.. autofunction:: configure

.. autofunction:: override_settings

.. autofunction:: setting

.. autofunction:: source_status

Reading what was recorded
=========================

.. autofunction:: snapshot

.. autoclass:: Snapshot
    :members:

.. autoclass:: Thread

.. autoclass:: Node
    :members:

.. autofunction:: flush

.. autofunction:: reset

.. autofunction:: print_report

.. autofunction:: export_json

Libraries, the server and exit
==============================

.. autofunction:: init

.. autofunction:: finalize

.. autofunction:: at_exit

.. autofunction:: start_server

.. autofunction:: server_running

.. autofunction:: server_port

.. autofunction:: publish

Overhead
========

.. autofunction:: total_push_count

.. autofunction:: total_pop_count

.. autofunction:: push_overhead_ns

.. autofunction:: pop_overhead_ns

.. autofunction:: abi_version

Viewer plugins
==============

.. automodule:: waggle.plugin
    :members: ViewerPlugin, PluginPanel, PluginAction, Requirement
