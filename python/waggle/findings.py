# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""What a zone's CPU numbers mean, in sentences an agent can act on.

Each rule reads only a zone's neutral summary fields (``waggle.cpu.summary/1``): shares of
samples, rates against peaks, efficiencies against work hints. None reads a vendor's raw counts,
so the same rules hold whatever measured the numbers. Each finding names the numbers behind it,
so a reader can check it, and says what to look at next.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

#: One line with this share of a zone's samples is the place to look first.
HOT_LINE_PCT = 30.0
#: Time in code without line information, from one calling line, worth naming.
CALLER_LINE_PCT = 30.0
#: Work that is compute bound, run at under this share of a core's peak.
LOW_PEAK_PCT = 25.0
#: Moving this many times the bytes the work needs (byte efficiency at most its inverse).
LOW_BYTE_EFFICIENCY = 0.5
#: Doing this many times the FLOPs the work needs.
REDUNDANT_FLOPS = 1.2
#: This many times the zone's ideal time.
GAP_TO_IDEAL = 4.0


def _where(row: dict[str, Any]) -> str:
    return f"{Path(row['file']).name}:{row['line']}" if row.get("file") else "(no line information)"


def zone_findings(zone: dict[str, Any]) -> list[str]:
    """The findings for one zone of a CPU summary, most telling first."""
    found: list[str] = []
    confident = not zone.get("low_confidence", True)
    if confident and (hot := (zone.get("hot_lines") or [None])[0]) and hot["samples_pct"] >= HOT_LINE_PCT:
        found.append(f"{hot['samples_pct']:.0f}% of samples in {hot['function']} at {_where(hot)}: the line to look at first.")
    if confident and (call := (zone.get("caller_lines") or [None])[0]) and call["samples_pct"] >= CALLER_LINE_PCT:
        found.append(
            f"{call['samples_pct']:.0f}% of samples in {call['callee']}, called from {_where(call)}: the time is the library's, "
            f"so its arguments (sizes, layout, how often it is called) are what to change."
        )
    compute, memory = zone.get("compute") or {}, zone.get("memory") or {}
    gap, bound = zone.get("gap_to_ideal"), zone.get("bound")
    if bound == "compute" and compute.get("peak_pct") is not None and compute["peak_pct"] < LOW_PEAK_PCT:
        found.append(
            f"Compute bound yet at {compute['peak_pct']:.0f}% of a core's peak: check that its loops vectorize to the widest "
            f"width (waggle cpu --asm) and that each core has its own share of the work."
        )
    if memory.get("byte_efficiency") is not None and memory["byte_efficiency"] < LOW_BYTE_EFFICIENCY:
        found.append(
            f"{1 / memory['byte_efficiency']:.1f}x the compulsory DRAM bytes: blocking for cache or fusing it with its "
            f"neighbors would cut the traffic."
        )
    if compute.get("redundant_flops") is not None and compute["redundant_flops"] >= REDUNDANT_FLOPS:
        found.append(
            f"Does {compute['redundant_flops']:.2f}x the FLOPs its work needs: padding, recomputation or masked vector lanes."
        )
    if gap is not None and gap >= GAP_TO_IDEAL:
        found.append(f"Takes {gap:.1f}x its ideal time ({bound} bound): at most that much faster is possible.")
    return found

