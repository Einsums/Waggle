# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The sampling source end to end: run the sampling test's [.program] case with WAGGLE_SOURCES=sampling
and WAGGLE_SAVE set, and check that ``waggle cpu`` reads the saved session's samples back with the
shares the program gave its zones; then its [.hot_line] case, and check that ``waggle cpu`` names the
loop that does nine tenths of the work by its source line and disassembles its function.

Usage: sampling_session.py <waggle_Sampling_test>. Prints "sampling: N samples as expected" on success,
which ctest requires; exits 4, which ctest counts as skipped, where perf_event cannot sample.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def check(condition: bool, what: str) -> None:
    if not condition:
        print(f"sampling: FAILED: {what}")
        sys.exit(1)


SOURCE = Path(__file__).resolve().parent / "unit" / "Sampling.cpp"


def marked_line(marker: str) -> int:
    """The line of tests/unit/Sampling.cpp that carries @p marker."""
    return next(n for n, text in enumerate(SOURCE.read_text().splitlines(), 1) if marker in text)


def record(program: str, case: str, saved: Path) -> dict | None:
    """Run @p case with sampling on, saving to @p saved; the cpu extension, or None if perf could not sample."""
    env = dict(os.environ, WAGGLE_SOURCES="sampling", WAGGLE_SAVE=str(saved), WAGGLE_REPORT="false")
    subprocess.run([program, case], env=env, check=True)
    check(saved.exists(), f"WAGGLE_SAVE wrote no session file at {saved}")
    return json.loads(saved.read_text()).get("extensions", {}).get("cpu")


def waggle_cpu(*args: str) -> dict:
    out = subprocess.run([sys.executable, "-m", "waggle", "cpu", *args, "--format", "json"], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def check_hot_line(program: str, tmp: Path) -> None:
    saved = tmp / "hot.json"
    check(record(program, "[.hot_line]", saved) is not None, "the hot_line run saved no samples")
    hot, cool = marked_line("WAGGLE_TEST_HOT_LINE"), marked_line("WAGGLE_TEST_COOL_LINE")
    lines = waggle_cpu(str(saved), "--zone", "hot_line", "--lines")
    check(not lines["symbols"]["problems"], f"modules not named: {lines['symbols']['problems']}")
    top = lines["hot_lines"][0]
    where = f"{top['file']}:{top['line']}"
    check(top["file"] is not None and Path(top["file"]).resolve() == SOURCE, f"the hottest line is {where}, not in {SOURCE}")
    check(top["line"] == hot, f"the hottest line is {top['line']}, not the hot loop's {hot}")
    check(top["samples_pct"] >= 85.0, f"the hot loop's line has {top['samples_pct']}% of the samples, under 85%")
    check(lines["hot_lines"][1]["line"] == cool, f"the second line is {lines['hot_lines'][1]['line']}, not the cool loop's {cool}")
    summary = waggle_cpu(str(saved))
    zone = next(z for z in summary["zones"] if z["name"] == "hot_line")
    check(zone["hot_lines"][0]["line"] == hot, "the summary's hottest line is not the hot loop's")
    asm = waggle_cpu(str(saved), "--zone", "hot_line", "--asm")
    check(asm["function"].endswith("hot_loop"), f"--asm disassembled {asm['function']}, not hot_loop")
    check(asm["samples_pct"] >= 85.0 and any(i["samples"] for i in asm["instructions"]), "--asm put no samples on hot_loop")
    print(f"sampling: line {hot} has {top['samples_pct']}% of hot_line's samples")


def check_caller_lines(program: str, tmp: Path) -> None:
    saved = tmp / "callers.json"
    check(record(program, "[.caller_lines]", saved) is not None, "the caller_lines run saved no samples")
    first, second = marked_line("WAGGLE_TEST_CALLER_A"), marked_line("WAGGLE_TEST_CALLER_B")
    calls = waggle_cpu(str(saved), "--zone", "caller_lines", "--lines")["caller_lines"]
    by_line = {c["line"]: c["samples"] for c in calls if c["file"] and Path(c["file"]).resolve() == SOURCE}
    a, b = by_line.get(first, 0), by_line.get(second, 0)
    check(a + b >= 400, f"only {a + b} of the library's samples were credited to lines {first} and {second}: {calls[:4]}")
    check(0.68 < a / (a + b) < 0.82, f"line {first} has {a / (a + b):.3f} of the library's samples, not about 0.75")
    print(f"sampling: lines {first} and {second} called the library for {a} and {b} samples")


def main() -> None:
    program = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        saved = Path(tmp) / "run.json"
        if record(program, "[.program]", saved) is None:
            print("sampling: no cpu extension: perf_event could not sample here, so the source recorded nothing")
            sys.exit(4)
        summary = waggle_cpu(str(saved), "--no-symbols")
        check(summary["schema"] == "waggle.cpu.summary/1", f"schema {summary['schema']}")
        zones = {z["name"]: z for z in summary["zones"]}
        check({"saved_a", "saved_b"} <= zones.keys(), f"zones {sorted(zones)}")
        a, b = zones["saved_a"]["samples"], zones["saved_b"]["samples"]
        # Cycles are sampled only while the thread runs, which a loaded machine cuts short: the share
        # is the check, and a few hundred samples bound it well enough.
        check(a + b >= 400, f"only {a + b} samples in 2 s at 997 Hz")
        check(0.68 < a / (a + b) < 0.82, f"saved_a has {a / (a + b):.3f} of the samples, not about 0.75")
        check(zones["saved_a"]["calls"] > 0, "the summary joins the profile's calls")
        check_hot_line(program, Path(tmp))
        check_caller_lines(program, Path(tmp))
    print(f"sampling: {summary['sampling']['samples']} samples as expected")

if __name__ == "__main__":
    main()
