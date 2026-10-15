# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The sampling source end to end: run the sampling test's [.program] case with WAGGLE_SOURCES=sampling
and WAGGLE_SAVE set, and check that ``waggle cpu`` reads the saved session's samples back with the
shares the program gave its zones.

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


def main() -> None:
    program = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        saved = Path(tmp) / "run.json"
        env = dict(os.environ, WAGGLE_SOURCES="sampling", WAGGLE_SAVE=str(saved), WAGGLE_REPORT="false")
        subprocess.run([program, "[.program]"], env=env, check=True)
        check(saved.exists(), f"WAGGLE_SAVE wrote no session file at {saved}")
        session = json.loads(saved.read_text())
        extension = session.get("extensions", {}).get("cpu")
        if extension is None:
            print("sampling: no cpu extension: perf_event could not sample here, so the source recorded nothing")
            sys.exit(4)
        out = subprocess.run(
            [sys.executable, "-m", "waggle", "cpu", str(saved), "--format", "json"], check=True, capture_output=True, text=True
        ).stdout
    summary = json.loads(out)
    check(summary["schema"] == "waggle.cpu.summary/1", f"schema {summary['schema']}")
    zones = {z["name"]: z for z in summary["zones"]}
    check({"saved_a", "saved_b"} <= zones.keys(), f"zones {sorted(zones)}")
    a, b = zones["saved_a"]["samples"], zones["saved_b"]["samples"]
    # Cycles are sampled only while the thread runs, which a loaded machine cuts short: the share is
    # the check, and a few hundred samples bound it well enough.
    check(a + b >= 400, f"only {a + b} samples in 2 s at 997 Hz")
    check(0.68 < a / (a + b) < 0.82, f"saved_a has {a / (a + b):.3f} of the samples, not about 0.75")
    check(zones["saved_a"]["calls"] > 0, "the summary joins the profile's calls")
    print(f"sampling: {summary['sampling']['samples']} samples as expected")


if __name__ == "__main__":
    main()
