# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""The machine file: each machine's measured peaks, which the ``roofline`` source's efficiencies
are measured against.

``waggle calibrate`` writes it, and so does Einsums' hardware calibration, so a machine is
calibrated once whichever runs first. It is JSON, ``{"schema": "waggle.machine/1", "machines":
[...]}``, one entry per host and CPU model, each with the ``peak`` FLOP rates and bandwidths
``waggle-calibrate`` prints. ``WAGGLE_MACHINE_FILE`` names it; by default
``~/.cache/waggle/machine.json``.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any

SCHEMA = "waggle.machine/1"


def default_path() -> Path:
    return Path(os.environ.get("WAGGLE_MACHINE_FILE") or Path.home() / ".cache" / "waggle" / "machine.json")


def load(path: str | Path | None = None) -> list[dict[str, Any]]:
    """Every machine the file holds; none if it does not exist or does not parse."""
    try:
        data = json.loads(Path(path or default_path()).read_text())
    except (OSError, ValueError):
        return []
    machines = data.get("machines") if isinstance(data, dict) else None
    return [m for m in machines if isinstance(m, dict)] if isinstance(machines, list) else []


def save(entry: dict[str, Any], path: str | Path | None = None) -> Path:
    """Put @p entry in the file, in place of any for the same host and CPU, and return the file."""
    path = Path(path or default_path())
    machines = [m for m in load(path) if (m.get("host"), m.get("cpu")) != (entry.get("host"), entry.get("cpu"))]
    machines.append(entry)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Written beside and renamed over, so a reader never sees half a file.
    with tempfile.NamedTemporaryFile("w", dir=path.parent, delete=False, suffix=".tmp") as out:
        json.dump({"schema": SCHEMA, "machines": machines}, out, indent=2)
        out.write("\n")
    Path(out.name).replace(path)
    return path


def find(machine: dict[str, Any], path: str | Path | None = None) -> dict[str, Any] | None:
    """The calibration for @p machine (a session's ``host``, ``cpu`` and ``sockets``): its own, or
    else one from another host with the same CPU and sockets, the latest, as a cluster's nodes are
    alike."""
    machines = load(path)
    for entry in machines:
        if entry.get("host") == machine.get("host") and entry.get("cpu") == machine.get("cpu"):
            return entry
    alike = [m for m in machines if m.get("cpu") == machine.get("cpu") and m.get("sockets") == machine.get("sockets")]
    return max(alike, key=lambda m: m.get("calibrated", "")) if alike else None
