#!/usr/bin/env python3
"""Classify PE HIGHLOW relocations by section and decoded x86 operand field."""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import re

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


HIGHLOW = 3
EXECUTE = 0x20000000


def audit(path: Path, generated_root: Path | None) -> dict:
    try:
        pe = pefile.PE(str(path), fast_load=False)
    except (pefile.PEFormatError, OSError) as exc:
        return {"file": path.name, "valid_pe": False, "error": str(exc)}

    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    executable = []
    for section in pe.sections:
        if not section.Characteristics & EXECUTE:
            continue
        size = min(max(section.Misc_VirtualSize, section.SizeOfRawData), section.SizeOfRawData)
        code = section.get_data()[:size]
        instructions = []
        for insn in decoder.disasm(code, pe.OPTIONAL_HEADER.ImageBase + section.VirtualAddress):
            instructions.append({
                "start": insn.address - pe.OPTIONAL_HEADER.ImageBase,
                "end": insn.address - pe.OPTIONAL_HEADER.ImageBase + insn.size,
                "imm_start": insn.address - pe.OPTIONAL_HEADER.ImageBase + insn.imm_offset if insn.imm_size else None,
                "imm_size": insn.imm_size,
                "disp_start": insn.address - pe.OPTIONAL_HEADER.ImageBase + insn.disp_offset if insn.disp_size else None,
                "disp_size": insn.disp_size,
                "mnemonic": insn.mnemonic,
                "op_str": insn.op_str,
                "bytes": bytes(insn.bytes).hex(),
            })
        executable.append({
            "name": section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
            "start": section.VirtualAddress,
            "end": section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData),
            "instructions": instructions,
            "starts": [instruction["start"] for instruction in instructions],
        })

    location_counts: Counter[str] = Counter()
    type_counts: Counter[str] = Counter()
    mnemonic_counts: Counter[str] = Counter()
    samples: list[dict] = []
    relocation_operand_instructions: list[tuple[int, str]] = []
    relocs = getattr(pe, "DIRECTORY_ENTRY_BASERELOC", ())
    total = 0
    for block in relocs:
        for entry in block.entries:
            total += 1
            if entry.type == 0:
                type_counts["ABSOLUTE"] += 1
                continue
            if entry.type != HIGHLOW:
                type_counts[f"OTHER_{entry.type}"] += 1
                location_counts["unsupported_type"] += 1
                continue
            type_counts["HIGHLOW"] += 1
            target = entry.rva
            section = next((s for s in executable if s["start"] <= target < s["end"]), None)
            if section is None:
                mapped = next((s for s in pe.sections
                               if s.VirtualAddress <= target < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)), None)
                location = mapped.Name.rstrip(b"\0").decode("ascii", errors="replace") if mapped else "unmapped"
                location_counts[f"non_exec:{location}"] += 1
                continue
            starts = section["starts"]
            idx = bisect_right(starts, target) - 1
            insn = section["instructions"][idx] if idx >= 0 else None
            if insn and insn["start"] <= target < insn["end"]:
                if insn["imm_start"] == target and insn["imm_size"] == 4:
                    kind = "exec_immediate32"
                elif insn["disp_start"] == target and insn["disp_size"] == 4:
                    kind = "exec_displacement32"
                else:
                    kind = "exec_other_instruction_bytes"
                location_counts[kind] += 1
                mnemonic_counts[insn["mnemonic"]] += 1
                if kind in ("exec_immediate32", "exec_displacement32"):
                    relocation_operand_instructions.append((insn["start"], kind))
                if len(samples) < 100:
                    samples.append({"rva": f"0x{target:08X}", "section": section["name"],
                                    "kind": kind, "instruction": insn})
            else:
                location_counts["exec_not_decoded"] += 1

    lifted_match = None
    if generated_root is not None:
        module_key = re.sub(r"[^A-Za-z0-9_.-]+", "_", path.stem)
        module_dir = generated_root / module_key
        lifted_starts: set[int] = set()
        comment = re.compile(r"^\s*/\*\s*([0-9A-Fa-f]{8})\s+")
        for source in sorted(module_dir.glob("lifted_*.cpp")):
            if source.name == "lifted_table.cpp":
                continue
            with source.open("r", encoding="utf-8", errors="replace") as stream:
                for line in stream:
                    match = comment.match(line)
                    if match:
                        lifted_starts.add(int(match.group(1), 16) - pe.OPTIONAL_HEADER.ImageBase)
        matched = sum(start in lifted_starts for start, _kind in relocation_operand_instructions)
        lifted_match = {
            "generated_module_directory": str(module_dir),
            "generated_instruction_comments": len(lifted_starts),
            "relocation_operand_entries": len(relocation_operand_instructions),
            "relocation_operand_entries_in_lifted_sources": matched,
            "relocation_operand_entries_without_lifted_comment": len(relocation_operand_instructions) - matched,
        }

    result = {
        "file": path.name,
        "valid_pe": True,
        "image_base": pe.OPTIONAL_HEADER.ImageBase,
        "relocation_entries_total": total,
        "relocation_type_counts": dict(sorted(type_counts.items())),
        "relocation_location_counts": dict(sorted(location_counts.items())),
        "executable_target_mnemonics": dict(sorted(mnemonic_counts.items())),
        "samples_first_100_executable_targets": samples,
    }
    if lifted_match is not None:
        result["generated_source_match"] = lifted_match
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("scripts_dir", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--generated-dir", type=Path,
                        help="optional batch root, e.g. generated/scripts/batch96")
    args = parser.parse_args()
    paths = sorted(args.scripts_dir.glob("*.dll"), key=lambda p: p.name.casefold())
    modules = [audit(path, args.generated_dir) for path in paths]
    summary: Counter[str] = Counter()
    types: Counter[str] = Counter()
    source_matches: Counter[str] = Counter()
    for module in modules:
        summary.update(module.get("relocation_location_counts", {}))
        types.update(module.get("relocation_type_counts", {}))
        for key, value in module.get("generated_source_match", {}).items():
            if isinstance(value, int):
                source_matches[key] += value
    result = {
        "source_directory": str(args.scripts_dir),
        "module_files_seen": len(paths),
        "valid_pe_files": sum(module.get("valid_pe", False) for module in modules),
        "relocation_type_totals": dict(sorted(types.items())),
        "relocation_location_totals": dict(sorted(summary.items())),
        "generated_source_match_totals": dict(sorted(source_matches.items())),
        "modules": modules,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "modules"}, indent=2))


if __name__ == "__main__":
    main()
