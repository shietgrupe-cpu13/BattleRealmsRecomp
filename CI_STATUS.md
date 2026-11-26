# BattleRealmsRecomp GitHub Actions status

## Repository safety audit

Audited the entire GitHub repository history on 2026-11-26 before changing visibility. The repository contained one initial commit with a short README only (GitHub reported size 0); it contained no source tree, game executable, DLL, H2O archive, assets, generated lifted code, credentials, tokens, or third-party code. The repository was changed from private to public after this audit. The local `E:\BattleRealmsRecomp` workspace is not a Git checkout and has not been uploaded.

## Workflow implementation

`.github/workflows/android-recomp.yml` adds relevant push, pull-request, manual, nightly, and `v*` stable-tag triggers; history secret scanning; a tracked-file policy that rejects proprietary binaries/assets and generated-code directories; source preflight; a game-file-free RA3-Native differential suite; a pinned Capstone/RA3 toolchain; private input download into runner temporary storage; ARM64 split-code compilation; Gradle APK build; diagnostics artifacts; and optional APK publication only to a separate private release repository. The private APK release also includes the pinned RA3 GPL source archive. The APK job requires a Battle Realms host-test entry point.

The workflow does not cache generated guest code or APKs. ccache/Actions caches cover only public Capstone and RA3 toolchain compilation. The game input archive is fetched from a private repository at run time, hash-checked, excluded from artifacts, and removed in an always-run cleanup step. A public Actions artifact is limited to build diagnostics; APK publication is gated on a private destination.

## Current blockers

- This GitHub repository contains the workflow and status documents but not the BattleRealmsRecomp source tree. Its source preflight skips static recompilation/APK stages.
- Required BattleRealmsRecomp source files must be added without proprietary game binaries/assets or generated lifted sources.
- Configure `BR_GAME_FILES_TOKEN` (read-only token), `BR_GAME_FILES_REPO`, and `BR_GAME_FILES_RELEASE` for a private release containing `battle-realms-inputs.zip` with the matching `Battle_Realms_F.exe` and `Scripts` directory. The workflow verifies the executable SHA-256 against the known older build.
- Configure `BR_PRIVATE_ARTIFACT_TOKEN` and `BR_PRIVATE_ARTIFACT_REPO` before APK publication. Do not publish the APK to this public repository because it embeds lifted game code.
- RA3-Native is GPL-3.0. Before distributing an APK containing its runtime, provide the corresponding source and license notices required for that build. No RA3 implementation code was copied into this repository by this change.
- A unified `tests/run_host_tests.sh` is required and not yet present. The Android build stays gated until it exists and passes.

## Verification status

The remote was audited and made public. On workflow branch `ci/android-static-recompilation`, run `45862794138` passed repository safety checks and RA3's generic differential suite (580/580). The BattleRealms APK job was skipped because the source tree and host regression entry point are absent. No BattleRealms build occurred.
