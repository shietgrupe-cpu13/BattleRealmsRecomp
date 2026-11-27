#!/usr/bin/env python3
"""Rebase script DLLs into disjoint guest ranges, then lift the rebased copies.

The source Scripts directory is read-only input. Every intermediate, manifest,
log, and generated file is constrained to the supplied workspace.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def ensure_inside(path: Path, workspace: Path) -> None:
    try:
        path.resolve().relative_to(workspace.resolve())
    except ValueError as exc:
        raise ValueError(f"output escapes workspace: {path}") from exc


def run_logged(command: list[str], log: Path, env: dict[str, str], cwd: Path) -> float:
    ensure_inside(log, cwd)
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT, check=False)
    elapsed = time.perf_counter() - started
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {' '.join(command)} (see {log})")
    return elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scripts-dir", type=Path, required=True,
                        help="read-only Scripts directory from the game installation")
    parser.add_argument("--analyze", type=Path, required=True)
    parser.add_argument("--lift", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--units", type=int, default=8)
    parser.add_argument("--output-name", default="rebased_batch96")
    parser.add_argument("--main-dir", type=Path, default=Path("generated/main96_hybrid"),
                        help="existing main-EXE generated directory to combine into dispatch")
    parser.add_argument("--additional-main-dir", type=Path, action="append", default=[],
                        help="additional incremental main-PE root output to include in dispatch")
    parser.add_argument("--base-start", type=lambda value: int(value, 0), default=0x20000000)
    parser.add_argument("--base-limit", type=lambda value: int(value, 0), default=0x70000000)
    args = parser.parse_args()

    source = args.scripts_dir.resolve()
    workspace = args.workspace.resolve()
    analyzer = args.analyze.resolve()
    lifter = args.lift.resolve()
    if args.units < 1:
        parser.error("--units must be positive")
    if not source.is_dir() or not analyzer.is_file() or not lifter.is_file():
        parser.error("Scripts directory, analyzer, and lifter must exist")
    if source == workspace or workspace in source.parents:
        parser.error("the input Scripts directory cannot be inside the output workspace")
    if Path(args.output_name).name != args.output_name:
        parser.error("--output-name must be one directory name")

    work = workspace / "work"
    analysis = workspace / "analysis"
    generated_root = workspace / "generated"
    rebased = work / "rebased_scripts"
    rebase_manifest = analysis / "rebased_scripts_manifest.json"
    rebase_log = analysis / "rebase_scripts.log"
    output_manifest = analysis / "rebased_script_pipeline.json"
    for path in (work, analysis, rebased, rebase_manifest, rebase_log,
                 output_manifest, workspace / "generated" / "scripts" / args.output_name):
        ensure_inside(path, workspace)

    temp = workspace / "tmp"
    cache = workspace / "cache"
    temp.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "TEMP": str(temp), "TMP": str(temp), "TMPDIR": str(temp),
        "PYTHONPYCACHEPREFIX": str(cache / "pycache"),
        "PIP_CACHE_DIR": str(cache / "pip"),
    })

    rebase_script = Path(__file__).with_name("rebase_script_dlls.py").resolve()
    lift_script = Path(__file__).with_name("lift_scripts.py").resolve()
    rebase_command = [sys.executable, str(rebase_script), str(source), str(rebased),
                      str(rebase_manifest), "--base-start", hex(args.base_start),
                      "--base-limit", hex(args.base_limit)]
    rebase_seconds = run_logged(rebase_command, rebase_log, env, workspace)
    lift_command = [sys.executable, str(lift_script), str(rebased), str(analyzer),
                    str(lifter), str(workspace), str(args.units), args.output_name]
    lift_seconds = run_logged(lift_command,
                              analysis / f"lift_{args.output_name}.log", env, workspace)

    rebase_data = json.loads(rebase_manifest.read_text(encoding="utf-8"))
    generated_manifest = workspace / "generated" / "scripts" / args.output_name / "manifest.json"
    generated_data = json.loads(generated_manifest.read_text(encoding="utf-8"))
    rebase_by_name = {row["file"].casefold(): row for row in rebase_data["modules"]}
    modules = []
    for row in generated_data["modules"]:
        rebased_row = rebase_by_name.get(row["file"].casefold())
        modules.append({
            "file": row["file"],
            "status": row["status"],
            "original_sha256": rebased_row["original_sha256"] if rebased_row else None,
            "rebased_sha256": rebased_row["rebased_sha256"] if rebased_row else row.get("sha256"),
            "assigned_image_base": rebased_row["assigned_image_base"] if rebased_row else None,
            "highlow_relocations": rebased_row["highlow_relocations"] if rebased_row else None,
            "lift_summary": row.get("lift_summary"),
            "error": row.get("error"),
        })

    combined_dir = generated_root / "combined_rebased"
    combined_table = combined_dir / "lifted_table.cpp"
    combined_manifest = combined_dir / "manifest.json"
    merger = Path(__file__).with_name("merge_lifted_tables.py").resolve()
    main_dir = args.main_dir if args.main_dir.is_absolute() else workspace / args.main_dir
    additional_main_dirs = [path if path.is_absolute() else workspace / path
                            for path in args.additional_main_dir]
    merge_command = [sys.executable, str(merger),
                     "--generated-root", str(generated_root),
                     "--main-dir", str(main_dir.resolve()),
                     "--scripts-dir", str(generated_manifest.parent),
                     "--output", str(combined_table), "--manifest", str(combined_manifest)]
    for extra_dir in additional_main_dirs:
        merge_command.extend(["--additional-main-dir", str(extra_dir.resolve())])
    merge_seconds = run_logged(merge_command, analysis / "merge_rebased_dispatch_table.log",
                               env, workspace)
    combined_data = json.loads(combined_manifest.read_text(encoding="utf-8"))

    report = {
        "schema": 1,
        "source_scripts_directory": str(source),
        "source_is_read_only": True,
        "workspace": str(workspace),
        "rebased_directory": str(rebased),
        "rebase_manifest": str(rebase_manifest),
        "generated_manifest": str(generated_manifest),
        "combined_dispatch_table": str(combined_table),
        "combined_dispatch_manifest": str(combined_manifest),
        "units_per_module": args.units,
        "output_name": args.output_name,
        "rebase_seconds": round(rebase_seconds, 3),
        "lifting_seconds": round(lift_seconds, 3),
        "dispatch_merge_seconds": round(merge_seconds, 3),
        "valid_modules_rebased": rebase_data["valid_modules_rebased"],
        "invalid_pe_skipped": len(rebase_data["invalid_pe"]),
        "rebase_errors": len(rebase_data["errors"]),
        "generated": generated_data["generated"],
        "lift_errors": generated_data["errors"],
        "combined_dispatch_entries": combined_data["entry_count"],
        "combined_guest_addresses_unique": combined_data["unique_guest_addresses"],
        "modules": modules,
    }
    output_manifest.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "modules"}, indent=2))
    return 1 if report["rebase_errors"] or report["lift_errors"] else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
