#!/usr/bin/env python3
"""Focused checks for runtime-observed PE entry-root validation."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from runtime_roots import read_runtime_roots


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="br-runtime-roots-") as temporary:
        roots_file = Path(temporary) / "roots.txt"
        roots_file.write_text(
            "# observed guest dispatch roots\n"
            "0x006A12DC\n"
            "0x006A13AC\n"
            "0x006A13A0\n"
            "0x006A13AC # duplicate\n",
            encoding="utf-8",
        )
        valid_roots = {0x006A12DC, 0x006A13A0, 0x006A13AC}
        roots = read_runtime_roots(roots_file, lambda address: address in valid_roots)
        check(roots == [0x006A12DC, 0x006A13A0, 0x006A13AC], "runtime roots must be sorted and deduplicated")

        roots_file.write_text("0x0000003E\n", encoding="utf-8")
        try:
            read_runtime_roots(roots_file, lambda _address: False)
        except ValueError as error:
            check("not in an executable PE section" in str(error), "non-code root rejection should explain why")
        else:
            raise AssertionError("non-executable runtime target must be rejected")

        roots_file.write_text("not-an-address\n", encoding="utf-8")
        try:
            read_runtime_roots(roots_file, lambda _address: True)
        except ValueError as error:
            check("invalid runtime root" in str(error), "malformed root rejection should explain why")
        else:
            raise AssertionError("malformed runtime target must be rejected")

    print("runtime-observed guest root validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
