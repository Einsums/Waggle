# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""Profiling Python code through waggle._core."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import threading

import pytest

if os.environ.get("WAGGLE_REQUIRE_CORE"):
    # Built with WAGGLE_BUILD_PYTHON: a module that fails to load (a DLL not found) fails here
    # rather than skipping every test quietly.
    import waggle._core as core
else:
    core = pytest.importorskip("waggle._core", reason="waggle._core is not built (WAGGLE_BUILD_PYTHON=OFF)")

import waggle  # noqa: E402


@pytest.fixture(autouse=True)
def recording():
    waggle.set_enabled(True)
    yield
    waggle.set_enabled(True)


def on_fresh_thread(body):
    """Run ``body`` on a thread of its own, so its zones sit directly below that thread's root."""
    worker = threading.Thread(target=body)
    worker.start()
    worker.join()


def test_a_zone_records_with_its_annotations_and_nesting():
    outer, inner = waggle.Zone("py: outer"), waggle.Zone("py: inner")

    def body():
        for i in range(3):
            with outer:
                waggle.annotate("phase", "setup")
                waggle.annotate("n", i)
                waggle.annotate("ratio", 0.5)
                with inner:
                    pass

    on_fresh_thread(body)
    snap = waggle.snapshot()
    node = snap.find("py: outer")
    assert node is not None and node.call_count == 3
    assert node.domain == "python"
    assert node.annotations["phase"] == "setup" and node.annotations["n"] == "2"
    child = snap.find("py: outer/py: inner")
    assert child is not None and child.call_count == 3
    assert node.inclusive_ns >= node.exclusive_ns + child.inclusive_ns - 1


def test_zone_by_name_is_registered_once():
    assert waggle.zone("py: by name") is waggle.zone("py: by name")
    assert waggle.zone("py: by name") is not waggle.zone("py: by name", domain="other")


def test_profile_names_the_function_and_where_it_is():
    @waggle.profile
    def build():
        return 7

    @waggle.profile(name="py: renamed", domain="mylib")
    def other():
        return 8

    def body():
        assert build() == 7 and build() == 7
        assert other() == 8

    on_fresh_thread(body)
    snap = waggle.snapshot()
    node = next(n for t in snap.threads for n in t.root.walk() if n.function.endswith("build"))
    assert node.call_count == 2 and node.file == __file__ and node.line > 0
    renamed = snap.find("py: renamed")
    assert renamed is not None and renamed.domain == "mylib"


def test_profile_refuses_generators():
    with pytest.raises(TypeError, match="coroutine or generator"):

        @waggle.profile
        def gen():
            yield 1


def test_a_zone_closes_exactly_when_it_opened():
    outer, skipped, inner = waggle.Zone("py: pair outer"), waggle.Zone("py: pair skipped"), waggle.Zone("py: pair inner")

    def body():
        with outer:
            waggle.set_enabled(False)
            with skipped:
                waggle.set_enabled(True)  # must not close outer on the way out
            with inner:
                pass

    on_fresh_thread(body)
    snap = waggle.snapshot()
    assert snap.find("py: pair outer/py: pair inner") is not None
    assert snap.find("py: pair inner") is None
    assert snap.find("py: pair outer/py: pair skipped") is None


def test_merged_snapshot_combines_threads():
    z = waggle.Zone("py: merged")

    def body():
        with z:
            pass

    for _ in range(3):
        on_fresh_thread(body)
    merged = waggle.snapshot(merge_threads=True)
    assert len(merged.threads) == 1
    assert merged.find("py: merged").call_count == 3


def test_settings_by_name():
    assert waggle.override_settings({"max_distinct_children": "100"}) == 0
    assert waggle.setting("max_distinct_children") == "100"
    assert waggle.setting("no such setting") is None
    assert waggle.configure({"no such setting": "1"}) == -1


def test_counts_and_overheads():
    pushes, pops = waggle.total_push_count(), waggle.total_pop_count()
    with waggle.Zone("py: counted"):
        pass
    assert waggle.total_push_count() == pushes + 1
    assert waggle.total_pop_count() == pops + 1
    assert waggle.push_overhead_ns() >= 0.0 and waggle.pop_overhead_ns() >= 0.0


def test_the_interface_version_is_the_headers():
    major, minor = waggle.abi_version()
    assert (major, minor) == (0, 1)


@pytest.mark.skipif(sys.platform == "win32", reason="checks ELF and Mach-O linkage")
def test_the_module_has_no_collector_of_its_own():
    # One collector per process: the module reaches libwaggle's and defines none of it.
    path = core.__file__
    if sys.platform == "darwin":
        tool, args = "otool", ["-L", path]
    else:
        tool, args = "readelf", ["-d", path]
    if shutil.which(tool) is None or shutil.which("nm") is None:
        pytest.skip(f"{tool} or nm is not installed")
    linked = subprocess.run([tool, *args], capture_output=True, text=True, check=True).stdout
    assert "waggle" in linked, linked
    defined_only = ["-gU"] if sys.platform == "darwin" else ["-g", "--defined-only"]  # Apple's nm and GNU's
    defined = subprocess.run(["nm", *defined_only, path], capture_output=True, text=True, check=True).stdout
    collector = [line for line in defined.splitlines() if " waggle_" in line or " _waggle_" in line]
    assert collector == [], collector
