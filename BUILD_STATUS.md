# Cloud build status

Updated 2026-11-27.

## Current state

- Repository visibility: public, changed only after auditing the full one-commit history and README.
- Runner policy: GitHub-hosted `ubuntu-latest` only; no paid or self-hosted runner configured.
- Workflow branch: `ci/android-static-recompilation`.
- Commit with initial workflow: `e187ef087889262b18b2c7e359c4d04652b52a32`.
- Current public remote contents do not include BattleRealmsRecomp implementation source.

## Runs

| Run | Event | Result | Verified work |
|---|---|---|---|
| [45851858872](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851858872) | Push | Audit passed; Android job skipped | Gitleaks history scan and tracked proprietary-file policy passed. |
| [45851897234](https://github.com/shietgrupe-cpu13/BattleRealmsRecomp/actions/runs/45851897234) | Manual dispatch | Audit passed; Android job skipped | Same audit; source preflight found required source tree missing. |

No compilation, static lifting, APK, runtime launch, rendering, or gameplay was verified by these runs.

## Blockers

- Missing local project files in the public Git remote.
- No private game-input repository/token/release configured.
- No private APK output repository/token configured.
- No CI host-test entry point committed.
- GPL-3.0 source/notice obligations must be satisfied for any distributed build incorporating RA3-Native runtime.

## Cache policy

Gradle dependency caching and ccache for Capstone/RA3 public-toolchain code are configured. Generated Battle Realms translation units, object files and APKs are not cached. `sccache` is not applicable to this C++/Java project because no Rust compilation target is present.
