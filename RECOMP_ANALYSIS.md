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

At pinned upstream commit `b46328a6ec6f52ce20db61a73e9f7aab3c5b3b90`, the inspected files show these mechanisms:

- `tools/analyze.cpp` maps PE sections/imports, starts from the PE entry, follows direct calls, conditional branches and tail jumps, identifies absolute IAT references and a limited indexed jump-table pattern, and scans aligned non-code section dwords for code pointers such as vtables/callbacks. Immediate-after-terminator and aligned gap-fill recovery add heuristic starts. It reports discovery and unresolved-flow statistics; these starts should retain confidence labels rather than be treated as confirmed functions.
- `tools/lift.cpp` decodes 32-bit x86 with Capstone, emits per-function C++, computes register summaries and register/flag liveness, models stack/register hand-off across calls, handles x87/SSE paths, preserves resolvable indirect call/tail dispatch, and emits explicit unsupported diagnostics.
- `tools/lift_pe.cpp` reads PE32 sections, resolves `_setjmp3` IAT/thunks, runs register summaries, emits split `lifted_N.cpp` files and a dispatch table, and reports function/instruction/unsupported coverage by mnemonic.
- `runtime/guest.cpp`, `cpu.h`, and `pe_loader.cpp` reserve a 4-GiB guest-address window, commit guest regions on demand, keep guest addresses 32-bit, and represent x86 integer/SIMD/x87 state explicitly. The loader maps PE32 headers/sections, binds imports to guest stubs, and initializes TLS directory metadata. `ICALL` caches the last lifted target per generated call site. These are the useful architectural parallels for BattleRealms' existing 32-bit guest model; the local BattleRealms runtime remains the owner of its behavior.
- `runtime/kernel32.cpp` creates independent host threads with a separate guest CPU, stack, TEB and TLS before invoking guest entrypoints; it also tracks waitables and per-thread wait diagnostics. Path, locale, Steam and other game-specific API choices must not be transplanted as Battle Realms semantics.
- `runtime/rt.cpp` keeps bounded per-thread function/caller rings, fatal dumps, periodic samples, and call/wait diagnostics. `runtime/launch.cpp` installs alternate signal stacks, logs faults, and starts a progress watchdog. These diagnostics patterns are reusable without adopting RA3 game assumptions.
- `runtime/imports.cpp` registers name/ordinal handlers with explicit x86 stack-pop counts and data imports. `runtime/d3d9.cpp` is RA3 D3D9-specific and does not implement Battle Realms D3D7. `runtime/glrec.cpp` serializes GL commands on a render thread; this is a design reference only where DDraw/D3D7 ordering and synchronization remain correct. `runtime/input.cpp` maps Android events into Windows messages and is game-control-specific.
- The live `.github/workflows/android.yml` uses GitHub-hosted Ubuntu, builds Capstone and analyzer/lifter from pinned inputs, fetches the game from a private release, emits 96 C++ partitions, configures Gradle/Android, caches compiler objects with ccache, and updates nightly/stable releases. The BattleRealms workflow adapts this while requiring private game inputs and a private APK output target; public caching is restricted to public toolchain objects.
- `tests/run_tests.py` assembles x86 snippets, executes a 32-bit native Linux reference, and compares lifted C++ registers, flags, memory, and stack. GitHub run 45862794138 passed 580/580; this validates generic lifter cases, not Battle Realms behavior.

BattleRealms already reuses RA3-style generated execution/runtime infrastructure locally, so replacing the lifter would duplicate working code. Adaptations add safety/preflight gates, the generic differential job, and keep game-derived object caches and APKs out of public caches/artifacts. The existing BattleRealms analysis remains the authority for game-specific discovery and runtime semantics.

BattleRealms already reuses RA3-style generated execution/runtime infrastructure locally, so replacing the lifter would duplicate working code. Adaptations made here add safety/preflight gates and keep game-derived object caches and APKs out of a public repository. The generic call/PE analysis behavior remains governed by the existing BattleRealms tools and should not be replaced solely to imitate RA3.

## Cloud limitations

The workflow's configured end-to-end build stages are not yet verifiable from this remote because it does not contain the required tools, runtime, Android project, tests, or private input/output configuration. The only completed remote CI result is a clean secret/file audit and an explicit build skip.
