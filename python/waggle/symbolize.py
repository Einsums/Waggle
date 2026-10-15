# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Instruction addresses from the ``sampling`` source, named by function, file and line.

The collector saves raw addresses and a module map: each loaded file's path, load base, executable
ranges, and its GNU build id, or where it has none (conda-forge links none) its size and time.
Here an address finds its module, the module's file is checked to be the one that ran, and its
addresses go to ``llvm-symbolizer`` when it is on the path, else ``addr2line``, in one batch per
module. A rebuilt or missing file is reported, not read: its lines would be wrong.

An address inside inlined code has a chain of frames, innermost first: the innermost is where the
time went (``new_allocator.h:151``), the outermost the function the code was compiled into and the
line in it (``Contract.cpp:212`` in ``pack_b``). Both are kept.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import elf


def short_name(name: str) -> str:
    """A demangled function name without its return type, argument list or the compiler's clone
    suffixes, as Waggle names functions elsewhere: ``void ns::f<int>(int) const [clone .cold]``
    is ``ns::f<int>``."""
    name = name.strip()
    while name.endswith("]") and " [clone " in name:
        name = name[: name.rindex(" [clone ")].rstrip()
    for qualifier in (" const", " volatile", " &&", " &"):
        if name.endswith(qualifier):
            name = name[: -len(qualifier)].rstrip()
    if name.endswith(")"):
        depth = 0
        for i in range(len(name) - 1, -1, -1):
            depth += {")": 1, "(": -1}.get(name[i], 0)
            if depth == 0:
                name = name[:i]
                break
    # The return type is whatever comes before the last space outside brackets.
    depth = 0
    for i in range(len(name) - 1, -1, -1):
        c = name[i]
        depth += {">": 1, ")": 1, "<": -1, "(": -1}.get(c, 0)
        if c == " " and depth == 0:
            return name[i + 1 :]
    return name


#: The lowest address the kernel uses on x86-64 and aarch64: a sample there skidded past the edge
#: of user space as the thread entered the kernel.
KERNEL_START = 0xFFFF_8000_0000_0000


@dataclass(frozen=True)
class Frame:
    function: str
    file: str  # empty when the file has no line information for it
    line: int  # 0 likewise


@dataclass(frozen=True)
class Location:
    """What one address is: its module, its address in the module's file, and its frames."""

    address: int
    module: str = ""  # the module's path; empty when no module covers the address
    offset: int = 0  # the address in the module's file, as addr2line and objdump take it
    frames: tuple[Frame, ...] = ()  # innermost first; empty when it could not be resolved

    @property
    def inner(self) -> Frame | None:
        return self.frames[0] if self.frames else None

    @property
    def outer(self) -> Frame | None:
        return self.frames[-1] if self.frames else None

    @property
    def function(self) -> str:
        """The innermost function, or the module and offset when none is known."""
        if self.inner is not None and self.inner.function:
            return self.inner.function
        if self.module:
            # Code with no file (the vDSO) cannot be named closer than its module, so its samples
            # are counted together; a file's unnamed code keeps its offset, to look up by hand.
            if not Path(self.module).is_absolute():
                return f"[{self.module}]"
            return f"{Path(self.module).name}+{self.offset:#x}"
        if self.address >= KERNEL_START:
            return "[kernel]"
        return f"{self.address:#x}"


@dataclass
class Result:
    locations: dict[int, Location] = field(default_factory=dict)
    tool: str = ""
    #: Modules whose addresses were left unnamed, and why: missing, rebuilt, or no tool.
    problems: dict[str, str] = field(default_factory=dict)


def _tool() -> tuple[str, list[str]] | None:
    """The symbolizer and its arguments before the file: llvm-symbolizer, else addr2line."""
    if path := shutil.which("llvm-symbolizer"):
        return path, ["--output-style=GNU", "--addresses", "--functions=linkage", "--demangle", "--inlines", "--obj"]
    for name in (os.environ.get("ADDR2LINE", ""), "addr2line"):
        if name and (path := shutil.which(name)):
            return path, ["-a", "-f", "-C", "-i", "-e"]
    return None


def module_problem(module: dict[str, Any]) -> str:
    """Why @p module's file cannot be read for the run that recorded it; empty if it can."""
    path = Path(module.get("path", ""))
    if not path.is_absolute():
        return "no file (code the kernel or a loader made)"
    if not path.is_file():
        return "the file is gone"
    recorded = module.get("build_id", "")
    if recorded:
        found = elf.build_id(path)
        return "" if found == recorded else "rebuilt since the run (its build id differs)"
    size, mtime = module.get("size", 0), module.get("mtime_ns", 0)
    if size or mtime:
        st = path.stat()
        if st.st_size != size or st.st_mtime_ns != mtime:
            return "changed since the run (its size or time differs)"
    return ""


def _module_of(modules: list[dict[str, Any]], address: int) -> dict[str, Any] | None:
    for module in modules:
        for start, end in module.get("ranges", []):
            if int(start, 16) <= address < int(end, 16):
                return module
    return None


def parse_gnu_output(text: str) -> dict[int, tuple[Frame, ...]]:
    """addr2line ``-a -f -i`` output, or llvm-symbolizer's in GNU style: each address on a line of
    its own, then a function line and a ``file:line`` line per frame, innermost first."""
    out: dict[int, tuple[Frame, ...]] = {}
    current: int | None = None
    frames: list[Frame] = []
    lines = [line for line in text.splitlines() if line.strip()]
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("0x") and all(c in "0123456789abcdefABCDEF" for c in line[2:]):
            if current is not None:
                out[current] = tuple(frames)
            current, frames = int(line, 16), []
            i += 1
            continue
        if i + 1 >= len(lines):
            break
        function, where = line, lines[i + 1].strip()
        where = where.split(" (discriminator", 1)[0]
        file, _, number = where.rpartition(":")
        name = "" if function == "??" else short_name(function)
        # A file with no line ("lib.cpp:?", from a library built without line information) is no
        # line information either.
        line_number = int(number) if number.isdigit() else 0
        frames.append(Frame(name, file if line_number and file not in ("", "??") else "", line_number))
        i += 2
    if current is not None:
        out[current] = tuple(frames)
    return out


_cache: dict[tuple[str, str, int, int], dict[int, tuple[Frame, ...]]] = {}


def _resolve(tool: tuple[str, list[str]], module: dict[str, Any], offsets: list[int]) -> dict[int, tuple[Frame, ...]]:
    key = (module["path"], module.get("build_id", ""), module.get("size", 0), module.get("mtime_ns", 0))
    known = _cache.setdefault(key, {})
    wanted = sorted(set(offsets) - known.keys())
    if wanted:
        program, args = tool
        proc = subprocess.run(
            [program, *args, module["path"]],
            input="\n".join(f"{o:#x}" for o in wanted) + "\n",
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
        known.update(parse_gnu_output(proc.stdout))
    return {o: known.get(o, ()) for o in offsets}


def symbolize(modules: list[dict[str, Any]], addresses: list[int]) -> Result:
    """Name each of @p addresses through @p modules, the ``cpu`` extension's module map."""
    result = Result()
    tool = _tool()
    result.tool = Path(tool[0]).name if tool else ""
    by_module: dict[str, tuple[dict[str, Any], list[int]]] = {}
    for address in set(addresses):
        module = _module_of(modules, address)
        if module is None:
            result.locations[address] = Location(address)
            continue
        by_module.setdefault(module["path"], (module, []))[1].append(address)
    for path, (module, members) in by_module.items():
        base = int(module.get("base", "0x0"), 16)
        problem = module_problem(module) or ("" if tool else "neither llvm-symbolizer nor addr2line is on the path")
        frames = {} if problem else _resolve(tool, module, [a - base for a in members])  # type: ignore[arg-type]
        if problem:
            result.problems[path] = problem
        for address in members:
            result.locations[address] = Location(address, path, address - base, frames.get(address - base, ()))
    return result
