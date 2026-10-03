# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""The profiler's data, as the server streams it and as session files store it.

The server (``src/Server.cpp``) sends JSON Lines over TCP. Each
line is one message with a ``type``: ``meta`` once per connection, then ``snapshot``
(the aggregated call tree of every thread), ``timeline`` (recent zone spans),
``memory`` (the allocation track), ``devices`` (device work by name), ``log``, ``output``, ``benchmark_result``, and ``response`` to a ``request``.

Nothing in this module imports Textual, so the bench commands and the tests use it
without the UI dependency.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass, field
from typing import Any


@dataclass
class ProfileNode:
    """One zone in the call tree, aggregated over every entry at this call path."""

    name: str = ""
    call_count: int = 0
    exclusive_ms: float = 0.0
    inclusive_ms: float = 0.0
    exclusive_min_ms: float = 0.0
    exclusive_max_ms: float = 0.0
    stddev_ms: float = 0.0
    file: str = ""
    line: int = 0
    function: str = ""
    #: A plain value, or ``{"avg", "min", "max", "last"}`` for a numeric annotation.
    annotations: dict[str, Any] = field(default_factory=dict)
    #: Hardware counter name -> ``{"total", ...}``.
    counters: dict[str, Any] = field(default_factory=dict)
    mem_alloc_count: int = 0
    mem_free_count: int = 0
    mem_alloc_bytes: int = 0
    mem_free_bytes: int = 0
    mem_current_bytes: int = 0
    mem_peak_bytes: int = 0
    #: Per-call duration histogram, bucket label -> count.
    histogram: dict[str, int] = field(default_factory=dict)
    children: list[ProfileNode] = field(default_factory=list)

    @property
    def mean_ms(self) -> float:
        return self.exclusive_ms / self.call_count if self.call_count > 0 else 0.0

    @property
    def location(self) -> str:
        if not self.file:
            return ""
        return f"{self.file}:{self.line}" if self.line > 0 else self.file


@dataclass
class ThreadData:
    thread_id: str = ""
    thread_name: str = ""
    children: list[ProfileNode] = field(default_factory=list)

    @property
    def label(self) -> str:
        return self.thread_name or f"Thread {self.thread_id}"


@dataclass
class ProfileSnapshot:
    seq: int = 0
    dropped: int = 0
    threads: dict[str, ThreadData] = field(default_factory=dict)

    def all_roots(self) -> list[ProfileNode]:
        """Every thread's top-level zones, in one list."""
        return [node for thread in self.threads.values() for node in thread.children]


@dataclass
class ProfileMeta:
    pid: int = 0
    hostname: str = ""
    executable: str = ""
    executable_path: str = ""
    start_time: str = ""
    git_commit: str = ""
    git_branch: str = ""
    build_type: str = ""
    counters: list[str] = field(default_factory=list)
    #: Request handlers the program registered: what a viewer can ask it. None when the server
    #: predates advertising them, which says nothing about what it answers.
    handlers: list[str] | None = None
    #: The libraries using the profiler, each a dict of name, version and build.
    clients: list[dict[str, Any]] = field(default_factory=list)
    #: The collector's optional instruments, each a dict of name, state and detail: "off", "active",
    #: or why one that was asked for records nothing ("waiting", "missed", "unavailable").
    sources: list[dict[str, str]] = field(default_factory=list)
    #: Copies of the collector that switched themselves off, each a dict of path and abi: the
    #: zones of the libraries that loaded them are missing.
    duplicates: list[dict[str, str]] = field(default_factory=list)

    def source_problems(self) -> list[str]:
        """What a person should know about sources asked for that record nothing, and about
        collectors switched off."""
        out = [
            f"Source {s.get('name', '?')} is {s.get('state', '?')}: {s.get('detail', '')}"
            for s in self.sources
            if s.get("state") not in ("off", "active")
        ]
        out += [f"A second copy of the collector at {d.get('path', '?')} switched itself off; its libraries' zones are missing" for d in self.duplicates]
        return out

    @property
    def label(self) -> str:
        return f"{self.executable or 'unknown'} ({self.start_time or '?'})"


@dataclass
class TimelineEvent:
    """One zone span, or one piece of device work, for the Gantt chart."""

    thread_id: str = ""
    name: str = ""
    start_ms: float = 0.0
    end_ms: float = 0.0
    #: The device queue the work ran on; empty for a host thread's zone.
    track: str = ""

    @property
    def row(self) -> str:
        """The Gantt row it belongs on: its thread's, or its device queue's."""
        return f"device:{self.track}" if self.track else self.thread_id


@dataclass
class DeviceWork:
    """Device work of one name on one device queue, summed."""

    track: str = ""
    name: str = ""
    count: int = 0
    total_ms: float = 0.0
    min_ms: float = 0.0
    max_ms: float = 0.0
    #: The host zone that submitted most of it; empty when none.
    submitter: str = ""

    @property
    def mean_ms(self) -> float:
        return self.total_ms / self.count if self.count else 0.0


def parse_devices(data: dict[str, Any]) -> list[DeviceWork]:
    return [
        DeviceWork(
            track=str(w.get("track", "")),
            name=str(w.get("name", "")),
            count=int(w.get("count", 0)),
            total_ms=float(w.get("total_ms", 0.0)),
            min_ms=float(w.get("min_ms", 0.0)),
            max_ms=float(w.get("max_ms", 0.0)),
            submitter=str(w.get("submitter", "")),
        )
        for w in data.get("work") or []
        if isinstance(w, dict)
    ]


def devices_to_dict(work: list[DeviceWork]) -> dict[str, Any]:
    """The shape of a ``devices`` message."""
    return {
        "work": [
            {"track": w.track, "name": w.name, "count": w.count, "total_ms": w.total_ms, "min_ms": w.min_ms,
             "max_ms": w.max_ms, "submitter": w.submitter}
            for w in work
        ]
    }  # fmt: skip


@dataclass
class LiveAllocation:
    """A block allocated with its address and not freed yet."""

    address: str = ""
    bytes: int = 0
    #: When it was allocated, on the timeline's clock.
    t_ms: float = 0.0
    thread_id: str = ""
    #: The zone open when it was allocated; empty outside every zone.
    zone: str = ""


@dataclass
class MemoryTrack:
    """Live bytes over time, and the largest allocations not yet freed, from ``memory`` messages.

    The server sends the whole curve to a viewer that connects and then only what is new, so
    :meth:`apply` appends; the list of live allocations is the server's current one each time.
    """

    #: (time in ms on the timeline's clock, live bytes), in time order.
    samples: list[tuple[float, int]] = field(default_factory=list)
    live: list[LiveAllocation] = field(default_factory=list)
    live_bytes: int = 0
    #: Allocations that move the curve but are never listed: recorded without an address, or
    #: made while the server's table of live allocations was full.
    untracked: int = 0
    #: The sequence number of the newest sample held.
    seq: int = 0

    MAX_SAMPLES = 65536

    def apply(self, data: dict[str, Any]) -> None:
        raw = [s for s in data.get("samples") or [] if isinstance(s, (list, tuple)) and len(s) == 2]
        if "seq" in data:
            # The samples are consecutive and end at seq; drop the ones already held.
            last = int(data["seq"])
            first = last - len(raw) + 1
            raw = raw[max(0, self.seq + 1 - first) :]
            self.seq = max(self.seq, last)
        new = [(float(t), int(b)) for t, b in raw]
        if new:
            ordered = not self.samples or new[0][0] >= self.samples[-1][0]
            self.samples.extend(new)
            # Threads are drained in turn, so samples from two of them can arrive out of order.
            if not ordered or any(a[0] > b[0] for a, b in zip(new, new[1:])):
                self.samples.sort(key=lambda s: s[0])
            del self.samples[: -self.MAX_SAMPLES]
        self.live = [
            LiveAllocation(
                address=str(a.get("address", "")),
                bytes=int(a.get("bytes", 0)),
                t_ms=float(a.get("t_ms", 0.0)),
                thread_id=str(a.get("tid", "")),
                zone=str(a.get("zone", "")),
            )
            for a in data.get("live") or []
            if isinstance(a, dict)
        ]
        self.live_bytes = int(data.get("live_bytes", self.samples[-1][1] if self.samples else 0))
        self.untracked = int(data.get("untracked", 0))

    def peak(self, start_ms: float, end_ms: float) -> int:
        """The most bytes live at any time in [start_ms, end_ms]."""
        return max(self.levels(start_ms, end_ms, 1), default=0)

    def levels(self, start_ms: float, end_ms: float, count: int) -> list[int]:
        """The most bytes live in each of *count* equal slices of [start_ms, end_ms].

        A slice with no sample of its own holds the level the last sample before it left.
        """
        if count <= 0 or end_ms <= start_ms:
            return []
        out = [0] * count
        times = [t for t, _ in self.samples]
        i = bisect.bisect_right(times, start_ms)
        level = self.samples[i - 1][1] if i > 0 else 0
        width = (end_ms - start_ms) / count
        for col in range(count):
            hi = start_ms + (col + 1) * width
            peak = level
            while i < len(self.samples) and self.samples[i][0] <= hi:
                level = self.samples[i][1]
                peak = max(peak, level)
                i += 1
            out[col] = peak
        return out


def parse_memory(data: dict[str, Any]) -> MemoryTrack:
    track = MemoryTrack()
    track.apply(data)
    return track


def memory_to_dict(track: MemoryTrack) -> dict[str, Any]:
    """The shape of a ``memory`` message holding the whole track."""
    return {
        "live_bytes": track.live_bytes,
        "untracked": track.untracked,
        "seq": track.seq,
        "samples": [[t, b] for t, b in track.samples],
        "live": [
            {"address": a.address, "bytes": a.bytes, "t_ms": a.t_ms, "tid": a.thread_id, "zone": a.zone} for a in track.live
        ],
    }


@dataclass
class LogEntry:
    level: int = 2  # spdlog's: 0 trace .. 5 critical; OUTPUT_LEVEL for println output
    timestamp: str = ""
    file: str = ""
    line: int = 0
    function: str = ""
    message: str = ""


#: The level given to ``output`` messages (a program's printed lines), above every log level.
OUTPUT_LEVEL = 7
LOG_LEVEL_NAMES = {0: "TRACE", 1: "DEBUG", 2: "INFO", 3: "WARN", 4: "ERROR", 5: "CRITICAL", OUTPUT_LEVEL: "OUTPUT"}


# ---------------------------------------------------------------------------
# JSON -> model
# ---------------------------------------------------------------------------


def parse_node(data: dict[str, Any]) -> ProfileNode:
    mem = data.get("memory") or {}
    histogram: dict[str, int] = {}
    raw_hist = data.get("histogram")
    if isinstance(raw_hist, dict):
        for bucket, count in raw_hist.items():
            try:
                histogram[bucket] = int(count)
            except (TypeError, ValueError):
                pass
    return ProfileNode(
        name=data.get("name", ""),
        call_count=data.get("call_count", 0),
        exclusive_ms=data.get("exclusive_ms", 0.0),
        inclusive_ms=data.get("inclusive_ms", 0.0),
        exclusive_min_ms=data.get("exclusive_min_ms", 0.0),
        exclusive_max_ms=data.get("exclusive_max_ms", 0.0),
        stddev_ms=data.get("stddev_ms", 0.0),
        file=data.get("file", ""),
        line=data.get("line", 0),
        function=data.get("function", ""),
        annotations=dict(data.get("annotations") or {}),
        counters=dict(data.get("counters") or {}),
        mem_alloc_count=mem.get("alloc_count", 0),
        mem_free_count=mem.get("free_count", 0),
        mem_alloc_bytes=mem.get("alloc_bytes", 0),
        mem_free_bytes=mem.get("free_bytes", 0),
        mem_current_bytes=mem.get("current_bytes", 0),
        mem_peak_bytes=mem.get("peak_bytes", 0),
        histogram=histogram,
        children=[parse_node(child) for child in data.get("children", [])],
    )


def parse_snapshot(data: dict[str, Any]) -> ProfileSnapshot:
    snap = ProfileSnapshot(seq=data.get("seq", 0), dropped=data.get("dropped", 0))
    for tid, tdata in (data.get("threads") or {}).items():
        snap.threads[str(tid)] = ThreadData(
            thread_id=str(tid),
            thread_name=tdata.get("name", ""),
            children=[parse_node(child) for child in tdata.get("children", [])],
        )
    return snap


def parse_meta(data: dict[str, Any]) -> ProfileMeta:
    return ProfileMeta(
        pid=data.get("pid", 0),
        hostname=data.get("hostname", ""),
        executable=data.get("executable", ""),
        executable_path=data.get("executable_path", ""),
        start_time=data.get("start_time", ""),
        git_commit=data.get("git_commit", ""),
        git_branch=data.get("git_branch", ""),
        build_type=data.get("build_type", ""),
        counters=list(data.get("counters") or []),
        handlers=list(data["handlers"]) if isinstance(data.get("handlers"), list) else None,
        clients=[c for c in data.get("clients") or [] if isinstance(c, dict)],
        sources=[s for s in data.get("sources") or [] if isinstance(s, dict)],
        duplicates=[d for d in data.get("duplicates") or [] if isinstance(d, dict)],
    )


def parse_timeline(data: dict[str, Any]) -> list[TimelineEvent]:
    return [
        TimelineEvent(
            thread_id=str(event.get("tid", "")),
            name=event.get("name", ""),
            start_ms=event.get("start_ms", 0.0),
            end_ms=event.get("end_ms", 0.0),
            track=str(event.get("track", "")),
        )
        for event in data.get("events", [])
    ]


def parse_log(data: dict[str, Any]) -> LogEntry:
    if data.get("type") == "output":
        return LogEntry(level=OUTPUT_LEVEL, timestamp=data.get("timestamp", ""), message=data.get("message", ""))
    return LogEntry(
        level=data.get("level", 2),
        timestamp=data.get("timestamp", ""),
        file=data.get("file", ""),
        line=data.get("line", 0),
        function=data.get("function", ""),
        message=data.get("message", ""),
    )


# ---------------------------------------------------------------------------
# model -> JSON (the same shapes, so a saved file loads back)
# ---------------------------------------------------------------------------


def node_to_dict(node: ProfileNode) -> dict[str, Any]:
    data: dict[str, Any] = {
        "name": node.name,
        "call_count": node.call_count,
        "exclusive_ms": node.exclusive_ms,
        "inclusive_ms": node.inclusive_ms,
        "exclusive_min_ms": node.exclusive_min_ms,
        "exclusive_max_ms": node.exclusive_max_ms,
        "stddev_ms": node.stddev_ms,
        "file": node.file,
        "line": node.line,
        "function": node.function,
        "annotations": node.annotations,
        "counters": node.counters,
        "children": [node_to_dict(child) for child in node.children],
    }
    if node.mem_alloc_bytes or node.mem_free_bytes:
        data["memory"] = {
            "alloc_count": node.mem_alloc_count,
            "free_count": node.mem_free_count,
            "alloc_bytes": node.mem_alloc_bytes,
            "free_bytes": node.mem_free_bytes,
            "current_bytes": node.mem_current_bytes,
            "peak_bytes": node.mem_peak_bytes,
        }
    if node.histogram:
        data["histogram"] = node.histogram
    return data


def snapshot_to_dict(snap: ProfileSnapshot) -> dict[str, Any]:
    return {
        "seq": snap.seq,
        "dropped": snap.dropped,
        "threads": {
            tid: {"name": thread.thread_name, "children": [node_to_dict(n) for n in thread.children]}
            for tid, thread in snap.threads.items()
        },
    }


def meta_to_dict(meta: ProfileMeta) -> dict[str, Any]:
    return {
        "pid": meta.pid,
        "hostname": meta.hostname,
        "executable": meta.executable,
        "executable_path": meta.executable_path,
        "start_time": meta.start_time,
        "git_commit": meta.git_commit,
        "git_branch": meta.git_branch,
        "build_type": meta.build_type,
        "counters": meta.counters,
        "clients": meta.clients,
        "sources": meta.sources,
        "duplicates": meta.duplicates,
    } | ({"handlers": meta.handlers} if meta.handlers is not None else {})
