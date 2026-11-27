#!/usr/bin/env python3
"""Extract an exact deterministic function slice from generated lift output for host tests."""
from __future__ import annotations

import argparse
import os
import re
import tempfile
from pathlib import Path


FUNCTION = re.compile(r"(?m)^void f_([0-9a-fA-F]{8})\(CPU\* c\)\s*\n\{")


def extract(main_dir: Path, addresses: list[int]) -> tuple[str, int]:
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
			functions[address] = text[match.start():end].rstrip()
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
	args = parser.parse_args()
	content, count = extract(args.main_dir, args.address)
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
