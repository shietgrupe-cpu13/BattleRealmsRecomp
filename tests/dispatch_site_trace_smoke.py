#!/usr/bin/env python3
"""Check diagnostic overlays report exact guest sites without editing cached output."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from trace_generated_dispatch_sites import instrument_source


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    fixture = '''#include "lifted_decl.h"
void f_00401234(CPU* c)
{
  /* 00401234 jmp dword ptr [eax*4 + 0x00405000] */
  { uint32_t t = G32(eax * 4 + 0x405000u);
    switch (t) {
    case 0x00401240u: goto L_00401240;
    default: break; }
    SAVE(); ICALL(t); return; }
L_00401240:;
}
void f_00401250(CPU* c) {}
'''
    generated, sites = instrument_source(fixture, 0x00401234, 0x00401240)
    check(len(sites) == 1, "expected one computed transfer site")
    check(sites[0]["guest_ip"] == "0x00401234", "must capture the exact x86 instruction IP")
    check(sites[0]["route"] == "local-block", "known interior block must remain local")
    check("traceGuestDispatchLocal(c, t, 0x00401234u, 0x00401234u" in generated,
          "local route must emit parent and guest IP")
    check("BR_ICALL_AT" not in generated, "a known local block must not be routed through dispatcher")

    fallback = fixture.replace("case 0x00401240u: goto L_00401240;", "case 0x00409999u: goto L_00409999;")
    generated, sites = instrument_source(fallback, 0x00401234, 0x00401240)
    check(sites[0]["route"] == "guest-dispatch", "missing local target must remain an explicit dispatch route")
    check("BR_ICALL_AT(t, 0x00401234u, 0x00401234u, \"indirect-jmp\")" in generated,
          "fallback must preserve exact parent, instruction IP and mechanism")
    check('#include "guest_dispatch_diagnostics.h"' in generated,
          "overlay must include the dispatch trace macro")

    with tempfile.TemporaryDirectory(prefix="br-dispatch-trace-") as temporary:
        root = Path(temporary)
        (root / "base.cpp").write_text(fixture, encoding="utf-8")
        overlay = root / "overlay.cpp"
        overlay.write_text(generated, encoding="utf-8")
        check((root / "base.cpp").read_text(encoding="utf-8") == fixture,
              "instrumentation must not mutate the reusable generated source")
    print("dispatch site trace smoke passed: local routing and dispatcher fallback are distinguished")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
