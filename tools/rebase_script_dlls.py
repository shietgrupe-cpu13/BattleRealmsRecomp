#!/usr/bin/env python3
"""Create deterministic, relocation-correct PE32 copies for module-specific lifting."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

import pefile


HIGHLOW = 3
ABSOLUTE = 0
ALIGNMENT = 0x10000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def aligned(value: int, alignment: int = ALIGNMENT) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def section_raw_offset(pe: pefile.PE, rva: int) -> int:
    section = pe.get_section_by_rva(rva)
    if section is None:
        raise ValueError(f"relocation RVA 0x{rva:08X} is outside sections")
    delta = rva - section.VirtualAddress
    if delta + 4 > section.SizeOfRawData:
        raise ValueError(f"relocation RVA 0x{rva:08X} is not fully file-backed")
    return section.PointerToRawData + delta


def rebase(path: Path, output: Path, new_base: int) -> dict:
    original_bytes = path.read_bytes()
    pe = pefile.PE(data=original_bytes, fast_load=False)
    if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise ValueError("not a PE32/I386 module")
    old_base = pe.OPTIONAL_HEADER.ImageBase
    delta = (new_base - old_base) & 0xFFFFFFFF
    if delta and not getattr(pe, "DIRECTORY_ENTRY_BASERELOC", None):
        raise ValueError("cannot rebase image without a base-relocation table")

    relocated = bytearray(original_bytes)
    relocation_records: list[tuple[int, int, int]] = []
    highlow_count = absolute_count = 0
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", ()):
        for entry in block.entries:
            if entry.type == ABSOLUTE:
                absolute_count += 1
                continue
            if entry.type != HIGHLOW:
                raise ValueError(f"unsupported relocation type {entry.type} at RVA 0x{entry.rva:08X}")
            file_offset = section_raw_offset(pe, entry.rva)
            if file_offset + 4 > len(original_bytes):
                raise ValueError(f"relocation at RVA 0x{entry.rva:08X} extends past file")
            before = struct.unpack_from("<I", original_bytes, file_offset)[0]
            after = (before + delta) & 0xFFFFFFFF
            struct.pack_into("<I", relocated, file_offset, after)
            relocation_records.append((entry.rva, file_offset, before))
            highlow_count += 1

    pe_offset = pe.DOS_HEADER.e_lfanew
    image_base_offset = pe_offset + 24 + 28
    struct.pack_into("<I", relocated, image_base_offset, new_base)

    rebased_pe = pefile.PE(data=bytes(relocated), fast_load=False)
    if rebased_pe.OPTIONAL_HEADER.ImageBase != new_base:
        raise ValueError("rebased PE header did not retain assigned image base")
    if rebased_pe.FILE_HEADER.Machine != 0x14C or rebased_pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise ValueError("rebased PE changed architecture")
    validated = 0
    for rva, _file_offset, before in relocation_records:
        after_data = rebased_pe.get_data(rva, 4)
        if len(after_data) != 4:
            raise ValueError(f"rebased relocation target unreadable at RVA 0x{rva:08X}")
        after = struct.unpack("<I", after_data)[0]
        if after != (before + delta) & 0xFFFFFFFF:
            raise ValueError(f"relocation mismatch at RVA 0x{rva:08X}")
        validated += 1
    if validated != highlow_count:
        raise ValueError("relocation validation count mismatch")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(relocated)
    return {
        "file": path.name,
        "output": str(output),
        "original_sha256": sha256(original_bytes),
        "rebased_sha256": sha256(relocated),
        "original_image_base": f"0x{old_base:08X}",
        "assigned_image_base": f"0x{new_base:08X}",
        "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage,
        "highlow_relocations": highlow_count,
        "absolute_padding_entries": absolute_count,
        "validated_highlow_relocations": validated,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scripts_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--base-start", type=lambda v: int(v, 0), default=0x20000000)
    parser.add_argument("--base-limit", type=lambda v: int(v, 0), default=0x70000000)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    cursor = aligned(args.base_start)
    rows: list[dict] = []
    errors: list[dict] = []
    invalid_pe: list[dict] = []
    candidates = sorted(args.scripts_dir.glob("*.dll"), key=lambda p: p.name.casefold())
    for source in candidates:
        try:
            source_pe = pefile.PE(str(source), fast_load=True)
            if source_pe.FILE_HEADER.Machine != 0x14C or source_pe.OPTIONAL_HEADER.Magic != 0x10B:
                raise ValueError("not a PE32/I386 module")
            base = cursor
            span = aligned(source_pe.OPTIONAL_HEADER.SizeOfImage)
            uint_end = base + span
            if uint_end > args.base_limit or uint_end > 0xFFFFFFFF:
                raise ValueError("assigned script DLL guest address range exhausted")
            target = args.output_dir / source.name
            rows.append(rebase(source, target, base))
            cursor = uint_end
        except pefile.PEFormatError as exc:
            invalid_pe.append({"file": source.name, "status": "invalid_pe_skipped", "error": str(exc)})
        except Exception as exc:
            errors.append({"file": source.name, "status": "error", "error": str(exc)})

    result = {
        "source_directory": str(args.scripts_dir),
        "output_directory": str(args.output_dir),
        "base_start": f"0x{aligned(args.base_start):08X}",
        "next_free_base": f"0x{cursor:08X}",
        "valid_modules_rebased": len(rows),
        "non_pe_skipped": len(invalid_pe),
        "invalid_pe": invalid_pe,
        "errors": errors,
        "modules": rows,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "modules"}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
