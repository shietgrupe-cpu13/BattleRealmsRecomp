#!/usr/bin/env python3
"""Compare a Unicorn x86 oracle with one already-compiled lifted-function runner."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from unicorn_x86_oracle import run as run_oracle


def source_addresses(path: Path, address: int) -> list[int]:
	text = path.read_text(encoding="utf-8")
	start = re.search(rf"(?m)^void f_{address:08x}\(CPU\* c\)\s*\n\{{", text, re.IGNORECASE)
	if start is None:
		raise ValueError(f"lifted source has no f_{address:08x}")
	end = re.search(r"(?m)^void f_[0-9a-fA-F]{8}\(CPU\* c\)", text[start.end():])
	body = text[start.start():start.end() + end.start() if end else len(text)]
	return [int(match, 16) for match in re.findall(r"/\*\s*([0-9a-fA-F]{8})\s+", body)]


def _run_candidate(command: list[str]) -> dict[str, Any]:
	proc = subprocess.run(command, capture_output=True, text=True, check=False)
	if proc.returncode:
		raise RuntimeError(f"lifted runner exited {proc.returncode}: {proc.stderr[-2000:]}")
	lines = [line for line in proc.stdout.splitlines() if line.strip()]
	if not lines:
		raise RuntimeError("lifted runner produced no machine-readable result")
	result = json.loads(lines[-1])
	if not isinstance(result, dict):
		raise ValueError("lifted runner result must be a JSON object")
	return result


DEFAULT_CHECKS = ("registers", "flags", "memory_writes", "return_address",
	"source_instruction_sequence")
AVAILABLE_CHECKS = {"registers", "flags", "memory_writes", "return_address", "source_instruction_sequence"}


def _expand_candidate(template: list[str], case_path: Path, exe: Path,
					  case: dict[str, Any], case_id: str) -> list[str]:
	values = {"case": str(case_path), "exe": str(exe), "id": case_id}
	values.update({key: str(value) for key, value in case.items()
		if isinstance(value, (str, int, float))})
	return [token.format_map(values) for token in template]


def _compare_one(exe: Path, case: dict[str, Any], candidate: list[str],
				 lifted_source: Path, checks_requested: list[str]) -> dict[str, Any]:
	started = time.perf_counter()
	oracle = run_oracle(exe, case)
	candidate_result = _run_candidate(candidate)
	address = int(case["address"], 0)
	checks_all = {
		"registers": oracle["registers"] == candidate_result.get("registers"),
		"flags": oracle["flags"] == candidate_result.get("flags"),
		"memory_writes": oracle["memory_writes"] == candidate_result.get("memory_writes"),
		"return_address": oracle["eip_after_return"] == candidate_result.get("eip_after_return"),
		"source_instruction_sequence": oracle["instruction_trace"] == source_addresses(lifted_source, address),
	}
	unknown = set(checks_requested) - AVAILABLE_CHECKS
	if unknown:
		raise ValueError("unknown differential checks: " + ", ".join(sorted(unknown)))
	checks = {name: checks_all[name] for name in checks_requested}
	return {"target": f"0x{address:08X}", "result": "PASS" if all(checks.values()) else "FAIL",
		"elapsed_seconds": round(time.perf_counter() - started, 6), "checks": checks,
		"oracle": oracle, "lifted": candidate_result}


def compare(exe: Path, case_file: Path | None, cases_file: Path | None,
			candidate: list[str], lifted_source: Path) -> dict[str, Any]:
	if bool(case_file) == bool(cases_file):
		raise ValueError("provide exactly one of --case or --cases")
	if case_file:
		items = [{"id": case_file.stem, "case": json.loads(case_file.read_text(encoding="utf-8"))}]
	else:
		document = json.loads(cases_file.read_text(encoding="utf-8"))
		items = document.get("cases") if isinstance(document, dict) else document
		if not isinstance(items, list) or not items:
			raise ValueError("--cases must contain a nonempty JSON array or {\"cases\": [...]}")
		if len(items) > 1:
			placeholders = {"case", "id"}
			for item in items:
				case_values = item.get("case", item) if isinstance(item, dict) else {}
				placeholders.update(key for key, value in case_values.items()
					if isinstance(value, (str, int, float)))
			if not any("{" + name + "}" in token for token in candidate for name in placeholders):
				raise ValueError("multi-case suites require a case-specific candidate placeholder such as {case} or {size}")
	started = time.perf_counter()
	rows = []
	with tempfile.TemporaryDirectory(prefix="br-unicorn-diff-") as temporary:
		for index, item in enumerate(items):
			case = item.get("case", item)
			case_id = str(item.get("id", f"case-{index + 1}"))
			requested = item.get("compare", list(DEFAULT_CHECKS))
			if not isinstance(requested, list) or not requested:
				raise ValueError(f"{case_id}: compare must be a nonempty array")
			case_path = Path(temporary) / f"case-{index:04d}.json"
			case_path.write_text(json.dumps(case), encoding="utf-8")
			row = _compare_one(exe, case, _expand_candidate(candidate, case_path, exe, case, case_id),
				lifted_source, requested)
			row["id"] = case_id
			rows.append(row)
	return {"schema": 2, "suite": cases_file.stem if cases_file else case_file.stem,
		"result": "PASS" if all(row["result"] == "PASS" for row in rows) else "FAIL",
		"elapsed_seconds": round(time.perf_counter() - started, 6), "case_count": len(rows), "cases": rows}


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--exe", type=Path, required=True)
	group = parser.add_mutually_exclusive_group(required=True)
	group.add_argument("--case", type=Path, help="one JSON oracle case")
	group.add_argument("--cases", type=Path, help="JSON array/object describing a differential regression suite")
	parser.add_argument("--candidate", nargs="+", required=True,
			help="compiled lifted-function runner; its final stdout line must be JSON")
	parser.add_argument("--lifted-source", type=Path, required=True)
	parser.add_argument("--output", type=Path)
	args = parser.parse_args()
	result = compare(args.exe, args.case, args.cases, args.candidate, args.lifted_source)
	text = json.dumps(result, indent=2) + "\n"
	if args.output:
		args.output.parent.mkdir(parents=True, exist_ok=True)
		args.output.write_text(text, encoding="utf-8")
	for row in result["cases"]:
		print(f"{row['result']} {row['id']} {row['target']} elapsed={row['elapsed_seconds']:.6f}s "
			f"checks={','.join(key for key, passed in row['checks'].items() if passed)}")
	print(f"{result['result']} cases={result['case_count']} elapsed={result['elapsed_seconds']:.6f}s")
	if result["result"] != "PASS":
		for row in result["cases"]:
			failed = [key for key, passed in row["checks"].items() if not passed]
			if failed:
				print(f"{row['id']} failed checks: {', '.join(failed)}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
