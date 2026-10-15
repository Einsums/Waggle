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

from . import elf
from .analysis import aggregate_flat
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
        help="Where each zone's CPU time goes, from a session recorded with WAGGLE_SOURCES=sampling",
        description="Zones ranked by their share of the CPU samples, each with its hot lines; or one zone's lines, "
        "addresses or machine code.",
    )
    cpu.add_argument("file", help="session file recorded with WAGGLE_SOURCES=sampling")
    cpu.add_argument("--session", default="-1", help="which session in a multi-session file: index or label (default: the last)")
    cpu.add_argument("--zone", metavar="NAME", help="one zone: its samples by address, or with --lines or --asm")
    view = cpu.add_mutually_exclusive_group()
    view.add_argument("--lines", action="store_true", help="with --zone: every source line with samples, and the functions")
    view.add_argument("--asm", action="store_true", help="with --zone: the hottest function's machine code, samples per instruction")
    cpu.add_argument("--line", type=int, metavar="N", help="with --asm: the function holding line N, its instructions marked")
    cpu.add_argument("--no-symbols", action="store_true", help="leave addresses unnamed, which is faster")
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


def summary(session: Session, *, symbols: bool = True) -> dict[str, Any]:
    """The summary of @p session's samples: every zone with samples, most first, with its hot lines."""
    ext = _extension(session)
    zones = _by_zone(ext)
    no_zone = zones.pop(NO_ZONE)
    total = sum(sum(c.values()) for c in zones.values()) + sum(no_zone.values())
    result = _symbols(ext, {a for c in zones.values() for a in c}, symbols)

    profile = {}
    if session.snapshot is not None:
        profile = {n.name: n for n in aggregate_flat([c for t in session.snapshot.threads.values() for c in t.children])}

    rows = []
    for name, counts in sorted(zones.items(), key=lambda item: (-sum(item[1].values()), item[0])):
        samples = sum(counts.values())
        if not samples:
            continue
        zone: dict[str, Any] = {"name": name}
        if (node := profile.get(name)) is not None:
            zone["calls"] = node.call_count
            zone["time_ms"] = {"exclusive": round(node.exclusive_ms, 3)}
        zone["samples"] = samples
        zone["samples_pct"] = _pct(samples, total)
        zone["low_confidence"] = samples < LOW_CONFIDENCE_SAMPLES
        if symbols:
            zone["hot_lines"] = hot_lines(counts, result, SUMMARY_TOP)
            zone["hot_functions"] = hot_functions(counts, result, SUMMARY_TOP)
        rows.append(zone)
    data: dict[str, Any] = {
        "schema": SCHEMA,
        "sampling": ext["sampling"],
        "zones": rows,
        "no_zone": {"samples": sum(no_zone.values()), "samples_pct": _pct(sum(no_zone.values()), total)},
    }
    if symbols:
        data["symbols"] = _symbols_note(result)
    return data


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
    result = _symbols(ext, set(counts), True)
    samples = sum(counts.values())
    return {
        "schema": SCHEMA,
        "name": name,
        "samples": samples,
        "low_confidence": samples < LOW_CONFIDENCE_SAMPLES,
        "hot_lines": hot_lines(counts, result),
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
    lines = [
        f"{s['event']} sampled at {s['rate_hz']} Hz on {s['threads']} thread(s): {s['samples']} samples, {s['lost']} lost",
        "",
        f"{'samples %':>9}  {'samples':>8}  {'excl ms':>10}  {'calls':>8}  zone",
    ]
    for z in zones:
        excl = z.get("time_ms", {}).get("exclusive")
        lines.append(
            f"{z['samples_pct']:>9.1f}  {z['samples']:>8}  {'' if excl is None else f'{excl:.1f}':>10}  "
            f"{z.get('calls', ''):>8}  {z['name']}{'  *' if z['low_confidence'] else ''}"
        )
        for hot in z.get("hot_lines", [])[:3]:
            lines.append(f"{'':>44}{hot['samples_pct']:>5.1f}%  {_location(hot)}  {hot['function']}")
    nz = data["no_zone"]
    lines.append(f"{nz['samples_pct']:>9.1f}  {nz['samples']:>8}  {'':>10}  {'':>8}  {NO_ZONE}")
    if any(z["low_confidence"] for z in zones):
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


def run(args: argparse.Namespace) -> int:
    from .report import _load, _pick

    if (args.lines or args.asm) and not args.zone:
        raise SystemExit("--lines and --asm take a zone: --zone NAME")
    if args.line is not None and not args.asm:
        raise SystemExit("--line goes with --asm")
    session = _pick(_load(args.file), args.session)
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
        data = summary(session, symbols=not args.no_symbols)
        text = _text_summary(data, args.top)
    print(json.dumps(data, indent=2) if args.format == "json" else text)
    return 0
