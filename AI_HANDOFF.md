# AI handoff — BattleRealmsRecomp cloud build migration

Updated 2026-11-27 (Asia/Shanghai).

## Current repository state

- Public repository: `shietgrupe-cpu13/BattleRealmsRecomp`.
- Work branch: `ci/android-static-recompilation`.
- The original local workspace at `E:\BattleRealmsRecomp` has no `.git` directory and was not pushed. The public remote initially contained only one README commit.
- No Battle Realms binaries, assets, generated lifted files, credentials, or third-party implementation code were uploaded.

## Implemented cloud workflow

`.github/workflows/android-recomp.yml` runs a full-history secret scan, a proprietary-file policy check, and a source-tree preflight. With complete source present, it is configured to fetch game inputs from a private release, verify the known executable hash, compile pinned Capstone/RA3 analyzer and lifter tools, run the existing main and script lift drivers, compile 96 main and 544 script ARM64 object partitions, run the declared host test entry point if present, and invoke Gradle for the debug APK. Guest-derived outputs are not cached. The APK destination is a separate private release repository; public artifacts are restricted to diagnostics.

## Actual build evidence

- Commit: `e187ef087889262b18b2c7e359c4d04652b52a32` (workflow and initial status note).
- Push run: [45851858872](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851858872).
- Manual run: [45851897234](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851897234).
- Both runs passed secret scanning and the tracked-file safety check. Both skipped the Android build because this public repository has no recompilation/runtime/Android source tree. No APK or generated code was produced.
- Local process inspection found no active BattleRealms-related compile or recompilation process to stop.

## Remaining work

1. Import a reviewed, license-compliant source snapshot into this repository; keep game data and lifted output private.
2. Validate the cloud pipeline against that actual source snapshot. The current green workflow result is a safety/preflight result only.
3. Configure the owner-controlled private game-input repo/release and read-only token.
4. Configure a separate private APK output repo/token. Never publish the APK as an artifact of this public repository.
5. Establish a host instruction/runtime regression test command. Do not describe APK compilation as game execution.
6. Complete GPL-3.0 compliance for the RA3-Native runtime included in generated builds.

## Reference

The live RA3-Native Android workflow at commit `b46328a6ec6f52ce20db61a73e9f7aab3c5b3b90` compiles Capstone and the analyzer/lifter, obtains its game binary from a private release, performs bulk lifting, builds an Android APK, and uses ccache. Its license is GPL-3.0. This BattleRealms workflow reuses the process shape but keeps lifted outputs and APKs out of public caches/artifacts.
