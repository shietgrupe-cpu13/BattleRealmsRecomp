# Implementation plan

1. **Create a safe source snapshot.** Add only source/tooling/runtime/Android code after checking licenses. Exclude executables, DLLs, H2O archives, maps, art/audio, generated lifted C++, generated objects, and signing keys.
2. **Make BattleRealms host tests runnable in CI.** Add `tests/run_host_tests.sh` for PE mapping/dispatch, Win32HLE, D3D7 software output, and audio/input probes. RA3's generic 580-case x86 differential suite already runs separately; it does not establish BattleRealms behavior.
3. **Configure private game inputs.** Place legally obtained exe and required `Scripts` DLLs in an owner-controlled private repository release; grant the workflow a read-only token. Confirm file hashes and input versions.
4. **Validate the lift pipeline.** Confirm Capstone and pinned RA3 tools build on Ubuntu; run the main EXE and script DLL lifters; preserve catalogs/logs; reject unsupported instruction growth beyond a reviewed threshold.
5. **Validate ARM64 object compilation.** Build the 96 main and 544 script partitions incrementally using NDK 28.2.13676358; link runtime and combined dispatch table; inspect ELF ABI and symbols.
6. **Build debug APK.** Run Gradle only after host tests and ARM64 linking pass. Keep game-derived code out of caches and public artifacts.
7. **Publish privately and test.** Upload APK to a distinct private output repo; install/test on Android; report menu, skirmish, graphics, input, audio, and performance independently.
8. **Enable nightly releases only after repeated clean builds.** Use a private destination and a reproducible versioning policy. Stable releases require explicit release authority and license review.

## Current stop point

Step 1 is not complete: GitHub contains the bootstrap README, workflow, and status documents, but not the project source. The pinned RA3 differential suite passes 580/580; the BattleRealms build remains skipped until source, its host-test entry point, and private game inputs are available.
