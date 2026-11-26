# AI handoff — BattleRealmsRecomp cloud build migration

Updated 2026-11-27 (Asia/Shanghai).

## Current repository state

- Public repository: `shietgrupe-cpu13/BattleRealmsRecomp`.
- Work branch: `ci/android-static-recompilation`.
- The branch has documentation-only follow-ups after tested workflow commit `cdf461d`.
- The original local workspace at `E:\BattleRealmsRecomp` has no `.git` directory and was not pushed. The public remote initially contained only one README commit.
- No Battle Realms binaries, assets, generated lifted files, credentials, or third-party implementation code were uploaded.

## Implemented cloud workflow

`.github/workflows/android-recomp.yml` runs a full-history secret scan, a proprietary-file policy check, and a source-tree preflight. With complete source present, it fetches game inputs from a private release, verifies the known executable hash, compiles pinned Capstone/RA3 analyzer and lifter tools, runs the main and script lift drivers, compiles 96 main and 544 script ARM64 object partitions, runs the required host test entry point, and invokes Gradle for the debug APK. Guest-derived outputs are not cached. Diagnostics are downloadable from this public repository; APKs and the pinned RA3 GPL source archive publish only to a separately configured private release repository. A game-file-free upstream lifter differential job runs independently. Nightly builds use the schedule; stable builds use `v*` tags.

## Actual build evidence

- Commit: `e187ef087889262b18b2c7e359c4d04652b52a32` (workflow and initial status note).
- Push run: [45851858872](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851858872).
- Manual run: [45851897234](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851897234).
- Differential run: [45862794138](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45862794138), commit `43ea086`.
- All runs passed secret scanning and the tracked-file safety check. The differential run also passed RA3-Native's x86-vs-lifted suite: 580/580 cases, with both result streams 1,224,960 bytes. This is generic lifter validation, not Battle Realms execution.
- Run [45864439082](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45864439082) passed the audit but failed differential setup: a restored Capstone cache occupied the source clone directory. The cache/build path collision has been corrected with an isolated build directory and new cache key; a successful rerun is pending.
- Runs [45864829031](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45864829031) and [45865017509](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45865017509) pass after that correction. Both pass 580/580 generic RA3 differential cases. The latter reports 18 ccache hits and 18 misses (50% of 36 calls); no wall-time benefit was measured. Both skip the BattleRealms APK job because the public repo still lacks project source.
- All runs skipped the Battle Realms APK job because this public repository has no recompilation/runtime/Android source tree. No Battle Realms APK or generated game code was produced.
- The latest run has no BattleRealms runtime milestone: only the safety audit and generic RA3 lifter tests executed. The Android step summary explicitly reports the missing source preflight and confirms that no game files, generated lift output or APK were fetched or published.
- Local process inspection found no active BattleRealms-related compile or recompilation process to stop.

## Remaining work

1. Import a reviewed, license-compliant source snapshot into this repository; keep game data and lifted output private.
2. Add `tests/run_host_tests.sh`; it is now a required gate for ARM64/APK builds.
3. Validate the full cloud pipeline against that source snapshot. Current runs cover safety and upstream generic instruction tests only.
4. Configure the owner-controlled private game-input repo/release and read-only token.
5. Configure a separate private APK output repo/token. Never publish the APK as an artifact of this public repository.
6. Complete GPL-3.0 compliance for the RA3-Native runtime included in generated builds.

## Reference

The live RA3-Native Android workflow at commit `b46328a6ec6f52ce20db61a73e9f7aab3c5b3b90` compiles Capstone and the analyzer/lifter, obtains its game binary from a private release, performs bulk lifting, builds an Android APK, and uses ccache. Its license is GPL-3.0. This BattleRealms workflow keeps lifted outputs and APKs out of public caches/artifacts. ccache is restricted to public toolchain builds; sccache is not applicable to the current C++/Java codebase.
