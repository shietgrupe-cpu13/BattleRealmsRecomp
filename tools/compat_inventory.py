#!/usr/bin/env python3
"""Compare Battle Realms PE imports with names registered by RA3's runtime."""
from __future__ import annotations

import json
from pathlib import Path
import re
import sys

import pefile


REGISTRATION = re.compile(
    r"\bIMP(?:_[A-Z0-9_]+)?\s*\(\s*([^,\s]+)\s*,\s*(?:\"([^\"]+)\"|([A-Za-z_]\w*))",
    re.MULTILINE,
)
DEFINE_STRING = re.compile(r'^\s*#\s*define\s+(\w+)\s+"([^"]+)"', re.MULTILINE)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: compat_inventory.py <exe> <ra3-runtime-dir> <workspace>")
    exe, runtime, workspace = (Path(arg).resolve() for arg in sys.argv[1:])
    pe = pefile.PE(str(exe), fast_load=False)
    imports: set[tuple[str, str]] = set()
    by_dll: dict[str, set[str]] = {}
    for desc in pe.DIRECTORY_ENTRY_IMPORT:
        dll = desc.dll.decode("ascii", errors="replace").lower()
        names = set()
        for sym in desc.imports:
            if sym.name:
                name = sym.name.decode("ascii", errors="replace")
                names.add(name)
                imports.add((dll, name.lower()))
        by_dll[dll] = names

    registrations: set[tuple[str, str]] = set()
    source_files = sorted([*runtime.rglob("*.cpp"), *runtime.rglob("*.h")])
    for path in source_files:
        source = path.read_text(encoding="utf-8", errors="replace")
        macros = {name: value.lower() for name, value in DEFINE_STRING.findall(source)}
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
        for match in REGISTRATION.finditer(source):
            dll_token, literal_name, symbol_name = match.groups()
            dll = dll_token.strip('"').lower()
            dll = macros.get(dll_token, dll)
            name = (literal_name or symbol_name or "").lower()
            if dll and name:
                registrations.add((dll, name))

    overlap = imports & registrations
    registered_names = {name for _, name in registrations}
    name_only_overlap = {pair for pair in imports if pair[1] in registered_names}
    unmatched = sorted((dll, name) for dll, names in by_dll.items() for name in names
                       if (dll, name.lower()) not in registrations)
    result = {
        "input": str(exe),
        "runtime": str(runtime),
        "static_imports_unique": len(imports),
        "runtime_registration_pairs_parsed": len(registrations),
        "name_and_dll_matches": len(overlap),
        "match_ratio": round(len(overlap) / len(imports), 4) if imports else 0,
        "name_matches_any_registered_dll": len(name_only_overlap),
        "name_only_match_ratio": round(len(name_only_overlap) / len(imports), 4) if imports else 0,
        "by_dll": {
            dll: {
                "imports": len(names),
                "name_and_dll_matches": sum((dll, name.lower()) in registrations for name in names),
                "name_matches_any_registered_dll": sum(name.lower() in registered_names for name in names),
                "unmatched_names": sorted(name for name in names if (dll, name.lower()) not in registrations),
            }
            for dll, names in sorted(by_dll.items())
        },
        "unmatched": [{"dll": dll, "name": name} for dll, name in unmatched],
        "warning": "A registration name match is only a static source-code indication; it does not verify API semantics, argument layouts, or runtime behavior.",
    }
    out = workspace / "analysis" / "ra3_import_name_overlap.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k not in ("by_dll", "unmatched")}, indent=2))
    for dll, row in result["by_dll"].items():
        print(f"{dll}: {row['name_and_dll_matches']}/{row['imports']} names match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
