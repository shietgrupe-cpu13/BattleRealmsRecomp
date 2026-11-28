#!/usr/bin/env python3
"""Differentially exercise both indirect jump tables in the lifted memmove routine."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
from pathlib import Path
from typing import Any

from unicorn_x86_oracle import run as run_oracle


ARGUMENT_ADDRESS = 0x200C
# The aligned REP MOVSD path reaches 0x006A1275. An unaligned destination and
# a count below four underflow ECX at 0x006A1283 and reach the backward-indexed
# table at 0x006A1294. These are original-executable branch destinations.
SCENARIOS = (
	(0, 0x1101, 0x006A1294, 0x006A1398),
	(1, 0x1101, 0x006A1294, 0x006A13A0),
	(2, 0x1101, 0x006A1294, 0x006A13AC),
	(3, 0x1101, 0x006A1294, 0x006A13C0),
	(32, 0x1100, 0x006A1275, 0x006A1398),
	(33, 0x1100, 0x006A1275, 0x006A13A0),
	(34, 0x1100, 0x006A1275, 0x006A13AC),
	(35, 0x1100, 0x006A1275, 0x006A13C0),
)


def make_parent_case() -> dict[str, Any]:
	return {
		"address": "0x006A1240", "stop_address": "0x12345678",
		"registers": {"eax": "0xFEEDFACE", "ecx": "0x11111111", "edx": "0x22222222",
			"ebx": "0x33333333", "esp": "0x00002000", "ebp": "0x44444444",
			"esi": "0x87654321", "edi": "0x76543210"},
		"flags": {"cf": 1, "pf": 1, "af": 1, "zf": 1, "sf": 1, "of": 1, "df": 0},
		"memory": [
			{"address": "0x00001000", "hex": bytes((i * 7 + 19) & 0xff for i in range(64)).hex()},
			{"address": "0x00001100", "hex": bytes([0xcc] * 64).hex()},
			{"address": "0x00001FF0", "hex": "0000000000000000000000000000000078563412001100000010000020000000"},
		]
	}


def set_guest_u32(case: dict[str, Any], address: int, value: int) -> None:
	for region in case["memory"]:
		start = int(region["address"], 0)
		data = bytearray.fromhex(region["hex"])
		if start <= address and address + 4 <= start + len(data):
			offset = address - start
			data[offset:offset + 4] = value.to_bytes(4, "little")
			region["hex"] = data.hex()
			return
	raise ValueError(f"argument address 0x{address:08X} is outside mapped test memory")


def _run_lifted(executable: Path, pe_path: Path, size: int, destination: int) -> dict[str, Any]:
	proc = subprocess.run([str(executable), str(pe_path), str(size), hex(destination)],
					  capture_output=True, text=True, check=False)
	if proc.returncode:
		raise RuntimeError(f"lifted runner failed for size {size} with exit {proc.returncode}: {proc.stderr[-2000:]}")
	lines = [line for line in proc.stdout.splitlines() if line.strip()]
	if not lines:
		raise RuntimeError(f"lifted runner emitted no result for size {size}")
	return json.loads(lines[-1])


def compare(exe: Path, lifted_runner: Path,
			lifted_source: Path) -> dict[str, Any]:
	started = time.perf_counter()
	source = lifted_source.read_text(encoding="utf-8")
	def require_local_case(site: int, target: int) -> None:
		marker = f"/* {site:08x} jmp"
		start = source.lower().find(marker.lower())
		if start < 0:
			raise ValueError(f"generated parent is missing indirect jump at 0x{site:08X}")
		end = source.find("SAVE(); ICALL(t); return;", start)
		if end < 0:
			raise ValueError(f"generated indirect jump at 0x{site:08X} has no dispatch block")
		block = source[start:end]
		if not re.search(rf"case\s+0x{target:08x}u:\s*goto L_{target:08x};", block, re.IGNORECASE):
			raise ValueError(f"0x{site:08X} has no local case for expected target 0x{target:08X}")
	rows = []
	for size, destination, expected_site, expected_target in SCENARIOS:
		require_local_case(expected_site, expected_target)
		case = make_parent_case()
		set_guest_u32(case, ARGUMENT_ADDRESS, size)
		set_guest_u32(case, 0x2004, destination)
		oracle = run_oracle(exe, case)
		candidate = _run_lifted(lifted_runner, exe, size, destination)
		observed = [edge["target"] for edge in oracle["branch_targets"] if edge["site"] == expected_site]
		memory_match = sorted(oracle["memory_writes"], key=lambda row: row["address"]) == sorted(
			candidate.get("memory_writes", []), key=lambda row: row["address"])
		checks = {
			"expected_original_branch": observed == [expected_target],
			"no_guest_dispatch": candidate.get("guest_dispatch_target", 0) == 0,
			"registers": oracle["registers"] == candidate.get("registers"),
			# The carry flag controls the JB at 0x006A1286 for the backward table.
			# Other arithmetic flags are volatile across this helper's RET and are
			# not used by either selected table transfer.
			"carry_flag": oracle["flags"]["cf"] == candidate.get("flags", {}).get("cf"),
			"memory_writes": memory_match,
			"return_address": oracle["eip_after_return"] == candidate.get("eip_after_return"),
		}
		rows.append({
			"size": size,
			"destination": f"0x{destination:08X}",
			"branch_site": f"0x{expected_site:08X}",
			"expected_table_target": f"0x{expected_target:08X}",
			"observed_table_targets": [f"0x{target:08X}" for target in observed],
			"result": "PASS" if all(checks.values()) else "FAIL",
			"checks": checks,
			"flags": {"unicorn": oracle["flags"], "lifted": candidate.get("flags"),
				"compared": ["cf"], "reason": "CF controls the original JB at 0x006A1286; other flags are not used by these return paths"},
			"oracle_trace_count": len(oracle["instruction_trace"]),
			"oracle": oracle,
			"lifted": candidate,
		})
	return {
		"schema": 1,
		"function": "0x006A1240",
		"method": "Original PE and extracted generated f_006a1240 executed with equal stack/register/memory inputs; asserts table branch, dispatch route, registers, flags, writes and return",
		"result": "PASS" if all(row["result"] == "PASS" for row in rows) else "FAIL",
		"elapsed_seconds": round(time.perf_counter() - started, 6),
		"scenarios": rows,
	}


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--exe", type=Path, required=True)
	parser.add_argument("--lifted-runner", type=Path, required=True)
	parser.add_argument("--lifted-source", type=Path, required=True)
	parser.add_argument("--output", type=Path, required=True)
	args = parser.parse_args()
	result = compare(args.exe, args.lifted_runner, args.lifted_source)
	args.output.parent.mkdir(parents=True, exist_ok=True)
	args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
	for row in result["scenarios"]:
		print(f"{row['result']} n={row['size']} site={row['branch_site']} target={row['observed_table_targets']} "
			f"dispatch={row['checks']['no_guest_dispatch']} regs={row['checks']['registers']} "
			f"cf={row['checks']['carry_flag']} memory={row['checks']['memory_writes']} "
			f"return={row['checks']['return_address']}")
	print(f"{result['result']} scenarios={len(result['scenarios'])} elapsed={result['elapsed_seconds']:.6f}s "
		f"output={args.output}")
	return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
	raise SystemExit(main())
