# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""The little of an ELF file that symbolizing samples needs: its GNU build id, which tells the file
that ran from a rebuilt one, and its function symbols, which give a function's extent for
disassembly. 64-bit little-endian files, as Linux on x86-64 and aarch64 has; anything else reads
as having neither.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

_SHT_SYMTAB, _SHT_NOTE, _SHT_DYNSYM = 2, 7, 11
_STT_FUNC = 2
_NT_GNU_BUILD_ID = 3


@dataclass(frozen=True)
class Function:
    """A function symbol: where it starts in the file's address space, its length, its name as linked."""

    start: int
    size: int
    name: str


def _read(path: str | Path) -> bytes | None:
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    # ELF magic, 64-bit (class 2), little-endian (data 1)
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    return data


def _sections(data: bytes) -> list[tuple[int, int, int, int, int, int]]:
    """(type, offset, size, link, entsize, name) of each section header."""
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    out = []
    for i in range(shnum):
        at = shoff + i * shentsize
        if at + 64 > len(data):
            break
        name, kind, _flags, _addr, offset, size, link, _info, _align, entsize = struct.unpack_from("<IIQQQQIIQQ", data, at)
        out.append((kind, offset, size, link, entsize, name))
    return out


def build_id(path: str | Path) -> str:
    """The file's GNU build id in hexadecimal, as the collector records it; empty if it has none."""
    data = _read(path)
    if data is None:
        return ""
    for kind, offset, size, _link, _entsize, _name in _sections(data):
        if kind != _SHT_NOTE:
            continue
        at, end = offset, min(offset + size, len(data))
        while at + 12 <= end:
            namesz, descsz, note = struct.unpack_from("<III", data, at)
            name = at + 12
            desc = name + (namesz + 3) // 4 * 4
            if note == _NT_GNU_BUILD_ID and data[name : name + namesz] == b"GNU\x00":
                return data[desc : desc + descsz].hex()
            at = desc + (descsz + 3) // 4 * 4
    return ""


@lru_cache(maxsize=64)
def _functions(path: str, stamp: tuple[int, int]) -> tuple[Function, ...]:
    data = _read(path)
    if data is None:
        return ()
    sections = _sections(data)
    # The full symbol table when the file has one; the dynamic one when it was stripped.
    for wanted in (_SHT_SYMTAB, _SHT_DYNSYM):
        found = []
        for kind, offset, size, link, entsize, _name in sections:
            if kind != wanted or entsize != 24 or link >= len(sections):
                continue
            strings = sections[link][1]
            for at in range(offset, min(offset + size, len(data)) - 23, 24):
                name, info, _other, _shndx, value, length = struct.unpack_from("<IBBHQQ", data, at)
                if info & 0xF == _STT_FUNC and value and length:
                    end = data.index(b"\x00", strings + name)
                    found.append(Function(value, length, data[strings + name : end].decode(errors="replace")))
        if found:
            return tuple(sorted(found, key=lambda f: f.start))
    return ()


def functions(path: str | Path) -> tuple[Function, ...]:
    """The file's function symbols, by start; read once per version of the file."""
    try:
        st = Path(path).stat()
    except OSError:
        return ()
    return _functions(str(path), (st.st_size, st.st_mtime_ns))


def function_at(path: str | Path, address: int) -> Function | None:
    """The function covering @p address in the file's address space, if any."""
    found = None
    for f in functions(path):
        if f.start > address:
            break
        if address < f.start + f.size:
            found = f
    return found
