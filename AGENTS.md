# BattleRealmsRecomp contributor and agent rules

1. Never perform heavy local compilation or static recompilation unless explicitly requested.
2. Use standard GitHub-hosted Actions as the primary build environment for eligible source, analysis, lifting, verification, and ARM64 work.
3. Perform source editing, lightweight analysis, reverse engineering and code review locally.
4. Commit and push changes before triggering remote builds.
5. Retrieve and analyze GitHub Actions build logs.
6. Fix compilation failures using evidence rather than assumptions.
7. Preserve existing project progress.
8. Do not fabricate successful compilation, runtime initialization or gameplay results.
9. Keep local builds available as an optional fallback.
10. Prioritize automated, reproducible and incremental development.

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
