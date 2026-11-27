#!/usr/bin/env python3
"""Create a diagnostic-only overlay for one verified guest parent function.

The source tree remains untouched. The overlay preserves generated bulk files
and replaces only the translation unit containing the requested parent. Only
dispatches whose computed target equals --target are logged at runtime.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


FUNCTION = re.compile(r"^void f_([0-9a-fA-F]{8})\(CPU\* c\)", re.MULTILINE)
INSTRUCTION = re.compile(r"/\*\s*([0-9a-fA-F]{8})\s+(jmp|call)\b[\s\S]*?\*/")
TARGET_DECL = re.compile(r"\buint32_t t\s*=\s*([^;]+);")
ICALL = re.compile(r"\bICALL\(t\)")
CASE_TARGET = re.compile(r"\bcase\s+0x([0-9a-fA-F]+)u\s*:")


def instrument_source(source: str, parent: int, target: int) -> tuple[str, list[dict[str, str]]]:
    functions = list(FUNCTION.finditer(source))
    for index, match in enumerate(functions):
        if int(match.group(1), 16) != parent:
            continue
        end = functions[index + 1].start() if index + 1 < len(functions) else len(source)
        body = source[match.start():end]
        calls = list(ICALL.finditer(body))
        if not calls:
            raise ValueError(f"f_{parent:08x} has no ICALL(t) sites")
        edits: list[tuple[int, int, str]] = []
        sites: list[dict[str, str]] = []
        handled_targets: set[int] = set()
        declarations = list(TARGET_DECL.finditer(body))
        declaration_sites: dict[int, tuple[int, str]] = {}
        for decl_index, decl_item in enumerate(declarations):
            comment = None
            for item in INSTRUCTION.finditer(body, 0, decl_item.start()):
                comment = item
            if comment is None:
                continue
            site_info = (int(comment.group(1), 16), "indirect-" + comment.group(2))
            table_addresses = set(re.findall(r"0x([0-9a-fA-F]{6,8})u?", decl_item.group(1)))
            # Some upstream lifter paths duplicate a computed jump after an
            # inlined basic block. The duplicate has no guest instruction
            # comment, but retains the original jump-table address.
            for earlier in reversed(declarations[:decl_index]):
                earlier_info = declaration_sites.get(earlier.start())
                earlier_addresses = set(re.findall(r"0x([0-9a-fA-F]{6,8})u?", earlier.group(1)))
                if earlier_info and table_addresses & earlier_addresses:
                    site_info = earlier_info
                    break
            declaration_sites[decl_item.start()] = site_info
        for call in calls:
            decl = None
            for item in declarations:
                if item.start() >= call.start():
                    break
                decl = item
            if decl is None:
                raise ValueError(f"no computed target expression for ICALL(t) in f_{parent:08x}")
            if decl.start() in handled_targets:
                continue
            handled_targets.add(decl.start())
            next_decl = TARGET_DECL.search(body, decl.end())
            block_end = min(call.start(), next_decl.start()) if next_decl else call.start()
            block = body[decl.end():block_end]
            local_target = any(int(case.group(1), 16) == target for case in CASE_TARGET.finditer(block))
            local_target = local_target or bool(re.search(
                rf"\bt\s*==\s*0x{target:x}u", block, re.IGNORECASE))
            if decl.start() not in declaration_sites:
                raise ValueError(f"cannot associate target expression with guest IP in f_{parent:08x}")
            site, mechanism = declaration_sites[decl.start()]
            if local_target:
                # The target already has a local block label. Report the branch
                # at target evaluation, before its switch takes the local goto.
                offset = match.start() + decl.end()
                edits.append((offset, offset,
                    f' br::runtime::traceGuestDispatchLocal(c, t, 0x{parent:08x}u, '
                    f'0x{site:08x}u, "{mechanism}");'))
                route = "local-block"
            else:
                replacement = (f'BR_ICALL_AT(t, 0x{parent:08x}u, 0x{site:08x}u, '
                               f'"{mechanism}")')
                edits.append((match.start() + call.start(), match.start() + call.end(), replacement))
                route = "guest-dispatch"
            row = {"guest_ip": f"0x{site:08X}", "mechanism": mechanism, "route": route}
            if row not in sites:
                sites.append(row)
        for begin, finish, value in sorted(edits, key=lambda edit: edit[0], reverse=True):
            source = source[:begin] + value + source[finish:]
        include = '#include "guest_dispatch_diagnostics.h"'
        if include not in source:
            anchor = '#include "lifted_decl.h"'
            if anchor not in source:
                raise ValueError("generated translation unit lacks lifted_decl.h include")
            source = source.replace(anchor, anchor + "\n" + include, 1)
        return source, sites
    raise ValueError(f"parent function f_{parent:08x} was not found in generated source")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--parent", type=lambda value: int(value, 0), default=0x006A1240)
    parser.add_argument("--target", type=lambda value: int(value, 0), default=0x006A13A0)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    if args.input_dir.resolve() == args.output_dir.resolve():
        parser.error("diagnostic overlay must be separate from the reusable generated cache")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    found = None
    for path in sorted(args.input_dir.glob("lifted_*.cpp")):
        text = path.read_text(encoding="utf-8")
        if not re.search(rf"^void f_{args.parent:08x}\(CPU\* c\)", text, re.MULTILINE | re.IGNORECASE):
            continue
        patched, sites = instrument_source(text, args.parent, args.target)
        destination = args.output_dir / path.name
        destination.write_text(patched, encoding="utf-8", newline="")
        found = {"source": path.name, "overlay": str(destination), "parent": f"0x{args.parent:08X}",
                 "target": f"0x{args.target:08X}", "sites": sites}
        break
    if found is None:
        raise SystemExit(f"parent f_{args.parent:08x} not found under {args.input_dir}")
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(found, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(found, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
