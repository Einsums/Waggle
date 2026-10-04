# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The viewer: its data model, session files, client, ``report`` and ``diff``, and the app driven
headless. What a library adds through a plugin is tested with that library."""

from __future__ import annotations

import asyncio
import importlib.util
import json

import pytest

from waggle import analysis, format as fmt
from waggle.client import ProfileClient, StreamState, parse_endpoint, read_recording, Recorder
from waggle.disasm import match_symbol, strip_listing
from waggle.model import MemoryTrack, ProfileNode, meta_to_dict, node_to_dict, parse_meta, parse_node, parse_snapshot
from waggle.session import (
    Session,
    export_snapshot,
    read_session_file,
    session_from_dict,
    write_session_file,
)
from waggle.testing import META, fake_server, server_export, snapshot_msg, wait_for


def node(name, excl=1.0, incl=None, calls=1, children=(), **kw):
    kids = list(children)
    return ProfileNode(
        name=name,
        exclusive_ms=excl,
        inclusive_ms=incl if incl is not None else excl + sum(c.inclusive_ms for c in kids),
        call_count=calls,
        children=kids,
        **kw,
    )


# ── model ─────────────────────────────────────────────────────────────────────


def test_node_round_trips_through_its_wire_form():
    original = node(
        "zone",
        2.5,
        calls=3,
        children=[node("child", 0.5)],
        annotations={"flops": {"avg": 10, "min": 10, "max": 10}},
        mem_alloc_bytes=1024,
        mem_peak_bytes=512,
        energy_nj=4_000_000,
        e_core_energy_nj=1_000_000,
        histogram={"1us": 2},
    )
    assert parse_node(json.loads(json.dumps(node_to_dict(original)))) == original


def test_parse_snapshot_keys_threads_by_string_id():
    snap = parse_snapshot(snapshot_msg())
    assert list(snap.threads) == ["7"]
    assert snap.threads["7"].label == "main"
    assert [n.name for n in snap.all_roots()] == ["outer"]


def test_stream_state_folds_each_message_type():
    state = StreamState()
    assert state.apply(META) == "meta" and state.meta.executable == "prog"
    state.apply(snapshot_msg())
    assert state.snapshot.threads["7"].children[0].name == "outer"
    state.apply({"type": "timeline", "events": [{"tid": 7, "name": "z", "start_ms": 0.0, "end_ms": 1.0}]})
    assert state.timeline[0].thread_id == "7"
    state.apply({"type": "output", "timestamp": "t", "message": "hello"})
    state.apply({"type": "log", "level": 3, "message": "careful"})
    assert [e.message for e in state.log_entries] == ["hello", "careful"] and state.log_total == 2


def memory_msg(seq, samples, live=(), untracked=0):
    return {
        "type": "memory",
        "live_bytes": samples[-1][1] if samples else 0,
        "untracked": untracked,
        "seq": seq,
        "samples": [list(s) for s in samples],
        "live": [{"address": hex(0x1000 + i), "bytes": b, "t_ms": t, "tid": 7, "zone": z} for i, (b, t, z) in enumerate(live)],
    }


def test_the_allocation_track_appends_what_is_new_and_drops_repeats():
    state = StreamState()
    assert state.memory is None
    # On connect the whole curve; the next update overlaps it by one sample.
    state.apply(memory_msg(3, [(1.0, 100), (2.0, 300), (3.0, 200)], live=[(200, 2.0, "solve")]))
    state.apply(memory_msg(4, [(3.0, 200), (4.0, 0)], untracked=2))
    track = state.memory
    assert track.samples == [(1.0, 100), (2.0, 300), (3.0, 200), (4.0, 0)]
    assert (track.seq, track.live_bytes, track.untracked, track.live) == (4, 0, 2, [])


def test_the_allocation_track_orders_samples_from_different_threads():
    track = MemoryTrack()
    track.apply(memory_msg(2, [(5.0, 10), (6.0, 20)]))
    track.apply(memory_msg(4, [(4.0, 30), (7.0, 40)]))  # a thread drained later, but earlier in time
    assert [t for t, _ in track.samples] == [4.0, 5.0, 6.0, 7.0]


def test_allocation_levels_take_each_slices_peak_and_carry_the_level_across_gaps():
    track = MemoryTrack(samples=[(1.0, 100), (2.0, 500), (2.5, 50), (7.0, 0)])
    # Slices [0,2] [2,4] [4,6] [6,8]: the 500 inside the second, then 50 held until 7.
    assert track.levels(0.0, 8.0, 4) == [500, 500, 50, 50]
    assert track.levels(3.0, 6.0, 1) == [50]
    assert track.peak(0.0, 8.0) == 500
    assert track.levels(1.0, 1.0, 4) == []


def test_the_allocation_track_saves_with_the_session(tmp_path):
    session = Session("s1", "mine", snapshot=parse_snapshot(snapshot_msg()))
    state = StreamState()
    state.apply(memory_msg(2, [(1.0, 64), (2.0, 192)], live=[(128, 2.0, "solve"), (64, 1.0, "")]))
    session.memory = state.memory
    path = tmp_path / "s.json"
    write_session_file(path, [session])
    loaded = session_from_dict(read_session_file(path)[0], "x")
    assert loaded.memory == session.memory
    # The server's export carries the same object under the same key.
    assert session_from_dict(server_export() | {"memory": memory_msg(1, [(0.5, 8)])}, "y").memory.samples == [(0.5, 8)]


# ── the shared time window ────────────────────────────────────────────────────


def time_window():
    pytest.importorskip("textual")
    from waggle.widgets.graphs import TimeWindow

    return TimeWindow()


def test_the_time_window_shows_everything_until_zoomed():
    window = time_window()
    assert window.resolve((10.0, 110.0)) == (10.0, 110.0)
    window.pan(0.5, (10.0, 110.0))  # nothing to pan across
    assert window.resolve((10.0, 110.0)) == (10.0, 110.0)


def test_zooming_while_following_keeps_the_right_edge_on_the_newest_data():
    window = time_window()
    window.zoom(4.0, (0.0, 100.0))
    assert window.following and window.resolve((0.0, 100.0)) == (75.0, 100.0)
    assert window.resolve((0.0, 200.0)) == (175.0, 200.0)  # more data arrived


def test_zooming_about_a_point_keeps_it_in_place():
    window = time_window()
    window.zoom(2.0, (0.0, 100.0), anchor_ms=20.0)
    start, end = window.resolve((0.0, 100.0))
    assert (start, end) == (10.0, 60.0) and not window.following
    assert (20.0 - start) / (end - start) == pytest.approx(0.2)


def test_panning_fixes_the_window_and_panning_to_the_end_follows_again():
    window = time_window()
    window.zoom(4.0, (0.0, 100.0))
    window.pan(-1.0, (0.0, 100.0))
    assert window.resolve((0.0, 100.0)) == (50.0, 75.0) and not window.following
    window.pan(-10.0, (0.0, 100.0))  # stops at the first data
    assert window.resolve((0.0, 100.0)) == (0.0, 25.0)
    window.pan(10.0, (0.0, 100.0))
    assert window.following


def test_zooming_out_past_the_data_shows_all_of_it():
    window = time_window()
    window.zoom(4.0, (0.0, 100.0))
    window.zoom(0.1, (0.0, 100.0))
    assert window.width_ms is None and window.following


# ── analysis ──────────────────────────────────────────────────────────────────


def tree():
    return [node("a", 1.0, children=[node("b", 4.0, children=[node("c", 2.0)]), node("c", 3.0)])]


def test_flatten_keeps_ancestors_of_filter_matches_and_honours_collapse():
    assert [r.node.name for r in analysis.flatten_tree(tree())] == ["a", "b", "c", "c"]
    assert [r.path for r in analysis.flatten_tree(tree(), "^b$")] == ["a", "a/b"]
    collapsed = analysis.flatten_tree(tree(), collapsed={"a/b"})
    assert [r.path for r in collapsed] == ["a", "a/b", "a/c"] and collapsed[1].collapsed


def test_invalid_regex_filter_falls_back_to_substring():
    assert analysis.name_matches("pack[A", "pack[")


def test_aggregate_flat_sums_each_name_across_call_sites():
    flat = {n.name: n for n in analysis.aggregate_flat(tree())}
    assert flat["c"].exclusive_ms == 5.0 and flat["c"].call_count == 2


def test_bottom_up_lists_callers_under_each_zone():
    up = {n.name: n for n in analysis.bottom_up(tree())}
    assert sorted(c.name for c in up["c"].children) == ["a", "b"]
    assert up["a"].children == []


def test_hot_path_follows_the_most_expensive_child():
    assert analysis.hot_path(tree()) == {"a", "a/b", "a/b/c"}


def test_sort_tree_does_not_touch_the_snapshot():
    roots = tree()
    ordered = analysis.sort_tree(roots, "exclusive")
    assert [n.name for n in ordered[0].children] == ["b", "c"]
    assert [n.name for n in analysis.sort_tree(roots, "name")[0].children] == ["b", "c"]
    assert [n.name for n in roots[0].children] == ["b", "c"]


def test_anomaly_flags_an_outlying_call():
    steady = node("z", 10.0, calls=10, exclusive_min_ms=0.9, exclusive_max_ms=1.1, stddev_ms=0.1)
    spiky = node("z", 10.0, calls=10, exclusive_min_ms=0.5, exclusive_max_ms=5.0, stddev_ms=0.5)
    assert not analysis.is_anomaly(steady) and analysis.is_anomaly(spiky)


def test_roofline_needs_flops_and_bytes():
    roots = [node("k", 1000.0, annotations={"flops": 2e9, "bytes_read": {"avg": 1e9}}), node("plain")]
    [point] = analysis.roofline_points(roots)
    assert point.name == "k" and point.intensity == pytest.approx(2.0) and point.gflops == pytest.approx(2.0)


def test_compare_orders_by_largest_change():
    a = parse_snapshot(snapshot_msg())
    b = parse_snapshot(snapshot_msg())
    b.threads["7"].children[0].children[1].exclusive_ms = 11.0  # "other": 1 -> 11
    rows = analysis.compare_snapshots(a, b)
    assert rows[0].name == "other" and rows[0].delta_ms == pytest.approx(10.0)


def test_format_helpers():
    assert len(fmt.make_bar(37.0)) == 10 and fmt.make_bar(100.0) == "█" * 10
    assert fmt.format_bytes(0) == "" and fmt.format_bytes(1536) == "1.5K" and fmt.format_bytes(-(3 << 20)) == "-3.0M"
    # Stable across processes, unlike hash(): the same zone keeps its color between runs.
    assert fmt.flame_color("pack_A") == fmt.flame_color("pack_A")
    assert fmt.sparkline([1, 2, 3]) == "▁▄█"


# ── session files ─────────────────────────────────────────────────────────────


def test_loads_the_servers_export_format(tmp_path):
    # Regression: the old viewer read only data["snapshot"], so every file written by
    # --einsums:profile:save opened as an empty tab.
    path = tmp_path / "s.json"
    path.write_text(json.dumps(server_export()))
    [record] = read_session_file(path)
    session = session_from_dict(record, "s1")
    assert session.snapshot is not None and session.snapshot.seq == 3
    assert session.label == "prog (T0)"  # the bare executable name is shared by every run


def test_loads_the_servers_appended_multi_session_format(tmp_path):
    path = tmp_path / "runs.json"
    path.write_text(json.dumps({"sessions": [server_export(), server_export("second run")]}))
    sessions = [session_from_dict(r, f"s{i}") for i, r in enumerate(read_session_file(path))]
    assert [s.label for s in sessions] == ["prog (T0)", "second run"]
    assert all(s.snapshot for s in sessions)


def test_loads_library_data_from_either_session_layout(tmp_path):
    graphs = [{"name": "scf", "nodes": [], "tensors": [], "edges": []}]
    current = server_export() | {"format": "waggle-session", "version": 1, "extensions": {"einsums.compute_graphs": graphs}}
    # Files written before the profiler became Waggle kept Einsums' graphs at the top level.
    legacy = server_export() | {"compute_graphs": graphs}
    for record in (current, legacy):
        path = tmp_path / "s.json"
        path.write_text(json.dumps(record))
        [loaded] = read_session_file(path)
        assert session_from_dict(loaded, "s1").extensions == {"einsums.compute_graphs": graphs}


def test_meta_carries_the_programs_handlers_and_clients():
    meta = parse_meta(dict(META) | {"handlers": ["get_taskpool_metrics"], "clients": [{"name": "einsums", "version": "2.0.0"}]})
    assert meta.handlers == ["get_taskpool_metrics"]
    assert [c["name"] for c in meta.clients] == ["einsums"]
    assert parse_meta(meta_to_dict(meta)) == meta
    # A server from before advertised nothing, which is not the same as advertising no handlers.
    assert parse_meta(dict(META)).handlers is None
    assert parse_meta(dict(META) | {"handlers": []}).handlers == []


def test_meta_says_why_a_requested_source_records_nothing():
    state = StreamState()
    state.apply(dict(META) | {"sources": [{"name": "openmp", "state": "off", "detail": ""}]})
    assert state.meta.source_problems() == []
    # The runtime starts after the viewer connected: the snapshot carries the new state.
    state.apply(snapshot_msg() | {"sources": [{"name": "openmp", "state": "missed", "detail": "set WAGGLE_SOURCES=openmp"}]})
    assert state.meta.source_problems() == ["Source openmp is missed: set WAGGLE_SOURCES=openmp"]
    state.apply(dict(META) | {"duplicates": [{"path": "/opt/libwaggle.so", "abi": "0.1"}]})
    assert "/opt/libwaggle.so" in state.meta.source_problems()[0]
    assert parse_meta(meta_to_dict(state.meta)) == state.meta


def test_a_handler_registered_after_connecting_reaches_the_viewer():
    # ComputeGraph registers get_compute_graphs on its first graph, often after the viewer
    # connected; the meta message is sent once, so snapshots repeat the list.
    state = StreamState()
    state.apply({"type": "meta"} | dict(META) | {"handlers": []})
    state.apply(snapshot_msg() | {"handlers": ["get_compute_graphs"]})
    assert state.meta.handlers == ["get_compute_graphs"]


def test_saved_sessions_load_back(tmp_path):
    session = Session("s1", "mine", snapshot=parse_snapshot(snapshot_msg()), bookmarks={"inner"})
    session.record_snapshot(session.snapshot)
    for count in (1, 2):
        path = tmp_path / f"{count}.json"
        write_session_file(path, [session] * count)
        loaded = [session_from_dict(r, "x") for r in read_session_file(path)]
        assert len(loaded) == count
        assert loaded[0].snapshot == session.snapshot and loaded[0].bookmarks == {"inner"}
        assert list(loaded[0].history["inner"]) == [5.0]


def test_saved_sessions_say_what_they_are(tmp_path):
    session = Session("s1", "mine", snapshot=parse_snapshot(snapshot_msg()))
    session.extensions["einsums.compute_graphs"] = [{"name": "g"}]
    path = tmp_path / "s.json"
    write_session_file(path, [session])
    data = json.loads(path.read_text())
    assert (data["format"], data["version"]) == ("waggle-session", 1)
    assert data["extensions"] == {"einsums.compute_graphs": [{"name": "g"}]}
    assert "compute_graphs" not in data


def test_export_writes_json_and_one_csv_row_per_node(tmp_path):
    export_snapshot(parse_snapshot(snapshot_msg()), tmp_path / "s.json", tmp_path / "s.csv")
    assert json.loads((tmp_path / "s.json").read_text())["threads"]["7"]["name"] == "main"
    assert len((tmp_path / "s.csv").read_text().splitlines()) == 1 + 3


# ── client ────────────────────────────────────────────────────────────────────


def test_a_server_on_this_machine_is_reached_on_loopback():
    from waggle.discovery import choose_host, is_local_address

    # 192.0.2.0/24 is reserved for documentation: never a local address.
    assert is_local_address("127.0.0.1") and not is_local_address("192.0.2.7")
    # A responder lists every interface's address, a container bridge's among them; one of this
    # machine's is enough to mean the server here, which listens on loopback.
    assert choose_host(["192.0.2.7", "127.0.0.1"]) == "127.0.0.1"
    assert choose_host(["192.0.2.7", "192.0.2.8"]) == "192.0.2.7"
    assert choose_host([]) is None


def test_parse_endpoint():
    assert parse_endpoint("19216") == ("127.0.0.1", 19216)
    assert parse_endpoint("box:1") == ("box", 1)
    with pytest.raises(ValueError):
        parse_endpoint("box:port")


def test_client_reassembles_long_lines_split_mid_character():
    # A snapshot line far longer than asyncio's 64 KiB readline limit, cut inside a
    # multi-byte character: the client must split lines from bytes, not decoded chunks.
    big = snapshot_msg()
    big["threads"]["7"]["children"][0]["annotations"] = {"note": "é" * 50_000}
    payload = (json.dumps(META) + "\n" + json.dumps(big, ensure_ascii=False) + "\n").encode()
    cut = payload.index("é".encode()) + 1
    chunks = [payload[:cut], payload[cut:]]

    async def main():
        server, port = await fake_server(chunks, on_request=lambda method: {"echo": method})
        client = ProfileClient("127.0.0.1", port)
        assert await client.connect()
        seen = []

        async def consume():
            async for msg in client.messages():
                seen.append(client.state.apply(msg))

        # Requests come from another task: the reply is read by the messages() loop.
        reader = asyncio.create_task(consume())
        while len(seen) < 2:
            await asyncio.sleep(0.01)
        assert await client.request("get_compute_graphs") == {"echo": "get_compute_graphs"}
        await client.close()
        await reader
        server.close()
        return seen, client.state

    seen, state = asyncio.run(main())
    assert seen == ["meta", "snapshot"]
    assert state.snapshot.threads["7"].children[0].annotations["note"] == "é" * 50_000


def test_request_without_connection_reports_it():
    assert asyncio.run(ProfileClient("127.0.0.1", 1).request("x")) == {"error": "not connected"}


def test_recording_round_trips(tmp_path):
    rec = Recorder(tmp_path / "r.jsonl")
    rec.write(META)
    rec.write(snapshot_msg())
    rec.close()
    assert [msg["type"] for _, msg in read_recording(tmp_path / "r.jsonl")] == ["meta", "snapshot"]


# ── disassembly lookup ────────────────────────────────────────────────────────


def test_symbol_lookup_matches_the_bare_function_name():
    mangled = "0000 b _ZGVZ4workvE5guard\n0010 T _ZN7einsums4blas5dgemmEv\n0020 T _Z10dgemm_like\n"
    pretty = "0000 b guard variable for work()::guard\n0010 T einsums::blas::dgemm()\n0020 T dgemm_like\n"
    assert match_symbol(mangled, pretty, "dgemm") == "_ZN7einsums4blas5dgemmEv"
    assert match_symbol(mangled, pretty, "work") is None  # only text symbols count


def test_strip_listing_drops_headers():
    listing = "\nfile:     file format elf64\n\nDisassembly of section .text:\n\n0010 <f()>:\n  10: ret\n\n"
    assert strip_listing(listing) == "0010 <f()>:\n  10: ret"


# ── the app, headless ─────────────────────────────────────────────────────────


needs_textual = pytest.mark.skipif(importlib.util.find_spec("textual") is None, reason="the app needs Textual")


def app_module():
    from waggle import app

    return app


@needs_textual
def test_every_key_has_an_action():
    KEYMAP, ProfilerApp = app_module().KEYMAP, app_module().ProfilerApp
    app = ProfilerApp(load=["unused"], mdns=False)
    for _, keys in KEYMAP:
        for key, action, _, _ in keys:
            name = action.split("(")[0]
            assert key == "space" or hasattr(app, f"action_{name}"), action


@needs_textual
def test_live_session_draws_and_every_key_runs():
    PANELS, ProfilerApp = app_module().PANELS, app_module().ProfilerApp

    async def main():
        server, port = await fake_server(
            [(json.dumps(m) + "\n").encode() for m in (META, snapshot_msg(1), snapshot_msg(2))]
        )
        app = ProfilerApp([("127.0.0.1", port)], mdns=False)
        async with app.run_test(size=(140, 60)) as pilot:
            await wait_for(pilot, lambda: app.active_view is not None and app.active_view._rows)
            assert [r.node.name for r in app.active_view._rows] == ["outer", "inner", "other"]
            assert app.active_session.label == "prog (T0)"
            for panel in PANELS:
                await app.run_action(f"toggle_panel('{panel}')")
                await pilot.pause()
            assert "outer" in app.query_one("#hotspots")._text
            await pilot.press("down", "space", "space", "e", "w", "e", "s", "a", "a", "u", "u", "h", "T", "b", "B", "d", "y")
            await pilot.pause()
            assert len(app.active_session.bookmarks) == 1
            await pilot.press("slash", *"other", "enter")
            await wait_for(pilot, lambda: [r.node.name for r in app.active_view._rows] == ["outer", "other"])
            await pilot.press("escape")
            await wait_for(pilot, lambda: len(app.active_view._rows) == 3)
            await pilot.press("question_mark")
            assert type(app.screen).__name__ == "HelpScreen"
            await pilot.press("escape")
            await pilot.press("q")
        server.close()

    asyncio.run(main())


@needs_textual
def test_the_allocation_track_lines_up_with_the_gantt_chart_and_zooms_with_it():
    from textual import events

    mod = app_module()
    timeline = {"type": "timeline", "events": [{"tid": 7, "name": "solve", "start_ms": 0.0, "end_ms": 100.0}]}
    memory = memory_msg(3, [(10.0, 4096), (50.0, 1 << 20), (90.0, 4096)], live=[(4096, 90.0, "solve")], untracked=1)

    async def main():
        server, port = await fake_server([(json.dumps(m) + "\n").encode() for m in (META, snapshot_msg(1), timeline, memory)])
        app = mod.ProfilerApp([("127.0.0.1", port)], mdns=False)
        async with app.run_test(size=(140, 60)) as pilot:
            await wait_for(pilot, lambda: app.active_session is not None and app.active_session.memory is not None)
            await pilot.press("G", "m")
            await pilot.pause()
            gantt, track = app.query_one(mod.GanttChart), app.query_one(mod.AllocationTrack)
            assert gantt._window == track._window == (0.0, 100.0)
            text = track.render().plain
            assert "1.0M" in text and "solve" in text and "0x1000" in text and "not listed" in text

            await pilot.press("right_square_bracket")
            await pilot.pause()
            assert gantt._window == track._window == (50.0, 100.0)
            await pilot.press("left_curly_bracket")
            await pilot.pause()
            assert track._window == (37.5, 87.5)
            # The block was allocated at 90 ms: not yet live at this window's end.
            assert "0x1000" not in track.render().plain

            # The wheel zooms about the time under the pointer, on either chart.
            x = track.LABEL_WIDTH + track.chart_width // 2
            anchor = track._time_at(x)
            track.post_message(events.MouseScrollUp(track, x, 3, 0, -1, 0, False, False, False))
            await pilot.pause()
            start, end = track._window
            assert end - start == pytest.approx(25.0) and gantt._window == track._window
            assert (anchor - start) / (end - start) == pytest.approx((x - track.LABEL_WIDTH) / track.chart_width)

            await pilot.press("0")
            await pilot.pause()
            assert track._window == (0.0, 100.0)

            # "[" once read as the start of a markup tag and swallowed the help after it.
            await pilot.press("question_mark")
            await pilot.pause()
            help_text = str(app.screen.query_one("VerticalScroll Static").render())
            assert "  [          Zoom out\n" in help_text and "[/]" not in help_text
            await pilot.press("escape", "q")
        server.close()

    asyncio.run(main())


@needs_textual
def test_device_work_has_rows_on_the_timeline_and_a_table():
    mod = app_module()
    timeline = {"type": "timeline", "events": [
        {"tid": 7, "name": "submit", "start_ms": 0.0, "end_ms": 10.0},
        {"track": "Metal: Apple M4 Max", "name": "mps gemm", "start_ms": 2.0, "end_ms": 8.0},
    ]}  # fmt: skip
    devices = {"type": "devices", "work": [
        {"track": "Metal: Apple M4 Max", "name": "mps gemm", "count": 3, "total_ms": 18.0, "min_ms": 5.0, "max_ms": 7.0,
         "submitter": "submit"},
    ]}  # fmt: skip

    async def main():
        server, port = await fake_server([(json.dumps(m) + "\n").encode() for m in (META, snapshot_msg(1), timeline, devices)])
        app = mod.ProfilerApp([("127.0.0.1", port)], mdns=False)
        async with app.run_test(size=(140, 60)) as pilot:
            await wait_for(pilot, lambda: app.active_session is not None and app.active_session.devices)
            await pilot.press("G", "k")
            await pilot.pause()
            gantt = app.query_one(mod.GanttChart).render().plain.splitlines()
            # The host thread's row, then the device queue's, labelled by the queue.
            assert gantt[1].startswith("T7") and gantt[2].startswith("Metal: Apple…")
            assert "mps gemm" in gantt[2]
            table = app.query_one("#devices")._text
            assert "Metal: Apple M4 Max" in table and "mps gemm" in table and "submit" in table and "18.000" in table
            await pilot.press("q")
        server.close()

    asyncio.run(main())


def test_device_work_saves_with_the_session(tmp_path):
    from waggle.model import parse_devices

    session = Session("s1", "mine", snapshot=parse_snapshot(snapshot_msg()))
    session.devices = parse_devices({"work": [{"track": "q", "name": "k", "count": 2, "total_ms": 3.0, "min_ms": 1.0, "max_ms": 2.0}]})
    write_session_file(tmp_path / "s.json", [session])
    loaded = session_from_dict(read_session_file(tmp_path / "s.json")[0], "x")
    assert loaded.devices == session.devices and loaded.devices[0].mean_ms == 1.5


def test_help_shows_the_character_a_key_types():
    pytest.importorskip("textual")
    from waggle.app import key_label

    assert [key_label(k) for k in ("question_mark", "slash", "right_square_bracket", "space", "ctrl+s", "G")] == [
        "?", "/", "]", "space", "ctrl+s", "G",
    ]  # fmt: skip


@needs_textual
def test_loaded_server_export_shows_its_tree(tmp_path):
    ProfilerApp = app_module().ProfilerApp
    path = tmp_path / "runs.json"
    path.write_text(json.dumps({"sessions": [server_export(), server_export("b")]}))

    async def main():
        app = ProfilerApp(load=[str(path)], mdns=False)
        async with app.run_test(size=(120, 40)) as pilot:
            await wait_for(pilot, lambda: app.active_view is not None and app.active_view._rows)
            assert len(app._ui) == 2
            await pilot.press("C")
            assert type(app.screen).__name__ == "CompareScreen"
            await pilot.press("escape", "q")

    asyncio.run(main())


# ── report and diff, without the viewer ───────────────────────────────────────


def _run_cli(*argv):
    from waggle.cli import main

    return main(list(argv))


def test_report_prints_hotspots_and_the_tree(tmp_path, capsys):
    path = tmp_path / "runs.json"
    path.write_text(json.dumps({"sessions": [server_export("first"), server_export("second")]}))
    assert _run_cli("report", str(path), "--format", "json") == 0
    rows = json.loads(capsys.readouterr().out)
    assert [r["name"] for r in rows] == ["inner", "outer", "other"]  # by exclusive time
    assert sum(r["pct"] for r in rows) == pytest.approx(100.0)
    assert _run_cli("report", str(path), "--session", "first", "--tree", "--format", "csv") == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[0] == "thread,name,calls,exclusive_ms,inclusive_ms,mean_ms" and len(lines) == 4
    assert _run_cli("report", str(path), "--top", "1") == 0
    out = capsys.readouterr().out
    assert out.startswith("second") and "inner" in out and "other" not in out


def test_diff_compares_sessions_and_can_fail_a_ci_job(tmp_path, capsys):
    slow = server_export("slow")
    slow["threads"]["7"]["children"][0]["children"][1]["exclusive_ms"] = 3.0  # "other": 1 ms -> 3 ms
    a, b = tmp_path / "a.json", tmp_path / "b.json"
    a.write_text(json.dumps(server_export("base")))
    b.write_text(json.dumps(slow))
    assert _run_cli("diff", str(a), str(b), "--format", "json") == 0
    rows = json.loads(capsys.readouterr().out)
    assert rows[0]["name"] == "other" and rows[0]["delta_pct"] == pytest.approx(200.0)
    assert _run_cli("diff", str(a), str(b), "--fail-above", "50") == 1
    assert "other (+200.0%)" in capsys.readouterr().err
    assert _run_cli("diff", str(a), str(b), "--fail-above", "50", "--min-ms", "2") == 0


def test_a_server_from_before_advertising_handlers_keeps_every_panel():
    from waggle.plugin import Requirement
    from waggle.session import Session

    requirement = Requirement(handler="get_taskpool_metrics")
    old = Session("s1", "old", meta=parse_meta(dict(META)))  # no handler list at all
    assert requirement.met_by(old, live=True)
    told_none = Session("s2", "new", meta=parse_meta(dict(META) | {"handlers": []}))
    assert not requirement.met_by(told_none, live=True)
    assert not requirement.met_by(old, live=False)  # and never without a live program
