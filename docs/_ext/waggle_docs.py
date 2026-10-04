# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Directives that render tables from Waggle itself, so the documentation cannot drift from it.

``.. waggle-keymap::`` renders the viewer's keys from ``waggle.app.KEYMAP``, the same list the
bindings and the in-app help come from. ``.. waggle-cli:: viewer|report|diff`` renders the
command's ``--help`` from its argument parser.
"""

from __future__ import annotations

from docutils import nodes
from docutils.statemachine import StringList
from sphinx.application import Sphinx
from sphinx.util.docutils import SphinxDirective


class KeymapDirective(SphinxDirective):
    """Every key the viewer binds, by section, with what it does."""

    def run(self) -> list[nodes.Node]:
        from waggle.app import KEYMAP, key_label

        lines: list[str] = []
        for section, keys in KEYMAP:
            lines += [f".. rubric:: {section}", "", ".. list-table::", "   :header-rows: 1", "   :widths: 15 85", ""]
            lines += ["   * - Key", "     - Does"]
            for key, _action, description, _footer in keys:
                shown = key_label(key).replace("`", "\\`").replace("*", "\\*")
                lines += [f"   * - ``{shown}``", f"     - {description}"]
            lines.append("")
        node = nodes.section()
        node.document = self.state.document
        self.state.nested_parse(StringList(lines, source="waggle.app.KEYMAP"), self.content_offset, node)
        return node.children


class CliDirective(SphinxDirective):
    """The ``--help`` of ``waggle`` (the viewer), ``waggle report`` or ``waggle diff``."""

    required_arguments = 1

    def run(self) -> list[nodes.Node]:
        import argparse

        from waggle import report
        from waggle.cli import viewer_parser

        which = self.arguments[0]
        if which == "viewer":
            parser = viewer_parser("waggle")
        else:
            top = argparse.ArgumentParser(prog="waggle")
            subparsers = top.add_subparsers(dest="action_name")
            report.add_parsers(subparsers)
            parser = subparsers.choices[which]
        # A fixed width, so the page does not depend on the terminal the docs were built in.
        import os

        os.environ["COLUMNS"] = "76"  # what the page's column shows without a scrollbar
        text = parser.format_help()
        block = nodes.literal_block(text, text)
        block["language"] = "text"
        return [block]


def setup(app: Sphinx) -> dict:
    app.add_directive("waggle-keymap", KeymapDirective)
    app.add_directive("waggle-cli", CliDirective)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
