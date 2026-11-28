#!/usr/bin/env python3
"""Check pinned RA3 lifter patch matching, behavior, and idempotence."""
from __future__ import annotations

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from patch_ra3_backward_jump_tables import NEW_SCAN, OLD_SCAN, patch_text  # noqa: E402


def check(condition: bool, message: str) -> None:
	if not condition:
		raise AssertionError(message)


def main() -> int:
	source = ("auto isEntryOther = [&](uint32_t t) { return t != entry; };\n" +
		  "prefix\n" + OLD_SCAN + "\nsuffix\n")
	patched, changed = patch_text(source)
	check(changed, "the unmodified pinned scanner should receive the backwards scan")
	check(patched.count(NEW_SCAN) == 1 and OLD_SCAN not in patched,
		  "patched source must contain exactly one replacement scanner")
	check("scanTable(tb, 1);" in patched and "scanTable(tb - 4, -1);" in patched,
		  "replacement must examine both positive and negative table indices")
	check("if (isEntryOther(v)) continue;" in patched,
		  "the original separate-function ownership rule must remain intact")
	second, changed_again = patch_text(patched)
	check(second == patched and not changed_again, "patch application must be idempotent")
	try:
		patch_text("different upstream source")
	except ValueError:
		pass
	else:
		raise AssertionError("unknown upstream source must fail closed")
	print("Backward jump-table patch smoke passed: bidirectional scan, ownership, idempotence, source guard")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
