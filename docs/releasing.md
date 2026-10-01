# Releasing

Releases are built on the public repository's GitHub-hosted runners by
`.github/workflows/release.yml`. Nothing is published automatically: the workflow opens a
**draft** release, and a maintainer reviews and publishes it.

## Cutting a release

1. Make sure `main` is green in CI (build and tests on every platform, the publish scan).
2. Tag the commit with a version and push the tag:

   ```text
   git tag -s v0.1.0 -m "EaW Revised v0.1.0"
   git push origin v0.1.0
   ```

3. The Release workflow runs the publish scan, builds and tests the core, builds the viewer
   extension, packages the archives, attests their build provenance and creates the draft.
4. Review the draft: the generated notes, the archives and `SHA256SUMS.txt`. Publish it by hand.

A failed run leaves no release. Fix the problem on `main`, delete the tag and tag again.

## What a release contains

For Windows x64 and Linux x64 (`tools/release/package_release.py`):

| Archive | Contents |
|---|---|
| `eaw-revised-<version>-<platform>-tools.zip` | The headless tools: `sim_headless`, `sim_bench`, `path_bench`, the asset, XML, scene, sky and unit scanners, `asset_validate`, `script_smoke`, `hud_movie_prepare` |
| `eaw-revised-<version>-<platform>-viewer.zip` | The Godot viewer project with its built GDExtension, `play-demo.cmd` / `play-demo.sh`, and the Python launcher and font extractor |
| `SHA256SUMS.txt` | The SHA-256 of every archive |

Each archive carries `LICENSE`, `THIRD_PARTY_NOTICES.md` and `README.md`. The provenance
attestations can be checked with `gh attestation verify <archive> --repo EaW-Revised/EaW-Revised`.

## What a release does not contain

- **No game data.** Players need their own installation of Empire at War with the Forces of
  Corruption expansion; the tools and the viewer read it with `--eawr-game-root` (or
  `EAWR_EAW_GAME_ROOT`). No models, textures, sounds, fonts, scripts or shader source from the
  original game are ever included.
- **No Godot.** Download the pinned standard Godot 4.7.2 binary and install Python
  3.8 or newer, then run `play-demo.cmd` (Windows) or `sh ./play-demo.sh` (Linux)
  from the extracted viewer archive. The launcher discovers or asks for the game
  and Godot paths and extracts validated fonts into its local `out/fonts/` before
  starting M2 with the lit interactive camera, audio and HUD enabled. See the
  [README](../README.md) and [the viewer guide](../apps/viewer/README.md).
  A standalone exported viewer is a later step (`apps/viewer/tools/export_package.py` already bundles
  the Godot licence texts for it).
- **No graphical test results.** GPU tests are not run on the hosted runners.
