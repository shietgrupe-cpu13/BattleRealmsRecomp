#!/usr/bin/env python3
"""Extract an exact deterministic function slice from generated lift output for host tests."""
from __future__ import annotations

import argparse
import os
import re
import tempfile
from pathlib import Path


FUNCTION = re.compile(r"(?m)^void f_([0-9a-fA-F]{8})\(CPU\* c\)\s*\n\{")


def capture_return_flags(function: str) -> str:
	"""Expose the lifter's local flag state at RET for differential tests only."""
	ret_comment = re.compile(r"/\*\s*[0-9a-fA-F]{8}\s+ret(?:\s|\*/)")
	result: list[str] = []
	position = 0
	for match in ret_comment.finditer(function):
		result.append(function[position:match.start()])
		end = function.find("\n\t}", match.end())
		if end < 0:
			end = len(function)
		else:
			end += len("\n\t}")
		block = function[match.start():end]
		block, count = re.subn(r"\bSAVE\(\);\s*return;", "SAVEFL(); SAVE(); return;", block, count=1)
		if count != 1:
			raise ValueError(f"RET block has no generated SAVE/return sequence: {match.group(0)}")
		result.append(block)
		position = end
	result.append(function[position:])
	return "".join(result)


def extract(main_dir: Path, addresses: list[int], *, with_return_flags: bool = False) -> tuple[str, int]:
	functions: dict[int, str] = {}
	preambles: list[str] = []
	for source in sorted(main_dir.glob("lifted_*.cpp"), key=lambda path: path.name):
		text = source.read_text(encoding="utf-8", errors="replace")
		matches = list(FUNCTION.finditer(text))
		if not matches:
			continue
		preamble = text[:matches[0].start()]
		if not preambles:
			preambles.append(preamble.rstrip())
		for index, match in enumerate(matches):
			address = int(match.group(1), 16)
			if address not in addresses:
				continue
			end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
			if address in functions:
				raise ValueError(f"duplicate generated function f_{address:08x}")
			function = text[match.start():end].rstrip()
			functions[address] = capture_return_flags(function) if with_return_flags else function
	missing = sorted(set(addresses) - set(functions))
	if missing:
		raise ValueError("generated functions absent: " + ", ".join(f"0x{address:08X}" for address in missing))
	content = "\n\n".join([*preambles, *(functions[address] for address in sorted(set(addresses)))]) + "\n"
	return content, len(functions)


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--main-dir", type=Path, required=True)
	parser.add_argument("--address", type=lambda value: int(value, 0), action="append", required=True)
	parser.add_argument("--output", type=Path, required=True)
	parser.add_argument("--capture-return-flags", action="store_true",
					 help="save local x86 flags at RET for differential tests; test output only")
	args = parser.parse_args()
	content, count = extract(args.main_dir, args.address,
						 with_return_flags=args.capture_return_flags)
	args.output.parent.mkdir(parents=True, exist_ok=True)
	fd, temporary = tempfile.mkstemp(prefix=args.output.name + ".", suffix=".tmp", dir=args.output.parent)
	try:
		with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
			stream.write(content)
		os.replace(temporary, args.output)
	finally:
		if os.path.exists(temporary):
			os.unlink(temporary)
	print(f"extracted_functions={count} bytes={len(content.encode('utf-8'))} output={args.output}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
