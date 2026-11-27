#!/usr/bin/env python3
"""Execute one x86-32 PE code block under Unicorn and emit observable state JSON."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import pefile
from capstone import CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_MODE_32, Cs
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_32, Uc
from unicorn.x86_const import (
	UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
	UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP,
)


REGS = {
	"eax": UC_X86_REG_EAX, "ecx": UC_X86_REG_ECX, "edx": UC_X86_REG_EDX, "ebx": UC_X86_REG_EBX,
	"esp": UC_X86_REG_ESP, "ebp": UC_X86_REG_EBP, "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
}
FLAG_BITS = {"cf": 0, "pf": 2, "af": 4, "zf": 6, "sf": 7, "df": 10, "of": 11}
PAGE = 0x1000


def align_down(value: int) -> int:
	return value & ~(PAGE - 1)


def align_up(value: int) -> int:
	return (value + PAGE - 1) & ~(PAGE - 1)


def load_image(uc: Uc, path: Path) -> pefile.PE:
	pe = pefile.PE(str(path), fast_load=True)
	if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
		raise ValueError("expected x86 PE32 input")
	base = int(pe.OPTIONAL_HEADER.ImageBase)
	size = align_up(int(pe.OPTIONAL_HEADER.SizeOfImage))
	uc.mem_map(base, size)
	uc.mem_write(base, pe.get_memory_mapped_image()[:int(pe.OPTIONAL_HEADER.SizeOfImage)])
	return pe


def run(exe: Path, case: dict[str, Any]) -> dict[str, Any]:
	uc = Uc(UC_ARCH_X86, UC_MODE_32)
	pe = load_image(uc, exe)
	start = int(case["address"], 0)
	stop = int(case["stop_address"], 0)
	regions = case["memory"]
	pages: set[int] = {align_down(stop)}
	for region in regions:
		address = int(region["address"], 0)
		length = len(bytes.fromhex(region["hex"]))
		pages.update(range(align_down(address), align_up(address + max(length, 1)), PAGE))
	for page in sorted(pages):
		if not (int(pe.OPTIONAL_HEADER.ImageBase) <= page <
				int(pe.OPTIONAL_HEADER.ImageBase) + align_up(int(pe.OPTIONAL_HEADER.SizeOfImage))):
			uc.mem_map(page, PAGE)
	for region in regions:
		uc.mem_write(int(region["address"], 0), bytes.fromhex(region["hex"]))
	uc.mem_write(stop, b"\x90")

	flags = 0x2
	for name, bit in FLAG_BITS.items():
		if int(case.get("flags", {}).get(name, 0)):
			flags |= 1 << bit
	uc.reg_write(UC_X86_REG_EFLAGS, flags)
	for name, register in REGS.items():
		uc.reg_write(register, int(case["registers"][name], 0))
	uc.reg_write(UC_X86_REG_EIP, start)

	md = Cs(CS_ARCH_X86, CS_MODE_32)
	md.detail = True
	trace: list[int] = []
	instructions: list[dict[str, Any]] = []
	branches: list[dict[str, int]] = []
	writes: list[dict[str, int]] = []
	stopped = False
	pending_branch: int | None = None
	def on_code(engine: Uc, address: int, _size: int, _user: object) -> None:
		nonlocal stopped, pending_branch
		if address == stop:
			if pending_branch is not None:
				branches.append({"site": pending_branch, "target": address})
			stopped = True
			engine.emu_stop()
			return
		if pending_branch is not None:
			branches.append({"site": pending_branch, "target": address})
			pending_branch = None
		if len(trace) >= 100000:
			engine.emu_stop()
			return
		trace.append(address)
		code = bytes(engine.mem_read(address, 16))
		insn = next(md.disasm(code, address, count=1), None)
		if insn is None:
			engine.emu_stop()
			return
		instructions.append({"address": address, "bytes": insn.bytes.hex(),
			"text": f"{insn.mnemonic} {insn.op_str}".strip()})
		if insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP) or insn.group(CS_GRP_RET):
			pending_branch = address
	def on_write(_engine: Uc, _access: int, address: int, size: int, value: int, _user: object) -> None:
		writes.append({"address": address, "size": size, "value": value})
	uc.hook_add(UC_HOOK_CODE, on_code)
	uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
	uc.emu_start(start, stop + 1, count=100000)
	if not stopped:
		raise RuntimeError(f"x86 execution did not return to 0x{stop:08X}; eip=0x{uc.reg_read(UC_X86_REG_EIP):08X}")
	final_flags = uc.reg_read(UC_X86_REG_EFLAGS)
	final_memory = []
	for region in regions:
		address = int(region["address"], 0)
		before = bytes.fromhex(region["hex"])
		after = bytes(uc.mem_read(address, len(before)))
		for offset, (old, new) in enumerate(zip(before, after, strict=True)):
			if old != new:
				final_memory.append({"address": address + offset, "before": old, "after": new})
	return {
		"registers": {name: uc.reg_read(reg) for name, reg in REGS.items()},
		"flags": {name: (final_flags >> bit) & 1 for name, bit in FLAG_BITS.items()},
		"memory_writes": final_memory,
		"write_events": writes,
		"eip_after_return": uc.reg_read(UC_X86_REG_EIP),
		"instruction_trace": trace,
		"branch_targets": branches,
		"disassembly": instructions,
	}


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--exe", type=Path, required=True)
	parser.add_argument("--case", type=Path, required=True)
	parser.add_argument("--output", type=Path)
	args = parser.parse_args()
	result = run(args.exe, json.loads(args.case.read_text(encoding="utf-8")))
	text = json.dumps(result, indent=2) + "\n"
	if args.output:
		args.output.parent.mkdir(parents=True, exist_ok=True)
		args.output.write_text(text, encoding="utf-8")
	else:
		print(text, end="")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
