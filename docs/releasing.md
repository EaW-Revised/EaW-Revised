# Releasing

Release assets are built on the pool from the exact public tag. The public repository's
`.github/workflows/release.yml` only verifies native Linux ARM64 and Apple Silicon after publication. Nothing is published automatically: a maintainer
reviews and publishes the **draft** release.

## Current candidate: v0.2.5

The last published release is v0.1.6 (2026-10-07). The next candidate is v0.2.5: the portable player
download with its double-click launcher, a debug download, playable space skirmish, LAN and
online multiplayer and a partially working Galactic Conquest. The galactic AI is on by default
in live campaigns; `--eawr-galactic-ai off` switches it off. Keep a candidate local until the
owner approves the public sync.

## Cutting a release: one-hour aim, three-hour deadline

T0 is the last feature merge. Before T0, the maintainer prepares the public sync PR,
public CI, release-core qualification, notes and AI-versus-AI progression proof on the
near-final head. Record setup, seed, economy development, station technology, mixed
fleets and combat. Reserve pool/GPU capacity. Keep the public clone's push URL disabled;
only the maintainer pushes through an explicit public URL after approval. Never tag
while the owner has paused release work.

| Time from T0 | Work (parallel unless a dependency is named) | Aim / deadline |
| --- | --- | --- |
| 0–10 min | Delta re-export, all publish scans, review/update/merge the sync PR. Record private/public commit mapping; review the delta against pre-T0 qualification. | 10 / 30 min |
| 10–15 min | Maintainer explicitly pushes the signed tag on reviewed public main. Pool checks out this exact public tag; tagging does not dispatch hosted work. | 15 / 35 min |
| 15–40 min | Incremental pool Release/Debug tools and online-enabled viewers, package and hash. ARM/Apple hosted verification starts only after publication, outside this critical path. | 40 / 120 min |
| As pool archives finish | Fresh-user harness on exact player and Debug bytes; prepare draft, upload tested archives, sums, notes and pool provenance. | 55 / 150 min |
| After all gates below | Owner approves; maintainer publishes manually. | T0+60 / T0+180 min |
| After publication | ARM/Apple verification may open a follow-up ticket; it never blocks or pulls a release. | Outside publication path |

The **critical path** is delta export/review → exact tag → pool build/package →
fresh-user checks → approval/publication. Pool core/viewer work should use separate output trees and run concurrently when capacity permits. Escalate capacity or
delta-qualification risk at T0+30 minutes; candidate bytes must exist by T0+120 for
final checks. Cold public CI plus cold hosted builds can exceed three hours and is
not the planned fast route. No gate is removed to meet a clock.

### Pool assets are the primary publication route

The release owner accepts builds from our own machines. Hosted completion is **not** a
publication gate. The pool route requires all of these conditions:

- Run public CI and release-core qualification before T0. Review the final delta
  explicitly and test affected code on the final candidate; rerun broad qualification
  when the delta invalidates it. Earlier green receipts alone do not prove later
  changes. Required public checks still apply.
- Build pool assets from the **exact public tagged commit**, not an approximately
  equivalent private head. Record tag object, commit/tree, build commands,
  compiler/dependency identities, logs and archive SHA-256 hashes in the release
  receipt. Run publish scans and existing packaging/runtime-pin checks. Keep relay
  and networking enabled in Windows Release and Debug viewers; isolate their outputs.
- Fresh-user harness passes **9/9 on the player and 9/9 on the Debug archive** actually
  uploaded. Use fresh folders including spaces/non-ASCII text; check hashes,
  first/repeated launch, fonts in user data, unchanged package files, setup, AI combat,
  unit purchase, mine income and the LAN/Online selector.
- Download draft assets and verify their hashes match tested bytes and SHA256SUMS.
  Review known issues and feedback links ([Ideas board](ideas.md)); all feature-specific
  release gates must pass and the owner must approve publication. Describe pool
  provenance honestly: a hosted attestation never applies to different pool bytes.
- Hosted verification starts only on the `release: published` event: no tag-triggered
  or pre-publication hosted jobs. Its native Linux ARM64 and Apple Silicon jobs are
  non-blocking and file follow-up tickets on failure. **They can never block or pull
  a release.** The x64 assets are proven by the pool route, without duplicate hosted work.

The maintainer creates the pool draft with `gh release create <tag> --draft --verify-tag`
and uploads tested archives, `SHA256SUMS.txt` and provenance with
`gh release upload <tag> <files> --repo <public-repo>` without `--clobber`. Download and
verify those exact bytes, then publish manually after owner approval. Hosted jobs have
no release-write permission and never create, replace, publish or delete release assets.

Pool builds retain publish scans, full applicable core CTest, tagged-main ancestry,
online-enabled Windows viewers, runtime/content checks, Debug symbols and checksums.
Core and viewer compilation run concurrently when capacity permits. Restore only
verified pinned dependencies; keep Release/Debug outputs isolated. Hosted attestations
are not claimed for pool bytes: attach honest pool source/build/hash provenance.
Preliminary fresh-user checks start as soon as candidate bytes exist.

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
pool build uses: `cmake --preset windows-msvc -B out/build/windows-msvc-debug
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug -DEAWR_WARNINGS_AS_ERRORS=OFF -DBUILD_TESTING=OFF`, the tool targets in
Debug, then `apps/viewer` with `--config Debug`. The static debug C runtime keeps the programs runnable on a PC without
Visual Studio. The Debug archive is not a gate and is not performance-representative.

Each archive carries `LICENSE`, `THIRD_PARTY_NOTICES.md` and `README.md`. Pool assets
carry a source/build/hash receipt; do not claim a hosted attestation for these bytes.

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
