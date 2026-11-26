# BattleRealmsRecomp contributor and agent rules

1. Never perform heavy local compilation or static recompilation unless explicitly requested.
2. Use GitHub Actions as the primary build environment.
3. Perform source editing, lightweight analysis, reverse engineering and code review locally.
4. Commit and push changes before triggering remote builds.
5. Retrieve and analyze GitHub Actions build logs.
6. Fix compilation failures using evidence rather than assumptions.
7. Preserve existing project progress.
8. Do not fabricate successful compilation, runtime initialization or gameplay results.
9. Keep local builds available as an optional fallback.
10. Prioritize automated, reproducible and incremental development.

## Data handling

- Keep original Battle Realms executables, DLLs, H2O archives, maps, sounds, textures, generated lifted code, and APKs containing lifted game code out of this public repository and its public Actions artifacts.
- Fetch legally obtained game inputs only from an owner-controlled private repository into ephemeral runner storage. Never place them in Actions caches.
- Publish an APK only to an explicitly configured private artifact repository.
- Review third-party licenses before linking, redistributing, or modifying runtime code. RA3-Native is GPL-3.0; satisfy its source and notice obligations for any distributed build that includes its runtime.
- Keep workflow permissions minimal and never print secrets or tokens.

## Build expectations

- Push source changes before starting Actions so logs correspond to a commit.
- Hosted builds use GitHub standard runners only. Do not configure paid/self-hosted runners.
- Cache public dependencies and open-source compiler objects only. Do not cache generated Battle Realms translation units, object files, or APKs in a public repository.
- Rust/sccache is not applicable to the current C++/Java Android project unless Rust is later introduced.
- Report compilation, runtime startup, rendering, and gameplay as separate milestones.
