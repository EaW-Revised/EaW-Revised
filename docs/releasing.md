# Releasing

Releases are built on the public repository's GitHub-hosted runners by
`.github/workflows/release.yml`. Nothing is published automatically: the workflow opens a
**draft** release, and a maintainer reviews and publishes it.

## Current candidate: v0.2.5

The last published release is v0.1.6 (2026-10-07). The next candidate is v0.2.5: the portable player
download with its double-click launcher, a debug download, playable space skirmish, LAN and
online multiplayer and a partially working Galactic Conquest. The galactic AI is off by default
in live campaigns; `--eawr-galactic-ai on` switches it on. Keep a candidate local until the
owner approves the public sync.

## Cutting a release

1. Prepare the public snapshot through a sync pull request. Wait for its CI to pass
   before merging it into public `main`; the private source commit must match the
   tested preview. Draft tester-facing release notes describing new features,
   known issues and how to report feedback: bugs as issues, new ideas on the
   [Ideas board](ideas.md).
2. Package the candidate locally and extract the player archive into a fresh,
   writable folder (the viewer archive with `play-demo.cmd` / `play-demo.sh` is the
   second route). Double-click `EAWR.exe` with an installed expansion. Verify map and
   faction selection, starting a skirmish against AI, buying a unit, and earning
   income from a completed mine, then start the debug archive's `EAWR.exe` once.
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
   git tag -s v0.2.0 -m "EaW Revised v0.2.0"
   git push origin v0.2.0
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
| `eaw-revised-<version>-windows-x64-player.zip` | The portable player folder for players (below) |
| `eaw-revised-<version>-windows-x64-debug.zip` | The same folder built in the Debug configuration, with PDBs, asserts and the debug tools (below) |
| `eaw-revised-<version>-<platform>-viewer.zip` | The Godot viewer project with its built GDExtension, `play-demo.cmd` / `play-demo.sh`, and the Python launcher and font extractor (testers and developers with their own Godot) |
| `SHA256SUMS.txt` | The SHA-256 of every archive |

### The player folder

`package_release.py --godot <Godot_v4.7.2-stable_win64.exe> --launcher <EAWR.exe>` (or `--allow-missing-launcher`) also writes the player zip
(Windows x64). Layout: `EAWR.exe` (the native launcher) (legacy EAWR-3962) at the root; `godot/` with the pinned standard
GUI runtime (its SHA-256 must equal the pin in `tools/remote_godot/payload_lib.py` and its `--version` must
report `4.7.2.stable.official.ed1daf0bf`; the console companion is not
shipped, so no console window opens); `project/` with the viewer project and its GDExtension; `licenses/`
with the project, Godot and godot-cpp licence texts, plus `LICENSE` and `THIRD_PARTY_NOTICES.md`;
`README.md` (this repository's `docs/player-guide.md`); and `runtime.json` (Godot identity and SHA-256, launcher
and extension SHA-256); and `SHA256SUMS` (the SHA-256 of every other file in the folder). The release is
not code-signed (owner decision) (legacy EAWR-3966): the player guide tells players about the SmartScreen warning and that
Smart App Control or a managed-PC policy may block it. The packager refuses to write the zip if it would carry a font, `.meg`, `StarWarsG.exe`
or other game file. The launcher starts `godot/Godot_v4.7.2-stable_win64.exe --path project -- <viewer args>`.

Nothing is written beside the program or the game (legacy EAWR-3963). The packager runs the pinned Godot once, headless,
to generate the project's `.godot` metadata (`extension_list.cfg`, `global_script_class_cache.cfg`,
`uid_cache.bin`; the editor layout is dropped) so a read-only folder starts without importing. The project has
a single scene and no imported resources, so no `.pck` export is needed yet. At start the viewer extracts the
four EmpireAtWar fonts from the player's `corruption/StarWarsG.exe` natively (no Python) into
`<user data>/fonts`, with the same checks as `tools/fonts/extract_eaw_fonts.py`: a pinned-build list, a
structural check of every font table and checksum, and the four faces found exactly. Settings, profile, saves,
replays, LAN desync bundles, logs and the font cache are all in Godot user data; reports and captures go only
where a flag names them (the launcher passes `--eawr-report` into user data). `--eawr-font-cache` and
`EAWR_FONT_CACHE` still override the cache for tests. An unpinned build is refused with a readable message
unless the viewer is started with `--eawr-allow-unknown-build`.

The acceptance checklist for a candidate (clean machine plus a normal user) is at the end of
[the player guide](player-guide.md).

### The debug folder

`package_release.py --debug --build-dir <Debug root build> --platform windows-x64 --version <version> --godot <Godot exe>
--launcher <EAWR.exe>` writes `eaw-revised-<version>-windows-x64-debug.zip` after the Release archives (both
viewer builds write the same `apps/viewer/project/bin`, so package the Release archives first and build the Debug
viewer last). It is laid out like the player folder with the viewer extension and the headless tools built in the
Debug configuration and their PDBs (`tools/`), plus `DEBUG-BUILD.md` ([the debug build guide](debug-build.md)). The
hosted workflow builds it in its own job: `cmake --preset windows-msvc -B out/build/windows-msvc-debug
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug -DEAWR_WARNINGS_AS_ERRORS=OFF -DBUILD_TESTING=OFF`, the tool targets in
Debug, then `apps/viewer` with `--config Debug`. The static debug C runtime keeps the programs runnable on a PC without
Visual Studio. The Debug archive is not a gate and is not performance-representative.

Each archive carries `LICENSE`, `THIRD_PARTY_NOTICES.md` and `README.md`. The provenance
attestations can be checked with `gh attestation verify <archive> --repo EaW-Revised/EaW-Revised`.

## What a release does not contain

- **No game data.** Players need their own installation of Empire at War with the Forces of
  Corruption expansion; the tools and the viewer read it with `--eawr-game-root` (or
  `EAWR_EAW_GAME_ROOT`). No models, textures, sounds, fonts, scripts or shader source from the
  original game are ever included.
- **No Godot in the tools and viewer zips.** The player and debug zips bundle the pinned runtime
  (`tools/release/fetch_pinned_godot.ps1` downloads and checks it). With the viewer zip, download the
  pinned standard Godot 4.7.2 binary and install Python
  3.8 or newer, then run `play-demo.cmd` (Windows) or `sh ./play-demo.sh` (Linux)
  from the extracted viewer archive. The launcher discovers or asks for the game
  and Godot paths and extracts validated fonts into its local `out/fonts/` before
  opening space skirmish setup. (The player folder does this in user data instead.) Choose a map, factions and teams to start a
  battle with the lit interactive camera, audio and HUD enabled; `--m2` keeps
  the fixed demonstration battle available. See the
  [README](../README.md) and [the viewer guide](../apps/viewer/README.md).
  A standalone exported viewer is a later step (`apps/viewer/tools/export_package.py` already bundles
  the Godot licence texts for it).
- **No graphical test results.** GPU tests are not run on the hosted runners.
