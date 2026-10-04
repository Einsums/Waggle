# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""The Perfetto trace end to end: run the trace test's [program] case with WAGGLE_TRACE set, and
check with Perfetto's own trace_processor that every zone, argument, track and flow arrived.

Usage: perfetto_trace.py <waggle_Trace_test>. Prints "perfetto: N zones as expected" on success, which
ctest requires, so a script that checks nothing fails. Needs the perfetto package, whose
trace_processor is downloaded on first use, or one named by PERFETTO_TRACE_PROCESSOR.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig


def rows(tp: TraceProcessor, sql: str) -> list[dict]:
    return [dict(row.__dict__) for row in tp.query(sql)]


def check(condition: bool, what: str) -> None:
    if not condition:
        print(f"perfetto: FAILED: {what}")
        sys.exit(1)


def main() -> None:
    program = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        trace = Path(tmp) / "program.pftrace"
        env = dict(os.environ, WAGGLE_TRACE=str(trace), WAGGLE_REPORT="false")
        subprocess.run([program, "[.program]"], env=env, check=True)
        binary = os.environ.get("PERFETTO_TRACE_PROCESSOR")
        tp = TraceProcessor(trace=str(trace), config=TraceProcessorConfig(bin_path=binary) if binary else TraceProcessorConfig())

        problems = rows(tp, "select name, value from stats where severity in ('error', 'data_loss') and value > 0")
        check(problems == [], f"trace_processor reported {problems}")

        slices = rows(
            tp,
            "select s.id, s.name, s.depth, s.dur, s.category, p.name as parent, t.name as thread "
            "from slice s left join slice p on s.parent_id = p.id "
            "left join thread_track tt on s.track_id = tt.id left join thread t using (utid) "
            "where s.name like 'trace: %'",
        )
        by_name: dict[str, list[dict]] = {}
        for s in slices:
            by_name.setdefault(s["name"], []).append(s)

        outer = by_name.get("trace: outer", [])
        check(len(outer) == 1, f"one outer zone, not {len(outer)}")
        check(outer[0]["thread"] == "trace main", f"the outer zone on 'trace main', not {outer[0]['thread']}")
        check(outer[0]["depth"] == 0 and outer[0]["dur"] > 0, "the outer zone at the top, with a duration")
        inner = by_name.get("trace: inner", [])
        check(len(inner) == 2 and all(s["parent"] == "trace: outer" and s["depth"] == 1 for s in inner), "two inner zones in the outer one")
        check(len(by_name.get("trace: leaf", [])) == 20000, "every leaf zone")
        work = by_name.get("trace: work", [])
        check(len(work) == 1 and work[0]["thread"] == "trace worker", "the worker's zone on its named thread")

        args = {
            a["key"]: a["display_value"]
            for a in rows(tp, f"select key, display_value from args where arg_set_id = (select arg_set_id from slice where id = {outer[0]['id']})")
        }
        check(args.get("debug.n") == "7" and args.get("debug.label") == "text", f"the outer zone's annotations, not {args}")

        kernel = rows(
            tp,
            "select s.id, s.dur, tr.name as track from slice s join track tr on s.track_id = tr.id where s.name = 'trace: kernel'",
        )
        check(len(kernel) == 1 and kernel[0]["track"] == "trace queue", f"the device span on its queue's track, not {kernel}")
        check(kernel[0]["dur"] == 1_000_000, "the device span's own times")
        flows = rows(
            tp,
            "select a.name as source, p.name as parent from flow f join slice a on f.slice_out = a.id "
            f"left join slice p on a.parent_id = p.id where f.slice_in = {kernel[0]['id']}",
        )
        check(flows == [{"source": "submit", "parent": "trace: launch"}], f"a flow from the submission in 'trace: launch', not {flows}")

        live = [
            int(r["value"])
            for r in rows(tp, "select c.value from counter c join counter_track t on c.track_id = t.id where t.name = 'live bytes' order by c.ts")
        ]
        check(live == [4096, 0], f"the live-bytes curve, not {live}")

        print(f"perfetto: {len(slices)} zones as expected")


if __name__ == "__main__":
    main()
