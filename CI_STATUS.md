# BattleRealmsRecomp GitHub Actions status

## Repository safety audit

Audited the entire GitHub repository history on 2026-11-26 before changing visibility. The repository contained one initial commit with a short README only (GitHub reported size 0); it contained no source tree, game executable, DLL, H2O archive, assets, generated lifted code, credentials, tokens, or third-party code. The repository was changed from private to public after this audit. The local `E:\BattleRealmsRecomp` workspace is not a Git checkout and has not been uploaded.

## Workflow implementation

`.github/workflows/android-recomp.yml` adds push, pull-request, manual, and nightly triggers; history secret scanning; a tracked-file policy that rejects proprietary binaries/assets and generated-code directories; source preflight; a pinned RA3-Native/Capstone analysis and lift toolchain; private game-input download into runner temporary storage; ARM64 split-code compilation; Gradle APK build; non-game diagnostics artifact upload; and optional APK publication only to a separately configured private release repository.

The workflow does not cache generated guest code or APKs. The game input archive is fetched from a private repository at run time, hash-checked, excluded from artifacts, and removed in an always-run cleanup step. A public Actions artifact is limited to diagnostics; APK publication is gated on a private destination.

## Current blockers

- This GitHub repository currently contains only its initial README. The workflow's source preflight will therefore skip the build; a successful static recompilation or APK build cannot be claimed yet.
- Required BattleRealmsRecomp source files must be added without proprietary game binaries/assets or generated lifted sources.
- Configure `BR_GAME_FILES_TOKEN` (read-only token), `BR_GAME_FILES_REPO`, and `BR_GAME_FILES_RELEASE` for a private release containing `battle-realms-inputs.zip` with the matching `Battle_Realms_F.exe` and `Scripts` directory. The workflow verifies the executable SHA-256 against the known older build.
- Configure `BR_PRIVATE_ARTIFACT_TOKEN` and `BR_PRIVATE_ARTIFACT_REPO` before APK publication. Do not publish the APK to this public repository because it embeds lifted game code.
- RA3-Native is GPL-3.0. Before distributing an APK containing its runtime, provide the corresponding source and license notices required for that build. No RA3 implementation code was copied into this repository by this change.
- A unified host regression command is not currently present in the public repo. The workflow will use `tests/run_host_tests.sh` or a `host-tests` CMake preset when supplied; otherwise it reports that generated ARM64 compilation was the available build validation.

## Verification status

The remote was audited and made public. The Actions workflow is on branch `ci/android-static-recompilation`; a run must still be triggered and inspected. Since the source tree is absent, the expected immediate result is a successful safety/preflight job and a skipped Android build, not a game build.
