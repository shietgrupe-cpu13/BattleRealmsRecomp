#!/usr/bin/env python3
"""Replace one cached lifted function, then emit its diagnostic overlay."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from trace_generated_dispatch_sites import FUNCTION, instrument_source


def function_bounds(source: str, address: int) -> tuple[int, int]:
	functions = list(FUNCTION.finditer(source))
	for index, match in enumerate(functions):
		if int(match.group(1), 16) == address:
			return match.start(), functions[index + 1].start() if index + 1 < len(functions) else len(source)
	raise ValueError(f"function f_{address:08x} is missing")


def make_overlay(input_dir: Path, output_dir: Path, parent: int, target: int,
				 replacement_source: Path | None, manifest: Path) -> dict[str, object]:
	if input_dir.resolve() == output_dir.resolve():
		raise ValueError("diagnostic overlay must be separate from reusable generated source")
	replacement_text = replacement_source.read_text(encoding="utf-8") if replacement_source else None
	output_dir.mkdir(parents=True, exist_ok=True)
	for path in sorted(input_dir.glob("lifted_*.cpp")):
		base = path.read_text(encoding="utf-8")
		try:
			start, end = function_bounds(base, parent)
		except ValueError:
			continue
		replacement_hash = None
		if replacement_text is not None:
			rstart, rend = function_bounds(replacement_text, parent)
			body = replacement_text[rstart:rend].rstrip()
			base = base[:start] + body + "\n" + base[end:]
			replacement_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()
		patched, sites = instrument_source(base, parent, target)
		destination = output_dir / path.name
		destination.write_text(patched, encoding="utf-8", newline="")
		result: dict[str, object] = {
			"source": path.name,
			"overlay": str(destination),
			"parent": f"0x{parent:08X}",
			"target": f"0x{target:08X}",
			"sites": sites,
			"function_replaced": replacement_text is not None,
			"replacement_sha256": replacement_hash,
		}
		manifest.parent.mkdir(parents=True, exist_ok=True)
		manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
		return result
	raise ValueError(f"parent f_{parent:08x} not found under {input_dir}")


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--input-dir", type=Path, required=True)
	parser.add_argument("--output-dir", type=Path, required=True)
	parser.add_argument("--manifest", type=Path, required=True)
	parser.add_argument("--parent", type=lambda value: int(value, 0), default=0x006A1240)
	parser.add_argument("--target", type=lambda value: int(value, 0), default=0x006A13A0)
	parser.add_argument("--replacement-source", type=Path)
	args = parser.parse_args()
	print(json.dumps(make_overlay(args.input_dir, args.output_dir, args.parent, args.target,
							  args.replacement_source, args.manifest), indent=2))
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
