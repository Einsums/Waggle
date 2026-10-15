# ----------------------------------------------------------------------------------------------
# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.
# ----------------------------------------------------------------------------------------------

"""``waggle cpu`` against a session with the ``cpu`` extension the sampling source saves."""

from __future__ import annotations

import json

import pytest

from waggle import cpu
from waggle.cli import main
from waggle.session import session_from_dict


def _zone(name: str, ms: float, calls: int, children: list | None = None) -> dict:
    return {"name": name, "call_count": calls, "exclusive_ms": ms, "inclusive_ms": ms, "children": children or []}


SESSION = {
    "format": "waggle-session",
    "version": 1,
    "label": "sampled",
    "threads": {
        "1": {"name": "main", "children": [_zone("solve", 300.0, 1, [_zone("contract", 600.0, 40)])]},
        "2": {"name": "thread-2", "children": [_zone("contract", 150.0, 10)]},
    },
    "extensions": {
        "cpu": {
            "schema": "waggle.cpu/1",
            "sampling": {"event": "cycles", "rate_hz": 997, "threads": 3, "samples": 1000, "lost": 2},
            "zones": [
                {"thread": 1, "path": ["solve"], "samples": 250, "addresses": {"0x10": 200, "0x20": 50}},
                {"thread": 1, "path": ["solve", "contract"], "samples": 500, "addresses": {"0x30": 400, "0x40": 100}},
                {"thread": 2, "path": ["contract"], "samples": 150, "addresses": {"0x30": 150}},
            ],
            "no_zone": {"samples": 100, "addresses": {"0x99": 100}},
        }
    },
}


def test_zones_are_ranked_by_their_share_of_the_samples():
    data = cpu.summary(session_from_dict(SESSION, "s0"))
    assert data["schema"] == "waggle.cpu.summary/1"
    assert data["sampling"]["lost"] == 2
    # A zone's samples are summed over its threads and call paths, as the flat report sums time.
    assert [(z["name"], z["samples"], z["samples_pct"]) for z in data["zones"]] == [("contract", 650, 65.0), ("solve", 250, 25.0)]
    contract = data["zones"][0]
    assert contract["calls"] == 50
    assert contract["time_ms"] == {"exclusive": 750.0}
    assert not contract["low_confidence"]
    assert data["no_zone"] == {"samples": 100, "samples_pct": 10.0}


def test_a_zone_under_200_samples_is_low_confidence():
    small = json.loads(json.dumps(SESSION))
    small["extensions"]["cpu"]["zones"][0]["samples"] = 150
    data = cpu.summary(session_from_dict(small, "s0"))
    assert next(z for z in data["zones"] if z["name"] == "solve")["low_confidence"]


def test_one_zone_by_address():
    data = cpu.zone_addresses(session_from_dict(SESSION, "s0"), "contract")
    assert data["samples"] == 650
    assert data["addresses"][0] == {"address": "0x30", "samples": 550, "samples_pct": 84.6}
    assert cpu.zone_addresses(session_from_dict(SESSION, "s0"), "(no zone)")["samples"] == 100


def test_the_command_reads_a_saved_file(tmp_path, capsys):
    path = tmp_path / "run.json"
    path.write_text(json.dumps(SESSION))
    assert main(["cpu", str(path), "--format", "json"]) == 0
    assert json.loads(capsys.readouterr().out)["zones"][0]["name"] == "contract"
    assert main(["cpu", str(path)]) == 0
    text = capsys.readouterr().out
    assert "cycles sampled at 997 Hz on 3 thread(s): 1000 samples, 2 lost" in text
    assert "(no zone)" in text


def test_a_session_without_samples_says_how_to_record_them(tmp_path):
    plain = {k: v for k, v in SESSION.items() if k != "extensions"}
    path = tmp_path / "plain.json"
    path.write_text(json.dumps(plain))
    with pytest.raises(SystemExit, match="WAGGLE_SOURCES=sampling"):
        main(["cpu", str(path)])
