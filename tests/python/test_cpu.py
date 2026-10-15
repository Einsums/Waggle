# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""``waggle cpu`` against a session with the ``cpu`` extension the sampling source saves."""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import shutil
import sys
from collections import Counter

import pytest

from waggle import cpu, elf
from waggle.cli import main
from waggle.model import ProfileNode
from waggle.session import session_from_dict
from waggle.symbolize import Frame, Location, Result, module_problem, parse_gnu_output, short_name
from waggle.widgets.panels import node_details


def _zone(name: str, ms: float, calls: int, children: list | None = None) -> dict:
    return {"name": name, "call_count": calls, "exclusive_ms": ms, "inclusive_ms": ms, "children": children or []}


SESSION = {
    "format": "waggle-session",
    "version": 1,
    "label": "sampled",
    "threads": {
        "1": {"name": "main", "children": [_zone("solve", 300.0, 1, [_zone("contract", 600.0, 40)])]},
        "2": {"name": "thread-2", "children": [_zone("contract", 150.0, 10)]},
    },
    "extensions": {
        "cpu": {
            "schema": "waggle.cpu/1",
            "sampling": {"event": "cycles", "rate_hz": 997, "threads": 3, "samples": 1000, "lost": 2},
            "zones": [
                {"thread": 1, "path": ["solve"], "samples": 250, "addresses": {"0x10": 200, "0x20": 50}},
                {"thread": 1, "path": ["solve", "contract"], "samples": 500, "addresses": {"0x30": 400, "0x40": 100}},
                {"thread": 2, "path": ["contract"], "samples": 150, "addresses": {"0x30": 150}},
            ],
            "no_zone": {"samples": 100, "addresses": {"0x99": 100}},
        }
    },
}


def test_zones_are_ranked_by_their_share_of_the_samples():
    data = cpu.summary(session_from_dict(SESSION, "s0"), symbols=False)
    assert data["schema"] == "waggle.cpu.summary/1"
    assert data["sampling"]["lost"] == 2
    # A zone's samples are summed over its threads and call paths, as the flat report sums time.
    assert [(z["name"], z["samples"], z["samples_pct"]) for z in data["zones"]] == [("contract", 650, 65.0), ("solve", 250, 25.0)]
    contract = data["zones"][0]
    assert contract["calls"] == 50
    assert contract["time_ms"] == {"exclusive": 750.0}
    assert not contract["low_confidence"]
    assert data["no_zone"] == {"samples": 100, "samples_pct": 10.0}


def test_a_zone_under_200_samples_is_low_confidence():
    small = json.loads(json.dumps(SESSION))
    small["extensions"]["cpu"]["zones"][0].update(samples=150, addresses={"0x10": 100, "0x20": 50})
    data = cpu.summary(session_from_dict(small, "s0"), symbols=False)
    assert next(z for z in data["zones"] if z["name"] == "solve")["low_confidence"]


def test_one_zone_by_address():
    data = cpu.zone_addresses(session_from_dict(SESSION, "s0"), "contract", symbols=False)
    assert data["samples"] == 650
    assert data["addresses"][0] == {"address": "0x30", "samples": 550, "samples_pct": 84.6}
    assert cpu.zone_addresses(session_from_dict(SESSION, "s0"), "(no zone)", symbols=False)["samples"] == 100


def test_the_command_reads_a_saved_file(tmp_path, capsys):
    path = tmp_path / "run.json"
    path.write_text(json.dumps(SESSION))
    assert main(["cpu", str(path), "--format", "json"]) == 0
    assert json.loads(capsys.readouterr().out)["zones"][0]["name"] == "contract"
    assert main(["cpu", str(path)]) == 0
    text = capsys.readouterr().out
    assert "cycles sampled at 997 Hz on 3 thread(s): 1000 samples, 2 lost" in text
    assert "(no zone)" in text


def test_a_session_without_samples_says_how_to_record_them(tmp_path):
    plain = {k: v for k, v in SESSION.items() if k != "extensions"}
    path = tmp_path / "plain.json"
    path.write_text(json.dumps(plain))
    with pytest.raises(SystemExit, match="WAGGLE_SOURCES=sampling"):
        main(["cpu", str(path)])


@pytest.mark.parametrize(
    ("demangled", "short"),
    [
        ("(anonymous namespace)::hot_loop(unsigned long) [clone .constprop.0]", "(anonymous namespace)::hot_loop"),
        ("void ns::f<int, long>(int) const", "ns::f<int, long>"),
        ("std::__new_allocator<unsigned long>::allocate(unsigned long, void const*)", "std::__new_allocator<unsigned long>::allocate"),
        ("Foo::operator()(int) const [clone .cold]", "Foo::operator()"),
        ("main", "main"),
    ],
)
def test_function_names_lose_their_return_type_arguments_and_clone_suffixes(demangled, short):
    assert short_name(demangled) == short


ADDR2LINE_OUTPUT = """\
0x0000000000004f18
std::__new_allocator<unsigned long>::allocate(unsigned long, void const*)
/include/c++/bits/new_allocator.h:151
CATCH2_INTERNAL_TEST_0()
/src/tests/unit/Sampling.cpp:100 (discriminator 2)
0x0000000000004f20
??
??:0
"""


def test_addr2line_output_gives_each_address_its_frames_innermost_first():
    frames = parse_gnu_output(ADDR2LINE_OUTPUT)
    assert frames[0x4F18] == (
        Frame("std::__new_allocator<unsigned long>::allocate", "/include/c++/bits/new_allocator.h", 151),
        Frame("CATCH2_INTERNAL_TEST_0", "/src/tests/unit/Sampling.cpp", 100),
    )
    assert frames[0x4F20] == (Frame("", "", 0),)


def test_hot_lines_name_the_innermost_line_and_what_it_was_inlined_into():
    inlined = Location(0x10, "/bin/prog", 0x10, (Frame("std::alloc", "/inc/a.h", 151), Frame("pack_b", "/src/c.cpp", 212)))
    plain = Location(0x20, "/bin/prog", 0x20, (Frame("pack_b", "/src/c.cpp", 214),))
    unnamed = Location(0x30, "/bin/prog", 0x30, ())
    result = Result(locations={0x10: inlined, 0x20: plain, 0x30: unnamed})
    lines = cpu.hot_lines(Counter({0x10: 60, 0x20: 30, 0x30: 10}), result)
    assert lines[0] == {
        "file": "/inc/a.h", "line": 151, "function": "std::alloc", "samples": 60, "samples_pct": 60.0,
        "inlined_into": {"function": "pack_b", "file": "/src/c.cpp", "line": 212},
    }  # fmt: skip
    assert lines[1]["line"] == 214 and "inlined_into" not in lines[1]
    assert lines[2] == {"file": None, "line": None, "function": "prog+0x30", "samples": 10, "samples_pct": 10.0}
    assert [f["function"] for f in cpu.hot_functions(Counter({0x10: 60, 0x20: 30, 0x30: 10}), result)] == [
        "std::alloc",
        "pack_b",
        "prog+0x30",
    ]


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="ELF files")
def test_a_file_changed_since_the_run_is_not_read(tmp_path):
    copy = tmp_path / "prog"
    shutil.copy(sys.executable, copy)
    st = copy.stat()
    found = elf.build_id(copy)
    if found:
        assert module_problem({"path": str(copy), "build_id": found}) == ""
        assert "rebuilt" in module_problem({"path": str(copy), "build_id": "00" * 20})
    assert module_problem({"path": str(copy), "size": st.st_size, "mtime_ns": st.st_mtime_ns}) == ""
    os.utime(copy, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))
    assert "changed" in module_problem({"path": str(copy), "size": st.st_size, "mtime_ns": st.st_mtime_ns})
    assert "gone" in module_problem({"path": str(tmp_path / "missing")})
    assert "no file" in module_problem({"path": "linux-vdso.so.1"})


def test_the_detail_panel_shows_a_zones_hot_lines():
    node = ProfileNode(name="contract", call_count=1, exclusive_ms=5.0, inclusive_ms=5.0)
    hot = [{"file": "/src/Contract.cpp", "line": 212, "function": "pack_b", "samples": 41, "samples_pct": 41.0}]
    text = node_details(node, 10.0, hot_lines=hot)
    assert "Hot lines" in text and "41.0%  Contract.cpp:212  pack_b" in text
    assert "Hot lines" not in node_details(node, 10.0)


@pytest.mark.skipif(importlib.util.find_spec("textual") is None, reason="the app needs Textual")
def test_the_viewer_names_a_loaded_sessions_hot_lines(tmp_path):
    from waggle.app import ProfilerApp
    from waggle.testing import wait_for

    path = tmp_path / "run.json"
    path.write_text(json.dumps(SESSION))

    async def main():
        app = ProfilerApp(load=[str(path)], mdns=False)
        async with app.run_test(size=(140, 50)) as pilot:
            await wait_for(pilot, lambda: app.active_view is not None and app.selected_node is not None)
            detail = app.query_one("#detail")
            # Symbolized in the background, then shown: the fixture's modules are not on disk, so
            # its addresses stay unnamed, but each zone still lists them as its hot lines.
            await wait_for(pilot, lambda: "Hot lines" in detail._text)
            await pilot.press("q")

    asyncio.run(main())
