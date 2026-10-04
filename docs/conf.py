# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Sphinx configuration for Waggle's documentation.

Built by the ``waggle_docs`` target (``-DWAGGLE_BUILD_DOCS=ON``), which generates the C and C++
references into ``reference/api/c`` and ``reference/api/cpp`` and puts the built ``waggle``
package on the path for the Python reference. Warnings are errors and every cross-reference must resolve (``-W -n``).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "_ext"))

project = "Waggle"
author = "The Einsums Developers"
copyright = "The Einsums Developers"  # noqa: A001 - Sphinx's name
release = os.environ.get("WAGGLE_VERSION", "0.1.0")
version = release

extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.intersphinx",
    "sphinx.ext.napoleon",
    "sphinx_copybutton",
    "sphinx_design",
    "waggle_docs",
]

intersphinx_mapping = {"python": ("https://docs.python.org/3", None)}

# The API reference is generated; these names are the platform's, the standard library's, or
# parameters of templates, none of which have pages to link to.
nitpick_ignore_regex = [
    (r"c(pp)?:identifier", r"u?int(8|16|32|64)_t|size_t|ptrdiff_t|uintptr_t|bool"),
    (r"cpp:identifier", r"std(::.*)?"),
    (r"cpp:identifier", r"fmt(::.*)?"),
    (r"cpp:identifier", r".*detail.*"),
    (r"cpp:identifier", r"(F|Fill|MakeName|ApplyArgs|FormatName|Body|T|Args|Value|Section|Handler)$"),
    (r"cpp:identifier", r"waggle(::[a-z][a-z0-9_]*)*$"),
    (r"py:class", r"(textual|rich)\..*"),
    (r"py:class", r"(App|Widget|Static|Message|ComposeResult|Path|Any|deque|.*\.Event)$"),
    # The viewer's own session and connection, which plugin callbacks receive; not public API.
    (r"py:class", r"(Session|ProfileClient)$"),
]

# Python's roles unqualified (:func:, :class:); the generated C and C++ pages name their domain.
primary_domain = "py"
highlight_language = "python"  # what docstrings hold; pages name their language
autodoc_member_order = "bysource"
autodoc_typehints = "description"
# Only "members": in autodoc an option is on when its key is present, whatever its value.
autodoc_default_options = {"members": True}

html_theme = "pydata_sphinx_theme"
html_title = "Waggle"
html_theme_options = {
    "github_url": "https://github.com/Einsums/Waggle",
    "navigation_with_keys": False,
    "show_toc_level": 2,
}
html_static_path = ["_static"]
templates_path = []
exclude_patterns = ["_build", "_ext"]

rst_prolog = """
.. |waggle| replace:: *Waggle*
"""
