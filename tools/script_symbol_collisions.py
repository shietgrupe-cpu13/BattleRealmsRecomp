#!/usr/bin/env python3
"""Quantify module-local lifted function address overlap across script DLLs."""
from __future__ import annotations

from collections import Counter, defaultdict
import json
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: script_symbol_collisions.py <generated-scripts-dir> <output-json>")
    root, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    locations: dict[str, list[str]] = defaultdict(list)
    total = 0
    for catalog in sorted(root.glob("*/function_catalog.json")):
        payload = json.loads(catalog.read_text(encoding="utf-8"))
        module = payload.get("file", catalog.parent.name)
        for function in payload.get("function_starts", []):
            va = function["va"].lower()
            locations[va].append(module)
            total += 1
    collisions = {va: sorted(modules) for va, modules in locations.items() if len(modules) > 1}
    result = {
        "module_catalogs": len({m for group in locations.values() for m in group}),
        "module_local_function_start_entries": total,
        "unique_preferred_base_addresses": len(locations),
        "addresses_shared_by_multiple_modules": len(collisions),
        "duplicate_address_occurrences": total - len(locations),
        "max_module_multiplicity": max((len(modules) for modules in locations.values()), default=0),
        "most_shared_addresses": [
            {"va": va, "module_count": len(modules), "modules": modules}
            for va, modules in sorted(collisions.items(), key=lambda row: (-len(row[1]), row[0]))[:25]
        ],
        "warning": "All script DLLs share the same preferred image base; distinct module loads need relocated guest addresses and module-specific generated dispatch symbols.",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "most_shared_addresses"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
