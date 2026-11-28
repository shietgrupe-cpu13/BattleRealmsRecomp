# Cloud build status

Updated 2026-11-27.

## Superseding renderer change — 2026-11-28

- Source commit `658e4e2249383c808bbd6b8399fee46dd66c51ea` passed public Actions run [46144218543](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46144218543). Public source/renderer regressions passed; the game-free synthetic ARM64 runtime/lift pipeline passed in 1m35s.
- The run did not fetch Battle Realms inputs, compile the full game lift, package an APK, or verify device FPS/rendering. No APK hash exists for this change yet.
- Merge-branch public run [46144785040](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46144785040) passed at `371ff443bbe9c4ac050fa48a28fd9dccd62f0b0f`.
- Latest stored private APK is from successful public run [46137256407](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46137256407), source commit `dafedbcc7f7859eb295d875852de760ea680fb73`, SHA-256 `8834c7290f2dd3a0bd6c349b1c8a07c969cc45c45c622a9cf580b03180539663`, size 84,584,451 bytes. It is not device-verified and predates the GLES batching/diagnostic changes.
- Protected-input run [46144965297](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46144965297) passed input hash, host tests, lift, ARM64 object inventory and APK packaging, but failed at private artifact storage because `GH_TOKEN` was not exported for `gh api meta`. No artifact is available and the runner copy is gone.
- Backport `c3b1200` adds the already-main-merged PR #3 `GH_TOKEN` fix to both private upload steps. After public CI and merge, rerun once; the next run must confirm the deploy-key push and provide a private APK SHA-256 before it is considered delivered.

## Superseding status on `android-new`

- `android-new` is based on `ci/android-static-recompilation` at `c35e552`; `main` is untouched. The public repository now includes the reviewed runtime/Android source snapshot and build metadata. The public source-safety workflow passed on `c35e552`; that is not an ARM64/APK or gameplay result.
- The private builder is outside this task's authorization and was not modified or triggered. Its most recent available attempt predates this branch and stopped at the missing lifted CMake recipe; `tools/lifted/CMakeLists.txt` is now in the public source snapshot. A fresh BattleRealms cloud build is still unverified.
- Existing private APK: `E:\BattleRealmsRecomp\work\installed_battlerealms.apk` (192,614,175 bytes, 2026-11-26). Saved `work\battle_realms_device_trace.txt` records guest entry at `0x006a2441` and first frame 1.684 s after guest start. This is historical device evidence, not a current install or proof of correct skirmish rendering.
- Saved foreground windows in `work\after_play_logcat.txt` show 12.17–13.29 presented FPS, 75.3–82.3 ms average frame time, 66.3–72.1 ms guest CPU/frame, 41.5–41.8 ms GPU batch-queue time/frame and 11.5–12.4 ms framebuffer conversion/frame. They are not a controlled before/after comparison and are not labeled as skirmish evidence.
- This branch changes compatible GLES batch accumulation to geometric capacity growth. Its focused test passed locally and in Actions run [45959175529](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45959175529), retaining 3,000 vertices with at most eight capacity changes. Device impact remains unmeasured.
- Relevant pushes to `android-new` now enter the public source-safety/component workflow. That workflow cannot access game inputs or emit a game-derived APK.
- Audio investigation: the retained 2026-11-27 phone trace shows AAudio callback duration 22–40 µs on average, zero xruns and zero deadline overruns. It records 44.1 kHz mono/stereo MS-ADPCM streams and up to 8‰ hard-clipped output samples in a later window. The public source branch now tests and fixes a decoder delta-state narrowing defect; no new APK or audible device result exists yet.
- Commit `515dfda` passed source checks and both D3D7/audio component tests in GitHub Actions [45964415836](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45964415836). This did not compile the ARM64 library, package an APK, or verify sound on Android.

## Current state

- Repository visibility: public, changed only after auditing the full one-commit history and README.
- Runner policy: GitHub-hosted `ubuntu-latest` only; no paid or self-hosted runner configured.
- Workflow branch: `ci/android-static-recompilation`.
- Latest workflow code tested: `cdf461d80ad7ca1f5174134dbe935a2a7a7579ae` (separates the Capstone source and build-cache paths). The branch also contains documentation-only follow-ups after that tested revision; initial workflow commit: `e187ef087889262b18b2c7e359c4d04652b52a32`.
- Current public remote contents do not include BattleRealmsRecomp implementation source.

## Runs

| Run | Event | Result | Verified work |
|---|---|---|---|
| [45851858872](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851858872) | Push | Audit passed; Android job skipped | Gitleaks history scan and tracked proprietary-file policy passed. |
| [45851897234](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851897234) | Manual dispatch | Audit passed; Android job skipped | Same audit; source preflight found required source tree missing. |
| [45862794138](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45862794138) | Manual dispatch | Safety and upstream differential passed; APK job skipped | Capstone built; RA3-Native x86-vs-lifted suite passed 580/580; both result streams 1,224,960 bytes. |
| [45864439082](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45864439082) | Manual dispatch | Differential setup failed; audit passed; APK job skipped | Restored Capstone cache occupied the source-clone destination (`git clone` exit 128). The build directory and v2 cache key are now separated; rerun pending. |
| [45864829031](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45864829031) | Manual dispatch | Safety and upstream differential passed; APK job skipped | Cache-path fix passed; RA3-Native differential suite passed 580/580. APK skipped because BattleRealms source is absent. |
| [45865017509](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45865017509) | Manual dispatch | Safety and upstream differential passed; APK job skipped | Repeat run passed 580/580, equal result streams of 1,224,960 bytes; ccache reported 18/36 (50%) hits. This is hit-count evidence only; no wall-time improvement was measured. |
| [45868770190](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45868770190) | Push | Safety and upstream differential passed; APK job skipped | Follow-up workflow change passed the safety audit and 580/580 generic differential cases; the BattleRealms source preflight is still false. |

The safety job passed. The Android preflight still reported the source tree missing and skipped the ARM64/APK job. No BattleRealms-specific compilation/lifting, APK, runtime launch, rendering, or gameplay was verified. The 580-case test validates the shared generic lifter suite only.

## Blockers

- Missing local project files in the public Git remote.
- No private game-input repository/token/release configured.
- No private APK output repository/token configured.
- No BattleRealms CI host-test entry point (`tests/run_host_tests.sh`) committed; this is now required before APK work can start.
- GPL-3.0 source/notice obligations must be satisfied for any distributed build incorporating RA3-Native runtime.

## Cache policy

Gradle dependency caching and ccache for Capstone/RA3 public-toolchain code are configured. The first differential run had 18 misses and no hits; the repeat reported 18 hits and 18 misses. This confirms cache reuse but is not a timing benchmark, so no speedup is claimed. Generated BattleRealms translation units, object files and APKs are not cached. `sccache` is not applicable to this C++/Java project because no Rust compilation target is present.
# Source migration checkpoint — 2026-11-27

## 2026-11-28 update — dispatch build provenance

- Private run [46110825730](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp-Private-Build/actions/runs/46110825730) on `codex/backward-jump-table-diagnostic` built an APK whose object inventory confirms the patched `dispatch_trace_overlay/lifted_45.cpp.o` replaced the stale bulk TU. The run's Unicorn differential passed 8/8 original-x86 comparisons, and its manifest classifies `0x006A1294` as a local block. Candidate APK SHA-256: `ee6ba90e04d2c297ea6d88baf657e78242102a78c734407fa72efeaf6be93202`.
- Newer private run [46122150616](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp-Private-Build/actions/runs/46122150616) used older private `main` (`d5221dace72c2329972ba59d602121bb1fd614e3`) and reported guest dispatch at the same site. Neither APK has been verified on-device in this checkpoint.
- Public run [46130808048](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46130808048) verifies only the game-free synthetic ARM64 pipeline and cache reuse; it did not build a Battle Realms APK.

- The public repository now carries the BattleRealms runtime, Android launcher and
  renderer sources, recompile/rebase tools, and host smoke-test inputs. The 1.32 GB
  generated tree, 3.4 GB Android build outputs, downloaded references/toolchains,
  local diagnostics, game executable, script DLLs, and assets remain excluded.
- The public workflow is restricted to source-boundary and source-tool checks. It
  no longer runs the game executable, uploads logs derived from the executable, or
  publishes an APK to public artifacts.
- An actual BattleRealms ARM64/APK cloud build has **not yet been verified**. It must
  run from a private builder repository so executable-derived code, logs, and APK
  remain private. The next build result must be reported from that private workflow.
- Distribution remains blocked pending owner review of GPL-3.0 RA3 runtime obligations
  and the proprietary game's terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
