#!/usr/bin/env python3
"""Validate generated translation units against compiled lift objects."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def source_files(directory: Path) -> list[Path]:
    return sorted((path for path in directory.rglob("lifted_*.cpp")
                   if path.name != "lifted_table.cpp"), key=lambda path: path.as_posix().casefold())


def object_suffix(source: Path, generated_root: Path, overlay_dir: Path | None = None,
                  replaced_source: Path | None = None) -> str:
    if replaced_source is not None and source.resolve() == replaced_source.resolve():
        assert overlay_dir is not None
        overlay_source = overlay_dir / source.name
        return overlay_source.relative_to(generated_root).as_posix() + ".o"
    return source.relative_to(generated_root).as_posix() + ".o"


def verify_objects(expected_suffixes: list[str], object_root: Path, label: str) -> None:
    objects = sorted((path for path in object_root.rglob("*.o")
                      if path.name != "lifted_table.cpp.o"), key=lambda path: path.as_posix().casefold())
    unmatched = list(objects)
    missing: list[str] = []
    duplicate: list[str] = []
    for suffix in expected_suffixes:
        hits = [path for path in unmatched if path.as_posix().replace("\\", "/").endswith(suffix)]
        if not hits:
            missing.append(suffix)
        elif len(hits) > 1:
            duplicate.append(suffix)
        else:
            unmatched.remove(hits[0])
    if missing or duplicate or unmatched:
        raise ValueError(
            f"{label} object inventory mismatch: expected={len(expected_suffixes)} "
            f"actual={len(objects)} missing={missing[:5]} duplicate={duplicate[:5]} "
            f"unexpected={[path.as_posix() for path in unmatched[:5]]}")


def make_inventory(*, generated_root: Path, main_dir: Path, scripts_dir: Path,
                   runtime_dir: Path, overlay_dir: Path, dispatch_manifest: Path,
                   runtime_manifest: Path, output: Path) -> dict[str, object]:
    manifest = json.loads(dispatch_manifest.read_text(encoding="utf-8"))
    if manifest.get("parent", "").lower() != "0x006a1240" or \
            manifest.get("target", "").lower() != "0x006a13a0":
        raise ValueError("dispatch overlay manifest does not identify the requested parent/target")
    source_name = manifest.get("source")
    if not isinstance(source_name, str) or Path(source_name).name != source_name:
        raise ValueError("dispatch overlay manifest has an invalid source name")
    replaced_source = main_dir / source_name
    overlay_source = overlay_dir / source_name
    if not replaced_source.is_file() or not overlay_source.is_file():
        raise ValueError("dispatch overlay must have both its original and instrumented source")
    if replaced_source.read_bytes() == overlay_source.read_bytes():
        raise ValueError("dispatch overlay source is identical to the cached original")
    overlay_text = overlay_source.read_text(encoding="utf-8")
    if "BR_ICALL_AT" not in overlay_text and "traceGuestDispatchLocal" not in overlay_text:
        raise ValueError("dispatch overlay source has no instrumentation call")

    main_sources = source_files(main_dir)
    if replaced_source.resolve() not in {path.resolve() for path in main_sources}:
        raise ValueError("dispatch replacement source is not part of the main generated source set")
    expected_main = [object_suffix(source, generated_root, overlay_dir, replaced_source)
                     for source in main_sources]
    script_sources = source_files(scripts_dir) + source_files(runtime_dir)
    expected_scripts = [object_suffix(source, generated_root) for source in script_sources]
    if len(set(expected_main)) != len(expected_main) or len(set(expected_scripts)) != len(expected_scripts):
        raise ValueError("generated source inventory contains duplicate object destinations")

    runtime_data = json.loads(runtime_manifest.read_text(encoding="utf-8"))
    runtime_roots = runtime_data.get("roots")
    if not isinstance(runtime_roots, list) or not runtime_roots:
        raise ValueError("incremental runtime-root manifest is missing its root list")
    if "0x006A13A0".lower() in {str(root).lower() for root in runtime_roots}:
        raise ValueError("interior target 0x006A13A0 must not be registered as a standalone root")

    result: dict[str, object] = {
        "schema": 1,
        "passed": True,
        "objects_verified": False,
        "main_object_count": len(expected_main),
        "script_object_count": len(expected_scripts),
        "main_objects": expected_main,
        "script_objects": expected_scripts,
        "diagnostic_replacement": {
            "original": replaced_source.relative_to(generated_root).as_posix(),
            "replacement": overlay_source.relative_to(generated_root).as_posix(),
            "parent": manifest["parent"],
            "target": manifest["target"],
            "sites": manifest.get("sites", []),
        },
        "incremental_roots": runtime_roots,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def verify_inventory_objects(inventory_path: Path, main_objects: Path, script_objects: Path,
                            table_object: Path) -> dict[str, object]:
    result = json.loads(inventory_path.read_text(encoding="utf-8"))
    if result.get("schema") != 1 or result.get("passed") is not True:
        raise ValueError("source inventory has not passed validation")
    verify_objects(result["main_objects"], main_objects, "main")
    verify_objects(result["script_objects"], script_objects, "script/runtime-root")
    if not table_object.is_file():
        raise ValueError(f"combined dispatch-table object is missing: {table_object}")
    result["combined_table_object"] = table_object.name
    result["objects_verified"] = True
    inventory_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    write = subparsers.add_parser("write", help="derive the expected object inventory from lifted sources")
    for name in ("generated-root", "main-dir", "scripts-dir", "runtime-dir", "overlay-dir",
                 "dispatch-manifest", "runtime-manifest", "output"):
        write.add_argument("--" + name, type=Path, required=True)
    verify = subparsers.add_parser("verify", help="compare compiled objects with the source inventory")
    for name in ("inventory", "main-objects", "script-objects", "table-object"):
        verify.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "write":
            result = make_inventory(
                generated_root=args.generated_root.resolve(), main_dir=args.main_dir.resolve(),
                scripts_dir=args.scripts_dir.resolve(), runtime_dir=args.runtime_dir.resolve(),
                overlay_dir=args.overlay_dir.resolve(), dispatch_manifest=args.dispatch_manifest.resolve(),
                runtime_manifest=args.runtime_manifest.resolve(), output=args.output.resolve())
        else:
            result = verify_inventory_objects(
                args.inventory.resolve(), args.main_objects.resolve(), args.script_objects.resolve(),
                args.table_object.resolve())
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"lift object inventory failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({key: result[key] for key in (
        "passed", "objects_verified", "main_object_count", "script_object_count", "diagnostic_replacement",
        "incremental_roots")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
