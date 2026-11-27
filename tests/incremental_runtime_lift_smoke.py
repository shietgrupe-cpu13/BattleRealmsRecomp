#!/usr/bin/env python3
"""Exercise deterministic runtime-root delta generation and cache invalidation."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import lift_runtime_roots as incremental


class FakeSection:
    IMAGE_SCN_MEM_EXECUTE = 0x20000000

    def contains_rva(self, rva: int) -> bool:
        return 0x1000 <= rva < 0x2000


class FakePe:
    OPTIONAL_HEADER = SimpleNamespace(ImageBase=0x00400000)
    sections = [FakeSection()]


class FakeResult:
    returncode = 0
    stdout = b"patched 0 instructions in 1 files"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="br-incremental-lift-") as temporary:
        root = Path(temporary)
        exe = root / "game.exe"
        lifter = root / "lift_pe"
        patcher = root / "patch.py"
        roots_file = root / "roots.txt"
        base_dir = root / "generated" / "main96_hybrid"
        output_dir = root / "generated" / "runtime_roots"
        log = root / "analysis" / "lift.log"
        for path in (exe, lifter, patcher):
            path.write_bytes(path.name.encode("ascii"))
        base_dir.mkdir(parents=True)
        (base_dir / "lifted_decl.h").write_text(
            "#pragma once\n#include \"lifted_prelude.h\"\nvoid f_00401000(CPU*);\n",
            encoding="utf-8")
        (base_dir / "lifted_table.cpp").write_text(
            "#include \"lifted_decl.h\"\nextern const LiftedEntry lifted_table[] = {\n"
            "{0x00401000u, f_00401000},\n};\nextern const unsigned lifted_count = 1;\n",
            encoding="utf-8")
        roots_file.write_text("0x00401100\n", encoding="ascii")
        incremental.pefile.PE = lambda _path, fast_load=False: FakePe()
        calls = []

        def fake_runner(command, **_kwargs):
            calls.append(command)
            if command[0] == str(lifter):
                root_list = Path(command[2]).read_text(encoding="ascii").splitlines()
                destination = Path(command[4])
                addresses = sorted(int(value, 16) for value in root_list)
                declarations = "".join(f"void f_{address:08x}(CPU*);\n" for address in addresses)
                table_rows = "".join(f"{{0x{address:08x}u, f_{address:08x}}},\n"
                                     for address in addresses)
                bodies = "".join(f"void f_{address:08x}(CPU* c) {{ c->r[0] = 7; }}\n"
                                 for address in addresses)
                (destination / "lifted_decl.h").write_text(
                    "#pragma once\n#include \"lifted_prelude.h\"\n" + declarations,
                    encoding="utf-8")
                (destination / "lifted_table.cpp").write_text(
                    "#include \"lifted_decl.h\"\nextern const LiftedEntry lifted_table[] = {\n"
                    + table_rows + "};\nextern const unsigned lifted_count = "
                    + f"{len(addresses)};\n", encoding="utf-8")
                (destination / "lifted_0.cpp").write_text(
                    "#include \"lifted_decl.h\"\n" + bodies, encoding="utf-8")
            return FakeResult()

        first = incremental.run_incremental_lift(exe, lifter, roots_file, base_dir,
                                                 output_dir, patcher, log, runner=fake_runner)
        check(first["cache"] == "miss", "first run must generate a delta")
        first_table = (output_dir / "lifted_table.cpp").read_text(encoding="utf-8")
        check("0x00401100u, f_00401100" in first_table, "delta table must register the observed root")
        check("../main96_hybrid/lifted_decl.h" in
              (output_dir / "lifted_decl.h").read_text(encoding="utf-8"),
              "delta header must reuse base declarations")

        second = incremental.run_incremental_lift(exe, lifter, roots_file, base_dir,
                                                  output_dir, patcher, log, runner=fake_runner)
        check(second["cache"] == "hit", "identical inputs must reuse generated delta")
        check(len(calls) == 2, "cache hit must skip both lifter and compatibility patcher")

        roots_file.write_text("0x00401100\n0x00401200\n", encoding="ascii")
        third = incremental.run_incremental_lift(exe, lifter, roots_file, base_dir,
                                                 output_dir, patcher, log, runner=fake_runner)
        check(third["cache"] == "miss", "a new root must invalidate only the delta cache")
        check("0x00401200u, f_00401200" in
              (output_dir / "lifted_table.cpp").read_text(encoding="utf-8"),
              "new root must appear in the regenerated delta table")
        check(len(calls) == 4, "new root must rerun the small lifter/patcher pair")

        source = output_dir / "lifted_0.cpp"
        source.write_text(source.read_text(encoding="utf-8") + "// corrupt cache\n", encoding="utf-8")
        fourth = incremental.run_incremental_lift(exe, lifter, roots_file, base_dir,
                                                  output_dir, patcher, log, runner=fake_runner)
        check(fourth["cache"] == "miss", "generated-source corruption must invalidate the delta cache")
        check(len(calls) == 6, "corrupt output must be regenerated")

    print("incremental runtime-root lift/cache smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
