#!/usr/bin/env python3
"""Lift runtime-discovered PE roots into a small, separately compiled TU.

The bulk executable lift is intentionally immutable when only the runtime root
list changes. The root-set fingerprint makes reuse deterministic and safe.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import pefile

from merge_lifted_tables import read_table
from runtime_roots import read_runtime_roots


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def generated_hashes(directory: Path) -> dict[str, str]:
    return {
        path.name: sha256_file(path)
        for path in sorted(directory.glob("lifted_*.cpp"))
        if path.is_file()
    } | {
        name: sha256_file(directory / name)
        for name in ("lifted_decl.h", "lifted_table.cpp")
    }


def cache_matches(output: Path, fingerprint: str) -> bool:
    manifest_path = output / "incremental_manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("fingerprint") != fingerprint:
            return False
        expected = manifest.get("generated_sha256")
        return isinstance(expected, dict) and expected == generated_hashes(output)
    except (OSError, ValueError, TypeError):
        return False


def compose_delta_header(output: Path, base: Path, roots: list[int],
                         installed_directory: Path | None = None) -> None:
    include = os.path.relpath(base / "lifted_decl.h", installed_directory or output).replace("\\", "/")
    lines = ["#pragma once\n", f'#include "{include}"\n']
    lines.extend(f"void f_{root:08x}(CPU*);\n" for root in roots)
    (output / "lifted_decl.h").write_text("".join(lines), encoding="utf-8", newline="\n")


def is_cacheable_root_output(output: Path, roots: list[int]) -> bool:
    table = output / "lifted_table.cpp"
    declaration = output / "lifted_decl.h"
    source = output / "lifted_0.cpp"
    if not table.is_file() or not declaration.is_file() or not source.is_file():
        return False
    try:
        rows = read_table(table)
    except (OSError, ValueError):
        return False
    expected = [(root, f"f_{root:08x}") for root in roots]
    if rows != expected:
        return False
    declarations = declaration.read_text(encoding="utf-8")
    body = source.read_text(encoding="utf-8")
    return all(f"void f_{root:08x}(CPU*)" in declarations and
               f"void f_{root:08x}(CPU* c)" in body for root in roots)


def run_incremental_lift(
    exe: Path,
    lifter: Path,
    roots_file: Path,
    base_dir: Path,
    output_dir: Path,
    patcher: Path,
    log_path: Path,
    *,
    runner=subprocess.run,
) -> dict[str, object]:
    for path in (exe, lifter, roots_file, patcher):
        if not path.is_file():
            raise FileNotFoundError(path)
    for name in ("lifted_decl.h", "lifted_table.cpp"):
        if not (base_dir / name).is_file():
            raise FileNotFoundError(base_dir / name)
    if output_dir.resolve().parent != base_dir.resolve().parent:
        raise ValueError("incremental output and bulk lift must be sibling directories under generated/")

    pe = pefile.PE(str(exe), fast_load=False)
    base_va = pe.OPTIONAL_HEADER.ImageBase
    executable_sections = [section for section in pe.sections if section.IMAGE_SCN_MEM_EXECUTE]

    def is_executable_va(address: int) -> bool:
        rva = (address - base_va) & 0xFFFFFFFF
        return any(section.contains_rva(rva) for section in executable_sections)

    roots = read_runtime_roots(roots_file, is_executable_va)
    base_rows = read_table(base_dir / "lifted_table.cpp")
    base_addresses = {address for address, _symbol in base_rows}
    duplicates = sorted(set(roots) & base_addresses)
    if duplicates:
        raise ValueError("runtime roots already exist in bulk dispatch table: " +
                         ", ".join(f"0x{address:08X}" for address in duplicates))

    identity = {
        "schema": 1,
        "exe_sha256": sha256_file(exe),
        "lifter_sha256": sha256_file(lifter),
        "patcher_sha256": sha256_file(patcher),
        "generator_sha256": sha256_file(Path(__file__).resolve()),
        "roots": [f"0x{address:08X}" for address in roots],
        "base_decl_sha256": sha256_file(base_dir / "lifted_decl.h"),
        "base_table_sha256": sha256_file(base_dir / "lifted_table.cpp"),
        "units": 1,
    }
    fingerprint = sha256_bytes(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode())
    if cache_matches(output_dir, fingerprint) and is_cacheable_root_output(output_dir, roots):
        return {"cache": "hit", "fingerprint": fingerprint, "roots": identity["roots"],
                "generated_bytes": sum(path.stat().st_size for path in output_dir.glob("lifted_*"))}

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="br-root-lift-", dir=output_dir.parent) as temporary:
        staged = Path(temporary) / "generated"
        staged.mkdir()
        root_list = Path(temporary) / "roots.txt"
        root_list.write_text("".join(f"{address:08x}\n" for address in roots), encoding="ascii")
        command = [str(lifter), str(exe), str(root_list), "--emit-dir", str(staged), "1"]
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("wb") as log:
            result = runner(command, stdout=log, stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            raise RuntimeError(f"root lifter exited {result.returncode}; see {log_path}")
        patch = runner([sys.executable, str(patcher), str(staged)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        if patch.returncode:
            raise RuntimeError("root compatibility patcher failed: " +
                               patch.stdout.decode("utf-8", errors="replace"))
        compose_delta_header(staged, base_dir, roots, output_dir)
        if not is_cacheable_root_output(staged, roots):
            raise RuntimeError(f"lifter output did not contain the requested roots: {identity['roots']}")
        hashes = generated_hashes(staged)
        manifest = {
            **identity,
            "fingerprint": fingerprint,
            "generation_seconds": round(time.perf_counter() - started, 3),
            "generated_sha256": hashes,
            "generated_bytes": sum((staged / name).stat().st_size for name in hashes),
        }
        (staged / "incremental_manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        output_dir.mkdir(parents=True, exist_ok=True)
        for stale in output_dir.glob("lifted_*.cpp"):
            stale.unlink()
        for name in (*hashes.keys(), "incremental_manifest.json"):
            os.replace(staged / name, output_dir / name)
    return {"cache": "miss", "fingerprint": fingerprint, "roots": identity["roots"],
            "generation_seconds": round(time.perf_counter() - started, 3),
            "generated_bytes": sum(path.stat().st_size for path in output_dir.glob("lifted_*"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--lift", type=Path, required=True)
    parser.add_argument("--roots", type=Path, required=True)
    parser.add_argument("--base-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--patcher", type=Path,
                        default=Path(__file__).with_name("patch_generated_x86_compat.py"))
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    result = run_incremental_lift(*(path.resolve() for path in
                                    (args.exe, args.lift, args.roots, args.base_dir,
                                     args.output_dir, args.patcher, args.log)))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
