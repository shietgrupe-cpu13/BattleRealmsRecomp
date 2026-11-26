# BattleRealmsRecomp CI bootstrap

This public repository currently contains CI configuration and handoff documents only. It does not contain the Battle Realms game, assets, generated lifted code, or the local recompilation source tree.

The Android workflow runs a repository secret/data audit and source preflight. Its full ARM64 recompilation and APK stages remain gated until a reviewed source snapshot and owner-controlled private game-input/output repositories are configured. APKs that contain lifted game code must only be published to a private destination.

See [BUILD_STATUS.md](BUILD_STATUS.md), [AI_HANDOFF.md](AI_HANDOFF.md), and [AGENTS.md](AGENTS.md).
