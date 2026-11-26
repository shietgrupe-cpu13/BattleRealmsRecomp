# BattleRealmsRecomp

This repository contains the recompilation tools, native compatibility runtime,
Android launcher sources, and host smoke tests. It intentionally excludes the
proprietary game executable and assets, generated lifted game code, downloaded
third-party checkouts, and local build products.

The source safety workflow runs on this public repository. Actual PE lifting,
ARM64 compilation, and APK packaging must run from the companion private builder
repository so game-specific code, detailed logs, and APK artifacts are not exposed
through public Actions logs or artifacts. The private builder fetches this source
tree and uses owner-supplied legally obtained game inputs.

See [BUILD_STATUS.md](BUILD_STATUS.md), [AI_HANDOFF.md](AI_HANDOFF.md),
[COMPATIBILITY_MATRIX.md](COMPATIBILITY_MATRIX.md), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
