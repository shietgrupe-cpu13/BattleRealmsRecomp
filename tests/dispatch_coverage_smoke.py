#!/usr/bin/env python3
"""Regression tests for evidence-ranked Ghidra/dispatch gap reporting."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_dispatch_coverage import analyze, scan_generated_coverage  # noqa: E402


def check(condition: bool, message: str) -> None:
	if not condition:
		raise AssertionError(message)


def main() -> int:
	ranges = [(0x00401000, 0x00402000)]
	rows = [
		{
			"record": "function",
			"entry": "0x00401000",
			"name": "FUN_00401000",
			"body_ranges": ["0x00401000..0x00401020"],
			"branches": [
				{"site": "0x00401004", "target": "0x00401010", "kind": "COMPUTED_JUMP"},
				{"site": "0x00401008", "target": "0x00403000", "kind": "COMPUTED_JUMP"},
			],
			"calls": [{"site": "0x0040100C", "target": "0x00401100", "kind": "COMPUTED_CALL"}],
		},
		{
			"record": "function",
			"entry": "0x00401100",
			"name": "FUN_00401100",
			"body_ranges": ["0x00401100..0x00401110"],
			"branches": [],
			"calls": [],
		},
	]
	with tempfile.TemporaryDirectory(prefix="br-dispatch-coverage-") as temporary:
		generated = Path(temporary)
		(generated / "lifted_0.cpp").write_text(
			"void f_00401000(CPU* c) {\nL_00401100:;\n/* 00401100 mov eax, eax */\n}\n",
			encoding="ascii")
		covered = scan_generated_coverage(generated, {0x00401100})
	check(covered[0x00401100] == [
		{"file": "lifted_0.cpp", "owner": "00401000", "evidence": "generated_label"},
		{"file": "lifted_0.cpp", "owner": "00401000", "evidence": "instruction_comment"}],
		"generated labels/instruction comments must identify an already-covered owner")
	result = analyze(rows, {0x00401000}, ranges, {0x00401010}, covered)
	by_target = {row["target"]: row for row in result["candidates"]}
	check(result["ghidra_executable_function_entries_missing"] == 1,
		"an unregistered Ghidra function entry must be counted")
	check(result["runtime_targets_missing_from_dispatch"] == ["0x00401010"],
		"runtime-observed target missing from dispatch must be explicit")
	check(by_target["0x00401010"]["evidence"] == ["runtime-observed", "COMPUTED_JUMP"],
		"computed jump and runtime evidence must be combined")
	check(by_target["0x00401010"]["references"][0]["parent_body_contains_target"],
		"an intra-function basic block must be distinguished from a new Ghidra function")
	check(by_target["0x00401010"]["classification"] == "intra_function_control_flow_target" and
		by_target["0x00401010"]["dispatch_guidance"] == "keep_as_parent_function_block",
		"an interior computed-jump target must not be promoted to an independent dispatch root")
	check("0x00403000" not in by_target,
		"non-executable branch targets must be rejected")
	check(by_target["0x00401100"]["evidence"] == ["ghidra-function-entry", "COMPUTED_CALL"],
		"missing direct-call destinations must retain both call-edge and entry evidence")
	check(by_target["0x00401100"]["dispatch_guidance"] ==
		"inspect_generated_owner_before_adding_dispatch_entry" and
		by_target["0x00401100"]["references"][0]["site"] == "0x0040100C",
		"a missing function entry must retain its xrefs and require validation before lifting")
	print("dispatch coverage smoke passed: function gaps, computed jumps, executable filtering, runtime evidence")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
