# Changelog

## 2026-11-28 — Targeted interior-dispatch trace preparation

- Inspected the existing generated `f_006a1240` without relifting: `0x006A13A0` has local routes at guest jumps `0x006A1275` and `0x006A1294`; `0x006A128D` contains a fallback `ICALL` when its local case does not match. This is candidate static cause, not a captured runtime cause.
- Added a disposable generated-TU overlay and runtime JSONL/logcat records for the exact dispatch target, guest caller/IP, parent, transfer kind and local-vs-dispatch route. The overlay did not modify the reusable generated lift.
- Expanded the Unicorn comparison command to run reusable multi-case suites. Existing PE-vs-lifted cases for 32–35 bytes passed, including registers, memory and return state (0.285669 s); local trace/differential harness tests and C++ syntax checks passed.
- Private diagnostic workflow is staged to omit `0x006A13A0` from the dispatcher and retain the two confirmed earlier targets, allowing the real fallback to be observed instead of masking it with a standalone root. GitHub Actions and Android runtime results are pending; no device is connected yet.
- `0x006A72ED` remains untouched. No gameplay or dispatch fix is claimed until a fresh trace identifies the executed site.

## 2026-11-27 - Indirect-target classification and Unicorn differential proof

- Compared the existing Ghidra export with the 34,774-entry bulk dispatch table: 13,733 of 13,743 executable Ghidra entries are registered; the 10 remaining entries are review candidates. The 1,199 broader candidate set is not auto-promoted to lift roots.
- Classified `0x006A13A0` as an interior computed-jump block inside registered `0x006A1240`; coverage output now recommends keeping it inside the parent.
- Corrected the POC to execute the original x86 parent in Unicorn. Counts 32 to 35 selected their expected tail blocks and matched lifted registers, guest-memory changes and return state. The isolated block differential and incremental/coverage smoke tests passed.
- Evidence is local under `E:\BattleRealmsRecomp\analysis`. No bulk relift, APK build, Android runtime claim, or gameplay change.

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

## 2026-11-28 — Bounded D3D7 texture diagnostics

- The latest Android log shows successful GLES draws but lacks per-texture identity/content data for the user's remaining partially black terrain. Added opt-in diagnostics that record a bounded set of unique D3D7 draw states and texture uploads, including surface identity, dimensions, pixel masks, source/converted hashes, black/transparent texel counts, and GLES upload errors.
- Diagnostics are off by default. The private diagnostic APK enables them with `-Pd3d7TextureDiagnostics=true`; normal builds do not hash pixels or emit the extra D3D events.
- Added a focused texture-probe smoke test. `bash tests/run_renderer_component_tests.sh` passed its vertex-batch, texture-probe, and MS-ADPCM tests. Android NDK `clang++ -fsyntax-only` passed with diagnostics both disabled and enabled.
- The first private APK workflow stopped in its early host build because diagnostic caller metadata was assigned outside its Android-only declaration. Guarded those assignments with `__ANDROID__`; the host `win32_startup.cpp` syntax check now passes. No lifting, ARM64 library, or APK step ran in that failed attempt.
- No terrain fix or FPS gain is claimed yet. The next evidence is a fresh Android log containing the actual texture/draw records while black patches are visible.

## 2026-11-28 — D3D7 state caching and terrain UV diagnostics

- Added a per-context GLES state cache for repeated framebuffer, viewport, program, sampler/texture, depth, blend, cull, VAO/buffer and stable uniform updates. Explicit invalidation covers texture/surface uploads, context rebinds and GPU readbacks. A focused host test covers value reuse and invalidation.
- Added `d3d_gl_state_calls_skipped` to Android performance-window diagnostics to measure cache effectiveness on-device; no FPS gain is claimed without a comparable device run.
- Extended the bounded D3D7 draw diagnostics with UV min/max and black texel counts inside the UV bounding region, and added a regression fixture with three black texels of four sampled.
- Read the latest retained device log: black-heavy source surfaces have zero GLES upload errors and are submitted by the 0x0064ABD1/FVF 0x1D2 path; this does not yet identify why terrain pixels render black. Compared 218 128x128 RGB TGA entries from the E: Grasslands archive against the logged 256x256 RGB565 source hashes (raw and nearest 2x / 5551→565 candidates); no match.
- `bash tests/run_renderer_component_tests.sh` passed all four focused components. NDK 28.2 syntax-only checks passed for the GLES backend with diagnostics off/on and for Android `brnative.cpp`; host syntax-only passed for `win32_startup.cpp`. No APK or device verification was run.
- CI run [46121999357](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/46121999357) passed on `diag/android-terrain-texture-evidence` at `e8fe246`: source safety, tool checks, dispatch tests, and D3D7 renderer component regressions passed. No APK was produced.
- Private ARM64/APK workflow [46122150616](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp-Private-Build/actions/runs/46122150616) succeeded for `diag/android-terrain-texture-evidence`. The newest Drive device log (uploaded 08:27 UTC) records three Play attempts entering `0x006A2441`, then failing at `0x006A1294` when its indirect jump to `0x006A13A0` is misrouted through guest dispatch. No rendered frame, D3D draw, or performance window is present; terrain/FPS changes are therefore unverified on device.
