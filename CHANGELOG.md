# Changelog

## 2026-11-27 — GitHub Actions cloud build bootstrap

- Audited the new remote's complete history; it contained only one README commit, no game data, credentials, source, or third-party implementation code.
- Changed repository visibility to public after that audit.
- Added `ci/android-static-recompilation` with a hosted-runner Android recompilation workflow, full-history secret scan, proprietary-file guard, source preflight, private input/output gates, pinned toolchain plan, split ARM64 compilation, Gradle packaging, diagnostics, and cancellations.
- Added contributor rules and handoff/analysis/compatibility/plan/build documents.
- Added a game-file-free RA3 x86-vs-lifted instruction differential job; it passed 580/580 cases with equal 1,224,960-byte native/lifted result streams.
- Added verified RA3 PE-loader, CPU-state, discovery, dispatch, threading, and diagnostics findings; RA3 D3D9 and game-specific API policies are marked as non-transferable.
- CI safety audits passed; the BattleRealms Android build stage was skipped because the public remote lacks source and `tests/run_host_tests.sh`. No BattleRealms APK was made.
- Follow-up run 45864439082 caught Capstone cache data colliding with the source-clone path; separated the cache directory and versioned its key. Rerun required.
- Fixed the Capstone source/build directory collision by storing the build cache separately and linking it into the expected source tree. Runs 45864829031 and 45865017509 passed the RA3 generic differential suite (580/580); run 45865017509 recorded 18/36 ccache hits. No timing gain is claimed.
- Latest full workflow run passed safety and differential jobs, but skipped the BattleRealms ARM64/APK job because required source files and `tests/run_host_tests.sh` are absent from the public repo. No BattleRealms APK was generated remotely. A separately existing local APK was handed off through the owner's private project Drive folder; it is not a product of this cloud workflow.
- Workflow preflight guard now permits only `tools/lifted/CMakeLists.txt` within the otherwise-prohibited `lifted` path family, so the required build recipe will not conflict with the generated-code safety rule. Push run 45868770190 passed the safety scan and 580/580 generic lifter tests; no BattleRealms source build ran.

## 2026-11-27 — Batch growth and branch trigger

- Replaced exact-size reserve-on-every-append in GLES compatible-draw batches with geometric vertex capacity growth. Regression passed: 1,000 triangle appends preserved 3,000 vertices with at most eight capacity changes. No device timing comparison yet.
- Added `android-new` to relevant-push workflow triggers and a renderer component test step. These checks do not lift or execute the Battle Realms binary.
- Actions run [45959175529](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45959175529) passed the full public source-safety job and the new allocation regression. No ARM64/APK or game behavior was tested.
- Reconciled stale source-migration status: public branch now contains runtime/launcher/tools/tests/build metadata; private builder remains untouched and no new APK was produced.

## 2026-11-27 — MS-ADPCM state-width fix

- Device audio logs from the retained skirmish attempt show stable AAudio callbacks (0 xruns / 0 deadline overruns) while Battle Realms requests 44.1 kHz mono/stereo MS-ADPCM conversion. Aggregate hard clipping rose to 8‰ in one late window, but that alone does not explain the audible artifact.
- Found signed-16 truncation of the adaptive MS-ADPCM delta after each decoded nibble. Decoder references keep the evolving step state wide; wraparound can invert or collapse the delta on later samples. Moved the production nibble decoder into `runtime/ms_adpcm.h` and retained 32-bit delta state with overflow-safe adaptation.
- Added a focused high-delta PCM regression to `tests/ms_adpcm_decoder_smoke.cpp` and ran `bash tests/run_renderer_component_tests.sh`: D3D7 batch regression and MS-ADPCM regression both passed. This checks arithmetic, not real asset/audio output.
- Pushed as `515dfda`; GitHub Actions [45964415836](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45964415836) passed source safety, Python tools compile, D3D7 batch regression and MS-ADPCM decoder regression. The workflow does not build the private game APK.
- Next verification is Android audio playback with a new authorized build and stream-level diagnostics. No new APK or device result is claimed by this change.
