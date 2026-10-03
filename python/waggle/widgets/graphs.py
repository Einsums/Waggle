# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Graphical panels: flame graph, thread Gantt chart, allocation track, and the plotext timeline and roofline."""

from __future__ import annotations

import math
import time
from collections import deque
from dataclasses import dataclass
from typing import Any

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal
from textual.message import Message
from textual.widget import Widget
from textual.widgets import Static

from ..analysis import RooflinePoint, roofline_points, sum_exclusive, sum_mem_current
from ..format import flame_color, format_bytes
from ..model import MemoryTrack, ProfileNode, ProfileSnapshot, TimelineEvent

try:
    from textual_plotext import PlotextPlot

    HAVE_PLOTEXT = True
except ImportError:  # pragma: no cover - depends on the environment
    HAVE_PLOTEXT = False


def _join(lines: list[Text]) -> Text:
    return Text("\n").join(lines)


@dataclass
class _Span:
    start: int
    end: int
    node: ProfileNode
    depth: int


class FlameGraph(Widget):
    """Icicle chart of the active thread: roots on top, width proportional to inclusive time.

    Click a bar to zoom into it; Escape or Backspace zooms back out.
    """

    can_focus = True
    MAX_DEPTH = 20

    class SpanClicked(Message):
        def __init__(self, node: ProfileNode) -> None:
            super().__init__()
            self.node = node

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._roots: list[ProfileNode] = []
        self._shown: list[ProfileNode] = []
        self._zoom: list[list[ProfileNode]] = []
        self._spans: list[_Span] = []

    def set_roots(self, roots: list[ProfileNode]) -> None:
        self._roots = roots
        if not self._zoom:
            self._shown = roots
        self._relayout()

    def zoom_in(self, node: ProfileNode) -> None:
        if node.children:
            self._zoom.append(self._shown)
            self._shown = [node]
            self._relayout()

    def zoom_out(self) -> None:
        if self._zoom:
            self._shown = self._zoom.pop()
            self._relayout()

    def zoom_reset(self) -> None:
        self._zoom.clear()
        self._shown = self._roots
        self._relayout()

    def on_resize(self) -> None:
        self._relayout()

    def _relayout(self) -> None:
        self._spans = []
        total = sum(n.inclusive_ms for n in self._shown)
        if total > 0:
            self._layout(self._shown, 0, 0, self.size.width or 120, total)
        self.refresh()

    def _layout(self, nodes: list[ProfileNode], depth: int, x0: int, x1: int, total: float) -> None:
        if depth >= self.MAX_DEPTH or x1 <= x0 or total <= 0:
            return
        x = float(x0)
        for node in nodes:
            width = node.inclusive_ms / total * (x1 - x0)
            start, end = int(x + 0.5), int(x + width + 0.5)
            if end <= start and width > 0.3:
                end = start + 1
            if end > start:
                self._spans.append(_Span(start, end, node, depth))
                self._layout(node.children, depth + 1, start, end, sum(c.inclusive_ms for c in node.children))
            x += width

    def render(self) -> Text:
        if not self._spans:
            return Text("No data yet", style="dim")
        width = self.size.width or 120
        top = 1 if self._zoom else 0
        depth_limit = min(max(s.depth for s in self._spans) + 1, max(1, (self.size.height or 20) - top))
        lines: list[Text] = []
        if self._zoom:
            lines.append(Text(f" Zoomed {len(self._zoom)} level(s) — Esc to zoom out ", style="bold reverse"))
        for depth in range(depth_limit):
            line, pos = Text(), 0
            for span in sorted((s for s in self._spans if s.depth == depth), key=lambda s: s.start):
                line.append(" " * (span.start - pos))
                bar = span.end - span.start
                label, ms = span.node.name, f" {span.node.inclusive_ms:.1f}ms"
                if bar >= len(label) + len(ms) + 2:
                    text = f" {label}{ms} "
                elif bar >= 4:
                    text = f" {label[: bar - 2]} "
                else:
                    text = "█" * bar
                line.append(text.ljust(bar)[:bar], style=f"black on {flame_color(span.node.name)}")
                pos = span.end
            line.append(" " * max(0, width - pos))
            lines.append(line)
        return _join(lines)

    def on_click(self, event: Any) -> None:
        row = event.y - (1 if self._zoom else 0)
        for span in self._spans:
            if span.depth == row and span.start <= event.x < span.end:
                self.post_message(self.SpanClicked(span.node))
                self.zoom_in(span.node)
                return

    def key_escape(self) -> None:
        self.zoom_out()

    def key_backspace(self) -> None:
        self.zoom_out()


#: The narrowest window the timeline zooms to: one microsecond.
MIN_WINDOW_MS = 1e-3


@dataclass
class TimeWindow:
    """The span of time the Gantt chart and the allocation track show, shared so they line up.

    With no width it shows all the data; with no end its right edge stays on the newest data, so a
    zoomed-in view of a live program keeps moving. Panning or zooming away from that edge fixes it.
    """

    width_ms: float | None = None
    end_ms: float | None = None

    @property
    def following(self) -> bool:
        return self.end_ms is None

    def resolve(self, extent: tuple[float, float]) -> tuple[float, float]:
        """The (start, end) shown, given the data's (first, last) times."""
        lo, hi = extent
        width = self.width_ms if self.width_ms is not None else hi - lo
        end = self.end_ms if self.end_ms is not None else hi
        return end - width, end

    def zoom(self, factor: float, extent: tuple[float, float], anchor_ms: float | None = None) -> None:
        """Narrow the window by *factor* (below 1 widens it), keeping *anchor_ms* where it is.

        Without an anchor it zooms about the right edge while following and the middle otherwise.
        """
        lo, hi = extent
        start, end = self.resolve(extent)
        if end <= start or hi <= lo:
            return
        width = max((end - start) / factor, MIN_WINDOW_MS)
        if width >= hi - lo:
            self.reset()
            return
        if anchor_ms is None:
            anchor_ms = end if self.following else (start + end) / 2
        new_start = anchor_ms - (anchor_ms - start) / (end - start) * width
        new_end = min(max(new_start + width, lo + width), hi)
        self.width_ms = width
        self.end_ms = None if new_end >= hi else new_end

    def pan(self, fraction: float, extent: tuple[float, float]) -> None:
        """Move the window by *fraction* of its width, later for a positive one."""
        if self.width_ms is None:
            return
        lo, hi = extent
        start, end = self.resolve(extent)
        new_end = max(end + fraction * (end - start), lo + self.width_ms)
        self.end_ms = None if new_end >= hi else new_end

    def reset(self) -> None:
        self.width_ms = self.end_ms = None


def time_extent(events: list[TimelineEvent], memory: MemoryTrack | None) -> tuple[float, float] | None:
    """The times the timeline panels span: the zone spans' when there are any, as the allocation
    track follows the Gantt chart, else the allocation curve's."""
    if events:
        return min(e.start_ms for e in events), max(e.end_ms for e in events)
    if memory is not None and memory.samples:
        return memory.samples[0][0], memory.samples[-1][0]
    return None


def _format_ms(value: float, width: float) -> str:
    """A time label with as many decimals as a window *width* ms wide needs."""
    if width >= 50:
        return f"{value:.0f}ms"
    if width >= 0.5:
        return f"{value:.2f}ms"
    return f"{value * 1000:.1f}us"


def _format_age(ms: float) -> str:
    if ms < 1.0:
        return f"{ms * 1000:.0f}us"
    if ms < 1000.0:
        return f"{ms:.1f}ms"
    return f"{ms / 1000:.2f}s"


def _time_axis(start: float, end: float, label_width: int, chart: int, label: str = "") -> Text:
    header = Text(f"{label:<{label_width}}", style="bold")
    step = max(1, chart // 5)
    for col in range(0, chart, step):
        tick = _format_ms(start + col / chart * (end - start), end - start)
        if len(tick) >= chart - col:  # a label that would be cut off is left out
            break
        header.append(tick.ljust(min(step, chart - col)))
    return header


class _TimeChart(Widget):
    """A chart with a label column and then time across, on the shared :class:`TimeWindow`.

    The mouse wheel zooms about the time under the pointer.
    """

    LABEL_WIDTH = 14

    class ZoomAt(Message):
        """Zoom by ``factor`` keeping ``anchor_ms`` under the pointer."""

        def __init__(self, factor: float, anchor_ms: float) -> None:
            super().__init__()
            self.factor = factor
            self.anchor_ms = anchor_ms

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._window: tuple[float, float] | None = None

    @property
    def chart_width(self) -> int:
        return max(10, (self.size.width or 120) - self.LABEL_WIDTH - 1)

    def _time_at(self, x: int) -> float | None:
        if self._window is None or x < self.LABEL_WIDTH:
            return None
        start, end = self._window
        return start + (x - self.LABEL_WIDTH) / self.chart_width * (end - start)

    def _wheel(self, event: Any, factor: float) -> None:
        anchor = self._time_at(event.x)
        if anchor is not None:
            event.stop()
            self.post_message(self.ZoomAt(factor, anchor))

    def on_mouse_scroll_up(self, event: Any) -> None:
        self._wheel(event, 2.0)

    def on_mouse_scroll_down(self, event: Any) -> None:
        self._wheel(event, 0.5)


class GanttChart(_TimeChart):
    """Recent zone spans per thread, from the server's ``timeline`` messages."""

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._events: list[TimelineEvent] = []

    def set_events(self, events: list[TimelineEvent], window: tuple[float, float] | None = None) -> None:
        """Show *events* over *window*, (start, end) in ms; all of them without one."""
        self._events = events
        self._window = window if window is not None else time_extent(events, None)
        self.refresh()

    def render(self) -> Text:
        if not self._events or self._window is None:
            return Text("No timeline data (it streams only from a live connection)", style="dim")
        t0, t1 = self._window
        span = t1 - t0
        if span <= 0:
            return Text("No time range", style="dim")
        chart = self.chart_width

        lines = [_time_axis(t0, t1, self.LABEL_WIDTH, chart, "thread")]

        threads: dict[str, list[TimelineEvent]] = {}
        for event in self._events:
            if event.end_ms >= t0 and event.start_ms <= t1:
                threads.setdefault(event.thread_id, []).append(event)
        for tid, events in sorted(threads.items())[: max(1, (self.size.height or 20) - 1)]:
            # The innermost zone wins a cell: later (nested) events overwrite earlier ones.
            owner: list[TimelineEvent | None] = [None] * chart
            for event in sorted(events, key=lambda e: (e.start_ms, -(e.end_ms - e.start_ms))):
                lo = max(0, min(chart - 1, int((event.start_ms - t0) / span * chart)))
                hi = max(lo + 1, min(chart, int((event.end_ms - t0) / span * chart)))
                owner[lo:hi] = [event] * (hi - lo)
            line = Text(f"T{tid}"[: self.LABEL_WIDTH - 1].ljust(self.LABEL_WIDTH), style="bold")
            col = 0
            while col < chart:
                event = owner[col]
                end = col
                while end < chart and owner[end] is event:
                    end += 1
                if event is None:
                    line.append(" " * (end - col))
                else:
                    width = end - col
                    text = f" {event.name} ".ljust(width)[:width] if len(event.name) + 2 <= width else "█" * width
                    style = f"black on {flame_color(event.name)}" if text[0] != "█" else flame_color(event.name)
                    line.append(text, style=style)
                col = end
            lines.append(line)
        return _join(lines)


class AllocationTrack(_TimeChart):
    """Live bytes over the timeline's window, and the allocations still live then, largest first."""

    CURVE_ROWS = 6
    LISTED = 12

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._memory: MemoryTrack | None = None

    def set_track(self, memory: MemoryTrack | None, window: tuple[float, float] | None) -> None:
        self._memory = memory
        self._window = window
        self.refresh()

    def render(self) -> Text:
        memory = self._memory
        if memory is None:
            return Text("No allocation track: the program sends one from a live connection or a saved session", style="dim")
        lines = [self._summary(memory)]
        if self._window is not None and self._window[1] > self._window[0]:
            lines.extend(self._curve(memory, *self._window))
        lines.extend(self._listing(memory))
        return _join(lines)

    def _summary(self, memory: MemoryTrack) -> Text:
        line = Text("Live ", style="bold")
        line.append(format_bytes(memory.live_bytes) or "0B")
        if self._window is not None:
            line.append(f"   peak in view {format_bytes(memory.peak(*self._window)) or '0B'}")
        if memory.untracked:
            line.append(f"   {memory.untracked} allocation(s) on the curve are not listed: no address given, or the list was full", style="dim")
        return line

    def _curve(self, memory: MemoryTrack, start: float, end: float) -> list[Text]:
        chart = self.chart_width
        levels = memory.levels(start, end, chart)
        top = max(levels, default=0)
        rows: list[Text] = []
        for row in range(self.CURVE_ROWS, 0, -1):
            label = format_bytes(top) if row == self.CURVE_ROWS else ("0B" if row == 1 else "")
            line = Text(label.rjust(self.LABEL_WIDTH - 1) + " ", style="dim")
            cells = []
            for level in levels:
                eighths = int(level / top * self.CURVE_ROWS * 8) if top > 0 else 0
                fill = max(0, min(8, eighths - (row - 1) * 8))
                cells.append(_EIGHTHS_UP[fill])
            line.append("".join(cells), style="cyan")
            rows.append(line)
        rows.append(_time_axis(start, end, self.LABEL_WIDTH, chart))
        return rows

    def _listing(self, memory: MemoryTrack) -> list[Text]:
        end = self._window[1] if self._window is not None else None
        # Live at the window's end: allocated by then and not freed since.
        shown = [a for a in memory.live if end is None or a.t_ms <= end]
        if not shown:
            return [Text("No allocation with an address is live", style="dim")]
        now = memory.samples[-1][0] if memory.samples else max(a.t_ms for a in shown)
        out = [Text(f"{'bytes':>10}  {'age':>10}  {'thread':<8}{'address':<20}zone", style="bold")]
        for a in shown[: self.LISTED]:
            age = _format_age(max(0.0, now - a.t_ms))
            out.append(Text(f"{format_bytes(a.bytes):>10}  {age:>10}  {'T' + a.thread_id:<8}{a.address:<20}{a.zone or '(no zone)'}"))
        if len(shown) > self.LISTED:
            out.append(Text(f"... and {len(shown) - self.LISTED} more", style="dim"))
        return out


#: Cells filled from the bottom in eighths, for the allocation curve.
_EIGHTHS_UP = " ▁▂▃▄▅▆▇█"


class _MissingPlotext(Static):
    def __init__(self, what: str, **kwargs: Any) -> None:
        super().__init__(f"The {what} needs textual-plotext (conda install -c conda-forge textual-plotext)", **kwargs)


class TimelinePlot(Widget):
    """Profiled CPU time per wall second, and live tracked memory, over the last two minutes."""

    HISTORY = 240  # samples, one per snapshot shown

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._t: deque[float] = deque(maxlen=self.HISTORY)
        self._cpu: deque[float] = deque(maxlen=self.HISTORY)
        self._mem: deque[float] = deque(maxlen=self.HISTORY)
        self._start = time.monotonic()
        self._last: tuple[float, float] | None = None  # (wall, total exclusive ms)

    def compose(self) -> ComposeResult:
        if not HAVE_PLOTEXT:
            yield _MissingPlotext("timeline")
            return
        with Horizontal():
            yield PlotextPlot(id="cpu-plot")
            yield PlotextPlot(id="mem-plot")

    def record(self, snap: ProfileSnapshot) -> None:
        now = time.monotonic()
        roots = snap.all_roots()
        total = sum_exclusive(roots)
        cpu = 0.0
        if self._last is not None and now > self._last[0]:
            cpu = (total - self._last[1]) / ((now - self._last[0]) * 1000.0) * 100.0
        self._last = (now, total)
        self._t.append(now - self._start)
        self._cpu.append(max(0.0, cpu))
        self._mem.append(sum_mem_current(roots) / (1 << 20))
        if self.display and HAVE_PLOTEXT:
            self._plot()

    def _plot(self) -> None:
        if len(self._t) < 2:
            return
        for plot_id, title, unit, values in (
            ("#cpu-plot", "Profiled CPU (% of one core)", "%", self._cpu),
            ("#mem-plot", "Live tracked memory", "MiB", self._mem),
        ):
            widget = self.query_one(plot_id, PlotextPlot)
            plt = widget.plt
            plt.clear_data()
            plt.clear_figure()
            plt.title(title)
            plt.xlabel("time (s)")
            plt.ylabel(unit)
            plt.ylim(0, max(max(values), 1.0))
            plt.plot(list(self._t), list(values), marker="braille")
            widget.refresh()


class RooflinePlot(Widget):
    """Zones annotated with ``flops`` and ``bytes_read``/``bytes_written`` against a roofline."""

    def __init__(self, peak_gflops: float = 200.0, peak_gb_per_s: float = 50.0, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.peak_gflops = peak_gflops
        self.peak_gb_per_s = peak_gb_per_s
        self.points: list[RooflinePoint] = []

    def compose(self) -> ComposeResult:
        if not HAVE_PLOTEXT:
            yield _MissingPlotext("roofline")
            return
        yield PlotextPlot(id="roofline-plot")

    def set_roots(self, roots: list[ProfileNode]) -> None:
        self.points = roofline_points(roots)
        if HAVE_PLOTEXT and self.points:
            self._plot()

    def _plot(self) -> None:
        # Log-log, but with the logarithms taken here: plotext's own log scale transforms the
        # data in place at every render, so the second redraw takes the log of a log and fails.
        widget = self.query_one("#roofline-plot", PlotextPlot)
        plt = widget.plt
        plt.clear_data()
        plt.clear_figure()
        plt.title("Roofline")
        plt.xlabel("arithmetic intensity (FLOP/byte)")
        plt.ylabel("GFLOP/s")
        ais = [p.intensity for p in self.points]
        gfs = [p.gflops for p in self.points]
        x0, x1 = math.log10(max(min(ais) * 0.5, 1e-3)), math.log10(max(ais) * 2.0)
        y0 = math.log10(max(min(gfs) * 0.5, 1e-3))
        y1 = math.log10(max(max(gfs) * 2.0, self.peak_gflops * 1.2))
        plt.xlim(x0, x1)
        plt.ylim(y0, y1)
        for axis, lo, hi in ((plt.xticks, x0, x1), (plt.yticks, y0, y1)):
            decades = list(range(math.floor(lo), math.ceil(hi) + 1))
            axis(decades, [f"{10.0**d:g}" for d in decades])
        xs = [x0 + (x1 - x0) * i / 100 for i in range(101)]
        roof = [math.log10(min(self.peak_gflops, self.peak_gb_per_s * 10.0**x)) for x in xs]
        plt.plot(xs, roof, marker="braille", label=f"roof ({self.peak_gflops:.0f} GFLOP/s, {self.peak_gb_per_s:.0f} GB/s)")
        plt.scatter([math.log10(v) for v in ais], [math.log10(v) for v in gfs], marker="dot")
        for p in sorted(self.points, key=lambda p: p.gflops, reverse=True)[:5]:
            plt.text(p.name[:20], math.log10(p.intensity), math.log10(p.gflops))
        widget.refresh()
