# BattleRealmsRecomp contributor and agent rules

1. Never perform heavy local compilation or static recompilation unless explicitly requested.
2. Use standard GitHub-hosted Actions as the primary build environment for eligible source, analysis, lifting, verification, and ARM64 work.
3. Perform source editing, lightweight analysis, reverse engineering and code review locally.
4. Commit and push changes before triggering remote builds.
5. Retrieve and analyze GitHub Actions build logs.
6. Fix compilation failures using evidence rather than assumptions.
7. Preserve existing project progress.
8. Do not fabricate successful compilation, runtime initialization or gameplay results.
9. Do not run local compilation, full regression suites, Gradle, APK packaging, or full relifts; use GitHub Actions unless the user explicitly requests a local build.
10. Prioritize automated, reproducible and incremental development.

## Mandatory workspace and storage

- The only local BattleRealmsRecomp workspace is `E:\BattleRealmsRecomp`.
- Keep all project files, temporary files, logs, analysis outputs, compiler scratch files, caches, generated code and build outputs on E:.
- Before any local tool that can write temporary data, set `TEMP`, `TMP` and `TMPDIR` to an E: task directory. Set compiler and tool cache variables (`CCACHE_DIR`, `GRADLE_USER_HOME`, `ANDROID_USER_HOME`, and equivalent) to E: when those tools are used.
- Never use C: or D: for project-generated files. The original `D:\Games\Battle Realms Complete` installation is strictly read-only.
- Inspect the exact E: destination before any cleanup. On C:, remove only a specific temporary artifact whose creation by this project has been confirmed. Never clear Windows, WSL, application or personal data to satisfy this rule.
- Local work is limited to editing, lightweight inspection, analysis, Git operations and device diagnostics. Run tests, compilation, lifting and APK packaging in GitHub Actions.

## Renderer and performance acceptance

- Treat terrain correctness and FPS as parallel goals. Do not defer performance until all gameplay works, and do not trade image correctness for speed.
- Trace `0x0064ABA5` and `0x0064ABD1` to the exact D3D7 methods, guest state, surfaces and texture inputs before changing rendering behavior. Determine whether black pixels are transparent, uninitialized, converted incorrectly or produced by guest state; do not hide them with a speculative shader workaround.
- A GLES batching equivalence change must preserve guest draw order, blending, depth, texture stages, surface identity, viewport and all backend-consumed state. Keep a regression that proves unused state coalesces and visible state still splits batches.
- Profile draw calls and batch fragmentation, state changes, texture conversion/uploads, shader and texture binding, guest dispatch, allocations/copies, JNI, audio, frame pacing, presentation and diagnostics. Keep diagnostic overhead bounded and optional.
- Compare identical map, camera, resolution, unit count and duration. Report average FPS, 1% low, median/P95/P99 frame time, CPU/GPU time, draws, batches, uploads, state changes, dispatch counts and diagnostic overhead where captured. Do not claim a gain without same-device comparable measurements; target stable 60 FPS where hardware permits.
- If the terrain root is not yet proven, proceed with justified performance changes and narrowly targeted diagnostics in one consolidated device build rather than blocking it on speculative rendering work.

## GitHub Actions release path

- Public Actions run game-independent source, dispatch, synthetic-lifting and renderer regressions, and compile eligible public ARM64 runtime components. Public jobs must not receive proprietary inputs or restricted artifacts.
- Keep game-input processing and game-derived outputs on protected private infrastructure until the authorized public-runner/private-input flow is verified. Do not expose proprietary files, generated game code, credentials or private diagnostics in public logs, caches, artifacts or releases.
- After relevant public checks pass, run one consolidated private-input APK build that includes all verified fixes. Verify workflow revision, input identity, generated-object inventory, source revisions, APK hash and ARM64 packaging. Avoid private builds for documentation-only edits and cancel obsolete work.
- Preserve the known-good backward-jump-table correction from private APK #24 and its provenance checks in future APKs. Keep logical fixes independently identifiable and reversible.
- Keep the Android diagnostic profile low overhead; include FPS, frame-time percentiles, guest/render timing, draw/batch counts, uploads, state-change skips, diagnostic sampling limits, memory/audio status and build identity.

## Data handling

- Keep original Battle Realms executables, DLLs, H2O archives, maps, sounds, textures, generated lifted game code, compiled objects containing lifted code, and APKs containing game code out of the public repository and public Actions caches, artifacts, logs, and releases.
- Public Actions may run the analyzer/lifter and ARM64 compiler on synthetic or otherwise public fixtures. Public cache keys and artifacts must be restricted to those public inputs and their outputs.
- The complete Battle Realms lift remains in the restricted workflow until a least-privilege read credential for private inputs and a private destination for generated code/APK outputs are configured and verified. Never fall back to a public artifact or cache.
- Public PR workflows must not receive private-input credentials. Restrict any future secret-bearing game build to trusted refs and actors, use sparse private-input checkout with persisted credentials disabled, suppress game-derived command output, and send only to private storage.
- Fetch legally obtained game inputs only from the owner-controlled private repository. Never place them in public Actions caches.
- Review third-party licenses before linking, redistributing, or modifying runtime code. RA3-Native is GPL-3.0; satisfy its source and notice obligations for any distributed build that includes its runtime.
- Keep workflow permissions minimal and never print secrets or tokens.

## Build expectations

- Public standard-hosted runners validate the public source and game-free portions of the full native recompilation pipeline.
- Restricted Battle Realms input processing and derived outputs stay private until a secure public-runner transfer path has been configured and tested.
- Cache keys must include compiler/NDK identity, pinned tool revisions, relevant flags, and the content identities of the public fixtures. Do not cache private generated code in a public repository.
- Rust/sccache is not applicable to this C++/Java Android project unless Rust is later introduced.
- Report analysis, lifting, compilation, APK packaging, Android runtime, rendering, and gameplay as separate milestones.
