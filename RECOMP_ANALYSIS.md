# BattleRealmsRecomp recompilation analysis

## Scope and evidence

This note separates the local source audit from the new, mostly empty GitHub repository. The local `E:\BattleRealmsRecomp` directory contains the ongoing project but is not a Git checkout; the remote originally contained only a README. No local source tree was copied to GitHub in this change.

## Existing local pipeline

- `tools/recompile.py` uses PE metadata plus the RA3 analyzer/lifter executables, preserves PE entry/export roots alongside heuristic starts, writes an evidence-tagged function catalog, generates split C++, and records unsupported-instruction/lift statistics.
- `tools/recompile_scripts_rebased.py`, `rebase_script_dlls.py`, `lift_scripts.py`, and `merge_lifted_tables.py` handle script DLL relocation, lifting, and a combined dispatch table.
- `tools/lifted/CMakeLists.txt` compiles split generated C++ object libraries and exposes an optional `BR_USE_CCACHE` launcher.
- Android native CMake expects 96 main object partitions, 544 script object partitions, and the combined table object before it links `libbrnative.so`. Gradle config targets arm64-v8a, Android 29+, compile SDK 34, and NDK 28.2.13676358.
- The actual local workspace has materially more functionality than the public remote. CI must not be presented as a build of that local state until a safe source snapshot is committed.

## RA3-Native technique comparison

The current upstream workflow performs a private-input pipeline: fetch a game binary from a private release, build Capstone and the C++ analyzer/lifter, bulk-lift into split translation units, compile Android code, then package an APK. Its source is GPL-3.0. The reusable mechanisms are immutable toolchain pins, split-function translation units, source-derived function catalogs, build-path separation, full CI logs, and short-lived private game input handling.

BattleRealms already reuses RA3-style generated execution/runtime infrastructure locally, so replacing the lifter would duplicate working code. Adaptations made here add safety/preflight gates and keep game-derived object caches and APKs out of a public repository. The generic call/PE analysis behavior remains governed by the existing BattleRealms tools and should not be replaced solely to imitate RA3.

## Cloud limitations

The workflow's configured end-to-end build stages are not yet verifiable from this remote because it does not contain the required tools, runtime, Android project, tests, or private input/output configuration. The only completed remote CI result is a clean secret/file audit and an explicit build skip.
