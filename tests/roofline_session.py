# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""The roofline source end to end: run the roofline test's programs with WAGGLE_SOURCES=roofline and
WAGGLE_SAVE set, and check what ``waggle cpu`` makes of their counts against their work hints: a
program computing each result twice does twice the FLOPs its work needs, a plain matrix multiply
does what it needs, and one reading every eighth double moves far more bytes than it needs.

Usage: roofline_session.py <waggle_Roofline_test>. Prints "roofline: efficiencies as expected" on
success, which ctest requires; exits 4, which ctest counts as skipped, where the source cannot count
(no PMU, a CPU whose FLOP events are not known, not Linux), unless WAGGLE_REQUIRE_PMU is set.
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
        print(f"roofline: FAILED: {what}")
        sys.exit(1)


def zone(program: str, case: str, name: str, tmp: Path, **extra: str) -> dict:
    saved = tmp / f"{name.split()[-1]}.json"
    env = dict(os.environ, WAGGLE_SOURCES="roofline", WAGGLE_SAVE=str(saved), WAGGLE_REPORT="false", **extra)
    subprocess.run([program, case], env=env, check=True, stdout=subprocess.DEVNULL)
    out = subprocess.run([sys.executable, "-m", "waggle", "cpu", str(saved), "--format", "json"], capture_output=True, text=True)
    zones = {z["name"]: z for z in json.loads(out.stdout)["zones"]} if out.returncode == 0 else {}
    if "compute" not in zones.get(name, {}):
        print(f"roofline: {name} was not counted: the source could not count here")
        sys.exit(1 if os.environ.get("WAGGLE_REQUIRE_PMU") else 4)
    return zones[name]


def main() -> None:
    program = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        recompute = zone(program, "[.recompute]", "roofline: recompute", Path(tmp))
        redundant = recompute["compute"]["redundant_flops"]
        check(1.9 < redundant < 2.1, f"recompute does {redundant}x the FLOPs its work needs, not about 2")

        dgemm = zone(program, "[.dgemm]", "roofline: dgemm", Path(tmp))
        redundant = dgemm["compute"]["redundant_flops"]
        check(0.97 < redundant < 1.03, f"the matrix multiply does {redundant}x the FLOPs its work needs, not about 1")

        # The bytes are last-level cache misses, which miss what the prefetcher brought in, so the
        # efficiency is above the 1/8 the program truly has: only checked to be well below 1.
        strided = zone(program, "[.strided]", "roofline: strided", Path(tmp))
        efficiency = strided["memory"]["byte_efficiency"]
        check(0.05 < efficiency < 0.5, f"reading every eighth double has byte efficiency {efficiency}, not well under 1")
        # With a real BLAS, on one thread as the counters are the zone's thread's: a tuned multiply
        # does what its work needs, and, where the machine is calibrated, runs near a core's peak.
        built = subprocess.run([program, "--list-tests", "[.dgemm_large]"], capture_output=True, text=True).stdout
        if "dgemm" in built:
            large = zone(program, "[.dgemm_large]", "roofline: dgemm_large", Path(tmp), OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1")
            redundant = large["compute"]["redundant_flops"]
            check(0.97 < redundant < 1.05, f"the BLAS multiply does {redundant}x the FLOPs its work needs, not about 1")
            if (peak := large["compute"].get("peak_pct")) is not None:
                check(peak > 40.0, f"the BLAS multiply runs at {peak}% of a core's peak, not near it")
                check(large.get("bound") == "compute", f"the BLAS multiply is {large.get('bound')} bound, not compute")
            print(f"roofline: the BLAS multiply at {peak}% of a core's peak")
    print(f"roofline: efficiencies as expected (strided {efficiency})")


if __name__ == "__main__":
    main()
