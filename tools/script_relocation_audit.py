#!/usr/bin/env python3
"""Read-only PE base-relocation inventory for Battle Realms script DLLs."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

import pefile


def audit(path: Path) -> dict:
    try:
        pe = pefile.PE(str(path), fast_load=False)
    except (pefile.PEFormatError, OSError) as exc:
        return {"path": str(path), "valid_pe": False, "error": str(exc)}

    type_counts: Counter[str] = Counter()
    relocation_count = 0
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", ()):
        for entry in block.entries:
            relocation_count += 1
            name = pefile.RELOCATION_TYPE.get(entry.type, f"UNKNOWN_{entry.type}")
            type_counts[name] += 1

    reloc_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]
    ]
    return {
        "path": str(path),
        "valid_pe": True,
        "machine": pe.FILE_HEADER.Machine,
        "is_dll": bool(pe.FILE_HEADER.Characteristics & 0x2000),
        "image_base": pe.OPTIONAL_HEADER.ImageBase,
        "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage,
        "entry_rva": pe.OPTIONAL_HEADER.AddressOfEntryPoint,
        "relocation_directory_rva": reloc_dir.VirtualAddress,
        "relocation_directory_size": reloc_dir.Size,
        "relocation_entries": relocation_count,
        "relocation_types": dict(sorted(type_counts.items())),
        "dynamic_base_flag": bool(pe.OPTIONAL_HEADER.DllCharacteristics & 0x40),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("script_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    rows = [audit(path) for path in sorted(args.script_dir.rglob("*.dll"), key=lambda p: str(p).lower())]
    valid = [row for row in rows if row["valid_pe"]]
    type_totals: Counter[str] = Counter()
    for row in valid:
        type_totals.update(row["relocation_types"])
    summary = {
        "source_directory": str(args.script_dir),
        "dll_files_seen": len(rows),
        "valid_pe_files": len(valid),
        "non_pe_files": len(rows) - len(valid),
        "preferred_image_bases": dict(sorted(Counter(f"0x{r['image_base']:08x}" for r in valid).items())),
        "modules_with_relocation_entries": sum(bool(r["relocation_entries"]) for r in valid),
        "modules_with_dynamic_base_flag": sum(bool(r["dynamic_base_flag"]) for r in valid),
        "total_relocation_entries": sum(r["relocation_entries"] for r in valid),
        "relocation_type_totals": dict(sorted(type_totals.items())),
        "modules": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "modules"}, indent=2))


if __name__ == "__main__":
    main()
