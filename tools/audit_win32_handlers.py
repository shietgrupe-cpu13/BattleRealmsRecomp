#!/usr/bin/env python3
"""Compare registered Win32 import handlers with Battle Realms PE imports.

The main executable import list is parsed from analysis/pe_report.txt. Scenario
DLL imports are parsed directly from the read-only game Scripts directory. The
script reports names and import scope; it does not claim that a matching name
implements complete Windows semantics.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PE_REPORT = ROOT / "analysis" / "pe_report.txt"
HANDLER_SOURCES = sorted((ROOT / "runtime").glob("win32_*.cpp"))
OUTPUT = ROOT / "analysis" / "win32_handler_coverage.json"
DEFAULT_SCRIPTS = Path(r"D:\Games\Battle Realms Complete\Scripts")

IMPORT_LINE = re.compile(r"^([^\s]+\.dll)\s+(\d+)\s+(.*)$", re.IGNORECASE)
REGISTRATION = re.compile(
	r'handlers\.registerName\(\s*"([^"]+)"\s*,\s*"([^"]+)"'
)
ORDINAL_REGISTRATION = re.compile(
	r'handlers\.registerOrdinal\(\s*"([^"]+)"\s*,\s*(\d+)'
)


def display_pair(pair: tuple[str, str]) -> str:
	return f"{pair[0]}!{pair[1]}"


def read_main_imports() -> set[tuple[str, str]]:
	imports: set[tuple[str, str]] = set()
	for line in PE_REPORT.read_text(encoding="utf-8").splitlines():
		match = IMPORT_LINE.match(line.strip())
		if not match:
			continue
		dll, _declared_count, names = match.groups()
		imports.update((dll.lower(), name.strip()) for name in names.split(",") if name.strip())
	return imports


def read_script_imports(scripts_dir: Path) -> tuple[
	set[tuple[str, str]], dict[str, set[tuple[str, str]]], list[dict[str, str]], int, Counter[str]
]:
	try:
		import pefile
	except ImportError as exc:
		raise RuntimeError("Python package 'pefile' is required to audit scenario DLL imports") from exc

	union: set[tuple[str, str]] = set()
	by_module: dict[str, set[tuple[str, str]]] = {}
	errors: list[dict[str, str]] = []
	files = sorted(scripts_dir.glob("*.dll"), key=lambda item: item.name.casefold())
	import_references = 0
	references_by_library: Counter[str] = Counter()
	for path in files:
		try:
			image = pefile.PE(str(path), fast_load=False)
			module_pairs: set[tuple[str, str]] = set()
			for descriptor in getattr(image, "DIRECTORY_ENTRY_IMPORT", []):
				library = descriptor.dll.decode("ascii", errors="replace").lower()
				for entry in descriptor.imports:
					name = (entry.name.decode("ascii", errors="replace")
						if entry.name else f"#{entry.ordinal}")
					module_pairs.add((library, name))
					import_references += 1
					references_by_library[library] += 1
			by_module[path.name] = module_pairs
			union.update(module_pairs)
		except Exception as exc:  # malformed/non-PE files are reported, never disassembled speculatively
			errors.append({"file": path.name, "error": str(exc)})
	return union, by_module, errors, import_references, references_by_library


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--scripts-dir", type=Path,
		default=Path(os.environ.get("BATTLE_REALMS_SCRIPTS_DIR", str(DEFAULT_SCRIPTS))))
	args = parser.parse_args()
	if not args.scripts_dir.is_dir():
		parser.error(f"scenario DLL directory does not exist: {args.scripts_dir}")

	main_imports = read_main_imports()
	script_imports, imports_by_module, parse_errors, script_import_references, references_by_library = read_script_imports(args.scripts_dir)
	# Scenario DLLs also import functions exported by the main game executable.
	# Those are tracked separately and do not count as platform API coverage.
	game_exe_dlls = {"battle_realms_f.exe"}
	script_game_imports = {pair for pair in script_imports if pair[0] in game_exe_dlls}
	script_platform_imports = script_imports - script_game_imports
	script_game_import_references = sum(references_by_library[dll] for dll in game_exe_dlls)
	script_platform_import_references = script_import_references - script_game_import_references

	handlers: set[tuple[str, str]] = set()
	for source in HANDLER_SOURCES:
		text = source.read_text(encoding="utf-8")
		handlers.update((dll.lower(), name) for dll, name in REGISTRATION.findall(text))
		handlers.update((dll.lower(), f"#{ordinal}") for dll, ordinal in ORDINAL_REGISTRATION.findall(text))

	main_matched = main_imports & handlers
	script_matched = script_platform_imports & handlers
	combined_imports = main_imports | script_platform_imports
	combined_matched = combined_imports & handlers
	missing_main = main_imports - handlers
	missing_scripts = script_platform_imports - handlers
	missing_combined = combined_imports - handlers
	extra = handlers - combined_imports
	missing_main_by_library = Counter(dll for dll, _name in missing_main)
	missing_scripts_by_library = Counter(dll for dll, _name in missing_scripts)
	missing_combined_by_library = Counter(dll for dll, _name in missing_combined)

	missing_module_counts: Counter[str] = Counter()
	for module, pairs in imports_by_module.items():
		missing_module_counts[module] = len((pairs - script_game_imports) - handlers)

	def percentage(count: int, total: int) -> float:
		return round(100.0 * count / total, 2) if total else 0.0

	report = {
		"schema": 2,
		"method": "exact case-sensitive function/ordinal and case-insensitive DLL name",
		"main_import_source": str(PE_REPORT.relative_to(ROOT)).replace("\\", "/"),
		"script_import_source": str(args.scripts_dir),
		"handler_sources": [str(path.relative_to(ROOT)).replace("\\", "/") for path in HANDLER_SOURCES],
		"main_import_pairs": len(main_imports),
		"script_dll_candidates": len(list(args.scripts_dir.glob("*.dll"))),
		"script_valid_import_modules": len(imports_by_module),
		"script_import_references": script_import_references,
		"script_game_export_import_references": script_game_import_references,
		"script_platform_import_references": script_platform_import_references,
		"script_unique_import_pairs_including_game_exports": len(script_imports),
		"script_game_export_pairs_excluded_from_api_coverage": len(script_game_imports),
		"script_platform_import_pairs": len(script_platform_imports),
		"combined_platform_import_pairs": len(combined_imports),
		"registered_handler_pairs": len(handlers),
		"main_exact_name_matches": len(main_matched),
		"main_coverage_percent": percentage(len(main_matched), len(main_imports)),
		"script_exact_name_matches": len(script_matched),
		"script_coverage_percent": percentage(len(script_matched), len(script_platform_imports)),
		"combined_exact_name_matches": len(combined_matched),
		"combined_coverage_percent": percentage(len(combined_matched), len(combined_imports)),
		"unimplemented_main_pairs": len(missing_main),
		"unimplemented_script_platform_pairs": len(missing_scripts),
		"unimplemented_combined_pairs": len(missing_combined),
		"registered_names_not_imported_by_main_or_scripts": len(extra),
		"matched_main": [display_pair(pair) for pair in sorted(main_matched)],
		"matched_scripts": [display_pair(pair) for pair in sorted(script_matched)],
		"unimplemented_main": [display_pair(pair) for pair in sorted(missing_main)],
		"unimplemented_script_platform": [display_pair(pair) for pair in sorted(missing_scripts)],
		"unimplemented_combined": [display_pair(pair) for pair in sorted(missing_combined)],
		"unimplemented_main_by_library": dict(sorted(missing_main_by_library.items())),
		"unimplemented_script_platform_by_library": dict(sorted(missing_scripts_by_library.items())),
		"unimplemented_combined_by_library": dict(sorted(missing_combined_by_library.items())),
		"extra_handlers": [display_pair(pair) for pair in sorted(extra)],
		"unimplemented_script_module_counts": dict(sorted(missing_module_counts.items())),
		"script_parse_errors": parse_errors,
		"interpretation": "Name overlap is not proof of API ABI, argument, error, or runtime behavior. Scenario imports from Battle_Realms_F.exe are excluded from platform API coverage.",
	}
	OUTPUT.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	print(f"main={len(main_matched)}/{len(main_imports)} ({report['main_coverage_percent']:.2f}%) "
		f"scripts={len(script_matched)}/{len(script_platform_imports)} ({report['script_coverage_percent']:.2f}%) "
		f"combined={len(combined_matched)}/{len(combined_imports)} ({report['combined_coverage_percent']:.2f}%) "
		f"DLLs={report['script_valid_import_modules']}/{report['script_dll_candidates']} "
		f"script_refs={script_import_references} parse_errors={len(parse_errors)}")
	print(f"wrote {OUTPUT}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
