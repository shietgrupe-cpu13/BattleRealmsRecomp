#!/usr/bin/env python3
"""Audit that lifted functions form one collision-free dispatch symbol set."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


FUNCTION = re.compile(r"_Z10(f_[0-9a-fA-F]{8})P3CPU$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--main-objects", type=Path, required=True)
    parser.add_argument("--script-objects", type=Path, required=True)
    parser.add_argument("--combined-table-object", type=Path, required=True)
    parser.add_argument("--expected-functions", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for path in (args.nm, args.main_objects, args.script_objects, args.combined_table_object):
        if not path.exists():
            parser.error(f"required input does not exist: {path}")

    objects = []
    table_name = "lifted_table.cpp.o"
    for root in (args.main_objects, args.script_objects):
        objects.extend(sorted((path for path in root.rglob("*.o")
                               if path.name != table_name), key=lambda p: str(p).casefold()))
    objects.append(args.combined_table_object)
    definitions: dict[str, str] = {}
    unresolved: dict[str, list[str]] = {}
    duplicate_definitions: dict[str, list[str]] = {}
    table_definitions: list[tuple[str, str]] = []
    object_count = 0
    batch_size = 48
    for start in range(0, len(objects), batch_size):
        batch = objects[start:start + batch_size]
        result = subprocess.run([str(args.nm), "--format=posix", "--extern-only",
                                 "--print-file-name", *(str(path) for path in batch)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace", check=False)
        if result.returncode:
            raise RuntimeError(f"llvm-nm failed: {result.stderr.strip()}")
        object_count += len(batch)
        for line in result.stdout.splitlines():
            if ": " not in line:
                continue
            object_path, payload = line.split(": ", 1)
            fields = payload.split()
            if len(fields) < 2:
                continue
            symbol, symbol_type = fields[0], fields[1]
            if symbol in ("lifted_table", "lifted_count"):
                if symbol_type.upper() in ("B", "D", "R", "S", "T", "V", "W"):
                    table_definitions.append((symbol, object_path))
            match = FUNCTION.fullmatch(symbol)
            if not match:
                continue
            function = match.group(1).lower()
            if symbol_type.upper() == "U":
                unresolved.setdefault(function, []).append(object_path)
            elif symbol_type.upper() in ("T", "W"):
                previous = definitions.get(function)
                if previous:
                    duplicate_definitions.setdefault(function, [previous]).append(object_path)
                else:
                    definitions[function] = object_path

    unresolved_functions = sorted(set(unresolved) - set(definitions))
    table_counts = {name: sum(1 for symbol, _path in table_definitions if symbol == name)
                    for name in ("lifted_table", "lifted_count")}
    report = {
        "schema": 1,
        "object_count": object_count,
        "function_definitions": len(definitions),
        "expected_dispatch_entries": args.expected_functions,
        "unresolved_lifted_functions": len(unresolved_functions),
        "duplicate_lifted_function_definitions": len(duplicate_definitions),
        "combined_table_symbol_definitions": table_counts,
        "passed": (len(definitions) == args.expected_functions and not unresolved_functions
                   and not duplicate_definitions and table_counts == {"lifted_table": 1,
                                                                      "lifted_count": 1}),
        "unresolved_sample": unresolved_functions[:25],
        "duplicate_sample": {key: value for key, value in list(duplicate_definitions.items())[:10]},
        "table_symbol_files": table_definitions,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("unresolved_sample", "duplicate_sample", "table_symbol_files")},
                     indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
