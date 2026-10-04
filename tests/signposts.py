# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""The signposts source end to end: record a program with Instruments (xctrace's Logging template),
export its os_signpost table, and check every zone arrived as an interval in its library's lane.

Usage: signposts.py <program>, the program being SignpostsProgram.cpp. Prints "signposts: N
intervals as expected" on success, which ctest requires, so a script that checks nothing fails.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

TABLE = '/trace-toc/run[@number="1"]/data/table[@schema="os-signpost"]'


def signposts(trace: Path) -> list[dict]:
    """The os-signpost table, one dict per row by column mnemonic, with "ns", the row's time in
    nanoseconds since the recording began. A value the export repeats is written once with an id and
    referred to by it after; a cell's "fmt" is for people, its text the value."""
    xml = subprocess.run(
        ["xcrun", "xctrace", "export", "--input", str(trace), "--xpath", TABLE], check=True, capture_output=True, text=True
    ).stdout
    root = ET.fromstring(xml)
    columns = [c.findtext("mnemonic") for c in root.iter("col")]
    shown: dict[str, str] = {}
    raw: dict[str, str] = {}

    def value(cell: ET.Element) -> tuple[str, str]:
        if "ref" in cell.attrib:
            return shown.get(cell.attrib["ref"], ""), raw.get(cell.attrib["ref"], "")
        for child in cell:
            value(child)
        pair = cell.attrib.get("fmt") or (cell.text or ""), cell.text or ""
        if "id" in cell.attrib:
            shown[cell.attrib["id"]], raw[cell.attrib["id"]] = pair
        return pair

    out = []
    for row in root.iter("row"):
        cells = [value(cell) for cell in row]
        entry: dict = {name: shown_value for name, (shown_value, _) in zip(columns, cells)}
        entry["ns"] = int(cells[0][1] or 0)
        out.append(entry)
    return out


def main(program: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "signposts.trace"
        # A recording that never ends is xctrace's, not the program's (CI's virtual Macs hang one
        # now and then before it starts recording), so it gets one more try; a wrong recording
        # fails at once.
        run = None
        for attempt in (1, 2):
            if trace.exists():
                shutil.rmtree(trace)
            try:
                run = subprocess.run(
                    ["xcrun", "xctrace", "record", "--template", "Logging", "--output", str(trace),
                     "--env", "WAGGLE_SOURCES=signposts", "--env", "WAGGLE_REPORT=false", "--launch", "--", program],
                    env=dict(os.environ), capture_output=True, text=True, timeout=120,
                )  # fmt: skip
                break
            except subprocess.TimeoutExpired as exc:
                # Usually waiting for an authorization to use developer tools (DevToolsSecurity -enable gives it).
                print(f"signposts: xctrace did not finish recording in {exc.timeout} s (attempt {attempt})")
                print(exc.stdout or "", exc.stderr or "", sep="\n")
        if run is None:
            return 1
        if run.returncode != 0 or not trace.exists():
            print(run.stdout, run.stderr, sep="\n")
            return 1
        found = [r for r in signposts(trace) if r.get("subsystem") == "waggle"]

    # An id names one interval among those open at once: the next zone at the same depth on the
    # same thread may take it again once the last has ended. So begins and ends pair in time order.
    problems = []
    open_by_id: dict[str, dict] = {}
    intervals = []
    for r in sorted(found, key=lambda r: r["ns"]):
        if r["event-type"] == "Begin":
            if r["identifier"] in open_by_id:
                problems.append(f"interval {r['identifier']} began again while open")
            open_by_id[r["identifier"]] = r
        elif r["event-type"] == "End":
            b = open_by_id.pop(r["identifier"], None)
            if b is None:
                problems.append(f"interval {r['identifier']} ended without a begin")
            else:
                intervals.append((b["message"], b["category"], b["ns"], r["ns"]))
    if open_by_id:
        problems.append(f"{len(open_by_id)} interval(s) never ended")

    want = {
        ("signpost outer", "sptest_a"): 2,
        ("signpost inner", "sptest_b"): 2,
        ("signpost dynamic 0", "zones"): 1,
        ("signpost dynamic 1", "zones"): 1,
    }
    got: dict[tuple[str, str], int] = {}
    for message, category, _, _ in intervals:
        got[(message, category)] = got.get((message, category), 0) + 1
    if got != want:
        problems.append(f"intervals by name and library: {got}, wanted {want}")
    # Each inner zone lies within an outer one.
    outers = [(s, e) for m, _, s, e in intervals if m == "signpost outer"]
    for m, _, s, e in intervals:
        if m == "signpost inner" and not any(start <= s and e <= end for start, end in outers):
            problems.append(f"an inner zone ({s}..{e} ns) lies within no outer one: {outers}")

    for p in problems:
        print("signposts:", p)
    print(f"signposts: {len(intervals)} intervals {'with problems' if problems else 'as expected'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
