# BattleRealmsRecomp

This repository contains the Battle Realms recompilation tools, native compatibility
runtime, Android launcher sources, and regression tests. The original game files are
not included; players must provide their own legally obtained copy.

Public GitHub Actions is the primary CI environment for source checks, compatibility
component tests, synthetic PE32 analysis/lifting, and Android ARM64 compilation of
game-free test code. Public caches and artifacts are limited to open-source tools and
the synthetic fixture. They never contain proprietary game inputs, game-derived lifted
translation units, object files, APKs, or private runtime traces.

The full Battle Realms lift and APK remain in the restricted build path while the
private-input read credential and private output channel are not configured for a
public workflow. That path is still required for the actual 97,604-entry game build;
the public synthetic pipeline does not claim to build or run Battle Realms.

See [AI_HANDOFF.md](AI_HANDOFF.md), [BUILD_STATUS.md](BUILD_STATUS.md),
[COMPATIBILITY_MATRIX.md](COMPATIBILITY_MATRIX.md), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
