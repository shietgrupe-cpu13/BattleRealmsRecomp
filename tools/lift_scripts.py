#!/usr/bin/env python3
"""Analyze and bulk-lift Battle Realms script DLLs without touching the install."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pefile


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_logged(command: list[str], log: Path, env: dict[str, str], cwd: Path) -> str:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT, check=False)
    text = log.read_text(encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {' '.join(command)} (see {log})")
    return text


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit("usage: lift_scripts.py <scripts-dir> <analyzer> <lifter> <workspace> <units> <output-name>")
    scripts, analyzer, lifter, workspace = (Path(arg).resolve() for arg in sys.argv[1:5])
    units = int(sys.argv[5])
    output_name = sys.argv[6]
    if units < 1:
        raise SystemExit("units must be positive")
    if not scripts.is_dir() or not analyzer.is_file() or not lifter.is_file():
        raise SystemExit("script directory, analyzer, and lifter must exist")
    analysis = workspace / "analysis" / "scripts"
    work = workspace / "work" / "scripts"
    generated = workspace / "generated" / "scripts" / output_name
    tmp = workspace / "tmp"
    for directory in (analysis, work, generated, tmp):
        directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"TEMP": str(tmp), "TMP": str(tmp), "TMPDIR": str(tmp),
                "PYTHONPYCACHEPREFIX": str(workspace / "cache" / "pycache"),
                "PIP_CACHE_DIR": str(workspace / "cache" / "pip")})

    rows: list[dict[str, object]] = []
    candidates = sorted(scripts.glob("*.dll"), key=lambda p: p.name.casefold())
    for index, dll in enumerate(candidates, 1):
        key = re.sub(r"[^A-Za-z0-9_.-]+", "_", dll.stem)
        row: dict[str, object] = {"file": dll.name, "sha256": sha256(dll)}
        try:
            pe = pefile.PE(str(dll), fast_load=False)
            if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
                raise ValueError("not PE32/I386")
            base = pe.OPTIONAL_HEADER.ImageBase
            exec_sections = [s for s in pe.sections if s.IMAGE_SCN_MEM_EXECUTE]

            def is_code(va: int) -> bool:
                rva = va - base
                return any(s.contains_rva(rva) for s in exec_sections)

            entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
            exports: dict[int, list[str]] = {}
            for exp in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []):
                if exp.address and is_code(base + exp.address):
                    name = exp.name.decode("ascii", errors="replace") if exp.name else f"ordinal_{exp.ordinal}"
                    exports.setdefault(base + exp.address, []).append(name)

            prefix = work / key
            analyze_text = run_logged([str(analyzer), str(dll), str(prefix)],
                                      analysis / f"{key}.analyze.log", env, workspace)
            ra3_file = Path(str(prefix) + ".functions.txt")
            ra3_starts = {int(line, 16) for line in ra3_file.read_text(encoding="ascii").splitlines() if line.strip()}
            export_starts = set(exports)
            roots = export_starts | ({entry} if is_code(entry) else set())
            starts = sorted(ra3_starts | roots)
            catalog = {
                "schema": 1, "file": dll.name, "sha256": row["sha256"],
                "image_base": base, "entrypoint_va": entry,
                "entrypoint_is_executable": is_code(entry),
                "ra3_analyzer_candidates": len(ra3_starts),
                "executable_exports_unique": len(export_starts),
                "metadata_confirmed_roots": len(roots),
                "metadata_roots_added": len(roots - ra3_starts),
                "union_function_starts": len(starts),
                "function_starts": [
                    {"va": f"0x{va:08X}",
                     "evidence": (["PE entry point"] if va == entry else []) +
                                 (["PE export"] if va in export_starts else []) +
                                 (["RA3 analyzer candidate (heuristic)"] if va in ra3_starts else []),
                     "names": exports.get(va, [])}
                    for va in starts
                ],
                "warning": "Analyzer starts are heuristic; PE entry/export roots are separately tagged.",
            }
            function_list = work / f"{key}.functions.txt"
            function_list.write_text("".join(f"{va:08x}\n" for va in starts), encoding="ascii")
            out_dir = generated / key
            out_dir.mkdir(parents=True, exist_ok=True)
            lift_text = run_logged([str(lifter), str(dll), str(function_list),
                                    "--emit-dir", str(out_dir), str(units)],
                                   analysis / f"{key}.lift.log", env, workspace)
            compat_patcher = Path(__file__).with_name("patch_generated_x86_compat.py").resolve()
            compat_text = run_logged([sys.executable, str(compat_patcher), str(out_dir)],
                                     analysis / f"{key}.compat.log", env, workspace)
            match = re.search(r"functions (\d+), fully lifted (\d+) \(([\d.]+)%\), instructions (\d+), unsupported (\d+) \(([\d.]+)%\)", lift_text)
            output_files = sorted(out_dir.glob("*"))
            row.update(catalog={k: v for k, v in catalog.items() if k != "function_starts"},
                       lift_summary=match.groups() if match else None,
                       compatibility_patch_summary=next(
                           (line.strip() for line in compat_text.splitlines() if line.startswith("patched ")),
                           "patched 0 instructions in 0 files",
                       ),
                       generated_files=len(output_files),
                       generated_bytes=sum(f.stat().st_size for f in output_files if f.is_file()),
                       analyzer_summary=next((line.strip() for line in analyze_text.splitlines()
                                              if line.startswith("functions:")), None),
                       status="generated")
            (out_dir / "function_catalog.json").write_text(
                json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
        except Exception as exc:
            message = str(exc)
            row.update(status=("invalid_pe_skipped" if "DOS Header magic not found" in message else "error"),
                       error=message)
        rows.append(row)
        print(f"[{index}/{len(candidates)}] {dll.name}: {row.get('status')} "
              f"{row.get('lift_summary', row.get('error', ''))}", flush=True)

    summary = {
        "schema": 1, "scripts_directory": str(scripts), "dll_candidates": len(candidates),
        "generated": sum(row.get("status") == "generated" for row in rows),
        "invalid_pe_skipped": sum(row.get("status") == "invalid_pe_skipped" for row in rows),
        "errors": sum(row.get("status") == "error" for row in rows),
        "units_per_module": units, "modules": rows,
    }
    (generated / "manifest.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 1 if summary["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
