#!/usr/bin/env python3
"""Run the checked-out RA3-Native analyzer/lifter and add PE export roots."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import pefile


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run_logged(args: list[str], log: Path, env: dict[str, str], cwd: Path) -> str:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as out:
        p = subprocess.run(args, cwd=cwd, env=env, stdout=out,
                           stderr=subprocess.STDOUT, check=False)
    if p.returncode:
        raise RuntimeError(f"command failed ({p.returncode}): {' '.join(args)}; see {log}")
    return log.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True, help="read-only game PE")
    ap.add_argument("--analyze", type=Path, required=True, help="RA3 analyze executable")
    ap.add_argument("--lift", type=Path, required=True, help="RA3 lift_pe executable")
    ap.add_argument("--workspace", type=Path, required=True, help="all outputs go here")
    ap.add_argument("--units", type=int, default=96)
    ap.add_argument("--pcrecomp-catalog", type=Path, help="optional PCRecomp disasm32 JSON; only entry_kind=start is merged")
    ns = ap.parse_args()
    exe, analyzer, lifter, ws = (p.resolve() for p in (ns.exe, ns.analyze, ns.lift, ns.workspace))
    if ns.units < 1:
        ap.error("--units must be positive")
    if not exe.is_file() or not analyzer.is_file() or not lifter.is_file():
        ap.error("exe, analyzer, and lifter must exist")

    ws.mkdir(parents=True, exist_ok=True)
    analysis = ws / "analysis"
    work = ws / "work"
    seed_mode = "hybrid" if ns.pcrecomp_catalog else "export_seeded"
    generated = ws / "generated" / f"main{ns.units}_{seed_mode}"
    tmp = ws / "tmp"
    cache = ws / "cache"
    for p in (analysis, work, generated, tmp, cache):
        p.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"TEMP": str(tmp), "TMP": str(tmp), "TMPDIR": str(tmp),
                "PIP_CACHE_DIR": str(cache / "pip"),
                "PYTHONPYCACHEPREFIX": str(cache / "pycache")})

    pe = pefile.PE(str(exe), fast_load=False)
    if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise RuntimeError("expected an x86 PE32 executable")
    base = pe.OPTIONAL_HEADER.ImageBase
    code_sections = [s for s in pe.sections if s.IMAGE_SCN_MEM_EXECUTE]
    def is_code_va(va: int) -> bool:
        rva = (va - base) & 0xFFFFFFFF
        return any(s.contains_rva(rva) for s in code_sections)

    prefix = work / "ra3_main"
    started = time.perf_counter()
    analyze_log = run_logged([str(analyzer), str(exe), str(prefix)],
                             analysis / "ra3_analyze.log", env, ws)
    ra3_list = Path(str(prefix) + ".functions.txt")
    if not ra3_list.exists():
        raise RuntimeError(f"analyzer did not produce {ra3_list}")
    ra3_candidates = {int(line, 16) for line in ra3_list.read_text().splitlines() if line.strip()}

    pcrecomp_starts: set[int] = set()
    pcrecomp_aliases = 0
    if ns.pcrecomp_catalog:
        pc = json.loads(ns.pcrecomp_catalog.resolve().read_text(encoding="utf-8"))
        if pc.get("image_base") != base:
            raise RuntimeError("PCRecomp catalog image base does not match executable")
        for item in pc.get("functions", []):
            if item.get("entry_kind") == "start":
                va = int(item["address"])
                if is_code_va(va):
                    pcrecomp_starts.add(va)
            elif item.get("entry_kind") == "alias":
                pcrecomp_aliases += 1

    entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    exports: dict[int, list[str]] = {}
    for item in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if item.address and is_code_va(base + item.address):
            name = item.name.decode("ascii", errors="replace") if item.name else f"ordinal_{item.ordinal}"
            exports.setdefault(base + item.address, []).append(name)
    export_vas = set(exports)
    roots = set(export_vas)
    if is_code_va(entry):
        roots.add(entry)
    all_funcs = sorted(ra3_candidates | roots | pcrecomp_starts)
    added_exports = export_vas - (ra3_candidates | pcrecomp_starts)
    # Strong PE metadata roots and heuristic RA3 discoveries remain explicitly separate.
    catalog = {
        "schema": 1,
        "input": str(exe),
        "input_sha256": sha256(exe),
        "image_base": base,
        "entrypoint_va": entry,
        "entrypoint_is_executable": is_code_va(entry),
        "ra3_analyzer_candidates": len(ra3_candidates),
        "executable_exports_unique": len(export_vas),
        "exports_already_in_ra3_catalog": len(export_vas & ra3_candidates),
        "pcrecomp_start_candidates": len(pcrecomp_starts),
        "pcrecomp_alias_entries_excluded": pcrecomp_aliases,
        "export_roots_added": len(added_exports),
        "union_function_starts": len(all_funcs),
        "metadata_confirmed_roots": len(roots),
        "candidate_starts_without_pe_metadata": len(set(all_funcs) - roots),
        "function_starts": [
            {"va": f"0x{va:08X}",
             "evidence": (["PE entry point"] if va == entry else []) +
                         (["PE export"] if va in export_vas else []) +
                         (["RA3 analyzer candidate (mixed direct-flow and heuristics)"] if va in ra3_candidates else []) +
                         (["PCRecomp start candidate (heuristic; may be data-pointer-derived)"] if va in pcrecomp_starts else []),
             "names": exports.get(va, [])}
            for va in all_funcs
        ],
        "warning": "RA3 and PCRecomp candidates include heuristics; PE metadata roots are separately tagged and PCRecomp aliases are excluded.",
    }
    (work / "function_catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    (work / "functions.txt").write_text("".join(f"{va:08x}\n" for va in all_funcs), encoding="ascii")
    lift_started = time.perf_counter()
    lift_log = run_logged([str(lifter), str(exe), str(work / "functions.txt"),
                           "--emit-dir", str(generated), str(ns.units)],
                          analysis / f"ra3_lift_main{ns.units}_{seed_mode}.log", env, ws)
    compat_patcher = Path(__file__).with_name("patch_generated_x86_compat.py").resolve()
    compat_log_path = analysis / f"patch_generated_x86_compat_main{ns.units}_{seed_mode}.log"
    compat_log = run_logged([sys.executable, str(compat_patcher), str(generated)],
                            compat_log_path, env, ws)
    lift_elapsed = time.perf_counter() - lift_started
    generated_files = sorted(generated.glob("*"))
    generated_bytes = sum(p.stat().st_size for p in generated_files if p.is_file())
    lift_match = re.search(r"functions (\d+), fully lifted (\d+) \(([\d.]+)%\), instructions (\d+), unsupported (\d+) \(([\d.]+)%\)", lift_log)
    stats = {
        "units": ns.units,
        "generation_seconds": round(lift_elapsed, 3),
        "generated_file_count": len(generated_files),
        "generated_bytes": generated_bytes,
        "lift_summary": lift_match.groups() if lift_match else None,
        "compatibility_patch_summary": next(
            (line.strip() for line in compat_log.splitlines() if line.startswith("patched ")),
            "patched 0 instructions in 0 files",
        ),
        "analyzer_summary": next((x.strip() for x in analyze_log.splitlines() if x.startswith("functions:")), None),
    }
    (work / f"manifest_main{ns.units}_{seed_mode}.json").write_text(json.dumps({"catalog": {k: v for k, v in catalog.items() if k != "function_starts"}, "generation": stats}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"catalog": {k: v for k, v in catalog.items() if k != "function_starts"}, "generation": stats}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        raise SystemExit(1)
