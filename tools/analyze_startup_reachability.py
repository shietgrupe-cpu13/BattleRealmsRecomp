#!/usr/bin/env python3
"""Conservatively trace direct calls from the lifted Battle Realms entrypoint.

This is static reachability over the generated C++: every direct call is
followed, while conditional branches are not evaluated. Indirect targets are
only attributed to imports when the generated call reads a constant PE IAT
slot. This is not a dynamic execution trace.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from collections import deque
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FUNCTION = re.compile(r"(?m)^void f_([0-9a-fA-F]{8})\(CPU\* c\)\s*\n\{")
DIRECT_CALL = re.compile(r"\bf_([0-9a-fA-F]{8})\(c\);")
CONSTANT_IAT_CALL = re.compile(
	r"G32\(\(uint32_t\)\(0x([0-9a-fA-F]{8})u\)\).*?ICALL\(t\)"
)
INDIRECT_CALL = re.compile(r"ICALL\(t\)")
MEMORY_OPERAND = re.compile(r"G32\(\(uint32_t\)\(([^)]*)\)\)")
HEX_CONSTANT = re.compile(r"^0x([0-9a-fA-F]{1,8})u?$")
IAT_NAMED_LINE = re.compile(
	r"^\s*([0-9a-fA-F]{8})\s+<none>\s+[0-9a-fA-F]{4}\s+(\S+)\s+[0-9a-fA-F]{8}\s*$"
)
IAT_ORDINAL_LINE = re.compile(
	r"^\s*([0-9a-fA-F]{8})\s+(\d+)\s+<none>\s+<none>\s+[0-9a-fA-F]{8}\s*$"
)
DLL_NAME = re.compile(r"^\s*DLL Name:\s*(\S+)", re.IGNORECASE)
HANDLER = re.compile(r'handlers\.registerName\(\s*"([^"]+)"\s*,\s*"([^"]+)"')
ORDINAL_HANDLER = re.compile(r'handlers\.registerOrdinal\(\s*"([^"]+)"\s*,\s*(\d+)')


def load_iat_names(path: Path, image_base: int) -> dict[int, tuple[str, str]]:
	result: dict[int, tuple[str, str]] = {}
	data = path.read_bytes()
	text = data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8", errors="replace")
	current_dll = "unknown.dll"
	for line in text.splitlines():
		dll_match = DLL_NAME.match(line)
		if dll_match:
			current_dll = dll_match.group(1)
		match = IAT_NAMED_LINE.match(line)
		if match:
			rva, name = match.groups()
			result[image_base + int(rva, 16)] = (current_dll.lower(), name)
			continue
		match = IAT_ORDINAL_LINE.match(line)
		if match:
			rva, ordinal = match.groups()
			result[image_base + int(rva, 16)] = (current_dll.lower(), f"#{ordinal}")
	return result


def load_handlers(runtime_dir: Path) -> set[tuple[str, str]]:
	result: set[tuple[str, str]] = set()
	for source in sorted(runtime_dir.glob("win32_*.cpp")):
		text = source.read_text(encoding="utf-8", errors="replace")
		result.update((dll.lower(), name) for dll, name in HANDLER.findall(text))
		result.update((dll.lower(), f"#{ordinal}") for dll, ordinal in ORDINAL_HANDLER.findall(text))
	return result


def load_pe_image(path: Path) -> tuple[int, bytes, list[tuple[int, int, int, bool]]]:
	"""Return image base, mapped bytes, and (VA start, VA end, executable) sections."""
	data = path.read_bytes()
	pe = struct.unpack_from("<I", data, 0x3C)[0]
	if data[pe:pe + 4] != b"PE\0\0":
		raise ValueError(f"{path} is not a PE image")
	section_count = struct.unpack_from("<H", data, pe + 6)[0]
	optional_size = struct.unpack_from("<H", data, pe + 20)[0]
	opt = pe + 24
	if struct.unpack_from("<H", data, opt)[0] != 0x10B:
		raise ValueError("expected PE32 optional header")
	image_base = struct.unpack_from("<I", data, opt + 28)[0]
	section_alignment = struct.unpack_from("<I", data, opt + 32)[0]
	image_size = struct.unpack_from("<I", data, opt + 56)[0]
	mapped = bytearray(image_size)
	header_size = struct.unpack_from("<I", data, opt + 60)[0]
	mapped[:min(header_size, len(data), image_size)] = data[:min(header_size, len(data), image_size)]
	sections = []
	section_table = opt + optional_size
	for index in range(section_count):
		entry = section_table + index * 40
		virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, entry + 8)
		characteristics = struct.unpack_from("<I", data, entry + 36)[0]
		copy_size = min(raw_size, max(0, image_size - rva), max(0, len(data) - raw_offset))
		if copy_size:
			mapped[rva:rva + copy_size] = data[raw_offset:raw_offset + copy_size]
		span = max(virtual_size, raw_size)
		sections.append((image_base + rva, image_base + rva + span,
			bool(characteristics & 0x20000000)))
	return image_base, bytes(mapped), sections


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--main-dir", type=Path, default=ROOT / "generated" / "main96_hybrid")
	parser.add_argument("--objdump", type=Path, default=ROOT / "analysis" / "battle_realms_objdump.txt")
	parser.add_argument("--runtime-dir", type=Path, default=ROOT / "runtime")
	parser.add_argument("--exe", type=Path, default=Path(r"D:\Games\Battle Realms Complete\Battle_Realms_F.exe"))
	parser.add_argument("--entrypoint", type=lambda value: int(value, 0), default=0x006A2441)
	parser.add_argument("--output", type=Path, default=ROOT / "analysis" / "startup_reachability.json")
	args = parser.parse_args()

	functions: dict[int, tuple[set[int], list[tuple[int, int]], list[tuple[str, str]]]] = {}
	for source in sorted(args.main_dir.glob("lifted_*.cpp"), key=lambda path: path.name):
		text = source.read_text(encoding="utf-8", errors="replace")
		starts = list(FUNCTION.finditer(text))
		for index, start in enumerate(starts):
			end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
			body = text[start.end():end]
			address = int(start.group(1), 16)
			direct = {int(match.group(1), 16) for match in DIRECT_CALL.finditer(body)}
			indirect = [(int(match.group(1), 16), match.start())
				for match in CONSTANT_IAT_CALL.finditer(body)]
			iat_offsets = {offset for _slot, offset in indirect}
			unresolved = []
			for match in INDIRECT_CALL.finditer(body):
				if any(offset <= match.start() < offset + 256 for offset in iat_offsets):
					continue
				line_start = body.rfind("\n", 0, match.start()) + 1
				line_end = body.find("\n", match.start())
				line = body[line_start:line_end if line_end >= 0 else len(body)]
				operand_match = MEMORY_OPERAND.search(line)
				operand = operand_match.group(1).strip() if operand_match else "register"
				constant = HEX_CONSTANT.match(operand)
				kind = "constant_memory_pointer" if constant else (
					"register_indirect" if operand == "register" else "computed_memory_pointer")
				unresolved.append((kind, line.strip()))
			functions[address] = (direct, indirect, unresolved)

	if args.entrypoint not in functions:
		parser.error(f"entrypoint 0x{args.entrypoint:08x} was not found under {args.main_dir}")
	imports = load_iat_names(args.objdump, image_base=0x00400000)
	image_base, mapped_image, sections = load_pe_image(args.exe)
	parents: dict[int, int | None] = {args.entrypoint: None}
	queue = deque([args.entrypoint])
	reachable_import_callers: dict[tuple[str, str], set[int]] = {}
	constant_import_sites = 0
	dynamic_indirect_sites = 0
	indirect_classes: dict[str, int] = {}
	indirect_samples: dict[str, list[dict[str, str]]] = {}
	constant_slot_sites = 0
	unresolved_constant_pointer_sites = 0
	constant_slots: dict[int, list[dict[str, str]]] = {}
	constant_slot_targets: dict[str, int] = {}
	missing_direct_targets: set[tuple[int, int]] = set()

	while queue:
		caller = queue.popleft()
		direct, indirect, unresolved = functions[caller]
		for kind, line in unresolved:
			dynamic_indirect_sites += 1
			indirect_classes[kind] = indirect_classes.get(kind, 0) + 1
			samples = indirect_samples.setdefault(kind, [])
			if len(samples) < 12:
				samples.append({"caller": f"0x{caller:08x}", "generated_call": line[:280]})
		for slot, _offset in indirect:
			api = imports.get(slot)
			if api is None:
				indirect_classes["constant_slot_not_in_IAT"] = indirect_classes.get("constant_slot_not_in_IAT", 0) + 1
				constant_slot_sites += 1
				target = None
				rva = slot - image_base
				if 0 <= rva <= len(mapped_image) - 4:
					target = struct.unpack_from("<I", mapped_image, rva)[0]
				target_section = next((section for section in sections if section[0] <= target < section[1]), None) if target is not None else None
				if target is None:
					status = "slot_unmapped"
				elif target_section is None or not target_section[2]:
					status = "target_not_executable"
				elif target not in functions:
					status = "executable_target_not_in_main_lift_table"
				else:
					status = "target_is_lifted_main_function"
				constant_slot_targets[status] = constant_slot_targets.get(status, 0) + 1
				if status != "target_is_lifted_main_function":
					unresolved_constant_pointer_sites += 1
				rows = constant_slots.setdefault(slot, [])
				if len(rows) < 4:
					rows.append({"caller": f"0x{caller:08x}",
						"target": f"0x{target:08x}" if target is not None else "unmapped",
						"classification": status})
			else:
				constant_import_sites += 1
				reachable_import_callers.setdefault(api, set()).add(caller)
		for target in sorted(direct):
			if target not in functions:
				missing_direct_targets.add((caller, target))
				continue
			if target not in parents:
				parents[target] = caller
				queue.append(target)

	def call_path(caller: int) -> list[str]:
		path = [caller]
		while parents[path[-1]] is not None:
			path.append(parents[path[-1]])  # type: ignore[arg-type]
		path.reverse()
		return [f"0x{address:08x}" for address in path]

	handlers = load_handlers(args.runtime_dir)
	api_rows = []
	for api, callers in reachable_import_callers.items():
		dll, name = api
		first = min(callers, key=lambda address: (len(call_path(address)), address))
		api_rows.append({
			"api": f"{dll}!{name}",
			"reachable_call_sites": sum(len([slot for slot, _ in functions[caller][1] if imports.get(slot) == api])
				for caller in callers),
			"handler_registered": api in handlers,
			"sample_call_path": call_path(first),
		})
	api_rows.sort(key=lambda row: (len(row["sample_call_path"]), row["api"].casefold()))
	unhandled_apis = [row for row in api_rows if not row["handler_registered"]]

	result = {
		"schema": 1,
		"method": "generated C++ direct-call BFS; all branch outcomes included; constant PE IAT calls mapped via named and ordinal objdump records; fixed non-IAT pointer slots read from a PE32 image map",
		"interpretation": "Static conservative reachability, not runtime execution. Constant import slots are treated as statically bindable, fixed slots pointing to lifted starts are resolved, and remaining fixed-data plus computed/register targets remain unresolved.",
		"entrypoint": f"0x{args.entrypoint:08x}",
		"main_lifted_functions": len(functions),
		"reachable_main_functions": len(parents),
		"reachable_main_function_percent": round(100.0 * len(parents) / len(functions), 2),
		"reachable_static_import_call_sites": constant_import_sites,
		"reachable_unique_static_import_names": len(api_rows),
		"reachable_import_names_with_handlers": len(api_rows) - len(unhandled_apis),
		"reachable_import_names_without_handlers": len(unhandled_apis),
		"reachable_unhandled_import_call_sites": sum(row["reachable_call_sites"] for row in unhandled_apis),
		"reachable_unresolved_indirect_sites": dynamic_indirect_sites + unresolved_constant_pointer_sites,
		"reachable_dynamic_indirect_sites": dynamic_indirect_sites,
		"reachable_unresolved_constant_pointer_sites": unresolved_constant_pointer_sites,
		"reachable_indirect_sites_with_static_import_targets": constant_import_sites,
		"reachable_indirect_sites_with_fixed_lifted_targets": constant_slot_targets.get("target_is_lifted_main_function", 0),
		"reachable_indirect_call_sites_total": constant_import_sites + dynamic_indirect_sites + constant_slot_sites,
		"reachable_dynamic_indirect_classes": {
			key: value for key, value in sorted(indirect_classes.items()) if key != "constant_slot_not_in_IAT"
		},
		"reachable_indirect_site_classes": dict(sorted(indirect_classes.items())),
		"reachable_unresolved_indirect_samples": indirect_samples,
		"reachable_constant_pointer_sites": constant_slot_sites,
		"reachable_unique_constant_pointer_slots": len(constant_slots),
		"reachable_constant_pointer_target_classification": dict(sorted(constant_slot_targets.items())),
		"reachable_constant_pointer_samples": [
			{"slot": f"0x{slot:08x}", "sites": sites}
			for slot, sites in sorted(constant_slots.items())[:250]
		],
		"untranslated_direct_targets": len(missing_direct_targets),
		"reachable_static_imports": api_rows,
		"reachable_static_imports_without_handlers": unhandled_apis,
		"untranslated_direct_target_samples": [
			{"caller": f"0x{caller:08x}", "target": f"0x{target:08x}"}
			for caller, target in sorted(missing_direct_targets)[:250]
		],
	}
	args.output.parent.mkdir(parents=True, exist_ok=True)
	args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	print(
		f"entry=0x{args.entrypoint:08x} functions={len(functions)} reachable={len(parents)} "
		f"({result['reachable_main_function_percent']}%) static_import_calls={constant_import_sites} "
		f"unique_imports={len(api_rows)} indirect_unresolved={dynamic_indirect_sites + unresolved_constant_pointer_sites} "
		f"dynamic_indirect={dynamic_indirect_sites} fixed_unresolved={unresolved_constant_pointer_sites} "
		f"untranslated_direct={len(missing_direct_targets)}"
	)
	print(f"wrote {args.output}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
