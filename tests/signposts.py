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


def describe_hang(pid: int, trace: Path) -> None:
    """What a recording that will not end is doing: xctrace's processes, a few seconds of each one's
    stacks, and how far the trace bundle got. Sampling xctrace needs root on a runner, which CI has
    without a password; elsewhere sample runs as the user and may be refused."""
    pids = [pid]
    for parent in pids:
        children = subprocess.run(["pgrep", "-P", str(parent)], capture_output=True, text=True).stdout.split()
        pids.extend(int(child) for child in children)
    listing = ["ps", "-o", "pid,ppid,stat,etime,time,command", "-p", ",".join(map(str, pids))]
    print(subprocess.run(listing, capture_output=True, text=True).stdout)
    sudo = ["sudo", "-n"] if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode == 0 else []
    for each in pids:
        sampled = subprocess.run([*sudo, "sample", str(each), "3"], capture_output=True, text=True, timeout=60)
        print(f"signposts: stacks of process {each}")
        print(sampled.stdout, sampled.stderr, sep="\n")
    if trace.exists():
        files = sorted(trace.rglob("*"))
        size = sum(f.stat().st_size for f in files if f.is_file())
        print(f"signposts: the trace bundle holds {len(files)} entries, {size} bytes")
    else:
        print("signposts: no trace bundle was written")


def describe_signpost_traffic(tmp: Path) -> None:
    """Who else emits signposts: two seconds of every process's, counted by process and subsystem.
    xctrace drops events, the program's among them, when another process emits signposts fast (a
    single busy loop of them loses every one), so a recording that lost the program's names here
    what it lost them to. The export streams: a busy machine fills it with millions of rows."""
    trace = tmp / "traffic.trace"
    try:
        subprocess.run(
            ["xcrun", "xctrace", "record", "--instrument", "os_signpost", "--all-processes", "--time-limit", "2s",
             "--output", str(trace)],
            capture_output=True, timeout=120, check=True,
        )  # fmt: skip
        export = subprocess.Popen(
            ["xcrun", "xctrace", "export", "--input", str(trace), "--xpath", TABLE], stdout=subprocess.PIPE
        )
        named: dict[str, str] = {}  # a process or subsystem by the id the export first gave it
        counts: dict[tuple[str, str], int] = {}
        for _, element in ET.iterparse(export.stdout):
            if element.tag in ("process", "subsystem") and "id" in element.attrib:
                named[element.attrib["id"]] = element.attrib.get("fmt", "")
            elif element.tag == "row":
                key = []
                for tag in ("process", "subsystem"):
                    cell = element.find(tag)
                    key.append("?" if cell is None else named.get(cell.attrib.get("ref", cell.attrib.get("id", "")), "?"))
                counts[(key[0], key[1])] = counts.get((key[0], key[1]), 0) + 1
                element.clear()
        export.wait(timeout=120)
    except (subprocess.SubprocessError, ET.ParseError) as exc:
        print(f"signposts: could not record the machine's signposts: {exc}")
        return
    print(f"signposts: {sum(counts.values())} signposts from every process in 2 s, the most by")
    for (process, subsystem), n in sorted(counts.items(), key=lambda item: -item[1])[:10]:
        print(f"signposts: {n:>9}  {process}  {subsystem}")


def main(program: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "signposts.trace"
        # Two ways a recording fails that are xctrace's, not the program's, and CI's virtual Macs
        # show both now and then: it never ends, or it ends cleanly having lost the program's
        # signposts, every one or some. They are lost when the logging stream attaches after the
        # program has emitted them, or to another process's busy stream of signposts. Either gets
        # one more try. A recording that captured the program but got it wrong is checked below
        # and fails at once, and so does a second empty one; both say what else was emitting.
        found: list[dict] = []
        for attempt in (1, 2):
            if trace.exists():
                shutil.rmtree(trace)
            run = subprocess.Popen(
                ["xcrun", "xctrace", "record", "--template", "Logging", "--output", str(trace),
                 "--env", "WAGGLE_SOURCES=signposts", "--env", "WAGGLE_REPORT=false", "--launch", "--", program],
                env=dict(os.environ), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )  # fmt: skip
            try:
                stdout, stderr = run.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                # A hang waiting for a developer-tools authorization stops before the program
                # launches (DevToolsSecurity -enable gives it). CI has also hung after the program
                # exited, while xctrace ended the recording; what it was doing is printed here.
                print(f"signposts: xctrace did not finish recording in 120 s (attempt {attempt})")
                describe_hang(run.pid, trace)
                run.kill()
                stdout, stderr = run.communicate()
                print(stdout, stderr, sep="\n")
                continue
            if run.returncode != 0 or not trace.exists():
                print(stdout, stderr, sep="\n")
                return 1
            found = [r for r in signposts(trace) if r.get("subsystem") == "waggle"]
            if found:
                break
            print(f"signposts: the recording holds no waggle signposts (attempt {attempt})")
        if not found:
            print("signposts: no recording captured the program's signposts")
            describe_signpost_traffic(Path(tmp))
            return 1

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
    if problems:
        with tempfile.TemporaryDirectory() as tmp:
            describe_signpost_traffic(Path(tmp))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
