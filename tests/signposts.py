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

ROUNDS = 40  # how many rounds the program emits, passed to it
WHOLE = 2  # how many of the last must arrive whole for a recording to count


def emitted(rounds: int) -> list[tuple[str, str, str, int | None]]:
    """Every signpost the program emits, in order: event type, category (the library's lane), the
    name a begin carries (an end carries none), and for an end the index of its begin."""
    out: list[tuple[str, str, str, int | None]] = []
    for i in range(rounds):
        start = len(out)
        out += [
            ("Begin", "sptest_a", "signpost outer", None),
            ("Begin", "sptest_b", "signpost inner", None),
            ("End", "sptest_b", "", start + 1),
            ("Begin", "zones", f"signpost dynamic {i}", None),
            ("End", "zones", "", start + 3),
            ("End", "sptest_a", "", start),
        ]
    return out


def check(found: list[dict]) -> tuple[list[str], int, int]:
    """What is wrong with the program's signposts as recorded, how many intervals they hold, and how
    many rounds arrived whole.

    A recording that starts late loses what the program emitted first and nothing after, so what it
    holds must be exactly the last signposts emitted, in order, every end paired with its begin by
    identifier unless that begin is among those lost. Anything else - a name or lane wrong, an end
    that is not its begin's, one missing from the middle - is the source's fault, not xctrace's."""
    want = emitted(ROUNDS)
    rows = sorted(found, key=lambda r: r["ns"])  # stable: a tie keeps the export's order
    got = [(r["event-type"], r["category"], r.get("message", "") if r["event-type"] == "Begin" else "")
           for r in rows]  # fmt: skip
    lost = len(want) - len(got)
    if lost < 0 or got != [w[:3] for w in want[lost:]]:
        # Matched from the end, the last signpost that differs is where one was dropped or changed.
        tail = [w[:3] for w in want]
        back = next(k for k in range(1, len(got) + 1) if k > len(tail) or got[-k] != tail[-k])
        expected = tail[-back] if back <= len(tail) else "nothing"
        where = f"signpost {len(got) - back} is {got[-back]}, wanted {expected}"
        return [f"{len(got)} signposts are not the last {len(got)} emitted: {where}"], 0, 0
    problems = []
    intervals = 0
    for j, row in enumerate(rows):
        begin = want[lost + j][3]
        if begin is None or begin < lost:
            continue
        intervals += 1
        opened = rows[begin - lost]["identifier"]
        if opened != row["identifier"]:
            problems.append(f"signpost {j} ends {row['identifier']}, but its begin was {opened}")
    whole = ROUNDS - (lost + 5) // 6
    return problems, intervals, whole


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
        # signposts, every one or the first ones. They are lost when the logging stream attaches
        # after the program has emitted them, or to another process's busy stream of signposts.
        # Losing the first rounds is fine while the last WHOLE arrive; a hang, an empty recording
        # or one too short gets one more try. A recording that is not the end of what the program
        # emitted is wrong, and fails at once, and so does a second failed try; both say what else
        # was emitting.
        found: list[dict] = []
        problems: list[str] = []
        intervals = whole = 0
        for attempt in (1, 2):
            if trace.exists():
                shutil.rmtree(trace)
            run = subprocess.Popen(
                ["xcrun", "xctrace", "record", "--template", "Logging", "--output", str(trace),
                 "--env", "WAGGLE_SOURCES=signposts", "--env", "WAGGLE_REPORT=false", "--launch", "--", program,
                 str(ROUNDS)],
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
            if not found:
                print(f"signposts: the recording holds no waggle signposts (attempt {attempt})")
                continue
            problems, intervals, whole = check(found)
            if problems or whole >= WHOLE:
                break
            print(f"signposts: the recording began late; only the last {whole} of {ROUNDS} rounds are whole "
                  f"(attempt {attempt})")  # fmt: skip
        if not problems and whole < WHOLE:
            print(f"signposts: no recording captured the last {WHOLE} rounds of the program's signposts")
            describe_signpost_traffic(Path(tmp))
            return 1

    for p in problems:
        print("signposts:", p)
    print(f"signposts: {intervals} intervals {'with problems' if problems else 'as expected'}")
    if problems:
        with tempfile.TemporaryDirectory() as tmp:
            describe_signpost_traffic(Path(tmp))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
