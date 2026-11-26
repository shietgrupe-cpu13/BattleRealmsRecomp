# Third-party software and distribution boundary

- **RA3-Native** is fetched at pinned commit
  `b46328a6ec6f52ce20db61a73e9f7aab3c5b3b90` during private builds. Its repository
  is GPL-3.0. The private builder must retain and make the corresponding source
  available alongside any distributed binary that contains its runtime code.
- **Capstone** is fetched at pinned commit
  `04fd70b5456c20fc27d1f2b62d6ed0820fc6b081` for analysis/lifting tools. Its source
  carries the license in the upstream repository; the builder does not commit a
  third-party checkout here.
- `tools/h2o_index_probe.py` is excluded because its implementation imports a
  PCRecomp-licensed PKWARE DCL decoder. `tools/BR2001_Graphics_Probe.py` is excluded
  because provenance and redistribution terms were not established in this audit.
- The BattleRealmsRecomp source files migrated here contained no per-file SPDX
  identifiers or repository license statement. No new license is asserted for that
  code; publication of source to this repository was requested by its owner.

The Battle Realms executable, script DLLs, assets, and generated lifted code are
copyrighted game-derived material. They are stored only in the owner's private
builder/input environment and are not committed to this repository. The APK and
game-specific build logs must remain private. This notice is not legal advice and
does not grant rights to redistribute the game or any derived binary.

**Release gate:** Do not publish or redistribute an APK that combines the GPL
runtime with Battle Realms-derived code until the owner has confirmed a licensing
path compatible with both components. Build artifacts from the private workflow are
for the owner's testing only.
