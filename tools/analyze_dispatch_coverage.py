#!/usr/bin/env python3
"""Compare Ghidra code-flow targets with the generated guest dispatch table.

The report is candidate discovery only: static edges do not by themselves prove
that a target should be exposed as an independently dispatched lifted entry.
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import time
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

TABLE_ENTRY = re.compile(r"\{\s*0x([0-9a-fA-F]{1,8})u\s*,\s*f_[0-9a-fA-F]{8}\s*\}")
FUNCTION_DECL = re.compile(rb"(?m)^void f_([0-9a-f]{8})\(cpu\* c\)")


def scan_generated_coverage(directory: Path, addresses: Iterable[int]) -> dict[int, list[dict[str, str]]]:
	"""Find generated instruction comments/labels for a bounded candidate set."""
	addresses = sorted(set(addresses))
	if not addresses:
		return {}
	choice = b"|".join(f"{address:08x}".encode("ascii") for address in addresses)
	coverage_pattern = re.compile(rb"/\*\s*((?:" + choice + rb"))\s|l_((?:" + choice + rb")):;")
	found: dict[int, set[tuple[str, str, str]]] = defaultdict(set)
	if not directory.is_dir():
		raise FileNotFoundError(directory)
	for source in sorted(directory.glob("lifted_*.cpp")):
		content = source.read_bytes().lower()
		declarations = [(match.start(), match.group(1).decode("ascii"))
			for match in FUNCTION_DECL.finditer(content)]
		declaration_offsets = [offset for offset, _owner in declarations]
		for match in coverage_pattern.finditer(content):
			group = 1 if match.group(1) else 2
			address = int(match.group(group), 16)
			declaration_index = bisect.bisect_right(declaration_offsets, match.start()) - 1
			owner = declarations[declaration_index][1] if declaration_index >= 0 else "unknown"
			kind = "instruction_comment" if group == 1 else "generated_label"
			found[address].add((source.name, owner, kind))
	return {address: [{"file": file, "owner": owner, "evidence": kind}
				  for file, owner, kind in sorted(rows)] for address, rows in found.items()}


def read_records(path: Path) -> list[dict[str, Any]]:
	rows = []
	with path.open("r", encoding="utf-8") as stream:
		for line_number, line in enumerate(stream, 1):
			if not line.strip():
				continue
			try:
				row = json.loads(line)
			except json.JSONDecodeError as exc:
				raise ValueError(f"invalid JSON at {path}:{line_number}: {exc}") from exc
			if isinstance(row, dict) and row.get("record") == "function":
				rows.append(row)
	return rows


def read_dispatch(path: Path) -> set[int]:
	return {int(match.group(1), 16) for match in TABLE_ENTRY.finditer(path.read_text(encoding="utf-8"))}


def executable_ranges(exe: Path) -> tuple[int, list[tuple[int, int]]]:
	import pefile

	pe = pefile.PE(str(exe), fast_load=True)
	if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
		raise ValueError("expected an x86 PE32 executable")
	base = int(pe.OPTIONAL_HEADER.ImageBase)
	ranges = []
	for section in pe.sections:
		if section.Characteristics & 0x20000000:
			start = base + int(section.VirtualAddress)
			ranges.append((start, start + max(int(section.Misc_VirtualSize), int(section.SizeOfRawData))))
	return base, ranges


def is_executable(address: int, ranges: list[tuple[int, int]]) -> bool:
	return any(start <= address < end for start, end in ranges)


def _address(value: Any) -> int | None:
	if not isinstance(value, str):
		return None
	try:
		return int(value, 0)
	except ValueError:
		return None


def _contains(row: dict[str, Any], address: int) -> bool:
	for item in row.get("body_ranges", []):
		if not isinstance(item, str) or ".." not in item:
			continue
		left, right = item.split("..", 1)
		start, end = _address(left), _address(right)
		# Ghidra's exported AddressSet ranges use inclusive end addresses.
		if start is not None and end is not None and start <= address <= end:
			return True
	return False


def analyze(records: list[dict[str, Any]], dispatch: set[int],
			exec_ranges: list[tuple[int, int]], runtime_targets: set[int] | None = None,
			generated_coverage: dict[int, list[dict[str, str]]] | None = None) -> dict[str, Any]:
	runtime_targets = runtime_targets or set()
	generated_coverage = generated_coverage or {}
	entries: dict[int, dict[str, Any]] = {}
	for row in records:
		address = _address(row.get("entry"))
		if address is not None:
			entries[address] = row

	missing_entries = [address for address in entries if is_executable(address, exec_ranges) and address not in dispatch]
	candidate_refs: dict[int, list[dict[str, str]]] = defaultdict(list)
	for parent, row in sorted(entries.items()):
		for branch in row.get("branches", []):
			if not isinstance(branch, dict):
				continue
			kind = str(branch.get("kind", "unknown")).upper()
			# Direct conditional/unconditional branch destinations already execute
			# inside their containing lifted function; they are not dispatch roots.
			if kind not in {"COMPUTED_JUMP", "INDIRECT_JUMP"}:
				continue
			target = _address(branch.get("target"))
			if target is None or target in dispatch or not is_executable(target, exec_ranges):
				continue
			candidate_refs[target].append({
				"parent": f"0x{parent:08X}",
				"site": str(branch.get("site", "")),
				"kind": kind,
				"parent_body_contains_target": _contains(row, target),
			})
		for call in row.get("calls", []):
			if not isinstance(call, dict):
				continue
			target = _address(call.get("target"))
			if target is None or target in dispatch or not is_executable(target, exec_ranges):
				continue
			candidate_refs[target].append({
				"parent": f"0x{parent:08X}",
				"site": str(call.get("site", "")),
				"kind": str(call.get("kind", "unknown")).upper(),
				"parent_body_contains_target": _contains(row, target),
			})

	missing_runtime = sorted(runtime_targets - dispatch)
	candidates = []
	for address in sorted(set(candidate_refs) | set(missing_runtime) | set(missing_entries)):
		refs = candidate_refs.get(address, [])
		entry = entries.get(address)
		intra_function_target = bool(refs) and all(
			ref["parent_body_contains_target"] and
			ref["kind"] in {"COMPUTED_JUMP", "INDIRECT_JUMP"}
			for ref in refs)
		if entry:
			classification = "ghidra_function_entry"
			dispatch_guidance = ("inspect_generated_owner_before_adding_dispatch_entry"
				if generated_coverage.get(address) else
				"validate_references_and_generated_coverage_before_lifting")
		elif intra_function_target:
			classification = "intra_function_control_flow_target"
			dispatch_guidance = "keep_as_parent_function_block"
		else:
			classification = "unclassified_executable_target"
			dispatch_guidance = "gather_more_evidence_before_lifting"
		candidates.append({
			"target": f"0x{address:08X}",
			"classification": classification,
			"dispatch_guidance": dispatch_guidance,
			"evidence": (["runtime-observed"] if address in runtime_targets else []) +
				(["ghidra-function-entry"] if entry else []) +
				(sorted({ref["kind"] for ref in refs}) if refs else []),
			"ghidra_entry_name": entry.get("name") if entry else None,
			"entry_xrefs": entry.get("entry_xrefs", []) if entry else [],
			"data_refs": entry.get("data_refs", []) if entry else [],
			"generated_code_coverage": generated_coverage.get(address, []),
			"dispatch_registered": address in dispatch,
			"executable": True,
			"references": refs,
		})
	return {
		"schema": 1,
		"method": "Ghidra JSONL function entries and control-flow/call targets compared with generated lifted_table.cpp and PE executable sections",
		"interpretation": "Candidates require validation against generated-body coverage, references and runtime evidence; Ghidra branch targets may be intra-function basic blocks and are not automatically promoted to function roots.",
		"ghidra_function_entries": len(entries),
		"ghidra_executable_function_entries": sum(is_executable(address, exec_ranges) for address in entries),
		"ghidra_function_entries_registered": sum(address in dispatch for address in entries),
		"ghidra_executable_function_entries_missing": len(missing_entries),
		"dispatch_entries": len(dispatch),
		"runtime_targets": [f"0x{address:08X}" for address in sorted(runtime_targets)],
		"runtime_targets_missing_from_dispatch": [f"0x{address:08X}" for address in missing_runtime],
		"missing_executable_candidate_count": len(candidates),
		"candidates": candidates,
	}


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--exe", type=Path, required=True)
	parser.add_argument("--ghidra-jsonl", type=Path, required=True)
	parser.add_argument("--dispatch-table", type=Path, required=True)
	parser.add_argument("--generated-dir", type=Path,
					help="optional split C++ directory; scan only missing Ghidra entries and runtime targets")
	parser.add_argument("--runtime-target", action="append", default=[], type=lambda value: int(value, 0))
	parser.add_argument("--output", type=Path, required=True)
	args = parser.parse_args()
	base, ranges = executable_ranges(args.exe)
	records = read_records(args.ghidra_jsonl)
	dispatch = read_dispatch(args.dispatch_table)
	runtime_targets = set(args.runtime_target)
	generated_coverage = None
	generated_scan_seconds = None
	missing_ghidra_entries: set[int] = set()
	if args.generated_dir:
		started = time.perf_counter()
		entry_addresses = {_address(row.get("entry")) for row in records}
		missing_ghidra_entries = {address for address in entry_addresses if address is not None and
			is_executable(address, ranges) and address not in dispatch}
		generated_coverage = scan_generated_coverage(args.generated_dir,
			missing_ghidra_entries | runtime_targets)
		generated_scan_seconds = round(time.perf_counter() - started, 6)
	result = analyze(records, dispatch, ranges, runtime_targets, generated_coverage)
	if generated_scan_seconds is not None:
		result["generated_source_scan_seconds"] = generated_scan_seconds
		result["missing_ghidra_entries_with_generated_code"] = [
			f"0x{address:08X}" for address in sorted(missing_ghidra_entries & set(generated_coverage))]
		result["missing_ghidra_entries_without_generated_code"] = [
			f"0x{address:08X}" for address in sorted(missing_ghidra_entries - set(generated_coverage))]
	result["image_base"] = f"0x{base:08X}"
	result["exe"] = str(args.exe)
	result["ghidra_jsonl"] = str(args.ghidra_jsonl)
	result["dispatch_table"] = str(args.dispatch_table)
	args.output.parent.mkdir(parents=True, exist_ok=True)
	args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
	source_coverage_summary = (f"{len(missing_ghidra_entries & set(generated_coverage or {}))}/"
		f"{len(missing_ghidra_entries)}" if args.generated_dir else "omitted")
	print(f"Ghidra functions={result['ghidra_function_entries']} executable={result['ghidra_executable_function_entries']} "
		f"registered={result['ghidra_function_entries_registered']} missing={result['ghidra_executable_function_entries_missing']} "
		f"runtime_missing={len(result['runtime_targets_missing_from_dispatch'])} "
		f"generated_covered={sum(bool(rows) for rows in (generated_coverage or {}).values())} "
		f"missing_entry_source_coverage={source_coverage_summary} "
		f"source_scan={generated_scan_seconds if generated_scan_seconds is not None else 'omitted'}s "
		f"candidate_count={result['missing_executable_candidate_count']} output={args.output}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
