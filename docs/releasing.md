# Releasing

Releases are built on the public repository's GitHub-hosted runners by
`.github/workflows/release.yml`. Nothing is published automatically: the workflow opens a
**draft** release, and a maintainer reviews and publishes it.

## Current candidate: v0.1.5

The current candidate is v0.1.5. The v0.2.0 milestone is reserved for finished
skirmish mode. Keep this candidate local until the owner approves the public sync.

## Cutting a release

1. Prepare the public snapshot through a sync pull request. Wait for its CI to pass
   before merging it into public `main`; the private source commit must match the
   tested preview. Draft tester-facing release notes describing new features,
   known issues and how to report feedback.
2. Package the candidate locally and extract the viewer archive into a fresh,
   writable folder. Run `play-demo.cmd` or `sh ./play-demo.sh` with the pinned
   Godot and an installed expansion. Verify map and faction selection, starting
   a skirmish against AI, buying a unit, and earning income from a completed mine.
   Retest the final candidate if its source commit changes.
3. Before tester releases, run an AI-versus-AI progression check. Verify that both
   sides develop their economies, advance station technology, field mixed fleets
   and engage in combat. Record the setup and seed alongside technology and fleet
   composition timelines for review.
4. Make sure `main` is green in CI (build and tests on every platform, the publish scan).
   The release workflow also scans on Windows: pinned text fixtures must retain
   their reviewed LF bytes after checkout.
5. Tag the commit with a version and push the tag:

   ```text
   git tag -s v0.1.5 -m "EaW Revised v0.1.5"
   git push origin v0.1.5
   ```

6. The Release workflow runs the publish scan, builds and tests the core, builds the viewer
   extension, packages the archives, attests their build provenance and creates the draft.
7. Review the draft: the tester-facing notes, the archives and `SHA256SUMS.txt`. Publish it by hand.

A failed run leaves no release. Fix the problem on `main`, delete the tag and tag again.

## What a release contains

For Windows x64 and Linux x64 (`tools/release/package_release.py`):

macOS support remains experimental; these release archives do not provide a macOS viewer.

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
  opening space skirmish setup. Choose a map, factions and teams to start a
  battle with the lit interactive camera, audio and HUD enabled; `--m2` keeps
  the fixed demonstration battle available. See the
  [README](../README.md) and [the viewer guide](../apps/viewer/README.md).
  A standalone exported viewer is a later step (`apps/viewer/tools/export_package.py` already bundles
  the Godot licence texts for it).
- **No graphical test results.** GPU tests are not run on the hosted runners.
