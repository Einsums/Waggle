# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The hand-written parts of the documentation against the code they describe."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_the_settings_reference_names_every_setting_and_variable():
    code = (ROOT / "src" / "Settings.cpp").read_text()
    page = (ROOT / "docs" / "reference" / "settings.rst").read_text()
    names = re.findall(r'f\(\d+, "([a-z_]+)"', code)
    variables = re.findall(r'take_\w+\("(WAGGLE_[A-Z_]+)"', code)
    assert names and variables, "the settings list in Settings.cpp changed shape; update this test"
    missing = [n for n in names if f"``{n}``" not in page] + [v for v in variables if f"``{v}``" not in page]
    assert missing == [], f"docs/reference/settings.rst does not document {missing}"


def test_the_sources_guide_covers_every_source():
    sources = set(re.findall(r'\.name = "([a-z]+)"', "".join(p.read_text() for p in (ROOT / "src").glob("*.cpp"))))
    page = (ROOT / "docs" / "guides" / "sources.rst").read_text()
    assert {"counters", "openmp", "signposts"} <= sources
    missing = [s for s in sorted(sources) if f"\n{s}\n===" not in page]
    assert missing == [], f"docs/guides/sources.rst has no section for {missing}"
