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


def test_a_switched_off_domain_records_nothing():
    on, off = waggle.Zone("py: domain on", domain="py_on"), waggle.Zone("py: domain off", domain="py_off")
    waggle.set_domain_enabled("py_off", False)
    try:
        assert not waggle.domain_enabled("py_off") and waggle.domain_enabled("py_on")

        def body():
            with on:
                with off:
                    pass

        on_fresh_thread(body)
    finally:
        waggle.set_domain_enabled("py_off", True)
    snap = waggle.snapshot()
    assert snap.find("py: domain on") is not None
    assert snap.find("py: domain on/py: domain off") is None


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


def test_a_python_program_that_never_finalizes_still_gets_its_report(tmp_path):
    # waggle registers waggle_at_exit with Python's atexit, so the report is written while the
    # interpreter's threads still run, not at the collector's unload, which on Windows comes after
    # every other thread has been stopped.
    report = tmp_path / "report.txt"
    script = "import waggle\nwith waggle.Zone('py: at exit'):\n    pass\n"
    env = dict(os.environ, WAGGLE_REPORT="true", WAGGLE_REPORT_FILE=str(report))
    subprocess.run([sys.executable, "-c", script], env=env, check=True, timeout=120)
    assert "py: at exit" in report.read_text()


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


def test_the_viewer_reads_a_programs_allocation_track(tmp_path):
    # End to end: a program records allocations, its server streams them, and the viewer's client
    # parses what the server wrote.
    import asyncio
    import socket

    from waggle.client import ProfileClient

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    script = (
        "import sys, waggle\n"
        "with waggle.Zone('py: allocate'):\n"
        "    waggle.mem_alloc(4096, 0xABC000)\n"
        "    waggle.mem_alloc(64, 0xDEF000)\n"
        "    waggle.mem_free(64, 0xDEF000)\n"
        "    waggle.mem_alloc(16)\n"
        "waggle.flush()\n"
        "print('ready', flush=True)\n"
        "sys.stdin.read()\n"
    )
    env = dict(os.environ, WAGGLE_SERVER="1", WAGGLE_PORT=str(port), WAGGLE_REPORT="0")
    program = subprocess.Popen([sys.executable, "-c", script], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert program.stdout.readline().strip() == "ready"

        async def read_track():
            client = ProfileClient("127.0.0.1", port)
            assert await client.connect(timeout=10.0)
            # Once the program is idle, updates stop carrying a memory message: one comes only
            # with new samples (the first update may repeat a few the connect already sent).
            empty_updates, snapshots = 0, 0
            async for msg in client.messages():
                kind = client.state.apply(msg)
                if kind == "memory" and not msg["samples"]:
                    empty_updates += 1
                snapshots += kind == "snapshot"
                if snapshots == 4:
                    break
            await client.close()
            return client.state.memory, empty_updates

        track, empty_updates = asyncio.run(asyncio.wait_for(read_track(), 30.0))
        assert empty_updates == 0
        assert [b for _, b in track.samples] == [4096, 4160, 4096, 4112]
        assert (track.live_bytes, track.untracked, track.seq) == (4112, 1, 4)
        assert [(a.address, a.bytes, a.zone) for a in track.live] == [("0xabc000", 4096, "py: allocate")]
    finally:
        program.stdin.close()
        program.wait(timeout=60)


def test_a_server_advertises_itself_to_the_viewer():
    # End to end: the program's server advertises over mDNS, and the viewer's browser finds it by
    # the port it listens on. CI sets WAGGLE_REQUIRE_MDNS, so a missing responder fails there
    # instead of skipping.
    if not os.environ.get("WAGGLE_REQUIRE_MDNS"):
        if sys.platform.startswith("linux") and not os.path.exists("/run/avahi-daemon/socket"):
            pytest.skip("Linux advertises through the Avahi daemon, which is not running")
        pytest.importorskip("zeroconf", reason="the viewer finds servers through python-zeroconf")
    import queue
    import socket

    from waggle.discovery import ServerBrowser

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, WAGGLE_SERVER="1", WAGGLE_PORT=str(port), WAGGLE_REPORT="0")
    script = "import sys, waggle\nwaggle.set_enabled(True)\nprint('ready', flush=True)\nsys.stdin.read()\n"
    program = subprocess.Popen([sys.executable, "-c", script], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    found: queue.Queue = queue.Queue()
    browser = ServerBrowser(lambda host, found_port, exe: found.put((host, found_port, exe)))
    try:
        assert program.stdout.readline().strip() == "ready"
        assert browser.start()
        while True:
            host, found_port, exe = found.get(timeout=30)  # raises queue.Empty if never found
            if found_port == port:
                break
        assert host == "127.0.0.1" and exe.startswith("python")
    finally:
        browser.stop()
        program.stdin.close()
        program.wait(timeout=60)
