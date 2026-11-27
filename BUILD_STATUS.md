# Cloud build status

Updated 2026-11-27.

## Superseding status on `android-new`

- `android-new` is based on `ci/android-static-recompilation` at `c35e552`; `main` is untouched. The public repository now includes the reviewed runtime/Android source snapshot and build metadata. The public source-safety workflow passed on `c35e552`; that is not an ARM64/APK or gameplay result.
- The private builder is outside this task's authorization and was not modified or triggered. Its most recent available attempt predates this branch and stopped at the missing lifted CMake recipe; `tools/lifted/CMakeLists.txt` is now in the public source snapshot. A fresh BattleRealms cloud build is still unverified.
- Existing private APK: `E:\BattleRealmsRecomp\work\installed_battlerealms.apk` (192,614,175 bytes, 2026-11-26). Saved `work\battle_realms_device_trace.txt` records guest entry at `0x006a2441` and first frame 1.684 s after guest start. This is historical device evidence, not a current install or proof of correct skirmish rendering.
- Saved foreground windows in `work\after_play_logcat.txt` show 12.17–13.29 presented FPS, 75.3–82.3 ms average frame time, 66.3–72.1 ms guest CPU/frame, 41.5–41.8 ms GPU batch-queue time/frame and 11.5–12.4 ms framebuffer conversion/frame. They are not a controlled before/after comparison and are not labeled as skirmish evidence.
- This branch changes compatible GLES batch accumulation to geometric capacity growth. Its focused test passes with 3,000 vertices and at most eight capacity changes. Device impact remains unmeasured.
- Relevant pushes to `android-new` now enter the public source-safety/component workflow. That workflow cannot access game inputs or emit a game-derived APK.

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
