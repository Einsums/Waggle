..
    Copyright (c) The Einsums Developers. All rights reserved.
    Licensed under the MIT License. See LICENSE.txt in the project root for license information.

========================
Extending the viewer
========================

A library can tell the viewer more than zones: answer requests, add sections to session files, and publish messages of its own.
A viewer *plugin* then shows that data as panels and actions bound to keys.
Einsums uses this for its task pool and its compute graphs.

On the program's side
=====================

Request handlers
    ``waggle::register_handler("get_compute_graphs", handler)`` answers a viewer's request with JSON.
    The program's ``meta`` message lists every handler, so the viewer offers only the panels a program can fill.

Session sections
    ``waggle::register_session_section("einsums.compute_graphs", section)`` adds a section to saved session files, under ``extensions`` and keyed by your library's namespace.

Published messages
    ``waggle::publish("my_event", json)`` sends a message of your own type to every connected viewer.

Handlers run on the server's thread; keep them quick and thread-safe.

On the viewer's side
====================

A plugin is a :class:`waggle.plugin.ViewerPlugin`: a name, a title for the help screen, and its panels and actions.

.. code-block:: python

    from waggle.plugin import PluginPanel, Requirement, ViewerPlugin
    from waggle.widgets.panels import TextPanel

    async def refresh(app, widget, session, client):
        reply = await client.request("get_compute_graphs") if client else None
        data = reply or session.extensions.get("einsums.compute_graphs")
        widget.show(render(data))

    def plugin() -> ViewerPlugin:
        return ViewerPlugin(
            name="einsums",
            title="Einsums",
            panels=[
                PluginPanel(
                    name="graphs",
                    key="K",
                    description="Compute graphs",
                    requirement=Requirement(handler="get_compute_graphs", extension="einsums.compute_graphs",
                                            what="compute graphs"),
                    make_widget=lambda widget_id: TextPanel(id=widget_id, classes="panel"),
                    refresh=refresh,
                    interval=1.0,
                ),
            ],
        )

A :class:`~waggle.plugin.Requirement` names the handler a panel calls on a live program and the session extension it reads from a saved one, so the viewer offers the panel only where it can be filled, and otherwise says what is missing.

Installing a plugin
===================

Register a callable returning the plugin under the ``waggle.viewer`` entry-point group, and ``waggle`` finds it:

.. code-block:: toml

    [project.entry-points."waggle.viewer"]
    mylib = "mylib.profile_plugin:plugin"

A library with its own command can start the viewer with its plugins directly: ``waggle.cli.main(argv, prog="einsums profiler", plugins=[plugin()])``.
