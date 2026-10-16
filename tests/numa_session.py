# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The numa source end to end: run the NUMA test's [.first_touch] and [.migrate] cases with
WAGGLE_SOURCES=numa and WAGGLE_SAVE set, and check that ``waggle cpu --numa`` finds the first-touch
mistake in the block one thread initialized and not in the one two did, and the thread moved
across nodes with its zone open.

Usage: numa_session.py <waggle_Numa_test>. Prints "numa: findings as expected" on success, which
ctest requires; exits 4, which ctest counts as skipped, on a machine of one NUMA node.
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
        print(f"numa: FAILED: {what}")
        sys.exit(1)


def record(program: str, case: str, saved: Path) -> dict:
    env = dict(os.environ, WAGGLE_SOURCES="numa", WAGGLE_SAVE=str(saved), WAGGLE_REPORT="false")
    run = subprocess.run([program, case], env=env, check=False, capture_output=True, text=True)
    if run.returncode == 4 or "one NUMA node" in run.stdout:
        print("numa: this machine has one NUMA node")
        sys.exit(4)
    check(run.returncode == 0, f"{case} failed:\n{run.stdout}{run.stderr}")
    out = subprocess.run(
        [sys.executable, "-m", "waggle", "cpu", str(saved), "--numa", "--format", "json"], check=True, capture_output=True, text=True
    ).stdout
    return json.loads(out)


def main() -> None:
    program = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        touched = record(program, "[.first_touch]", Path(tmp) / "first_touch.json")
        blocks = {b["zone"]: b for b in touched["blocks"]}
        check({"numa: serial init", "numa: parallel init"} <= blocks.keys(), f"blocks {sorted(blocks)}")
        serial, parallel = blocks["numa: serial init"], blocks["numa: parallel init"]
        check(max(serial["nodes_pct"].values()) >= 95, f"the serial block is {serial['nodes_pct']}, not on one node")
        check(len(parallel["nodes_pct"]) >= 2, f"the parallel block is {parallel['nodes_pct']}, not on two nodes")
        first = [f for f in touched["findings"] if f["kind"] == "first_touch"]
        check([f["zone"] for f in first] == ["numa: serial init"], f"first-touch findings for {[f['zone'] for f in first]}")

        moved = record(program, "[.migrate]", Path(tmp) / "migrate.json")
        migrations = [f for f in moved["findings"] if f["kind"] == "thread_migration"]
        check([f["zone"] for f in migrations] == ["numa: migrating"], f"migration findings for {[f['zone'] for f in migrations]}")
    print("numa: findings as expected")


if __name__ == "__main__":
    main()
