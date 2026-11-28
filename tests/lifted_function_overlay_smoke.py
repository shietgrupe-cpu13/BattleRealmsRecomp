#!/usr/bin/env python3
"""Ensure one regenerated function replaces only its cached function body."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from create_dispatch_trace_overlay import make_overlay  # noqa: E402


def check(condition: bool, message: str) -> None:
	if not condition:
		raise AssertionError(message)


def main() -> int:
	base_function = """#include \"lifted_decl.h\"\nvoid f_00401234(CPU* c)\n{\n  /* 00401234 jmp dword ptr [eax*4 + 0x00405000] */\n  { uint32_t t = G32(eax * 4 + 0x405000u);\n    SAVE(); ICALL(t); return; }\n}\nvoid f_00401250(CPU* c) {}\n"""
	regenerated = """#include \"lifted_decl.h\"\nvoid f_00401234(CPU* c)\n{\n  /* 00401234 jmp dword ptr [eax*4 + 0x00405000] */\n  { uint32_t t = G32(eax * 4 + 0x405000u);\n    switch (t) { case 0x00401240u: goto L_00401240; default: break; }\n    SAVE(); ICALL(t); return; }\nL_00401240:;\n}\nvoid f_00401250(CPU* c) {}\n"""
	with tempfile.TemporaryDirectory(prefix="br-function-overlay-") as temporary:
		root = Path(temporary)
		bulk, replacement, output, manifest = (root / name for name in
			("bulk", "replacement", "overlay", "manifest.json"))
		bulk.mkdir()
		replacement.mkdir()
		(bulk / "lifted_0.cpp").write_text(base_function, encoding="utf-8")
		(replacement / "lifted_0.cpp").write_text(regenerated, encoding="utf-8")
		result = make_overlay(bulk, output, 0x00401234, 0x00401240,
						  replacement / "lifted_0.cpp", manifest)
		body = (output / "lifted_0.cpp").read_text(encoding="utf-8")
		check(result["function_replaced"] is True, "manifest must identify the targeted function replacement")
		check(result["sites"][0]["route"] == "local-block", "the replacement local target must be recognized")
		check("L_00401240:;" in body and "void f_00401250(CPU* c) {}" in body,
			  "overlay must use new parent code and preserve neighboring cached functions")
		check((bulk / "lifted_0.cpp").read_text(encoding="utf-8") == base_function,
			  "overlay generation must not mutate cached bulk output")
	print("lifted function overlay smoke passed: targeted replacement, dispatch route, cached neighbor preservation")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
