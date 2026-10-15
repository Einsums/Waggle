# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""``waggle cpu``: where each zone's CPU time goes, from the ``sampling`` source.

Reads the ``cpu`` extension a session saves when it was recorded with ``WAGGLE_SOURCES=sampling``,
and gives a summary per zone for a person or an agent to read: zones ranked by their share of the
samples, beside the time and calls the profile measured. The summary's schema is
``waggle.cpu.summary/1``; later sources add fields to it, never change the ones here.
"""

from __future__ import annotations

import argparse
import json
from typing import Any

from .analysis import aggregate_flat
from .session import Session

SCHEMA = "waggle.cpu.summary/1"

#: A zone with fewer samples than this has a share a longer run could move: the summary says so.
LOW_CONFIDENCE_SAMPLES = 200


def add_parser(sub) -> None:
    cpu = sub.add_parser(
        "cpu",
        help="Where each zone's CPU time goes, from a session recorded with WAGGLE_SOURCES=sampling",
        description="Zones ranked by their share of the CPU samples, or one zone's samples by address.",
    )
    cpu.add_argument("file", help="session file recorded with WAGGLE_SOURCES=sampling")
    cpu.add_argument("--session", default="-1", help="which session in a multi-session file: index or label (default: the last)")
    cpu.add_argument("--zone", metavar="NAME", help="one zone's samples by instruction address")
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


def summary(session: Session) -> dict[str, Any]:
    """The summary of @p session's samples: every zone with samples, most first."""
    ext = _extension(session)
    by_name: dict[str, int] = {}
    for zone in ext.get("zones", []):
        name = zone["path"][-1]
        by_name[name] = by_name.get(name, 0) + int(zone["samples"])
    no_zone = int(ext.get("no_zone", {}).get("samples", 0))
    total = sum(by_name.values()) + no_zone

    profile = {}
    if session.snapshot is not None:
        profile = {n.name: n for n in aggregate_flat([c for t in session.snapshot.threads.values() for c in t.children])}

    zones = []
    for name, samples in sorted(by_name.items(), key=lambda item: (-item[1], item[0])):
        zone: dict[str, Any] = {"name": name}
        if (node := profile.get(name)) is not None:
            zone["calls"] = node.call_count
            zone["time_ms"] = {"exclusive": round(node.exclusive_ms, 3)}
        zone["samples"] = samples
        zone["samples_pct"] = _pct(samples, total)
        zone["low_confidence"] = samples < LOW_CONFIDENCE_SAMPLES
        zones.append(zone)
    return {
        "schema": SCHEMA,
        "sampling": ext["sampling"],
        "zones": zones,
        "no_zone": {"samples": no_zone, "samples_pct": _pct(no_zone, total)},
    }


def zone_addresses(session: Session, name: str) -> dict[str, Any]:
    """Zone @p name's samples by instruction address, summed over its threads and call paths."""
    ext = _extension(session)
    zones = [z for z in ext.get("zones", []) if z["path"][-1] == name]
    if not zones and name != "(no zone)":
        known = sorted({z["path"][-1] for z in ext.get("zones", [])})
        raise SystemExit(f"no samples in zone {name!r}; zones with samples: {', '.join(known) or 'none'}")
    if name == "(no zone)":
        zones = [ext.get("no_zone", {})]
    counts: dict[str, int] = {}
    for zone in zones:
        for address, n in zone.get("addresses", {}).items():
            counts[address] = counts.get(address, 0) + int(n)
    samples = sum(counts.values())
    return {
        "schema": SCHEMA,
        "name": name,
        "samples": samples,
        "low_confidence": samples < LOW_CONFIDENCE_SAMPLES,
        "addresses": [
            {"address": address, "samples": n, "samples_pct": _pct(n, samples)}
            for address, n in sorted(counts.items(), key=lambda item: (-item[1], item[0]))
        ],
    }


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
    nz = data["no_zone"]
    lines.append(f"{nz['samples_pct']:>9.1f}  {nz['samples']:>8}  {'':>10}  {'':>8}  (no zone)")
    if any(z["low_confidence"] for z in zones):
        lines += ["", f"* under {LOW_CONFIDENCE_SAMPLES} samples: a longer run gives a surer share"]
    return "\n".join(lines)


def _text_zone(data: dict[str, Any], top: int) -> str:
    rows = data["addresses"][:top] if top else data["addresses"]
    lines = [f"{data['name']}: {data['samples']} samples", "", f"{'samples %':>9}  {'samples':>8}  address"]
    lines += [f"{r['samples_pct']:>9.1f}  {r['samples']:>8}  {r['address']}" for r in rows]
    return "\n".join(lines)


def run(args: argparse.Namespace) -> int:
    from .report import _load, _pick

    session = _pick(_load(args.file), args.session)
    if args.zone:
        data = zone_addresses(session, args.zone)
        text = _text_zone(data, args.top)
    else:
        data = summary(session)
        text = _text_summary(data, args.top)
    print(json.dumps(data, indent=2) if args.format == "json" else text)
    return 0
