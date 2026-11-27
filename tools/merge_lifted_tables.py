#!/usr/bin/env python3
"""Build one deterministic dispatch table for the main PE and rebased scripts."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


ENTRY = re.compile(r"\{\s*0x([0-9a-fA-F]+)u?\s*,\s*f_([0-9a-fA-F]+)\s*\},")
COUNT = re.compile(r"lifted_count\s*=\s*(\d+)\s*;")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_table(path: Path) -> list[tuple[int, str]]:
    source = path.read_text(encoding="utf-8")
    rows = [(int(match.group(1), 16), f"f_{match.group(2).lower()}")
            for match in ENTRY.finditer(source)]
    count = COUNT.search(source)
    if not count:
        raise ValueError(f"missing lifted_count in {path}")
    if int(count.group(1)) != len(rows):
        raise ValueError(f"table row count mismatch in {path}: {len(rows)} rows, expected {count.group(1)}")
    for address, symbol in rows:
        if int(symbol[2:], 16) != address:
            raise ValueError(f"table address/symbol mismatch in {path}: 0x{address:08X} {symbol}")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-root", type=Path, required=True,
                        help="common generated/ directory, used for stable include paths")
    parser.add_argument("--main-dir", type=Path, required=True)
    parser.add_argument("--additional-main-dir", type=Path, action="append", default=[],
                        help="additional independently generated main-PE root tables")
    parser.add_argument("--scripts-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    root = args.generated_root.resolve()
    main_dir = args.main_dir.resolve()
    additional_main_dirs = [path.resolve() for path in args.additional_main_dir]
    scripts_dir = args.scripts_dir.resolve()
    output = args.output.resolve()
    manifest = args.manifest.resolve()
    for path in (main_dir, *additional_main_dirs, scripts_dir, output, manifest):
        try:
            path.relative_to(root)
        except ValueError as exc:
            parser.error(f"path must remain under --generated-root: {path}")

    tables: list[tuple[Path, Path, list[tuple[int, str]]]] = []
    main_table = main_dir / "lifted_table.cpp"
    main_decl = main_dir / "lifted_decl.h"
    if not main_table.is_file() or not main_decl.is_file():
        parser.error("main lifted_table.cpp and lifted_decl.h must exist")
    tables.append((main_table, main_decl, read_table(main_table)))
    for extra_dir in additional_main_dirs:
        table = extra_dir / "lifted_table.cpp"
        declaration = extra_dir / "lifted_decl.h"
        if not table.is_file() or not declaration.is_file():
            parser.error(f"additional main lift directory is incomplete: {extra_dir}")
        tables.append((table, declaration, read_table(table)))

    script_manifest_path = scripts_dir / "manifest.json"
    script_manifest = json.loads(script_manifest_path.read_text(encoding="utf-8"))
    if script_manifest.get("generated") != 68 or script_manifest.get("errors") != 0:
        parser.error("expected all 68 valid script modules to be generated without errors")
    for module in sorted((p for p in scripts_dir.iterdir() if p.is_dir()), key=lambda p: p.name.casefold()):
        table = module / "lifted_table.cpp"
        decl = module / "lifted_decl.h"
        if not table.is_file() or not decl.is_file():
            parser.error(f"incomplete lifted module: {module}")
        tables.append((table, decl, read_table(table)))

    all_rows: list[tuple[int, str]] = []
    seen: dict[int, tuple[str, Path]] = {}
    module_info = []
    include_files: list[Path] = []
    for table, decl, rows in tables:
        include_files.append(decl)
        duplicate = next((address for address, _symbol in rows if address in seen), None)
        if duplicate is not None:
            raise ValueError(f"duplicate guest function address 0x{duplicate:08X} in {table} and {seen[duplicate][1]}")
        for address, symbol in rows:
            seen[address] = (symbol, table)
        all_rows.extend(rows)
        module_info.append({
            "module": table.parent.name,
            "table": str(table),
            "functions": len(rows),
            "min_guest_va": f"0x{min((x[0] for x in rows), default=0):08X}",
            "max_guest_va": f"0x{max((x[0] for x in rows), default=0):08X}",
            "table_sha256": digest(table.read_bytes()),
        })

    all_rows.sort(key=lambda item: (item[0], item[1]))
    if len({symbol for _, symbol in all_rows}) != len(all_rows):
        raise ValueError("function symbols collide despite guest addresses being distinct")
    includes = sorted({path.relative_to(root).as_posix() for path in include_files}, key=str.casefold)
    lines = ["// Generated by tools/merge_lifted_tables.py; do not edit.\n"]
    lines.extend(f'#include "{include}"\n' for include in includes)
    lines.append("\nextern const LiftedEntry lifted_table[] = {\n")
    lines.extend(f"\t{{0x{address:08x}u, {symbol}}},\n" for address, symbol in all_rows)
    lines.extend(["};\n", f"extern const unsigned lifted_count = {len(all_rows)};\n"])
    encoded = "".join(lines).encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(encoded)
    result = {
        "schema": 1,
        "entry_count": len(all_rows),
        "unique_guest_addresses": len(seen),
        "unique_function_symbols": len({symbol for _, symbol in all_rows}),
        "module_count": len(tables),
        "generated_bytes": len(encoded),
        "output_sha256": digest(encoded),
        "modules": module_info,
    }
    manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "modules"}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
