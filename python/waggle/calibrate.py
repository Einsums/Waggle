# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""``waggle calibrate``: measure this machine's peaks and keep them in the machine file.

The measuring is ``waggle-calibrate``'s, a small program built with Waggle, as it must run
vector instructions on every core at once. It wants the machine to itself: run it in an exclusive
job, not on a shared login node, where ``--threads`` keeps a quick check to a few cores.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import shutil
import subprocess
from pathlib import Path

from . import machine


def add_parser(sub) -> None:
    cal = sub.add_parser(
        "calibrate",
        help="Measure this machine's FLOP and DRAM bandwidth peaks for the roofline source, and keep them",
        description="Runs waggle-calibrate, which needs the machine to itself: an FMA loop on every core, then a "
        "STREAM triad on each socket. The result goes in the machine file (WAGGLE_MACHINE_FILE, else "
        "~/.cache/waggle/machine.json), where waggle cpu finds it.",
    )
    cal.add_argument("--threads", type=int, help="use at most N cores, for a quick check; the result says it is partial")
    cal.add_argument("--seconds", type=float, help="seconds per FLOP measurement (default: 1)")
    cal.add_argument(
        "--stream-mb", type=int, help="megabytes per triad array (default: four times the last-level cache; 256 with --threads)"
    )
    cal.add_argument("--file", help="the machine file to write (default: WAGGLE_MACHINE_FILE, else ~/.cache/waggle/machine.json)")
    cal.add_argument("--print", action="store_true", help="print the measurement as JSON and keep nothing")
    cal.set_defaults(action=run)


def find_program() -> str | None:
    """waggle-calibrate: beside this package, as the build and an install put it; WAGGLE_CALIBRATE;
    else on the path."""
    for candidate in (Path(__file__).with_name("waggle-calibrate"), os.environ.get("WAGGLE_CALIBRATE")):
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which("waggle-calibrate")


def measure(threads: int | None = None, seconds: float | None = None, stream_mb: int | None = None) -> dict:
    program = find_program()
    if program is None:
        raise SystemExit("waggle-calibrate was not found: it is built with Waggle (not with MSVC); or set WAGGLE_CALIBRATE")
    args = [program]
    for flag, value in (("--threads", threads), ("--seconds", seconds), ("--stream-mb", stream_mb)):
        if value is not None:
            args += [flag, str(value)]
    proc = subprocess.run(args, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise SystemExit(f"waggle-calibrate failed: {proc.stderr.strip()}")
    result = json.loads(proc.stdout)
    result["calibrated"] = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    result["by"] = "waggle calibrate"
    return result


def run(args: argparse.Namespace) -> int:
    result = measure(args.threads, args.seconds, args.stream_mb)
    if args.print:
        print(json.dumps(result, indent=2))
        return 0
    path = machine.save(result, args.file)
    peak = result["peak"]
    print(f"{result['cpu']} on {result['host']}: {result['sockets']} socket(s), {result['threads']} of {result['cpus']} CPUs used")
    print(f"  FP64 {peak['fp64_gflops']:.0f} GFLOP/s ({peak['fp64_gflops_per_core']:.1f} per core), FP32 {peak['fp32_gflops']:.0f} GFLOP/s")
    sockets = ", ".join(f"{bw:.0f}" for bw in peak["dram_gbs_per_socket"])
    print(f"  DRAM {peak['dram_gbs']:.0f} GB/s ({sockets} per socket)")
    if not result["complete"]:
        print("  Partial: not every CPU was used, so the totals are low; calibrate with the machine to itself and no --threads.")
    print(f"Kept in {path}")
    return 0
