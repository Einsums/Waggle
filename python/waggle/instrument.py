# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Profiling Python code: zones, the ``profile`` decorator, and snapshots of what was recorded.

Typical use::

    import waggle

    SOLVE = waggle.Zone("solve")          # registered once

    def solve(n):
        with SOLVE:
            waggle.annotate("n", n)
            ...

    @waggle.profile
    def build_fock(density):
        ...

    with waggle.zone("setup"):            # the same, with the Zone looked up by its name
        ...

    print(waggle.snapshot().find("solve").call_count)

Zones belong to the domain ``"python"`` unless given another, so reports and viewers can tell them
from the zones of the C and C++ libraries in the same process. Everything here goes through
``waggle._core``, the compiled module over Waggle's C interface; without it zones record nothing.
"""

from __future__ import annotations

import functools
import inspect
import json
import threading
from dataclasses import dataclass, field
from typing import Any

_core_module: Any = None


def _core() -> Any:
    """The compiled module, imported on first use; ImportError if it was not built."""
    global _core_module
    if _core_module is None:
        import atexit

        from . import _core as core

        # Write the outputs while the interpreter's threads still run: the collector's own unload
        # comes after them, and on Windows after every other thread has been stopped.
        atexit.register(core.at_exit)
        _core_module = core
    return _core_module


def available() -> bool:
    """Whether ``waggle._core`` is present, so that zones record."""
    try:
        _core()
    except ImportError:
        return False
    return True


# Whether each open zone opened, per thread: a zone closes only if its begin opened one, so
# switching recording while it is open cannot unbalance the zones around it.
_opened = threading.local()


def _stack() -> list[bool]:
    try:
        return _opened.stack
    except AttributeError:
        _opened.stack = []
        return _opened.stack


class Zone:
    """A zone at one call site: ``with zone:`` times the block.

    The site is registered when the zone is made, so entering it costs a call into the collector
    and nothing else. One ``Zone`` may be entered from several threads at once and recursively.
    """

    __slots__ = ("name", "_site", "_domain")

    def __init__(self, name: str, *, file: str = "", line: int = 0, func: str = "", domain: str = "python") -> None:
        self.name = name
        core = _core()
        self._site = core.register_site(name, file, line, func, domain)
        self._domain = core.register_domain(domain)

    def __enter__(self) -> Zone:
        _stack().append(_core_module.zone_begin(self._site, 0, self._domain))
        return self

    def __exit__(self, *exc: object) -> None:
        if _stack().pop():
            _core_module.zone_end()


_zones: dict[tuple[str, str, int, str, str], Zone] = {}
_zones_lock = threading.Lock()


def zone(name: str, *, file: str = "", line: int = 0, func: str = "", domain: str = "python") -> Zone:
    """The :class:`Zone` for this site, made on first use: ``with waggle.zone("setup"):``."""
    key = (name, file, line, func, domain)
    found = _zones.get(key)
    if found is None:
        with _zones_lock:
            found = _zones.get(key)
            if found is None:
                found = _zones[key] = Zone(name, file=file, line=line, func=func, domain=domain)
    return found


def profile(func: Any = None, /, *, name: str | None = None, domain: str = "python") -> Any:
    """Record every call of a function as a zone.

    Usable bare, or with a zone name of your own::

        @waggle.profile
        def build_fock(density):
            ...

        @waggle.profile(name="SCF iteration")
        def iterate(state):
            ...

    The zone is named after the function's qualified name unless ``name`` is given, and carries
    its source file, first line and name. Stack it beneath ``@staticmethod`` or ``@classmethod``.

    Coroutine and generator functions are refused: a zone must close before any zone opened after
    it on the same thread, and one held open across an ``await`` or a ``yield`` would not. Time the
    synchronous work inside them with :func:`zone` instead.

    The site is registered on the first call, so decorating needs no compiled module; without one
    a call costs one check.
    """
    if func is None:
        return lambda f: profile(f, name=name, domain=domain)

    if inspect.iscoroutinefunction(func) or inspect.isasyncgenfunction(func) or inspect.isgeneratorfunction(func):
        raise TypeError(
            f"profile cannot time {getattr(func, '__qualname__', func)!r}: it is a coroutine or generator "
            "function, and a zone held open across an await or a yield would not close before zones "
            "opened after it on the same thread. Use waggle.zone() around its synchronous parts."
        )
    code = getattr(func, "__code__", None)
    qualname = getattr(func, "__qualname__", getattr(func, "__name__", repr(func)))
    file = code.co_filename if code is not None else ""
    line = code.co_firstlineno if code is not None else 0
    label = name if name is not None else qualname
    site: list[Zone | None] = []  # filled on the first call; None where nothing records

    @functools.wraps(func)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        if not site:
            site.append(Zone(label, file=file, line=line, func=qualname, domain=domain) if available() else None)
        z = site[0]
        if z is None:
            return func(*args, **kwargs)
        with z:
            return func(*args, **kwargs)

    return wrapper


def annotate_dims(key: str, dims: Any) -> None:
    """Attach a sequence of dimension sizes as ``<key>.0``, ``<key>.1``, ... annotations."""
    annotate = _core().annotate
    for i, d in enumerate(dims):
        annotate(f"{key}.{i}", int(d))


@dataclass
class Node:
    """One zone in a :class:`Snapshot`: its statistics and the zones below it."""

    name: str
    file: str
    line: int
    function: str
    domain: str
    call_count: int
    exclusive_ns: int
    inclusive_ns: int
    exclusive_min_ns: int
    exclusive_max_ns: int
    mem_alloc_bytes: int
    mem_free_bytes: int
    annotations: dict[str, str] = field(default_factory=dict)
    children: list[Node] = field(default_factory=list)

    def find(self, path: str) -> Node | None:
        """The zone at ``path`` below this one, names joined by ``/``, or None."""
        node: Node | None = self
        for name in path.split("/") if path else []:
            node = next((c for c in node.children if c.name == name), None) if node is not None else None
        return node

    def walk(self) -> Any:
        """This zone and every zone below it, depth first."""
        yield self
        for child in self.children:
            yield from child.walk()


@dataclass
class Thread:
    id: int
    name: str
    root: Node


@dataclass
class Snapshot:
    """What was recorded when it was taken, one tree per thread (or one, merged)."""

    threads: list[Thread]

    def find(self, path: str) -> Node | None:
        """The zone at ``path`` below a thread's root, on the first thread that has one."""
        for thread in self.threads:
            found = thread.root.find(path)
            if found is not None:
                return found
        return None


def _node(data: dict[str, Any]) -> Node:
    children = [_node(c) for c in data.pop("children")]
    return Node(**data, children=children)


def snapshot(merge_threads: bool = False) -> Snapshot:
    """Copy what has been recorded, while recording continues."""
    data = json.loads(_core().snapshot_json(merge_threads))
    return Snapshot([Thread(t["id"], t["name"], _node(t["root"])) for t in data["threads"]])
