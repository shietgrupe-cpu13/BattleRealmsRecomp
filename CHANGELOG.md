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
