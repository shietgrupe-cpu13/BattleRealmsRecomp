# AI handoff — BattleRealmsRecomp recovery

Updated 2026-11-27 (Asia/Shanghai), branch `android-new`.

## Current state and restrictions

- Public work branch: `shietgrupe-cpu13/BattleRealmsRecomp:android-new`, created from `ci/android-static-recompilation` at `c35e552`. `main` is untouched.
- The public branch carries the reviewed runtime, Android launcher, renderer, tools, host smoke tests and lifted build metadata. Game executables/assets, generated game-derived C++, local build output, private logs and APKs remain excluded.
- Do not modify or trigger the companion private repository without explicit user authorization. Do not run heavy local builds.
- Public source-safety run `45873981727` passed on `c35e552`; this was not a BattleRealms lift, ARM64 library, APK or gameplay test.

## Latest implementation and evidence

- Fixed a missed Actions trigger: relevant pushes to `android-new` now run the source-safety/component workflow.
- Fixed compatible GLES draw-batch growth: renderer appends previously reserved exactly the next vertex count. `runtime/d3d7_vertex_batch.h` now grows geometrically and is used for both first batch creation and later compatible draws.
- `tests/d3d7_vertex_batch_smoke.cpp` exercises 1,000 triangle appends, verifies all 3,000 vertices remain, rejects invalid input, and bounds capacity changes. `bash tests/run_renderer_component_tests.sh` passed locally. New CI result is pending; device performance is unmeasured.
- Existing local APK `E:\BattleRealmsRecomp\work\installed_battlerealms.apk` is 192,614,175 bytes (2026-11-26). Saved device trace shows guest entry `0x006a2441` and first frame after 1.684 s from guest start. This is historical evidence; it is not a current install or skirmish proof.
- Saved performance windows report 12.17–13.29 FPS, 75.3–82.3 ms average frame time, 66.3–72.1 ms guest CPU/frame, 41.5–41.8 ms batch queue/frame and 11.5–12.4 ms framebuffer conversion/frame. No controlled comparison exists yet.

## Reference integration decisions

- RA3-Native is the closest x86-to-ARM64 static-lift and workflow analogue and already informs this project's lift/runtime architecture. Its D3D9 renderer is not transferable to the game's D3D7 path; GPL-3.0 distribution obligations remain unresolved.
- PCRecomp is MIT and useful for Win32HLE/runtime semantics; it is not a replacement for the existing RA3-derived lifting path.
- SC2 Android Recomp is MIT and useful for game-free validation and Android recompilation workflow. Its D3D9/Vulkan implementation does not implement Battle Realms' D3D7 fixed-function behavior.
- D7VK (Zlib) and Wine are behavioral references for D3D7/DirectDraw. D7VK's Vulkan/Wine platform assumptions and Wine's implementation/license must be reviewed before code reuse.
- web-recomp-kit has relevant DDraw/D3D2 layering and SDL3 architecture, but GitHub reports `NOASSERTION` for license metadata; no code reuse until its exact license is established.
- DxWrapper's DDrawCompat/DD7to9, XenonRecomp, and N64Recomp are reference-only for now: desktop wrapper / different ISA assumptions. Check license at file/package level before any reuse.

## Next

1. Wait for and inspect Actions on the `android-new` push; expect only source-safety and the independent renderer component test.
2. Obtain authorization before any private-builder action; then prove lifted compilation, ARM64 linking and APK packaging there.
3. Repeat the same Android skirmish scene to measure this capacity change. Independently inspect black terrain and audio clipping from current logs/captures.
4. Continue from observed first divergence; do not claim a renderer fix from successful compilation or component tests.
