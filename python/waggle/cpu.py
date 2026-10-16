# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""``waggle cpu``: where each zone's CPU time goes, from the ``sampling`` source.

Reads the ``cpu`` extension a session saves when it was recorded with ``WAGGLE_SOURCES=sampling``,
and gives a summary per zone for a person or an agent to read: zones ranked by their share of the
samples, beside the time and calls the profile measured, each with its hot lines and functions.
``--lines`` gives every line of one zone, ``--asm`` its hottest function's machine code with the
samples on each instruction. The summary's schema is ``waggle.cpu.summary/1``; later sources add
fields to it, never change the ones here.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
from collections import Counter
from typing import Any

from . import elf, machine
from .findings import zone_findings
from .analysis import aggregate_flat, numeric_annotation
from .session import Session
from .symbolize import Location, Result, symbolize

SCHEMA = "waggle.cpu.summary/1"

#: A zone with fewer samples than this has a share a longer run could move: the summary says so.
LOW_CONFIDENCE_SAMPLES = 200

#: Hot lines and functions the summary gives each zone.
SUMMARY_TOP = 5

NO_ZONE = "(no zone)"


def add_parser(sub) -> None:
    cpu = sub.add_parser(
        "cpu",
        help="Where each zone's CPU time goes, from a session recorded with WAGGLE_SOURCES=sampling or roofline",
        description="Zones ranked by their share of the CPU samples, each with its hot lines; or one zone's lines, "
        "addresses or machine code.",
    )
    cpu.add_argument("file", help="session file recorded with WAGGLE_SOURCES=sampling, roofline, or both")
    cpu.add_argument("--session", default="-1", help="which session in a multi-session file: index or label (default: the last)")
    cpu.add_argument("--zone", metavar="NAME", help="one zone: its samples by address, or with --lines or --asm")
    view = cpu.add_mutually_exclusive_group()
    view.add_argument("--lines", action="store_true", help="with --zone: every source line with samples, and the functions")
    view.add_argument("--asm", action="store_true", help="with --zone: the hottest function's machine code, samples per instruction")
    view.add_argument("--numa", action="store_true", help="every checked block's NUMA placement over time, and the findings")
    cpu.add_argument("--line", type=int, metavar="N", help="with --asm: the function holding line N, its instructions marked")
    cpu.add_argument("--no-symbols", action="store_true", help="leave addresses unnamed, which is faster")
    cpu.add_argument("--machine-file", help="the machine file to read peaks from (default: WAGGLE_MACHINE_FILE, else ~/.cache/waggle/machine.json)")
    cpu.add_argument("--top", type=int, default=20, help="rows to show (default: 20; 0 for all)")
    cpu.add_argument("--format", choices=("text", "json"), default="text")
    cpu.set_defaults(action=run)


def _extension(session: Session) -> dict[str, Any]:
    ext = session.extensions.get("cpu")
    if not isinstance(ext, dict) or "sampling" not in ext:
        raise SystemExit(
            f"session {session.label!r} has no CPU samples: record it with WAGGLE_SOURCES=sampling and WAGGLE_SAVE=FILE"
        )
    return ext


def _pct(part: int, whole: int) -> float:
    return round(100.0 * part / whole, 1) if whole else 0.0


def _by_zone(ext: dict[str, Any]) -> dict[str, Counter[int]]:
    """Each zone's samples by address, summed over its threads and call paths."""
    zones: dict[str, Counter[int]] = {}
    for zone in ext.get("zones", []):
        counts = zones.setdefault(zone["path"][-1], Counter())
        for address, n in zone.get("addresses", {}).items():
            counts[int(address, 16)] += int(n)
    counts = zones.setdefault(NO_ZONE, Counter())
    for address, n in ext.get("no_zone", {}).get("addresses", {}).items():
        counts[int(address, 16)] += int(n)
    return zones


def _callers_by_zone(ext: dict[str, Any]) -> dict[str, Counter[tuple[int, int]]]:
    """Each zone's samples in code without line information, by (caller, callee) address."""
    zones: dict[str, Counter[tuple[int, int]]] = {}
    for zone in ext.get("zones", []):
        counts = zones.setdefault(zone["path"][-1], Counter())
        for caller, callee, n in zone.get("callers", []):
            counts[(int(caller, 16), int(callee, 16))] += int(n)
    return zones


def _caller_addresses(callers: Counter[tuple[int, int]]) -> set[int]:
    return {a for pair in callers for a in pair}


def _symbols(ext: dict[str, Any], addresses: set[int], wanted: bool) -> Result:
    if not wanted:
        return Result(locations={a: Location(a) for a in addresses})
    return symbolize(ext.get("modules", []), sorted(addresses))


def _symbols_note(result: Result) -> dict[str, Any]:
    return {"tool": result.tool, "problems": result.problems}


def _line_key(location: Location) -> tuple[str, int, str]:
    inner = location.inner
    if inner is None or not inner.file:
        return ("", 0, location.function)
    return (inner.file, inner.line, inner.function)


def hot_lines(counts: Counter[int], result: Result, top: int = 0) -> list[dict[str, Any]]:
    """Samples by source line, as a share of @p counts' samples, most first. A line inside inlined
    code also names the function and line it was compiled into."""
    total = sum(counts.values())
    lines: Counter[tuple[str, int, str]] = Counter()
    into: dict[tuple[str, int, str], tuple[str, str, int]] = {}
    for address, n in counts.items():
        location = result.locations.get(address, Location(address))
        key = _line_key(location)
        lines[key] += n
        outer = location.outer
        if outer is not None and len(location.frames) > 1:
            into.setdefault(key, (outer.function, outer.file, outer.line))
    rows = []
    for (file, line, function), n in sorted(lines.items(), key=lambda item: (-item[1], item[0])):
        row: dict[str, Any] = {"file": file or None, "line": line or None, "function": function, "samples": n, "samples_pct": _pct(n, total)}
        if (key := (file, line, function)) in into:
            row["inlined_into"] = {"function": into[key][0], "file": into[key][1] or None, "line": into[key][2] or None}
        rows.append(row)
    return rows[:top] if top else rows


def caller_lines(callers: Counter[tuple[int, int]], total: int, result: Result, top: int = 0) -> list[dict[str, Any]]:
    """Samples in code without line information (a library, the vDSO), by the line that called
    into it and the function it called, as a share of the zone's @p total samples, most first."""
    lines: Counter[tuple[str, int, str, str]] = Counter()
    for (caller, callee), n in callers.items():
        file, line, function = _line_key(result.locations.get(caller, Location(caller)))
        lines[(file, line, function, result.locations.get(callee, Location(callee)).function)] += n
    rows = [
        {"file": file or None, "line": line or None, "function": function, "callee": callee, "samples": n, "samples_pct": _pct(n, total)}
        for (file, line, function, callee), n in sorted(lines.items(), key=lambda item: (-item[1], item[0]))
    ]
    return rows[:top] if top else rows


def hot_functions(counts: Counter[int], result: Result, top: int = 0) -> list[dict[str, Any]]:
    """Samples by the innermost function, for a first look."""
    total = sum(counts.values())
    functions: Counter[str] = Counter()
    for address, n in counts.items():
        functions[result.locations.get(address, Location(address)).function] += n
    rows = [
        {"function": f, "samples": n, "samples_pct": _pct(n, total)}
        for f, n in sorted(functions.items(), key=lambda item: (-item[1], item[0]))
    ]
    return rows[:top] if top else rows


_ROOFLINE_COUNTERS = ("fp64_flops", "fp32_flops", "llc_miss_bytes")


def _inclusive_totals(session: Session) -> dict[str, tuple[float, dict[str, int]]]:
    """Each zone's inclusive time and roofline counters, summed over its outermost calls on every
    thread: both are measured from a zone's open to its close, children and all, so a call inside
    another of the same name is already counted in it."""
    totals: dict[str, tuple[float, dict[str, int]]] = {}
    if session.snapshot is None:
        return totals
    for thread in session.snapshot.threads.values():
        pending = [(child, frozenset()) for child in thread.children]
        while pending:
            node, above = pending.pop()
            if node.name not in above:
                ms, counters = totals.get(node.name, (0.0, {}))
                counters = dict(counters)
                for name in _ROOFLINE_COUNTERS:
                    if isinstance(value := (node.counters or {}).get(name), dict):
                        counters[name] = counters.get(name, 0) + int(value.get("total", 0))
                # Work hints are per call: each call's, times the calls.
                for hint in ("work.flops", "work.bytes"):
                    if (per_call := numeric_annotation(node.annotations or {}, hint)) is not None:
                        counters[hint] = counters.get(hint, 0) + int(per_call * node.call_count)
                totals[node.name] = (ms + node.inclusive_ms, counters)
            pending += [(child, above | {node.name}) for child in node.children]
    return totals


def _roofline(inclusive_ms: float, counters: dict[str, int]) -> dict[str, Any]:
    """A zone's FLOPs and estimated DRAM bytes from the roofline source's counters, per second of
    its inclusive time: the counters are read as it opens and closes, so they include its children."""
    out: dict[str, Any] = {}
    seconds = inclusive_ms / 1e3
    total = lambda name: counters.get(name, 0)  # noqa: E731
    for precision in ("fp64", "fp32"):
        if f"{precision}_flops" in counters:
            flops = total(f"{precision}_flops")
            out["compute"] = {"flops": flops, "gflops": round(flops / seconds / 1e9, 3) if seconds else None, "precision": precision}
    if "llc_miss_bytes" in counters:
        nbytes = total("llc_miss_bytes")
        out["memory"] = {"bytes": nbytes, "gbs": round(nbytes / seconds / 1e9, 3) if seconds else None, "source": "llc_misses"}
    if "compute" in out and "memory" in out and out["memory"]["bytes"]:
        out["intensity"] = round(out["compute"]["flops"] / out["memory"]["bytes"], 3)
    return out


def _efficiency(zone: dict[str, Any], inclusive_ms: float, counters: dict[str, int], peak: dict[str, Any]) -> None:
    """Add to @p zone what its counts and work hints mean against the machine's @p peak. A zone's
    counters are its own thread's, so its FLOP rate is per second of thread time, against one core's
    peak; its work hints are the whole operation's, against the whole machine's."""
    compute, memory = zone.get("compute"), zone.get("memory")
    precision = compute["precision"] if compute else "fp64"
    if compute and compute.get("gflops") is not None and peak.get(f"{precision}_gflops_per_core"):
        compute["peak_pct"] = round(100.0 * compute["gflops"] / peak[f"{precision}_gflops_per_core"], 1)
    work_flops, work_bytes = counters.get("work.flops"), counters.get("work.bytes")
    if work_flops and compute:
        compute["redundant_flops"] = round(compute["flops"] / work_flops, 3)
    if work_bytes and memory and memory["bytes"]:
        memory["byte_efficiency"] = round(work_bytes / memory["bytes"], 3)
    flop_s = (work_flops or 0) / (peak.get(f"{precision}_gflops", 0) * 1e9) if peak.get(f"{precision}_gflops") else 0.0
    byte_s = (work_bytes or 0) / (peak.get("dram_gbs", 0) * 1e9) if peak.get("dram_gbs") else 0.0
    if work_flops or work_bytes:
        if max(flop_s, byte_s) > 0:
            zone["gap_to_ideal"] = round(inclusive_ms / 1e3 / max(flop_s, byte_s), 2)
        # Which roof the work itself meets first. Without hints a zone's own counts cannot say: one
        # that does little of either sits under both roofs, bound by neither.
        zone["bound"] = "compute" if flop_s >= byte_s else "memory"


def _machine(session: Session, ext: dict[str, Any] | None, path: str | None) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    """The summary's machine block, and the peaks to measure against, when it was calibrated."""
    recorded = (ext or {}).get("machine")
    if not isinstance(recorded, dict) or not recorded:
        return None, None
    block = dict(recorded)
    entry = machine.find(recorded, path)
    if entry is None:
        block["peak"] = None
        block["note"] = "not calibrated: run waggle calibrate on this machine, or one like it, with the machine to itself"
        return block, None
    block["peak"] = entry.get("peak")
    block["calibrated"] = entry.get("calibrated")
    if entry.get("host") != recorded.get("host"):
        block["calibrated_on"] = entry.get("host")
    if not entry.get("complete", True):
        block["note"] = f"calibrated with {entry.get('threads')} of {entry.get('cpus')} CPUs: the totals are low"
    return block, entry.get("peak")


# ---- NUMA placement ------------------------------------------------------------------------------

#: A block this much on one node is placed there; the first-touch finding wants it at least.
FIRST_TOUCH_NODE_PCT = 90.0
#: ...while at least this share of the threads' CPU time ran on other nodes.
FIRST_TOUCH_OFF_NODE_PCT = 25.0
#: A block with more than this share of its pages untouched at the end.
UNTOUCHED_PCT = 50.0
#: A block this large wants huge pages...
HUGE_BLOCK_BYTES = 1 << 30
#: ...and has fewer than this share of them.
HUGE_PCT = 10.0


def _gb(nbytes: int) -> str:
    return f"{nbytes / 2**30:.1f} GB" if nbytes >= 2**30 else f"{nbytes / 2**20:.0f} MB"


def numa_report(ext: dict[str, Any] | None) -> dict[str, Any] | None:
    """Each checked block as the numa source last saw it, and the findings: first touch, untouched
    pages, thread migration and missing huge pages, each naming the zone that allocated the block
    and the numbers behind it. None when the source did not run."""
    numa = (ext or {}).get("numa")
    if not isinstance(numa, dict):
        return None
    blocks, findings = [], []
    for block in numa.get("blocks", []):
        placements = block.get("placements") or []
        if not placements:
            continue
        last = placements[-1]
        placed = sum(last["pages"])
        nodes_pct = {str(n): round(100.0 * count / placed, 1) for n, count in enumerate(last["pages"]) if count} if placed else {}
        busy = block.get("busy_s_by_node") or []
        main = max(range(len(last["pages"])), key=lambda n: last["pages"][n]) if placed else None
        off_node = sum(b for n, b in enumerate(busy) if n != main) if main is not None else 0.0
        off_pct = round(100.0 * off_node / sum(busy), 1) if sum(busy) > 0 else None
        untouched_pct = round(100.0 * last["untouched"] / last["sampled"], 1) if last["sampled"] else 0.0
        zone = block.get("zone") or "(no zone)"
        row = {"zone": zone, "thread": block.get("thread"), "address": block.get("address"), "bytes": block["bytes"],
               "freed": block.get("freed", False), "nodes_pct": nodes_pct, "untouched_pct": untouched_pct,
               "threads_off_node_pct": off_pct, "huge_pct": last.get("huge_pct", 0.0),
               "history": [{"t_ms": p["t_ms"], "pages": p["pages"], "untouched": p["untouched"]} for p in placements]}  # fmt: skip
        blocks.append(row)
        what = f"{zone} allocated {_gb(block['bytes'])}"
        if main is not None and nodes_pct.get(str(main), 0) >= FIRST_TOUCH_NODE_PCT and off_pct is not None and off_pct >= FIRST_TOUCH_OFF_NODE_PCT:
            findings.append({"kind": "first_touch", "zone": zone, "address": block.get("address"),
                             "text": f"{what}, {nodes_pct[str(main)]:.0f}% on node {main}, while {off_pct:.0f}% of the threads' CPU time "
                                     f"ran on other nodes: initialize it in parallel with the compute's schedule, or interleave it "
                                     f"(numactl --interleave)"})  # fmt: skip
        if untouched_pct > UNTOUCHED_PCT:
            findings.append({"kind": "untouched", "zone": zone, "address": block.get("address"),
                             "text": f"{what}, {untouched_pct:.0f}% of it never touched: it is larger than it needs to be, or used "
                                     f"later than it is allocated"})  # fmt: skip
        if block["bytes"] >= HUGE_BLOCK_BYTES and last.get("huge_pct", 0.0) < HUGE_PCT:
            findings.append({"kind": "no_huge_pages", "zone": zone, "address": block.get("address"),
                             "text": f"{what}, {last.get('huge_pct', 0.0):.0f}% in huge pages: enable transparent huge pages for it "
                                     f"(madvise MADV_HUGEPAGE), to cut TLB misses"})  # fmt: skip
    for move in numa.get("migrations", []):
        findings.append({"kind": "thread_migration", "zone": move["zone"], "thread": move["thread"],
                         "text": f"{move['zone']}: its thread ran on another node with the zone open ({move['count']} time(s)): bind "
                                 f"threads, for example OMP_PROC_BIND=close with OMP_PLACES=cores"})  # fmt: skip
    return {"nodes": numa.get("nodes"), "node_cpus": numa.get("node_cpus"), "passes": numa.get("passes"),
            "blocks": sorted(blocks, key=lambda b: -b["bytes"]), "findings": findings}  # fmt: skip


def summary(session: Session, *, symbols: bool = True, machine_file: str | None = None) -> dict[str, Any]:
    """The summary of @p session's CPU sources: every zone with samples, most first, with its hot
    lines, then any other zone the roofline source counted; each with its FLOPs and bytes."""
    recorded = session.extensions.get("cpu") if isinstance(session.extensions.get("cpu"), dict) else None
    ext = recorded if recorded is not None and "sampling" in recorded else None
    machine_block, peak = _machine(session, recorded, machine_file)
    profile = {}
    if session.snapshot is not None:
        profile = {n.name: n for n in aggregate_flat([c for t in session.snapshot.threads.values() for c in t.children])}
    inclusive = _inclusive_totals(session)
    counted = {name: _roofline(ms, counters) for name, (ms, counters) in inclusive.items() if _roofline(ms, counters)}
    numa = numa_report(recorded)
    if ext is None and not counted and numa is None:
        raise SystemExit(
            f"session {session.label!r} has no CPU data: record it with WAGGLE_SOURCES=sampling (or roofline, or numa) and "
            "WAGGLE_SAVE=FILE"
        )

    zones = _by_zone(ext) if ext else {NO_ZONE: Counter()}
    callers = _callers_by_zone(ext) if ext else {}
    no_zone = zones.pop(NO_ZONE)
    total = sum(sum(c.values()) for c in zones.values()) + sum(no_zone.values())
    addresses = {a for c in zones.values() for a in c} | {a for c in callers.values() for a in _caller_addresses(c)}
    result = _symbols(ext, addresses, symbols and ext is not None) if ext else Result()

    sampled = [name for name, counts in sorted(zones.items(), key=lambda item: (-sum(item[1].values()), item[0])) if counts]
    others = sorted((n for n in counted if n not in zones or not zones[n]), key=lambda n: -inclusive[n][0])
    # Zones that allocated a checked block, after the rest.
    allocating = list(dict.fromkeys(b["zone"] for b in (numa or {}).get("blocks", [])))
    others += [n for n in allocating if n not in sampled and n not in others]
    rows = []
    for name in sampled + others:
        counts = zones.get(name, Counter())
        samples = sum(counts.values())
        zone: dict[str, Any] = {"name": name}
        if (node := profile.get(name)) is not None:
            zone["calls"] = node.call_count
            zone["time_ms"] = {"exclusive": round(node.exclusive_ms, 3), "inclusive": round(inclusive[name][0], 3)}
            zone.update(counted.get(name, {}))
            if peak and name in inclusive:
                _efficiency(zone, *inclusive[name], peak)
        if ext is not None:
            zone["samples"] = samples
            zone["samples_pct"] = _pct(samples, total)
            zone["low_confidence"] = samples < LOW_CONFIDENCE_SAMPLES
        if symbols and ext is not None and samples:
            zone["hot_lines"] = hot_lines(counts, result, SUMMARY_TOP)
            if callers.get(name):
                zone["caller_lines"] = caller_lines(callers[name], samples, result, SUMMARY_TOP)
            zone["hot_functions"] = hot_functions(counts, result, SUMMARY_TOP)
        if numa is not None:
            mine = [b for b in numa["blocks"] if b["zone"] == name]
            if mine:
                zone["numa"] = [{"block_gb": round(b["bytes"] / 2**30, 3), "nodes_pct": b["nodes_pct"],
                                 "threads_off_node_pct": b["threads_off_node_pct"], "huge_pct": b["huge_pct"]} for b in mine]  # fmt: skip
        # The zone's own findings, then its blocks' placement ones.
        placement = [f["text"] for f in (numa or {}).get("findings", []) if f["zone"] == name]
        if found := zone_findings(zone) + placement:
            zone["findings"] = found
        rows.append(zone)
    data: dict[str, Any] = {"schema": SCHEMA, "machine": machine_block, "sampling": ext["sampling"] if ext else None, "zones": rows}
    if ext is not None:
        data["no_zone"] = {"samples": sum(no_zone.values()), "samples_pct": _pct(sum(no_zone.values()), total)}
    if symbols and ext is not None:
        data["symbols"] = _symbols_note(result)
    if numa is not None:
        data["numa"] = {"nodes": numa["nodes"], "findings": [f["text"] for f in numa["findings"]]}
    return data


# ---- Before and after ----------------------------------------------------------------------------


def _noise_pct(node_a: Any, node_b: Any) -> float | None:
    """Twice the relative standard error of the difference of two zones' mean time per call, in
    percent: a rate that changed by less moved within the zone's own run-to-run spread."""
    parts = []
    for node in (node_a, node_b):
        if node is None or not node.call_count or node.exclusive_ms <= 0:
            return None
        mean = node.exclusive_ms / node.call_count
        parts.append((node.stddev_ms / mean) ** 2 / node.call_count)
    return round(200.0 * sum(parts) ** 0.5, 1)


def cpu_diff(a: Session, b: Session, machine_file: str | None = None) -> list[dict[str, Any]]:
    """Each zone's CPU numbers in session @p a and in @p b, matched by name, with how much each
    changed and whether by more than the zone's own noise."""
    sa, sb = summary(a, symbols=False, machine_file=machine_file), summary(b, symbols=False, machine_file=machine_file)
    flat_a, flat_b = (
        {n.name: n for n in aggregate_flat([c for t in s.snapshot.threads.values() for c in t.children])} if s.snapshot else {}
        for s in (a, b)
    )
    zones_a, zones_b = {z["name"]: z for z in sa["zones"]}, {z["name"]: z for z in sb["zones"]}
    metrics = (("samples_pct", lambda z: z.get("samples_pct")),
               ("gflops", lambda z: (z.get("compute") or {}).get("gflops")),
               ("peak_pct", lambda z: (z.get("compute") or {}).get("peak_pct")),
               ("gbs", lambda z: (z.get("memory") or {}).get("gbs")),
               ("byte_efficiency", lambda z: (z.get("memory") or {}).get("byte_efficiency")),
               ("redundant_flops", lambda z: (z.get("compute") or {}).get("redundant_flops")),
               ("gap_to_ideal", lambda z: z.get("gap_to_ideal")))  # fmt: skip
    rows = []
    for name in [n for n in zones_a if n in zones_b]:
        za, zb = zones_a[name], zones_b[name]
        noise = _noise_pct(flat_a.get(name), flat_b.get(name))
        for metric, get in metrics:
            va, vb = get(za), get(zb)
            if va is None or vb is None:
                continue
            change = round(100.0 * (vb - va) / va, 1) if va else None
            row = {"name": name, "metric": metric, "a": va, "b": vb, "change_pct": change}
            if metric == "samples_pct":
                # A share of samples is binomial: its noise is the samples', not the calls'.
                spread = sum((v / 100) * (1 - v / 100) / max(n, 1) for v, n in ((va, za["samples"]), (vb, zb["samples"])))
                row["noise_pct"] = round(200.0 * spread**0.5 / (va / 100), 1) if va else None
            else:
                row["noise_pct"] = noise
            row["significant"] = change is not None and row["noise_pct"] is not None and abs(change) > row["noise_pct"]
            rows.append(row)
    return rows


def _text_diff(rows: list[dict[str, Any]]) -> str:
    lines = ["CPU, zone by zone (* a change larger than the zone's run-to-run noise):", "",
             f"{'change':>8}  {'noise':>7}  {'A':>10}  {'B':>10}  {'metric':<16} zone"]  # fmt: skip
    for r in rows:
        change = "" if r["change_pct"] is None else f"{r['change_pct']:+.1f}%"
        noise = "" if r["noise_pct"] is None else f"±{r['noise_pct']:.1f}%"
        lines.append(f"{change:>8}{'*' if r['significant'] else ' '} {noise:>7}  {r['a']:>10.4g}  {r['b']:>10.4g}  {r['metric']:<16} {r['name']}")
    return "\n".join(lines)


def _zone_counts(session: Session, name: str) -> tuple[dict[str, Any], Counter[int]]:
    ext = _extension(session)
    zones = _by_zone(ext)
    if not zones.get(name):
        known = sorted(n for n, c in zones.items() if c and n != NO_ZONE)
        raise SystemExit(f"no samples in zone {name!r}; zones with samples: {', '.join(known) or 'none'}")
    return ext, zones[name]


def zone_addresses(session: Session, name: str, *, symbols: bool = True) -> dict[str, Any]:
    """Zone @p name's samples by instruction address, summed over its threads and call paths."""
    ext, counts = _zone_counts(session, name)
    result = _symbols(ext, set(counts), symbols)
    samples = sum(counts.values())
    rows = []
    for address, n in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        row: dict[str, Any] = {"address": f"{address:#x}", "samples": n, "samples_pct": _pct(n, samples)}
        if symbols:
            location = result.locations[address]
            row["function"] = location.function
            if location.inner is not None and location.inner.file:
                row["file"], row["line"] = location.inner.file, location.inner.line
        rows.append(row)
    data: dict[str, Any] = {
        "schema": SCHEMA,
        "name": name,
        "samples": samples,
        "low_confidence": samples < LOW_CONFIDENCE_SAMPLES,
        "addresses": rows,
    }
    if symbols:
        data["symbols"] = _symbols_note(result)
    return data


def zone_lines(session: Session, name: str) -> dict[str, Any]:
    """Every source line and function with samples in zone @p name."""
    ext, counts = _zone_counts(session, name)
    callers = _callers_by_zone(ext).get(name, Counter())
    result = _symbols(ext, set(counts) | _caller_addresses(callers), True)
    samples = sum(counts.values())
    return {
        "schema": SCHEMA,
        "name": name,
        "samples": samples,
        "low_confidence": samples < LOW_CONFIDENCE_SAMPLES,
        "hot_lines": hot_lines(counts, result),
        "caller_lines": caller_lines(callers, samples, result),
        "hot_functions": hot_functions(counts, result),
        "symbols": _symbols_note(result),
    }


_INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s*(.*)$")
_LINE_MARK = re.compile(r"^(/.*|[A-Za-z]:\\.*|\S+\.\w+):(\d+)(?: \(discriminator \d+\))?$")


def zone_asm(session: Session, name: str, line: int | None = None) -> dict[str, Any]:
    """The machine code of zone @p name's hottest function, or of the function holding its hottest
    address on source line @p line, with each instruction's samples."""
    ext, counts = _zone_counts(session, name)
    result = _symbols(ext, set(counts), True)
    candidates = counts
    if line is not None:
        on_line = Counter({a: n for a, n in counts.items() if (i := result.locations[a].inner) is not None and i.line == line})
        if not on_line:
            raise SystemExit(f"zone {name!r} has no samples on line {line}")
        candidates = on_line
    hottest = result.locations[max(candidates.items(), key=lambda item: (item[1], -item[0]))[0]]
    if not hottest.module:
        raise SystemExit(f"{hottest.address:#x} is in no module the session recorded")
    if problem := result.problems.get(hottest.module):
        raise SystemExit(f"{hottest.module}: {problem}")
    function = elf.function_at(hottest.module, hottest.offset)
    if function is None:
        raise SystemExit(f"{hottest.module} has no function symbol covering {hottest.offset:#x}")
    objdump = shutil.which("objdump") or shutil.which("llvm-objdump")
    if objdump is None:
        raise SystemExit("the machine code needs objdump (or llvm-objdump) on the path")
    listing = subprocess.run(
        [objdump, "-d", "-l", "-C", "--no-show-raw-insn", f"--start-address={function.start:#x}",
         f"--stop-address={function.start + function.size:#x}", hottest.module],
        capture_output=True, text=True, timeout=300, check=False,
    ).stdout  # fmt: skip

    base = hottest.address - hottest.offset
    in_function = {a - base: n for a, n in counts.items() if function.start <= a - base < function.start + function.size}
    total = sum(counts.values())
    instructions = []
    source_line = None
    for text in listing.splitlines():
        if mark := _LINE_MARK.match(text.strip()):
            source_line = int(mark.group(2))
            continue
        if not (m := _INSTRUCTION.match(text)) or not m.group(2):
            continue
        offset = int(m.group(1), 16)
        n = in_function.get(offset, 0)
        instructions.append(
            {"address": f"{offset:#x}", "text": m.group(2).strip(), "line": source_line, "samples": n, "samples_pct": _pct(n, total)}
        )
    return {
        "schema": SCHEMA,
        "name": name,
        "module": hottest.module,
        # The function the code was compiled into: the outermost frame, which inlining leaves whole.
        "function": hottest.outer.function if hottest.outer is not None else hottest.function,
        "samples": sum(in_function.values()),
        "samples_pct": _pct(sum(in_function.values()), total),
        "line": line,
        "instructions": instructions,
    }


def _shown(path: str) -> str:
    """@p path relative to the current directory when it is under it, for text a person reads."""
    try:
        relative = os.path.relpath(path)
    except ValueError:  # another drive, on Windows
        return path
    return path if relative.startswith("..") else relative


def _location(row: dict[str, Any]) -> str:
    return f"{_shown(row['file'])}:{row['line']}" if row.get("file") else "(no line information)"


def _symbols_footer(note: dict[str, Any] | None) -> list[str]:
    if not note:
        return []
    lines = []
    if not note.get("tool"):
        lines.append("Addresses are unnamed: neither llvm-symbolizer nor addr2line is on the path.")
    for path, problem in sorted(note.get("problems", {}).items()):
        lines.append(f"Not named: {path}: {problem}")
    return [""] + lines if lines else []


def _text_summary(data: dict[str, Any], top: int) -> str:
    s = data["sampling"]
    zones = data["zones"][:top] if top else data["zones"]
    lines = []
    if (m := data.get("machine")) is not None:
        if m.get("peak"):
            p = m["peak"]
            where = f", calibrated on {m['calibrated_on']}" if m.get("calibrated_on") else ""
            lines.append(f"{m['cpu']}: peaks {p['fp64_gflops']:.0f} GFLOP/s FP64, {p['dram_gbs']:.0f} GB/s DRAM{where}")
        if m.get("note"):
            lines.append(f"  {m['note']}")
        lines.append("")
    if s:
        lines += [f"{s['event']} sampled at {s['rate_hz']} Hz on {s['threads']} thread(s): {s['samples']} samples, {s['lost']} lost", ""]
    lines.append(f"{'samples %':>9}  {'samples':>8}  {'excl ms':>10}  {'calls':>8}  zone")
    for z in zones:
        excl = z.get("time_ms", {}).get("exclusive")
        share = f"{z['samples_pct']:>9.1f}  {z['samples']:>8}" if "samples" in z else f"{'':>9}  {'':>8}"
        lines.append(
            f"{share}  {'' if excl is None else f'{excl:.1f}':>10}  "
            f"{z.get('calls', ''):>8}  {z['name']}{'  *' if z.get('low_confidence') else ''}"
        )
        if (compute := z.get("compute")) is not None:
            memory = z.get("memory") or {}
            text = f"{compute['gflops']} GFLOP/s {compute['precision']}"
            if memory:
                text += f"    {memory['gbs']} GB/s (estimated from last-level cache misses)"
            if "intensity" in z:
                text += f"    {z['intensity']} FLOP/byte"
            lines.append(f"{'':>44}{text}")
            notes = []
            if "peak_pct" in compute:
                notes.append(f"{compute['peak_pct']}% of a core's peak")
            if "redundant_flops" in compute:
                notes.append(f"{compute['redundant_flops']}x the FLOPs its work needs")
            if "byte_efficiency" in memory:
                notes.append(f"byte efficiency {memory['byte_efficiency']}")
            if "gap_to_ideal" in z:
                notes.append(f"{z['gap_to_ideal']}x its ideal time ({z['bound']} bound)")
            if notes:
                lines.append(f"{'':>44}{', '.join(notes)}")
        for hot in z.get("hot_lines", [])[:3]:
            lines.append(f"{'':>44}{hot['samples_pct']:>5.1f}%  {_location(hot)}  {hot['function']}")
        for finding in z.get("findings", []):
            lines.append(f"{'':>44}! {finding}")
        for call in z.get("caller_lines", [])[:2]:
            lines.append(f"{'':>44}{call['samples_pct']:>5.1f}%  {_location(call)}  calls {call['callee']}")
    if nz := data.get("no_zone"):
        lines.append(f"{nz['samples_pct']:>9.1f}  {nz['samples']:>8}  {'':>10}  {'':>8}  {NO_ZONE}")
    if any(z.get("low_confidence") for z in zones):
        lines += ["", f"* under {LOW_CONFIDENCE_SAMPLES} samples: a longer run gives a surer share"]
    return "\n".join(lines + _symbols_footer(data.get("symbols")))


def _text_addresses(data: dict[str, Any], top: int) -> str:
    rows = data["addresses"][:top] if top else data["addresses"]
    lines = [f"{data['name']}: {data['samples']} samples", "", f"{'samples %':>9}  {'samples':>8}  {'address':<18}  function"]
    lines += [f"{r['samples_pct']:>9.1f}  {r['samples']:>8}  {r['address']:<18}  {r.get('function', '')}" for r in rows]
    return "\n".join(lines + _symbols_footer(data.get("symbols")))


def _text_lines(data: dict[str, Any], top: int) -> str:
    rows = data["hot_lines"][:top] if top else data["hot_lines"]
    lines = [f"{data['name']}: {data['samples']} samples", "", f"{'samples %':>9}  {'samples':>8}  line"]
    for r in rows:
        lines.append(f"{r['samples_pct']:>9.1f}  {r['samples']:>8}  {_location(r)}  {r['function']}")
        if into := r.get("inlined_into"):
            lines.append(f"{'':>21}inlined into {into['function']}" + (f" at {_shown(into['file'])}:{into['line']}" if into.get("file") else ""))
    calls = data.get("caller_lines", [])
    if calls:
        lines += ["", "Time in code without line information, by the line that called it:", "",
                  f"{'samples %':>9}  {'samples':>8}  line"]  # fmt: skip
        for r in calls[:top] if top else calls:
            lines.append(f"{r['samples_pct']:>9.1f}  {r['samples']:>8}  {_location(r)}  {r['function']}  calls {r['callee']}")
    functions = data["hot_functions"][:top] if top else data["hot_functions"]
    lines += ["", f"{'samples %':>9}  {'samples':>8}  function"]
    lines += [f"{r['samples_pct']:>9.1f}  {r['samples']:>8}  {r['function']}" for r in functions]
    return "\n".join(lines + _symbols_footer(data.get("symbols")))


def _text_asm(data: dict[str, Any]) -> str:
    lines = [f"{data['function']}  ({_shown(data['module'])})", f"{data['samples_pct']:.1f}% of {data['name']}'s samples", ""]
    previous = None
    for i in data["instructions"]:
        if i["line"] != previous and i["line"] is not None:
            lines.append(f"{'':>16}line {i['line']}")
            previous = i["line"]
        mark = ">" if data["line"] is not None and i["line"] == data["line"] else " "
        share = f"{i['samples_pct']:5.1f}%" if i["samples"] else ""
        lines.append(f"{mark}{share:>7} {i['samples'] or '':>6}  {i['address'][2:]:>8}:  {i['text']}")
    return "\n".join(lines)


def _text_numa(data: dict[str, Any]) -> str:
    lines = [f"{data['nodes']} NUMA nodes, {data['passes']} passes", "", f"{'size':>9}  {'off-node CPU':>12}  {'huge':>5}  nodes  zone"]
    for b in data["blocks"]:
        nodes = ", ".join(f"{n}: {pct:.0f}%" for n, pct in b["nodes_pct"].items()) or "untouched"
        off = "" if b["threads_off_node_pct"] is None else f"{b['threads_off_node_pct']:.0f}%"
        lines.append(f"{_gb(b['bytes']):>9}  {off:>12}  {b['huge_pct']:>4.0f}%  {nodes}  {b['zone']}{'  (freed)' if b['freed'] else ''}")
    if data["findings"]:
        lines += ["", "Findings:"] + [f"  {f['text']}" for f in data["findings"]]
    return "\n".join(lines)


def run(args: argparse.Namespace) -> int:
    from .report import _load, _pick

    if (args.lines or args.asm) and not args.zone:
        raise SystemExit("--lines and --asm take a zone: --zone NAME")
    if args.line is not None and not args.asm:
        raise SystemExit("--line goes with --asm")
    session = _pick(_load(args.file), args.session)
    if args.numa:
        data = numa_report(session.extensions.get("cpu") if isinstance(session.extensions.get("cpu"), dict) else None)
        if data is None:
            raise SystemExit(f"session {session.label!r} has no NUMA placement: record it with WAGGLE_SOURCES=numa and WAGGLE_SAVE=FILE")
        print(json.dumps(data, indent=2) if args.format == "json" else _text_numa(data))
        return 0
    if args.asm:
        data = zone_asm(session, args.zone, args.line)
        text = _text_asm(data)
    elif args.lines:
        data = zone_lines(session, args.zone)
        text = _text_lines(data, args.top)
    elif args.zone:
        data = zone_addresses(session, args.zone, symbols=not args.no_symbols)
        text = _text_addresses(data, args.top)
    else:
        data = summary(session, symbols=not args.no_symbols, machine_file=args.machine_file)
        text = _text_summary(data, args.top)
    print(json.dumps(data, indent=2) if args.format == "json" else text)
    return 0
