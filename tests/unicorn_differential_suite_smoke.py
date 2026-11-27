#!/usr/bin/env python3
"""Exercise multi-case Unicorn-vs-lifted comparison orchestration."""
from pathlib import Path
import json
import sys
import tempfile
import types

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
oracle_module = types.ModuleType("unicorn_x86_oracle")
oracle_module.run = lambda *_args: {}
sys.modules["unicorn_x86_oracle"] = oracle_module
import compare_lifted_x86_block as differential


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="br-diff-suite-") as temporary:
        root = Path(temporary)
        exe, source = root / "sample.exe", root / "lifted.cpp"
        exe.write_bytes(b"oracle input placeholder")
        source.write_text("intentionally not parsed by mocked source map", encoding="utf-8")
        cases = root / "cases.json"
        cases.write_text(json.dumps({"cases": [
            {"id": "left", "case": {"address": "0x1000", "token": "a"},
             "compare": ["registers", "memory_writes", "return_address"]},
            {"id": "right", "case": {"address": "0x2000", "token": "b"},
             "compare": ["registers", "flags"]},
        ]}), encoding="utf-8")

        differential.run_oracle = lambda _exe, case: {
            "registers": {"eax": 1 if case["token"] == "a" else 2},
            "flags": {"zf": 1}, "memory_writes": [], "eip_after_return": 0x1234,
            "instruction_trace": [int(case["address"], 0)],
        }
        differential.source_addresses = lambda _source, address: [address]

        def candidate(command: list[str]):
            case_path = Path(command[command.index("--case") + 1])
            case = json.loads(case_path.read_text(encoding="utf-8"))
            return {"registers": {"eax": 1 if case["token"] == "a" else 2},
                    "flags": {"zf": 1}, "memory_writes": [], "eip_after_return": 0x1234}

        differential._run_candidate = candidate
        result = differential.compare(exe, None, cases,
            ["candidate", "--case", "{case}"], source)
        check(result["result"] == "PASS" and result["case_count"] == 2,
              "all configured cases must aggregate into a passing suite")
        check([row["id"] for row in result["cases"]] == ["left", "right"],
              "suite must preserve case identity and order")
        check("flags" not in result["cases"][0]["checks"],
              "each case must compare only its declared observable state")
        check("flags" in result["cases"][1]["checks"],
              "per-case comparisons must include explicitly requested flags")

        differential._run_candidate = lambda command: {"registers": {"eax": 99}}
        failed = differential.compare(exe, None, cases,
            ["candidate", "--case", "{case}"], source)
        check(failed["result"] == "FAIL" and all(row["result"] == "FAIL" for row in failed["cases"]),
              "one or more observable mismatches must fail the suite")
    print("Unicorn differential suite smoke passed: case templating, selected state, aggregate failure")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
