#!/usr/bin/env python3
"""Apply Battle Realms local instruction fixes to RA3-generated C++ output.

This post-pass leaves the upstream RA3-Native reference tree untouched. It
handles x87 FXAM and 32-bit XLATB forms that occur in this title and are emitted
as unsupported calls by the current upstream lifter.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FXAM = re.compile(r'unsupported\(c,\s*0x([0-9a-fA-F]+)u,\s*"fxam "\);')
FXAM_INLINE = re.compile(
    r'const double fxam_value_(?P<va>[0-9a-fA-F]+) = ST\(c, 0\);'
    r'(?:(?!\n\t/\*).)*?c->fcc = fxam_cc_(?P=va);', re.DOTALL
)
FXAM_LEGACY = re.compile(
    r'const double value = ST\(c, 0\);\n'
    r'\s*uint16_t cc = std::signbit\(value\) \? 0x0200u : 0u;\n'
    r'\s*if \(std::isnan\(value\)\) cc \|= 0x0100u;\n'
    r'\s*else if \(std::isinf\(value\)\) cc \|= 0x0500u;\n'
    r'\s*else if \(value == 0\.0\) cc \|= 0x4000u;\n'
    r'\s*else if \(std::fpclassify\(value\) == FP_SUBNORMAL\) cc \|= 0x4400u;\n'
    r'\s*else cc \|= 0x0400u;'
)
XLATB = re.compile(r'unsupported\(c,\s*0x([0-9a-fA-F]+)u,\s*"xlatb "\);')
FLD_EXT80 = re.compile(
    r'unsupported\(c,\s*0x([0-9a-fA-F]+)u,\s*"fld xword ptr \[([^]]+)\]"\);'
)
FPREM = re.compile(r'unsupported\(c,\s*0x([0-9a-fA-F]+)u,\s*"fprem(1?) "\);')
FSTP_EXT80 = re.compile(
    r'unsupported\(c,\s*0x([0-9a-fA-F]+)u,\s*"fstp xword ptr \[([^]]+)\]"\);'
)
FXAM_STALE_CC = re.compile(r'br_fxam\(c\);\s*c->fcc\s*=\s*cc;')
FXCH_BLOCK = re.compile(
    r'(\/\*\s*[0-9a-fA-F]+\s+fxch st\((\d+)\)\s*\*\/\s*\{\s*)'
    r'\{\s*double t = ST\(c, 0\); ST\(c, 0\) = ST\(c, \2\); ST\(c, \2\) = t;\s*\}'
    r'(\s*\})'
)
FPUSH_CALL = re.compile(r'\bfpush\(c,')
FPOP_CALL = re.compile(r'\bfpop\(c\)')
FARITH_CALL = re.compile(r'\bfarith\(c,')

EXT80_HELPER = r"""
static inline double br_load_x87_ext80(uint32_t address)
{
	const uint64_t significand = guest::load<uint64_t>(address);
	const uint16_t signExponent = guest::load<uint16_t>(address + 8u);
	const bool negative = (signExponent & 0x8000u) != 0u;
	const uint32_t exponent = signExponent & 0x7fffu;
	if (exponent == 0x7fffu) {
		if (significand == 0x8000000000000000ull)
			return std::copysign(std::numeric_limits<double>::infinity(), negative ? -1.0 : 1.0);
		return std::copysign(std::numeric_limits<double>::quiet_NaN(), negative ? -1.0 : 1.0);
	}
	if (exponent == 0u && significand == 0u)
		return std::copysign(0.0, negative ? -1.0 : 1.0);
	// Pseudo-denormals and unnormal encodings are not valid x87 values.
	if ((exponent != 0u && (significand & 0x8000000000000000ull) == 0u) ||
		(exponent == 0u && (significand & 0x8000000000000000ull) != 0u))
		return std::numeric_limits<double>::quiet_NaN();
	const int effectiveExponent = static_cast<int>(exponent ? exponent : 1u) - 16446;
	const long double value = std::ldexp(static_cast<long double>(significand), effectiveExponent);
	return static_cast<double>(negative ? -value : value);
}
"""


def patch_file(path: Path) -> int:
    source = path.read_text(encoding="utf-8")

    def fxam(match: re.Match[str]) -> str:
        return "br_fxam(c);"

    source, fxam_count = FXAM.subn(fxam, source)
    source, fxam_inline_count = FXAM_INLINE.subn("br_fxam(c);", source)
    source, fxam_legacy_count = FXAM_LEGACY.subn("br_fxam(c);", source)
    source, fxam_stale_cc_count = FXAM_STALE_CC.subn("br_fxam(c);", source)
    source, xlatb_count = XLATB.subn(
        r"eax = (eax & 0xffffff00u) | G8((uint32_t)(ebx + (eax & 0xffu)));",
        source,
    )
    def fld_ext80(match: re.Match[str]) -> str:
        operand = match.group(2).strip()
        if re.fullmatch(r"0x[0-9a-fA-F]+", operand):
            address = f"static_cast<uint32_t>({operand}u)"
        elif re.fullmatch(r"(?:eax|ecx|edx|ebx|esp|ebp|esi|edi)", operand):
            address = f"static_cast<uint32_t>({operand})"
        else:
            memory = re.fullmatch(
                r"(eax|ecx|edx|ebx|esp|ebp|esi|edi)\s*([+-])\s*(0x[0-9a-fA-F]+|[0-9]+)",
                operand,
            )
            if not memory:
                raise ValueError(f"unsupported FLD EXT80 address expression: {operand}")
            base, operator, displacement = memory.groups()
            if not displacement.startswith("0x"):
                displacement = hex(int(displacement, 10))
            address = f"static_cast<uint32_t>({base} {operator} {displacement}u)"
        return f"fpush(c, br_load_x87_ext80({address}));"

    source, ext80_count = FLD_EXT80.subn(fld_ext80, source)
    source, fstp_ext80_count = FSTP_EXT80.subn(
        lambda match: f"br_fstp_ext80(c, {address_expression(match.group(2))});",
        source,
    )
    def fprem(match: re.Match[str]) -> str:
        suffix = match.group(1).lower()
        one = match.group(2) == "1"
        operation = "std::remainder" if one else "std::fmod"
        quotient_round = "std::nearbyint" if one else "std::trunc"
        return (
            f"const double fprem_a_{suffix} = ST(c, 0);\n"
            f"const double fprem_b_{suffix} = ST(c, 1);\n"
            f"const double fprem_q_{suffix} = fprem_a_{suffix} / fprem_b_{suffix};\n"
            f"ST(c, 0) = {operation}(fprem_a_{suffix}, fprem_b_{suffix});\n"
            f"c->fcc &= static_cast<uint16_t>(~0x4700u);\n"
            f"if (std::isfinite(fprem_q_{suffix}) && fprem_b_{suffix} != 0.0) {{\n"
            f"\tconst double fprem_qmod8_{suffix} = std::fmod({quotient_round}(fprem_q_{suffix}), 8.0);\n"
            f"\tconst uint32_t fprem_qbits_{suffix} = static_cast<uint32_t>(static_cast<int32_t>(fprem_qmod8_{suffix})) & 7u;\n"
            f"\tif (fprem_qbits_{suffix} & 4u) c->fcc |= 0x0100u;\n"
            f"\tif (fprem_qbits_{suffix} & 2u) c->fcc |= 0x4000u;\n"
            f"\tif (fprem_qbits_{suffix} & 1u) c->fcc |= 0x0200u;\n"
            f"}}"
        )

    source, fprem_count = FPREM.subn(fprem, source)
    source, fxch_count = FXCH_BLOCK.subn(
        lambda match: f"{match.group(1)}br_fxch(c, {match.group(2)}u);{match.group(3)}",
        source,
    )
    source, fpush_count = FPUSH_CALL.subn("br_fpush(c,", source)
    source, fpop_count = FPOP_CALL.subn("br_fpop(c)", source)
    source, farith_count = FARITH_CALL.subn("br_farith(c,", source)
    count = (fxam_count + fxam_inline_count + fxam_legacy_count + fxam_stale_cc_count +
             xlatb_count + ext80_count + fstp_ext80_count + fprem_count + fxch_count +
             fpush_count + fpop_count + farith_count)
    if count:
        if '#include "br_x87.h"' not in source:
            source = source.replace(
                '#include "lifted_decl.h"',
                '#include "lifted_decl.h"\n#include "br_x87.h"',
                1,
            )
        if ext80_count and "static inline double br_load_x87_ext80" not in source:
            source = source.replace(
                "#include <cmath>",
                "#include <cmath>\n#include <limits>\n" + EXT80_HELPER.strip(),
                1,
            )
        path.write_text(source, encoding="utf-8", newline="")
    return count


def address_expression(operand: str) -> str:
    operand = operand.strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+", operand):
        return f"static_cast<uint32_t>({operand}u)"
    if re.fullmatch(r"(?:eax|ecx|edx|ebx|esp|ebp|esi|edi)", operand):
        return f"static_cast<uint32_t>({operand})"
    memory = re.fullmatch(
        r"(eax|ecx|edx|ebx|esp|ebp|esi|edi)\s*([+-])\s*(0x[0-9a-fA-F]+|[0-9]+)",
        operand,
    )
    if not memory:
        raise ValueError(f"unsupported x87 memory address expression: {operand}")
    base, operator, displacement = memory.groups()
    if not displacement.startswith("0x"):
        displacement = hex(int(displacement, 10))
    return f"static_cast<uint32_t>({base} {operator} {displacement}u)"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("generated_root", type=Path)
    args = parser.parse_args()
    total = 0
    files = 0
    for path in sorted(args.generated_root.rglob("lifted_*.cpp")):
        count = patch_file(path)
        if count:
            files += 1
            total += count
            print(f"{path}: patched {count} instruction(s)")
    print(f"patched {total} instruction(s) in {files} file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
