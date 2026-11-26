#!/usr/bin/env python3
"""Differentially run one rebased lifted leaf against its original PE bytes."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
    UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)


REGS = {
    "eax": UC_X86_REG_EAX, "ecx": UC_X86_REG_ECX, "edx": UC_X86_REG_EDX,
    "ebx": UC_X86_REG_EBX, "esp": UC_X86_REG_ESP, "ebp": UC_X86_REG_EBP,
    "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
}
DATA = 0x30001000
STACK = 0x31000000
RETURN = 0xDEADBEEF
VALUE = 0x89ABCDEF


def extract_function(module: Path, function: str) -> str:
    pattern = re.compile(rf"(?m)^void {re.escape(function)}\(CPU\* c\)\s*\{{")
    for source in sorted(module.glob("lifted_*.cpp")):
        if source.name == "lifted_table.cpp":
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        match = pattern.search(text)
        if not match:
            continue
        next_function = re.search(r"(?m)^void f_[0-9a-fA-F]{8}\(CPU\* c\)\s*\{", text[match.end():])
        end = match.end() + next_function.start() if next_function else len(text)
        return text[match.start():end].rstrip() + "\n"
    raise ValueError(f"generated function not found: {function}")


def extract_function_source(source_path: Path, function: str) -> str:
    """Read one standalone replacement definition with the generated function ABI."""
    text = source_path.read_text(encoding="utf-8-sig", errors="replace")
    pattern = re.compile(rf"(?m)^void {re.escape(function)}\(CPU\* c\)\s*\{{")
    match = pattern.search(text)
    if not match:
        raise ValueError(f"replacement source does not define {function}(CPU*): {source_path}")
    next_function = re.search(r"(?m)^void f_[0-9a-fA-F]{8}\(CPU\* c\)\s*\{", text[match.end():])
    end = match.end() + next_function.start() if next_function else len(text)
    return text[match.start():end].rstrip() + "\n"


def host_run(pe: pefile.PE, rva: int) -> dict[str, int]:
    base = pe.OPTIONAL_HEADER.ImageBase
    image_size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, image_size)
    mapped = pe.get_memory_mapped_image()
    uc.mem_write(base, mapped)
    uc.mem_map(DATA & ~0xFFF, 0x1000)
    uc.mem_write(DATA + 4, VALUE.to_bytes(4, "little"))
    uc.mem_map(STACK, 0x10000)
    initial = {
        "eax": 0x11111111, "ecx": DATA, "edx": 0x22222222,
        "ebx": 0x33333333, "esp": STACK + 0x7FFC,
        "ebp": 0x44444444, "esi": 0x55555555, "edi": 0x66666666,
    }
    uc.mem_write(initial["esp"], RETURN.to_bytes(4, "little"))
    for name, reg in REGS.items():
        uc.reg_write(reg, initial[name])
    uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    instructions = list(decoder.disasm(pe.get_data(rva, 16), base + rva))
    if [insn.mnemonic for insn in instructions[:2]] != ["mov", "ret"]:
        raise ValueError(f"chosen RVA is no longer the expected leaf: {instructions[:4]}")
    uc.emu_start(base + rva, RETURN, count=16)
    output = {name: uc.reg_read(reg) & 0xFFFFFFFF for name, reg in REGS.items()}
    output["eflags"] = uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFFFFFF
    output["data"] = int.from_bytes(uc.mem_read(DATA + 4, 4), "little")
    output["return_slot"] = int.from_bytes(uc.mem_read(STACK + 0x7FFC, 4), "little")
    return output


def make_harness(function_source: str) -> str:
    return '''#include "lifted_prelude.h"
#include <cstdio>

''' + function_source + r'''
int main()
{
    if (!guest::init() || !guest::commit(0x30000000u, 0x10000u) ||
        !guest::commit(0x31000000u, 0x10000u)) return 10;
    guest::store<uint32_t>(0x30001004u, 0x89abcdefu);
    CPU c{};
    c.r[EAX] = 0x11111111u; c.r[ECX] = 0x30001000u;
    c.r[EDX] = 0x22222222u; c.r[EBX] = 0x33333333u;
    c.r[ESP] = 0x31007ffcu; c.r[EBP] = 0x44444444u;
    c.r[ESI] = 0x55555555u; c.r[EDI] = 0x66666666u;
    c.df = 0; c.t_cf = 1; c.t_pf = 0; c.t_af = 1;
    c.t_zf = 0; c.t_sf = 1; c.t_of = 0;
    guest::store<uint32_t>(c.r[ESP], 0xdeadbeefu);
    FUNCTION_NAME(&c);
    std::printf("eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x df=%08x cf=%02x pf=%02x af=%02x zf=%02x sf=%02x of=%02x data=%08x return_slot=%08x\n",
        c.r[EAX], c.r[ECX], c.r[EDX], c.r[EBX], c.r[ESP], c.r[EBP],
        c.r[ESI], c.r[EDI], c.df, c.t_cf, c.t_pf, c.t_af, c.t_zf,
        c.t_sf, c.t_of, guest::load<uint32_t>(0x30001004u),
        guest::load<uint32_t>(0x31007ffcu));
    return 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-dll", type=Path, required=True)
    parser.add_argument("--rebased-dll", type=Path, required=True)
    parser.add_argument("--generated-module", type=Path, required=True)
    parser.add_argument("--native-source", type=Path,
                        help="optional standalone C++ replacement defining the same lifted ABI symbol")
    parser.add_argument("--rva", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()

    original = pefile.PE(str(args.original_dll), fast_load=False)
    rebased = pefile.PE(str(args.rebased_dll), fast_load=False)
    if (original.FILE_HEADER.Machine != 0x14C or original.OPTIONAL_HEADER.Magic != 0x10B or
            rebased.FILE_HEADER.Machine != 0x14C or rebased.OPTIONAL_HEADER.Magic != 0x10B):
        parser.error("both inputs must be PE32/I386 images")
    if original.OPTIONAL_HEADER.SizeOfImage != rebased.OPTIONAL_HEADER.SizeOfImage:
        parser.error("rebased image changed SizeOfImage")
    if original.OPTIONAL_HEADER.ImageBase == rebased.OPTIONAL_HEADER.ImageBase:
        parser.error("rebased DLL still uses its original image base")
    if not any(section.VirtualAddress <= args.rva < section.VirtualAddress + max(
            section.Misc_VirtualSize, section.SizeOfRawData) and section.IMAGE_SCN_MEM_EXECUTE
            for section in original.sections):
        parser.error("RVA is not in an executable PE section")

    rebased_va = rebased.OPTIONAL_HEADER.ImageBase + args.rva
    function = f"f_{rebased_va:08x}"
    function_source = extract_function_source(args.native_source, function) if args.native_source else \
        extract_function(args.generated_module, function)
    harness = make_harness(function_source).replace("FUNCTION_NAME", function)
    workspace = args.workspace.resolve()
    output_dir = workspace / "work" / "differential_lift_smoke"
    build_dir = workspace / "build" / "differential_lift_smoke"
    temp = workspace / "tmp"
    cache = workspace / "cache"
    for path in (output_dir, build_dir, temp):
        path.mkdir(parents=True, exist_ok=True)
    source_path = output_dir / f"{function}.cpp"
    exe_path = build_dir / f"{function}.exe"
    source_path.write_text(harness, encoding="utf-8")
    env = os.environ.copy()
    env.update({"TEMP": str(temp), "TMP": str(temp), "TMPDIR": str(temp),
                "PYTHONPYCACHEPREFIX": str(cache / "pycache")})
    runtime = workspace / "references" / "RA3-Native" / "runtime"
    bridge = workspace / "build" / "pe32_loader" / "libbr_ra3_guest_bridge.a"
    address_space = workspace / "build" / "pe32_loader" / "libbr_guest_address_space.a"
    pe_library = workspace / "build" / "pe32_loader" / "libbr_pe32_image.a"
    command = [str(args.compiler), "-std=c++17", "-O2", "-fno-strict-aliasing",
               "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
               "-I", str(runtime), str(source_path), str(bridge), str(address_space),
               str(pe_library), "-o", str(exe_path)]
    build_log = output_dir / "compile.log"
    with build_log.open("wb") as log:
        result = subprocess.run(command, cwd=workspace, env=env, stdout=log,
                                stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"host harness compilation failed; see {build_log}")

    process = subprocess.run([str(exe_path)], cwd=workspace, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace", check=False)
    if process.returncode:
        raise RuntimeError(f"lifted harness failed ({process.returncode}): {process.stdout}")
    cpp_values = {name: int(value, 16) for name, value in
                  re.findall(r"(eax|ecx|edx|ebx|esp|ebp|esi|edi|df|cf|pf|af|zf|sf|of|data|return_slot)=([0-9a-fA-F]+)", process.stdout)}
    original_values = host_run(original, args.rva)
    expected = {key: original_values[key] for key in REGS}
    expected.update({"df": 0, "cf": 1, "pf": 0, "af": 1, "zf": 0, "sf": 1, "of": 0,
                     "data": VALUE, "return_slot": RETURN})
    mismatches = {key: {"original_x86": f"0x{value:08x}",
                        "lifted_cpp": f"0x{cpp_values.get(key, 0):08x}"}
                  for key, value in expected.items() if cpp_values.get(key) != value}
    report = {
        "schema": 1,
        "original_dll": str(args.original_dll.resolve()),
        "rebased_dll": str(args.rebased_dll.resolve()),
        "original_image_base": f"0x{original.OPTIONAL_HEADER.ImageBase:08x}",
        "rebased_image_base": f"0x{rebased.OPTIONAL_HEADER.ImageBase:08x}",
        "function_rva": f"0x{args.rva:08x}",
        "original_function_va": f"0x{original.OPTIONAL_HEADER.ImageBase + args.rva:08x}",
        "lifted_function_va": f"0x{rebased_va:08x}",
        "candidate_kind": "native_replacement" if args.native_source else "generated_lift",
        "candidate_source": str(args.native_source.resolve()) if args.native_source else None,
        "x86_original": {key: f"0x{value:08x}" for key, value in original_values.items()},
        "lifted_cpp": {key: f"0x{value:08x}" for key, value in cpp_values.items()},
        "mismatches": mismatches,
        "passed": not mismatches,
        "scope": "one leaf function: integer register, guest memory read, return stack, and preserved status fields",
    }
    report_path = output_dir / "result.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("x86_original", "lifted_cpp", "mismatches")}, indent=2))
    if mismatches:
        print(json.dumps({"mismatches": mismatches}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
