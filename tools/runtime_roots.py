"""Parse runtime-observed guest entry addresses for the static lifter."""
from __future__ import annotations

from pathlib import Path
from typing import Callable


def read_runtime_roots(path: Path | None, is_executable_va: Callable[[int], bool]) -> list[int]:
    """Read unique VA roots and reject malformed or non-executable addresses."""
    if path is None:
        return []

    roots: set[int] = set()
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        value = raw_line.split("#", 1)[0].strip()
        if not value:
            continue
        try:
            address = int(value, 0)
        except ValueError as exc:
            raise ValueError(f"invalid runtime root at {path}:{line_number}: {value!r}") from exc
        if not 0 <= address <= 0xFFFFFFFF:
            raise ValueError(f"runtime root outside 32-bit address space at {path}:{line_number}")
        if not is_executable_va(address):
            raise ValueError(f"runtime root is not in an executable PE section at {path}:{line_number}: 0x{address:08X}")
        roots.add(address)
    return sorted(roots)
