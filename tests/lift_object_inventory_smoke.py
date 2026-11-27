#!/usr/bin/env python3
"""Regression coverage for generated-source to ARM64 object inventory checks."""
from pathlib import Path
import json
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from write_lift_object_inventory import make_inventory, verify_inventory_objects


def put(path: Path, value: str = "object") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")
    return path


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="br-object-inventory-") as temporary:
        root = Path(temporary)
        generated = root / "generated"
        main_src = generated / "main96_hybrid"
        script_src = generated / "scripts" / "rebased_batch96" / "script_a"
        runtime_src = generated / "runtime_roots"
        overlay = generated / "dispatch_trace_overlay"
        original = put(main_src / "lifted_0.cpp", "original body")
        put(main_src / "lifted_1.cpp", "other body")
        overlay_file = put(overlay / "lifted_0.cpp", "BR_ICALL_AT diagnostic body")
        put(script_src / "lifted_0.cpp")
        put(runtime_src / "lifted_0.cpp")
        dispatch_manifest = put(root / "dispatch.json", json.dumps({
            "source": original.name, "overlay": str(overlay_file),
            "parent": "0x006A1240", "target": "0x006A13A0", "sites": []}))
        runtime_manifest = put(root / "runtime.json", json.dumps({
            "roots": ["0x006A12DC", "0x006A13AC"]}))
        main_objects = root / "main-objects"
        script_objects = root / "script-objects"
        put(main_objects / "main96_hybrid" / "lifted_1.cpp.o")
        put(main_objects / "dispatch_trace_overlay" / "lifted_0.cpp.o")
        put(script_objects / "scripts" / "rebased_batch96" / "script_a" / "lifted_0.cpp.o")
        put(script_objects / "runtime_roots" / "lifted_0.cpp.o")
        table_object = put(root / "lifted_table_combined.o")
        output = root / "inventory.json"

        result = make_inventory(
            generated_root=generated, main_dir=main_src, scripts_dir=generated / "scripts",
            runtime_dir=runtime_src, overlay_dir=overlay, dispatch_manifest=dispatch_manifest,
            runtime_manifest=runtime_manifest, output=output)
        check(result["main_object_count"] == 2, "overlay must replace, not add to, main objects")
        check(result["script_object_count"] == 2, "incremental root TU must be counted as an additive shard")
        check(result["diagnostic_replacement"]["replacement"] == "dispatch_trace_overlay/lifted_0.cpp",
              "replacement source must be recorded in the inventory")
        result = verify_inventory_objects(output, main_objects, script_objects, table_object)
        check(result["objects_verified"], "compiled objects must match the source manifest")
        check(json.loads(output.read_text(encoding="utf-8"))["passed"], "inventory manifest must be persisted")

        put(script_objects / "runtime_roots" / "stale.o")
        try:
            verify_inventory_objects(output, main_objects, script_objects, table_object)
        except ValueError as exc:
            check("unexpected" in str(exc), "unexpected/stale objects must be diagnosed")
        else:
            raise AssertionError("stale compiled objects must fail inventory validation")
    print("lift object inventory smoke passed: overlay replacement and additive root shard are validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
