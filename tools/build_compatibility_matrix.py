#!/usr/bin/env python3
"""Build a conservative import + dynamic COM compatibility inventory.

The report separates name registration from exercised behavior. A handler is
never considered supported merely because a vtable slot or import is bound.
Input executables default to the E: reference copy so the original D: tree is
not needed by this audit.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path

import pefile


ROOT = Path(__file__).resolve().parents[1]
HANDLER_RE = re.compile(
	r'handlers\.registerName\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([A-Za-z_]\w*)\s*,\s*(\d+)\s*\)'
)
ORDINAL_RE = re.compile(
	r'handlers\.registerOrdinal\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([A-Za-z_]\w*)\s*,\s*(\d+)\s*\)'
)
VTABLE_RE = re.compile(
	r'constexpr\s+std::array<const char\*,\s*(\d+)>\s+(k\w+Methods)\s*=\s*\{\{(.*?)\}\};',
	re.DOTALL,
)
STRING_RE = re.compile(r'"([A-Za-z_][A-Za-z0-9_]*)"')
COM_ARRAYS = {
	"kDdraw7Methods": ("ddraw.dll", "IDirectDraw7"),
	"kD3d7Methods": ("ddraw.dll", "IDirect3D7"),
	"kDdrawSurfaceMethods": ("ddraw.dll", "IDirectDrawSurface7"),
	"kD3d7DeviceMethods": ("ddraw.dll", "IDirect3DDevice7"),
	"kD3d7VertexBufferMethods": ("ddraw.dll", "IDirect3DVertexBuffer7"),
	"kDirectInput7Methods": ("dinput.dll", "IDirectInput7A"),
	"kDirectInputDevice7Methods": ("dinput.dll", "IDirectInputDevice7A"),
	"kDirectSoundMethods": ("dsound.dll", "IDirectSound"),
	"kDirectSoundBufferMethods": ("dsound.dll", "IDirectSoundBuffer"),
	"kDirectSound3DListenerMethods": ("dsound.dll", "IDirectSound3DListener"),
}
DYNAMIC_EXPORTS = {
	"ddraw.dll": ("DirectDrawCreateEx", "DirectDrawEnumerateExA"),
	"dinput.dll": ("DirectInputCreateEx",),
	"dsound.dll": ("DirectSoundCreate",),
}
SUBSYSTEM = {
	"kernel32.dll": "KERNEL32 / CRT",
	"user32.dll": "USER32 / window and input",
	"gdi32.dll": "GDI",
	"ddraw.dll": "DirectDraw7 / Direct3D7",
	"dinput.dll": "DirectInput",
	"dsound.dll": "DirectSound",
	"winmm.dll": "WinMM / timers and audio",
	"msacm32.dll": "MSACM audio conversion",
	"imm32.dll": "IME / text input",
	"advapi32.dll": "Registry / security",
	"wsock32.dll": "Winsock 1.1",
	"ws2_32.dll": "Winsock 2",
	"rpcrt4.dll": "RPC / UUID",
	"shell32.dll": "Shell integration",
}


def canon(dll: str, name: str) -> tuple[str, str]:
	return dll.casefold(), name.casefold()


def parse_imports(path: Path) -> tuple[list[dict], Counter[str]]:
	pe = pefile.PE(str(path), fast_load=False)
	rows: list[dict] = []
	references: Counter[str] = Counter()
	for descriptor in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
		dll = descriptor.dll.decode("ascii", errors="replace").casefold()
		for item in descriptor.imports:
			name = (item.name.decode("ascii", errors="replace") if item.name
				else f"#{item.ordinal}")
			rows.append({"dll": dll, "name": name, "iat_rva": int(item.address - pe.OPTIONAL_HEADER.ImageBase),
				"ordinal": int(item.ordinal) if not item.name else None})
			references[dll] += 1
	return rows, references


def read_handlers(runtime: Path) -> dict[tuple[str, str], dict]:
	result: dict[tuple[str, str], dict] = {}
	for source in sorted(runtime.glob("win32_*.cpp")):
		text = source.read_text(encoding="utf-8", errors="replace")
		for match in HANDLER_RE.finditer(text):
			dll, name, function, stack_bytes = match.groups()
			line = text.count("\n", 0, match.start()) + 1
			result[canon(dll, name)] = {
				"file": source.relative_to(ROOT).as_posix(), "line": line,
				"handler": function, "stdcall_argument_bytes": int(stack_bytes),
			}
		for match in ORDINAL_RE.finditer(text):
			dll, ordinal, function, stack_bytes = match.groups()
			line = text.count("\n", 0, match.start()) + 1
			result[canon(dll, f"#{ordinal}")] = {
				"file": source.relative_to(ROOT).as_posix(), "line": line,
				"handler": function, "stdcall_argument_bytes": int(stack_bytes),
			}
	return result


def parse_com_methods(runtime: Path) -> list[dict]:
	methods: list[dict] = []
	for source in sorted(runtime.glob("win32_*.cpp")):
		text = source.read_text(encoding="utf-8", errors="replace")
		for match in VTABLE_RE.finditer(text):
			array_name = match.group(2)
			if array_name not in COM_ARRAYS:
				continue
			dll, interface = COM_ARRAYS[array_name]
			names = STRING_RE.findall(match.group(3))
			if len(names) != int(match.group(1)):
				raise ValueError(f"{source}:{array_name}: declared {match.group(1)} slots, parsed {len(names)}")
			for index, name in enumerate(names):
				methods.append({"dll": dll, "name": f"{interface}::{name}", "interface": interface,
					"vtable_slot": index, "vtable_source": source.relative_to(ROOT).as_posix()})
	return methods


def test_sources(tools: Path) -> dict[tuple[str, str], list[str]]:
	"""Find smoke tests that bind an API name to a stub that they dispatch.

	The old proximity heuristic missed real tests when stub declarations and
	call_target sites were separated by large test sections. Keep this
	conservative: a source counts only when it binds a literal API name to a
	variable and that same variable is passed to call_target.
	"""
	out: dict[tuple[str, str], list[str]] = {}
	static_stub = re.compile(
		r'\b(?:const\s+)?(?:uint32_t|auto)\s+(\w+)\s*=\s*iatStubFor\(\s*"([^"]+)"\s*,\s*"([^"]+)"'
	)
	kernel_stub = re.compile(
		r'\b(?:const\s+)?(?:uint32_t|auto)\s+(\w+)\s*=\s*iatStub\(\s*"([^"]+)"'
	)
	dynamic_stub = re.compile(
		r'\b(?:const\s+)?(?:uint32_t|auto)\s+(\w+)\s*=\s*[^;\n]*?getOrCreateDynamicStub\(\s*"([^"]+)"\s*,\s*"([^"]+)"'
	)
	# A few focused smoke sections use a local dynamic-stub helper and a
	# stdcall wrapper so they can share stack setup across COM methods. Count
	# those only when the same named stub is passed to that wrapper.
	dsound_helper_stub = re.compile(
		r'\b(?:const\s+)?(?:uint32_t|auto)\s+(\w+)\s*=\s*soundDynamicStub\(\s*"([^"]+)"'
	)
	for path in sorted(tools.glob("**/*smoke*")):
		if not path.is_file() or path.suffix not in (".cpp", ".py"):
			continue
		text = path.read_text(encoding="utf-8", errors="replace")
		bindings: list[tuple[str, str, str]] = []
		for match in static_stub.finditer(text):
			variable, dll, name = match.groups()
			bindings.append((variable, dll, name))
		for match in kernel_stub.finditer(text):
			variable, name = match.groups()
			bindings.append((variable, "kernel32.dll", name))
		for match in dynamic_stub.finditer(text):
			variable, dll, name = match.groups()
			bindings.append((variable, dll, name))
		for variable, dll, name in bindings:
			dispatch = re.compile(r'\bcall_target\s*\([^;]*\b' + re.escape(variable) + r'\b')
			wrapped_dispatch = re.compile(r'\bsoundDispatch\s*\(\s*' + re.escape(variable) + r'\b')
			if dispatch.search(text) or wrapped_dispatch.search(text):
				key = canon(dll, name)
				out.setdefault(key, []).append(path.relative_to(ROOT).as_posix())
		for match in dsound_helper_stub.finditer(text):
			variable, name = match.groups()
			if re.search(r'\bsoundDispatch\s*\(\s*' + re.escape(variable) + r'\b', text):
				key = canon("dsound.dll", name)
				out.setdefault(key, []).append(path.relative_to(ROOT).as_posix())
	return out


def reachability_rows() -> dict[tuple[str, str], dict]:
	path = ROOT / "analysis" / "startup_reachability_current.json"
	if not path.exists():
		path = ROOT / "analysis" / "startup_reachability.json"
	if not path.exists():
		return {}
	data = json.loads(path.read_text(encoding="utf-8"))
	result = {}
	for row in data.get("reachable_static_imports", []):
		api = row.get("api", "")
		if "!" not in api:
			continue
		dll, name = api.split("!", 1)
		result[canon(dll, name)] = row
	return result


def classify(row: dict, handler: dict | None, tests: list[str], evidence: dict) -> str:
	key = f"{row['dll']}!{row['name']}"
	if key in evidence.get("verified", []):
		return "verified"
	if not handler:
		return "missing"
	if "unsupported" in handler["handler"].casefold():
		return "partial"
	return "tested" if tests else "partial"


def priority(row: dict, scope: str) -> str:
	lib = row["dll"]
	if lib in ("wsock32.dll", "ws2_32.dll"):
		return "multiplayer / network; not a single-player startup prerequisite"
	if scope == "scenario-platform-import":
		return "campaign / skirmish scenario startup; confirm by scenario call trace"
	return "startup / menu / skirmish / gameplay path; use reachability and runtime traces to narrow"


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--exe", type=Path, default=ROOT / "work" / "windows_reference_copy" / "Battle_Realms_F.exe")
	parser.add_argument("--scripts-dir", type=Path, default=ROOT / "work" / "windows_reference_copy" / "Scripts")
	parser.add_argument("--runtime", type=Path, default=ROOT / "runtime")
	parser.add_argument("--output", type=Path, default=ROOT / "analysis" / "compatibility_matrix")
	args = parser.parse_args()
	imports, ref_counts = parse_imports(args.exe)
	handlers = read_handlers(args.runtime)
	methods = parse_com_methods(args.runtime)
	tests = test_sources(ROOT / "tools")
	reachability = reachability_rows()
	evidence_path = ROOT / "analysis" / "compat_verified_runtime.json"
	evidence = json.loads(evidence_path.read_text(encoding="utf-8")) if evidence_path.exists() else {}

	rows: list[dict] = []
	seen_imports: set[tuple[str, str]] = set()
	for item in imports:
		key = canon(item["dll"], item["name"])
		if key in seen_imports:
			continue
		seen_imports.add(key)
		implementation = handlers.get(key)
		test_refs = tests.get(key, [])
		call_evidence = reachability.get(key, {})
		call_path = call_evidence.get("sample_call_path", [])
		rows.append({
			"api": f"{item['dll']}!{item['name']}", "name": item["name"], "dll": item["dll"],
			"subsystem": SUBSYSTEM.get(item["dll"], "Other imported Windows API"),
			"scope": "main-pe-import", "guest_callers": call_path[-1:] if call_path else [],
			"sample_guest_call_path": call_path,
			"call_frequency": call_evidence.get("reachable_call_sites"),
			"call_frequency_kind": "static reachable call sites" if call_evidence else None,
			"implementation": f"{implementation['file']}:{implementation['line']}" if implementation else None,
			"handler": implementation["handler"] if implementation else None,
			"stdcall_argument_bytes": implementation["stdcall_argument_bytes"] if implementation else None,
			"status": classify(item, implementation, test_refs, evidence),
			"windows_semantics": "Name binding alone is not semantic support; preserve the API's documented validation, output, return/error, encoding, and calling-convention behavior.",
			"required_tests": ["documented success and failure cases", "guest pointer/output validation", "x86 stack cleanup", "LastError/HRESULT where applicable"],
			"test_sources": sorted(set(test_refs)), "priority": priority(item, "main-pe-import"),
			"iat_rva": f"0x{item['iat_rva']:08x}", "ordinal": item["ordinal"],
		})

	# Scenario script DLLs are read from the E: reference copy. Their imports are
	# separately tagged because they are not necessarily loaded during startup.
	scenario_pairs: set[tuple[str, str]] = set()
	script_files = sorted(args.scripts_dir.glob("*.dll"), key=lambda p: p.name.casefold()) if args.scripts_dir.is_dir() else []
	parse_errors = []
	for path in script_files:
		try:
			for item in parse_imports(path)[0]:
				key = canon(item["dll"], item["name"])
				if item["dll"] == "battle_realms_f.exe" or key in scenario_pairs:
					continue
				scenario_pairs.add(key)
				implementation = handlers.get(key)
				test_refs = tests.get(key, [])
				rows.append({
					"api": f"{item['dll']}!{item['name']}", "name": item["name"], "dll": item["dll"],
					"subsystem": SUBSYSTEM.get(item["dll"], "Other imported Windows API"),
					"scope": "scenario-platform-import", "guest_callers": [], "call_frequency": None,
					"implementation": f"{implementation['file']}:{implementation['line']}" if implementation else None,
					"handler": implementation["handler"] if implementation else None,
					"stdcall_argument_bytes": implementation["stdcall_argument_bytes"] if implementation else None,
					"status": classify(item, implementation, test_refs, evidence),
					"windows_semantics": "Scenario import; exact dynamic load/call behavior is not proven by import presence alone.",
					"required_tests": ["load representative scenario DLL", "resolve imports/guest exports", "validate callback ABI and lifetime"],
					"test_sources": sorted(set(test_refs)), "priority": priority(item, "scenario-platform-import"),
					"iat_rva": None, "ordinal": item["ordinal"],
				})
		except Exception as exc:
			parse_errors.append({"file": path.name, "error": str(exc)})

	for dll, names in DYNAMIC_EXPORTS.items():
		for name in names:
			key = canon(dll, name)
			implementation = handlers.get(key)
			test_refs = tests.get(key, [])
			rows.append({
				"api": f"{dll}!{name}", "name": name, "dll": dll,
				"subsystem": SUBSYSTEM.get(dll, "Dynamically resolved Windows API"),
				"scope": "dynamic-getprocaddress-export", "guest_callers": [], "call_frequency": None,
				"implementation": f"{implementation['file']}:{implementation['line']}" if implementation else None,
				"handler": implementation["handler"] if implementation else None,
				"stdcall_argument_bytes": implementation["stdcall_argument_bytes"] if implementation else None,
				"status": classify({"dll": dll, "name": name}, implementation, test_refs, evidence),
				"windows_semantics": "Dynamically resolved export verified in GetProcAddress implementation; match module lifetime, export name and failure error.",
				"required_tests": ["module load/GetProcAddress success", "unknown export failure and LastError", "stdcall ABI", "guest callback/output behavior"],
				"test_sources": sorted(set(test_refs)), "priority": "startup or subsystem-specific dynamic initialization",
				"iat_rva": None, "ordinal": None,
			})

	for method in methods:
		key = canon(method["dll"], method["name"])
		implementation = handlers.get(key)
		test_refs = tests.get(key, [])
		rows.append({
			"api": f"{method['dll']}!{method['name']}", "name": method["name"].split("::")[-1], "dll": method["dll"],
			"subsystem": SUBSYSTEM.get(method["dll"], "COM interface"), "scope": "dynamic-com-vtable",
			"interface": method["interface"], "vtable_slot": method["vtable_slot"],
			"guest_callers": [], "call_frequency": None,
			"implementation": f"{implementation['file']}:{implementation['line']}" if implementation else None,
			"handler": implementation["handler"] if implementation else None,
			"stdcall_argument_bytes": implementation["stdcall_argument_bytes"] if implementation else None,
			"status": classify(method, implementation, test_refs, evidence),
			"windows_semantics": f"COM {method['interface']} vtable slot {method['vtable_slot']}; validate HRESULT, guest pointers, state transitions, reference ownership and stdcall ABI.",
			"required_tests": ["vtable slot maps to method", "valid/invalid guest pointers", "COM reference-count lifetime", "state transition and HRESULT", "stack cleanup"],
			"test_sources": sorted(set(test_refs)), "priority": "graphics / input / audio path as exercised; slot presence is not implementation",
			"iat_rva": None, "ordinal": None,
		})

	rows.sort(key=lambda r: (r["scope"], r["subsystem"].casefold(), r["api"].casefold()))
	status_counts = Counter(row["status"] for row in rows)
	scope_counts = Counter(row["scope"] for row in rows)
	unique_main = len(seen_imports)
	result = {
		"schema": 1,
		"generated_from": {"exe": str(args.exe), "runtime": str(args.runtime), "scenario_dir": str(args.scripts_dir)},
		"summary": {
			"main_import_references_including_ordinals": len(imports), "main_unique_import_functions": unique_main,
			"main_import_references_by_dll": dict(sorted(ref_counts.items())),
			"scenario_dll_candidates": len(script_files), "scenario_unique_platform_import_pairs": len(scenario_pairs),
			"combined_unique_main_and_scenario_import_pairs": len(seen_imports | scenario_pairs),
			"dynamic_com_vtable_methods": len(methods), "dynamic_com_methods_with_handlers": sum(bool(handlers.get(canon(m['dll'], m['name']))) for m in methods),
			"status_counts": dict(sorted(status_counts.items())), "scope_counts": dict(sorted(scope_counts.items())),
			"scenario_parse_errors": parse_errors,
		},
		"interpretation": "Conservative static inventory. `tested` means a host smoke source explicitly dispatches the API and the current CTest suite passed; `verified` requires an explicit entry in analysis/compat_verified_runtime.json. Caller samples and call_frequency are from static direct-call BFS, not dynamic execution frequency. Runtime call frequency is not currently collected.",
		"entries": rows,
	}
	args.output.parent.mkdir(parents=True, exist_ok=True)
	json_path = args.output.with_suffix(".json")
	csv_path = args.output.with_suffix(".csv")
	json_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
	with csv_path.open("w", encoding="utf-8-sig", newline="") as stream:
		writer = csv.DictWriter(stream, fieldnames=["api", "scope", "subsystem", "interface", "vtable_slot",
			"implementation", "handler", "stdcall_argument_bytes", "status", "guest_callers", "call_frequency",
			"required_tests", "priority"])
		writer.writeheader()
		for row in rows:
			writer.writerow({key: json.dumps(row.get(key), ensure_ascii=False) if isinstance(row.get(key), (list, dict)) else row.get(key)
				for key in writer.fieldnames})
	print(json.dumps(result["summary"], indent=2))
	print(f"wrote {json_path} and {csv_path}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
