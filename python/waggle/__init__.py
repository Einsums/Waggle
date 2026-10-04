# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Waggle's Python side: profiling Python code, the terminal viewer, and ``report`` and ``diff``.

``waggle.zone``, ``waggle.Zone``, ``waggle.profile``, ``waggle.annotate``, ``waggle.snapshot`` and
the rest record into the process's one collector through ``waggle._core`` (see
:mod:`waggle.instrument`); they are imported on first use, so ``import waggle`` stays cheap and
works where the compiled module is not built.

The viewer connects to the server a program's profiler starts (``WAGGLE_SERVER=1``, or a library's
own option), opens saved session files, and replays recorded streams. Only the viewer needs
Textual, imported on use. Libraries add panels and actions through :mod:`waggle.plugin`.
"""

__version__ = "0.1.0"

# Names waggle.instrument defines; every other public name comes from the compiled module.
_INSTRUMENT = frozenset(
    {"Zone", "zone", "profile", "annotate_dims", "snapshot", "Snapshot", "Node", "Thread", "available", "source_status"}
)


def __getattr__(name: str):  # PEP 562: resolve the profiling API on first use
    if name in _INSTRUMENT:
        from . import instrument

        value = getattr(instrument, name)
    elif name.startswith("_"):
        raise AttributeError(name)
    else:
        from .instrument import _core

        try:
            value = getattr(_core(), name)
        except ImportError as exc:
            raise AttributeError(f"waggle.{name} needs waggle._core, which this installation does not have ({exc})") from exc
    globals()[name] = value
    return value
