#!/usr/bin/env python3
"""Exercise BattleRealms' public lift pipeline on a synthetic, non-game PE32."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import sys
import time


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(args: list[str], *, cwd: Path, log: Path | None = None) -> str:
    try:
        result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, check=True)
    except subprocess.CalledProcessError as error:
        if log:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text((error.stdout or "") + (error.stderr or ""), encoding="utf-8")
        details = ""
        if log and log.is_file():
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-120:]
            details = "\n" + "\n".join(tail)
        raise RuntimeError(
            f"command failed ({error.returncode}): {args[0]}"
            + (f"; see {log}" if log else "") + details
        ) from error
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
    return result.stdout


def invoke_recompile(root: Path, exe: Path, workspace: Path, analyze: Path, lift: Path) -> dict:
    output = run(
        [
            sys.executable,
            str(root / "tools" / "recompile.py"),
            "--exe", str(exe),
            "--analyze", str(analyze),
            "--lift", str(lift),
            "--workspace", str(workspace),
            "--units", "4",
            "--reuse-generated",
        ],
        cwd=root,
        log=workspace / "analysis" / "public_lift_pipeline.log",
    )
    try:
        return json.loads(output)
    except json.JSONDecodeError as error:
        raise RuntimeError("recompile.py did not emit its JSON summary") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analyze", type=Path, required=True)
    parser.add_argument("--lift", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--expect-cache-hit", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    workspace = args.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    fixture_src = root / "tests" / "fixtures" / "public_lift_fixture.c"
    exe = workspace / "public_lift_fixture.exe"
    obj = workspace / "public_lift_fixture.obj"
    compile_log = workspace / "analysis" / "fixture_compile.log"
    link_log = workspace / "analysis" / "fixture_link.log"

    run(
        [
            "clang-cl", "--target=i686-pc-windows-msvc", "/Brepro", "/O1", "/GS-", "/nologo",
            "/c", str(fixture_src), f"/Fo{obj}",
        ],
        cwd=root,
        log=compile_log,
    )
    run(
        [
            "lld-link", "/nologo", "/machine:x86", "/subsystem:console",
            "/nodefaultlib", "/Brepro", "/entry:fixture_entry", f"/out:{exe}", str(obj),
        ],
        cwd=root,
        log=link_log,
    )
    if not exe.is_file():
        raise RuntimeError("lld-link completed without producing the synthetic PE32")
    first_fixture_hash = sha256(exe)
    # Repeating identical deterministic tool invocations must yield the same PE.
    run(
        ["clang-cl", "--target=i686-pc-windows-msvc", "/Brepro", "/O1", "/GS-", "/nologo",
         "/c", str(fixture_src), f"/Fo{obj}"],
        cwd=root, log=workspace / "analysis" / "fixture_compile_repeat.log",
    )
    run(
        ["lld-link", "/nologo", "/machine:x86", "/subsystem:console", "/nodefaultlib",
         "/Brepro", "/entry:fixture_entry", f"/out:{exe}", str(obj)],
        cwd=root, log=workspace / "analysis" / "fixture_link_repeat.log",
    )
    if sha256(exe) != first_fixture_hash:
        raise RuntimeError("synthetic PE32 fixture is not byte-reproducible across identical tool invocations")

    try:
        import pefile
    except ImportError as error:
        raise RuntimeError("install tools/requirements-ci.txt before this smoke") from error
    pe = pefile.PE(str(exe), fast_load=False)
    if pe.FILE_HEADER.Machine != 0x14C or pe.OPTIONAL_HEADER.Magic != 0x10B:
        raise RuntimeError("fixture is not a PE32 x86 executable")
    if not pe.OPTIONAL_HEADER.AddressOfEntryPoint:
        raise RuntimeError("fixture has no entry point")

    generated = workspace / "generated" / "main4_export_seeded"
    started = time.perf_counter()
    first = invoke_recompile(root, exe, workspace, args.analyze.resolve(), args.lift.resolve())
    first_seconds = time.perf_counter() - started
    first_status = first.get("generation", {}).get("generation_cache")
    if first_status not in {"miss", "hit"}:
        raise RuntimeError(f"unexpected initial generation-cache status: {first_status!r}")
    if args.expect_cache_hit and first_status != "hit":
        raise RuntimeError("Actions restored the exact generated-source cache, but recompile.py rejected it")
    if not generated.is_dir() or not list(generated.glob("lifted_*.cpp")):
        raise RuntimeError("lifter produced no generated translation units")
    catalog_path = workspace / "work" / "function_catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if not catalog.get("entrypoint_is_executable") or catalog.get("union_function_starts", 0) < 1:
        raise RuntimeError("analyzer did not retain the executable entry point")

    outputs_before = {
        path.name: sha256(path)
        for path in sorted(generated.iterdir())
        if path.is_file() and path.name != "generation_cache.json"
    }
    started = time.perf_counter()
    second = invoke_recompile(root, exe, workspace, args.analyze.resolve(), args.lift.resolve())
    second_seconds = time.perf_counter() - started
    if second.get("generation", {}).get("generation_cache") != "hit":
        raise RuntimeError("unchanged synthetic input did not reuse content-addressed lift output")
    outputs_after = {
        path.name: sha256(path)
        for path in sorted(generated.iterdir())
        if path.is_file() and path.name != "generation_cache.json"
    }
    if outputs_after != outputs_before:
        raise RuntimeError("cached deterministic output changed on the unchanged input")

    # Seed a second workspace with the first output, then change one fixture
    # instruction. The content identity must invalidate the copied generation.
    variant_workspace = workspace / "invalidation_probe"
    variant_generated = variant_workspace / "generated" / "main4_export_seeded"
    variant_generated.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(generated, variant_generated)
    variant_src = variant_workspace / "public_lift_fixture_variant.c"
    source_text = fixture_src.read_text(encoding="utf-8")
    variant_text = source_text.replace("0x35", "0x36")
    if variant_text == source_text:
        raise RuntimeError("could not form the cache-invalidation fixture variant")
    variant_src.write_text(variant_text, encoding="utf-8")
    variant_obj = variant_workspace / "public_lift_fixture_variant.obj"
    variant_exe = variant_workspace / "public_lift_fixture_variant.exe"
    run(
        ["clang-cl", "--target=i686-pc-windows-msvc", "/Brepro", "/O1", "/GS-", "/nologo",
         "/c", str(variant_src), f"/Fo{variant_obj}"],
        cwd=root, log=variant_workspace / "analysis" / "fixture_compile.log",
    )
    run(
        ["lld-link", "/nologo", "/machine:x86", "/subsystem:console", "/nodefaultlib",
         "/Brepro", "/entry:fixture_entry", f"/out:{variant_exe}", str(variant_obj)],
        cwd=root, log=variant_workspace / "analysis" / "fixture_link.log",
    )
    invalidated = invoke_recompile(root, variant_exe, variant_workspace, args.analyze.resolve(), args.lift.resolve())
    if invalidated.get("generation", {}).get("generation_cache") != "miss":
        raise RuntimeError("changed PE32 input incorrectly reused stale generated code")
    if sha256(variant_exe) == sha256(exe):
        raise RuntimeError("cache invalidation fixture did not change the PE input hash")

    report = {
        "scope": "synthetic PE32 fixture only; no Battle Realms inputs",
        "fixture_sha256": sha256(exe),
        "fixture_reproducible": True,
        "entrypoint_rva": f"0x{pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}",
        "function_starts": catalog["union_function_starts"],
        "generated_translation_units": len(outputs_after),
        "generated_bytes": sum((generated / name).stat().st_size for name in outputs_after),
        "first_cache_status": first_status,
        "first_run_seconds": round(first_seconds, 3),
        "first_lift_generation_seconds": first["generation"]["generation_seconds"],
        "unchanged_cache_status": second["generation"]["generation_cache"],
        "unchanged_lift_generation_seconds": second["generation"]["generation_seconds"],
        "unchanged_run_seconds": round(second_seconds, 3),
        "deterministic_outputs": True,
        "changed_input_cache_status": invalidated["generation"]["generation_cache"],
    }
    (workspace / "public_lift_pipeline_report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
