#!/usr/bin/env python3
"""Patch the pinned RA3 lifter to discover bounded backward-indexed jump tables.

The upstream lifter scans only forward from a memory jump's displacement. Some
MSVC switch tables use a displacement at the first *code* address after the
table and negative indices (for example, Battle Realms 0x006A1294). This patch
adds a symmetric backward scan while retaining the upstream code-target and
function-entry checks.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re


OLD_SCAN = """else if (o.type == X86_OP_MEM && o.mem.index != X86_REG_INVALID && o.mem.disp)
				{
					uint32_t tb = (uint32_t)o.mem.disp;
					for (uint32_t k = 0; k < 2048; k++)
					{
						uint32_t v;
						if (!r.read(tb + k * 4, (uint8_t*)&v, 4) || !r.isCode(v)) break;
						if (isEntryOther(v)) continue;   // a case that is another function's entry: reached via the default tail call
						in.succ.push_back(v);
						work.push_back(v);
					}
				}"""

NEW_SCAN = """else if (o.type == X86_OP_MEM && o.mem.index != X86_REG_INVALID && o.mem.disp)
				{
					const int64_t tb = (int64_t)(uint32_t)o.mem.disp;
					auto scanTable = [&](int64_t first, int64_t step)
					{
						for (uint32_t k = 0; k < 2048; k++)
						{
							const int64_t slot = first + (int64_t)k * step * 4;
							if (slot < 0 || slot > 0xffffffffLL) break;
							uint32_t v;
							if (!r.read((uint32_t)slot, (uint8_t*)&v, 4) || !r.isCode(v)) break;
							if (isEntryOther(v)) continue;   // a case that is another function's entry: reached via the default tail call
							in.succ.push_back(v);
							work.push_back(v);
						}
					};
					scanTable(tb, 1);
					scanTable(tb - 4, -1);
				}"""


def patch_text(source: str) -> tuple[str, bool]:
	if NEW_SCAN in source:
		return source, False
	if source.count(OLD_SCAN) != 1:
		raise ValueError("expected exactly one unmodified indexed-jump scanner in pinned RA3 tools/lift.cpp")
	if "auto isEntryOther =" not in source or "in.succ.push_back(v);" not in OLD_SCAN:
		raise ValueError("pinned RA3 lifter source does not match the expected control-flow analysis")
	return source.replace(OLD_SCAN, NEW_SCAN, 1), True


def patch_file(path: Path) -> bool:
	source = path.read_text(encoding="utf-8")
	patched, changed = patch_text(source)
	if changed:
		path.write_text(patched, encoding="utf-8", newline="\n")
	return changed


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("source", type=Path, help="RA3-Native tools/lift.cpp from the pinned checkout")
	args = parser.parse_args()
	if not args.source.is_file():
		parser.error(f"lifter source does not exist: {args.source}")
	changed = patch_file(args.source)
	print("patched backward-indexed jump-table discovery" if changed else "backward-indexed jump-table patch already present")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
