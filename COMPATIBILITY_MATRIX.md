# Compatibility and CI readiness matrix

| Area | Local project evidence | Remote/CI status | Next validation |
|---|---|---|---|
| PE analysis and function discovery | `tools/recompile.py` runs the existing RA3 analyzer and merges PE entry/export roots; heuristic candidates are tagged separately. | Source absent; not run in Actions. | Add reviewed source and compare function catalog counts to stored local baseline. |
| x86-to-C++ lifting | Existing RA3 `lift_pe` driver and BattleRealms recompile scripts generate split C++; instruction result is emitted to analysis logs. | Source and executable unavailable to this remote. | Run differential instruction tests on standard runner with private inputs; fail on unsupported opcodes. |
| PE loader / guest memory / dispatch | Local `runtime/pe32_loader` and Win32HLE/runtime sources exist; app expects combined lifted dispatch tables. | Not present in GitHub repository. | Add safe runtime source and build a host runtime smoke target. |
| Win32 compatibility | Local runtime has separate base, file, memory, sync, startup, and graphics-facing layers. Completion is partial and API presence is not proof of semantic coverage. | No handlers can be built or tested in the remote. | Run compatibility inventory and API self-tests after source import. |
| DirectDraw / Direct3D 7 | Local D3D7 renderer and regression probes exist; user-reported Android terrain/rendering and performance issues remain. | Not present in GitHub repository. | Host framebuffer golden tests, then Android test/device evidence. |
| Audio/input/filesystem | Local Android/runtime implementations and probes exist; past device reports described audio glitches and touch work remains under validation. | No source or runtime test in CI. | Add focused host tests and a documented device smoke checklist. |
| ARM64 native library | Local Android CMake links exactly 96 main partitions and 544 script partitions plus combined table. | Build job skipped by preflight; no ARM64 compile. | Verify object counts, link `libbrnative.so`, inspect ELF machine type. |
| APK | Previous local/device reports exist in project history, but this new cloud workflow produced none. | No APK artifact/release. | Build only after native link passes; publish solely to private output repo. |
| Security and licensing | RA3-Native license is GPL-3.0; source must be supplied/noticed for distributed runtime builds. | Current remote contains no third-party implementation code. | Complete license/source compliance review before distributing APK. |
