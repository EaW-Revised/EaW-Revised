# Releasing

Release assets are built on the pool from the exact public tag. The public repository's
`.github/workflows/release.yml` only verifies native Linux ARM64 and Apple Silicon after publication. Nothing is published automatically: a maintainer
reviews and publishes the **draft** release.

## Private crash symbols

Windows builds compile with embedded debug information to preserve compiler-cache
hits. `EAWR_DEBUG_SYMBOLS` defaults OFF in all configurations, including Debug:
ordinary development and pool builds link with `/DEBUG:NONE` and write no PDBs.
For local debugging, configure with `-DEAWR_DEBUG_SYMBOLS=ON` as well.
Configure both the root and viewer with `-DEAWR_DEBUG_SYMBOLS=ON` for a release
or crash-symbol package. Only the viewer, player launcher and eleven headless
tools in the release packager's `TOOLS` list then link PDBs with `/DEBUG:FULL`;
tests and other development binaries still use `/DEBUG:NONE`.
The pool's `--require-symbols` enables this flag explicitly for root and viewer
builds and requires retention/import to pass. The profiling marker enables the
same flag; the melee benchmark also configures it ON explicitly.
Release optimization stays enabled. Existing test PDBs in warm
build trees remain until explicitly removed; Ninja does not delete stale outputs.
Pool jobs retain compressed private symbol bundles for ten recent jobs, plus any
job named by an active build-slot owner. They never enter the player artifact.
The offload client verifies and imports available bundles into the coordinator's
private store (`EAWR_SYMBOL_STORE` overrides the coordinator's configured location).
CI retains matching compressed bundles on the persistent runner before workspace
cleanup, with the same ten-job limit and protection for active runner slots.
Ordinary pool builds and CI cleanup report retention failures as `EAWR-SYMBOLS ...
skipped: <reason>` without changing the build result. Pass `--require-symbols` to
the Windows offload client when retention and import must pass; release packaging
always requires its private symbols.

The store indexes PDBs by GUID and age and binaries by timestamp and image size.
When retained Release or RelWithDebInfo targets emit PDBs (`EAWR_DEBUG_SYMBOLS=ON`),
their CodeView records contain only the PDB basename. `/PDBALTPATH` does not change
the PDB's GUID or age, so the symbol store and `tools/symbolize.py` can still find
matching symbols. With clang-cl and lld-link, paths are prefix-mapped at compile
time only; the PDB contents have not been measured for remaining build paths.
Portable manifests under `builds/<build identity>/` record exact hashes; they carry
no build-machine paths. The coordinator store has no automatic expiry. Release
packaging requires matching Windows PDBs and writes a private symbols archive there,
outside the player downloads and public checksums. `--symbol-store` selects a
different private release store. Retain released build identities indefinitely.

To inspect a player-provided dump, run
`python tools/symbolize.py <dump> --store <private store>` with 64-bit Windows Python.
It uses the built-in Windows DbgHelp library to read the exception context and walk
dump memory, with matching private images/PDBs. Matching local system images provide
OS unwind metadata; missing metadata can truncate the stack. It never follows image
paths from the player's dump or uses a network symbol server. Optional
`--llvm-symbolizer <path>` uses the LLVM executable beside the provisioned clang-cl
for name lookup; `--debugger <cdb.exe>` selects an existing CDB installation instead.
No debugger installation is required. Dumps stay on the player's computer until
they choose to share one; neither the viewer nor the symbol tools upload them.

## Release status

The last published release is v0.3.1 (2026-10-28); the next candidate is v0.3.2.
The published release includes the portable player download with its double-click launcher,
a debug download, playable space skirmish, LAN and online multiplayer and a partially working Galactic Conquest. The galactic AI is on by default
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
  changes. Public hosted CI is advisory for this release route; the maintainer may
  admin-merge the reviewed sync without waiting, as authorized by the release owner.
- Build pool assets from the **exact public tagged commit**, not an approximately
  equivalent private head. Record tag object, commit/tree, build commands,
  compiler/dependency identities, logs and archive SHA-256 hashes in the release
  receipt. Run publish scans and existing packaging/runtime-pin checks. Keep relay
  and networking enabled in Windows Release and Debug viewers; isolate their outputs.
- Full replay76 on the final candidate executable: 19 fixtures x 1/2/4/8 workers (76 runs), run with
  the default `--workers` (see [worker-offload.md](worker-offload.md#worker-subsets---workers)). The
  receipt must pass this gate, and a release is not cut if it exits non-zero. Pass the top-level
  `receipt.json` in the replay76 run's `--out` directory (not a group receipt inside it):

  ```text
  python tools/offload/remote_replays.py --check-full <receipt.json>
  ```

  Exit 0 only for a full, ok, `subset: false` receipt. A per-PR subset receipt (for example workers
  1+8), a failed, incomplete, inconsistent or old-format receipt exits 1; an unreadable file exits 3.
  Record the receipt path and its SHA-256 in the release receipt.
- Fresh-user harness passes **10/10 on the player and 10/10 on the Debug archive** actually
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

The maintainer creates the pool draft with
`gh release create <tag> --repo <public-repo> --draft --verify-tag`
and uploads tested archives, `SHA256SUMS.txt` and provenance with
`gh release upload <tag> <files> --repo <public-repo>` without `--clobber`. Download and
verify those exact bytes, then publish manually after owner approval. Hosted jobs have
no release-write permission and never create, replace, publish or delete release assets.

Pool builds retain publish scans, core CTest excluding labels `game-data|installed-required`,
separate installed-data/GPU acceptance, tagged-main ancestry,
online-enabled Windows viewers, runtime/content checks, Debug symbols and checksums.
Core and viewer compilation run concurrently when capacity permits. Restore only
verified pinned dependencies; keep Release/Debug outputs isolated. Hosted attestations
are not claimed for pool bytes: attach honest pool source/build/hash provenance.
Preliminary fresh-user checks start as soon as candidate bytes exist.

The maintainer tags and pushes the reviewed public commit explicitly (never through the
NO-PUSH remote):

```text
git -C <public-clone> tag -s v0.3.2 <reviewed-public-commit> -m "EaW Revised v0.3.2"
git -C <public-clone> push https://github.com/EaW-Revised/EaW-Revised.git refs/tags/v0.3.2
```

The Windows publish scan must see the reviewed LF bytes of pinned text fixtures after
checkout. Check the candidate's `.gitattributes`; never waive a fixture hash failure as
a packaging detail. Scan a clean source export rather than a warm build directory: a
retained `out/build/**/_deps/**` can contain upstream cryptographic test vectors that
look like secrets. If a maintainer configures an exclusion for those dependency caches,
keep it confined to `out/build/**/_deps/**`; never exclude project sources or archive
payloads. Independently scan the final extracted archives and SHA256SUMS before upload.

## Release-notes template

Copy the download guide below into the draft release notes. Replace `<version>` with
its full tag (for example, `v0.3.1`); list only assets actually uploaded. Add the release's
changes, known issues and feedback links alongside it. Do not carry sizes from an older release.

### Which file do I need?

**To play, download only the Windows player zip.** The other files are for bug reports,
modders and testers. Every game download needs your own Forces of Corruption installation.

| File | Who needs it |
| --- | --- |
| `eaw-revised-<version>-windows-x64-player.zip` | **Everyone who wants to play on Windows.** Extract into a writable folder and double-click `EAWR.exe`. Includes the bundled Godot 4.7.2 engine, game, mod launcher (`EAWR Mods.exe`), `eawr_loadcheck.exe` to check a mod chain. |
| `eaw-revised-<version>-windows-x64-debug.zip` | **Crash and bug reports.** The game with Debug checks, symbols and debug menu entries, plus the command-line tools in Debug mode. It is much slower: use the player zip to play or measure performance. Players in one match should use the same kind of build. See `DEBUG-BUILD.md` inside. |
| `eaw-revised-<version>-windows-x64-tools.zip` | **Modders and testers.** Eleven command-line simulation, asset/XML/scene/unit/sky scan, script check, HUD movie preparation and benchmark programs. See the archive's `README.md` for each program. Windows may need the Microsoft Visual C++ runtime. |
| `eaw-revised-<version>-windows-x64-viewer.zip` | **Testers with their own engine.** Bare game project for standard Godot 4.7.2 and Python 3.8+. Developer/tester route on a stock Godot engine, Safe render thread (the stock engine cannot do Separate); not a supported way to play or to measure performance; use the player zip. Start with `play-demo.cmd`; Safe render thread only. The player zip already includes the game and engine. |
| `eaw-revised-<version>-linux-x64-tools.zip` | **Linux modders and testers.** The same eleven command-line tools built for Linux x64. See `README.md` inside. |
| `eaw-revised-<version>-linux-x64-viewer.zip` | **Linux testers with their own engine.** Bare game project for standard Godot 4.7.2 and Python 3.8+. Developer/tester route on a stock Godot engine, Safe render thread (the stock engine cannot do Separate); not a supported way to play or to measure performance; use the player zip. Start with `sh ./play-demo.sh`; Safe render thread only. No bundled Linux player runtime. |
| `update-manifest.json` | **Launcher updater.** Metadata and hashes for the Windows player download; keep it beside the other release assets. |
| `SHA256SUMS.txt` | Checksums of the Windows player, tools and viewer zips. |
| `SHA256SUMS-debug.txt` | Checksum of the Windows Debug zip. |
| `SHA256SUMS-linux-x64.txt` | Checksums of the Linux x64 tools and viewer zips. |

To check a Windows download, open PowerShell in its folder and run
`Get-FileHash .\eaw-revised-<version>-windows-x64-player.zip` (use your downloaded file's name).
The Hash must match that file's line in the appropriate checksum file; ignore letter case.
For Linux, run `sha256sum -c SHA256SUMS-linux-x64.txt` beside both downloaded Linux archives.

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
| `update-manifest.json` | Written beside the Windows player zip for the launcher's updater (schema 1): archive name, size and SHA-256, every file of the player folder with size, SHA-256 and component (`game`, `launcher`, `service`, `layers`), tag, version, channel (`stable`, or `preview` for a `-suffix` version; `--channel` overrides), build id (the packaged commit), `min_updater` 1 and `signature` fixed to the empty string (releases stay unsigned; integrity is HTTPS plus these hashes). Deterministic, validated against the client's rules before it is written. Its SHA-256 is listed in `SHA256SUMS.txt`. Upload it as a release asset like the zips; the packager never uploads. |

### The player folder

Set `EAWR_REVISED_LAYER_DIR` or pass `--revised-layer <dir>` to provide the Revised layer source containing `Data/Audio` and `Data/XML`. It is copied to `Revised/Data` beside `EAWR.exe`; player and Debug packages require it; `--allow-missing-revised-layer` permits a preview without it and warns. `--public` requires the exact reviewed layer recorded in `tools/release/revised-layer-manifest.json`; missing, extra or changed files fail packaging. The advisor selection voices are included. The pool wrapper accepts `-RevisedLayer`, `-AllowMissingRevisedLayer`, and `-Public`. For a pool package produced with `-AllowMissingRevisedLayer`, run `python tools/release/add_revised_layer.py <extracted-player-folder> --revised-layer <dir> --zip <player.zip>` locally to add it and refresh the folder checksums and zip. Never commit layer content.


`package_release.py --godot <Godot_v4.7.2-stable_win64.exe> --launcher <EAWR.exe>` also writes the player zip
(Windows x64). These existing commands keep the normal layout and never fetch a manager pin.
Opt in to the manager with `--with-mods-manager --mods-service <eawr_mods_service.exe>`; this mode requires
both pool-built executables and validates them and the pinned artifact before packaging. Use
`--launcher-artifact <manager.zip>` for a local zip matching `config/launcher-pin.json`, or omit it to
download and verify the pinned GitHub release with GitHub CLI. Manager mode adds `eawr_mods_service.exe`,
`launcher/`, `launcher-pin.json` and `EAWR Mods.exe` (which starts `EAWR.exe --mods` without a console). Build the `eawr_mods_start`
target beside `EAWR.exe`; manager packaging requires both executables from the same launcher output folder.
To install the optional desktop shortcut, run `"EAWR Mods.exe" --create-shortcut`.
The native launcher is required by the player CLI; the former option to omit it has been removed.
The normal layout is `EAWR.exe` (the native launcher) (legacy EAWR-3962) at the root; `godot/` with the pinned patched
GUI runtime (the patched EAWR fork build, see `third_party/godot/README.md`: its SHA-256 must equal
`PINNED_GODOT_SHA256` in `tools/release/package_release.py` and its `--version` must
report `4.7.2.stable.eawr.fad06b579`; the console companion is not
shipped, so no console window opens); `project/` with the viewer project and its GDExtension; `licenses/`
with the project, Godot and godot-cpp licence texts, plus `LICENSE` and `THIRD_PARTY_NOTICES.md`;
`README.md` (this repository's `docs/player-guide.md`); and `runtime.json` (Godot identity and SHA-256, launcher
and extension SHA-256); and `SHA256SUMS` (the SHA-256 of every other file in the folder). The release is
not code-signed (owner decision) (legacy EAWR-3966): the player guide tells players about the SmartScreen warning and that
Smart App Control or a managed-PC policy may block it. The packager refuses to write the zip if it would carry a font, `.meg`, `StarWarsG.exe`
or other game file. `EAWR.exe` hashes that engine before starting it and compares the SHA-256 with `godot_sha256` in `runtime.json`; a
mismatch or a missing hash logs `EAWR-ENGINE-UNPATCHED` and adds `--eawr-engine-unverified` to the viewer arguments
(play is never blocked; see `apps/launcher/README.md`). The launcher starts `godot/Godot_v4.7.2-stable_win64.exe --path project -- <viewer args>`; the project selects the Separate render thread
(`rendering/driver/threads/thread_model=2`). The Safe render thread is unsupported (owner decision 10-31): no start file for it ships and no batch file ships in the player zip.
`EAWR.exe --safe-render-thread` remains only as an engine-level crash fallback.

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

The packager deletes the patched Godot's `project/bin/~*` import copies and refuses any player or Debug payload with a `~*` file or a PDB the Debug build does not own.

It also refuses a shipped binary whose build identity is not the packaged commit (`--expect-identity <commit>`, default git HEAD); builds re-run CMake configure when HEAD moves, so a warm pool tree cannot keep an old identity.

The acceptance checklist for a candidate (clean machine plus a normal user) is at the end of
[the player guide](player-guide.md).

### The debug folder

`package_release.py --debug --build-dir <Debug root build> --platform windows-x64 --version <version> --godot <Godot exe>
--launcher <EAWR.exe>` writes `eaw-revised-<version>-windows-x64-debug.zip`. The parallel release route
uses separate worktrees (or separate hosts), each with its own `apps/viewer/project/bin`, so Release
and Debug builds cannot overwrite each other. For a single-worktree fallback, package the Release
archives first, then build and package Debug; never run both viewer builds concurrently in one worktree. It is laid out like the player folder with the viewer extension and the headless tools built in the
Debug configuration and their PDBs (`tools/`), plus `DEBUG-BUILD.md` ([the debug build guide](debug-build.md)). The
pool build uses: `cmake --preset windows-msvc -B out/build/windows-msvc-debug
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug -DEAWR_WARNINGS_AS_ERRORS=OFF -DBUILD_TESTING=OFF`, the tool targets in
Debug, then `apps/viewer` with `--config Debug`. The static debug C runtime keeps the programs runnable on a PC without
Visual Studio. The Debug archive is not performance-representative. Fresh-user 10/10 on BOTH the player
and Debug archives is a publication gate.

Each archive carries `LICENSE`, `THIRD_PARTY_NOTICES.md` and `README.md`. Tools and viewer archives use
`tools/release/readme-tools.md` and `tools/release/readme-viewer.md` respectively; the player and Debug
archives keep the player guide, with the separate Debug guide in the Debug archive. Pool assets
carry a source/build/hash receipt; do not claim a hosted attestation for these bytes.

## Fresh-user gate and host-side packaging (maintainer tools)

The fresh-user gate runs the extracted player or Debug archive the way a new player would (restricted PATH, empty
APPDATA, a folder name with a space and a non-ASCII letter) on a GPU rig host, through the normal rig queue.
`tools/release/fresh_user/Run-FreshUser.ps1 -Package <zip> -Kind player|debug -ExpectedSha256 <sha> -Machine <host>` stages
the zip on the host, puts `fresh_user_player.py` into the worktree as the untracked suite
`tests/presentation/renderer/test_release_player_fresh_user.py` and runs it with `Invoke-RigSuite.ps1`; all collected cases must pass (currently ten)
for each archive (hashes, first and repeated launch, fonts in user data, unchanged package folder, setup, AI combat, unit
purchase, mine income, LAN/Online selector). `Run-FreshUser.ps1` builds a clean one-commit checkout of the release commit with
the suite added (the rig suite preflight refuses a dirty tree), prints a PASS 10/10 verdict from the cases, the receipt and the
host GPU evidence, and exits non-zero otherwise (`Invoke-RigSuite` itself reports "no GPU proof" for this suite because the
packaged Godot is launched by the suite, not by the runner). The suite redirects the registered renderer helpers' `subprocess.run` and
`subprocess.Popen` launches to the extracted archive's own Godot and project, and gives the Debug archive a longer
per-case budget (the Debug build is not performance-representative). The worktree must sit at the release commit. `Run-FreshUser.ps1` closes its rig sessions and removes the staged archive and its checkout on every exit.

`tools/release/pool_host/Package-OnHost.ps1` packages a finished pool build on the build host that holds it (a staged copy of
the slot tree, so later jobs cannot disturb it) with `package_release.py --with-mods-manager`, the pinned patched Godot and
the pinned manager artifact, with `--expect-identity <the commit being packaged>` (the packager removes the engine's `~`-prefixed import copies itself and refuses any shipped
binary that embeds another commit's build identity, so a warm pool tree cannot ship stale bytes; the staged copy has no `.git`, hence the explicit
commit; `-ViewerTree` takes the viewer extension and its PDB from the slot that built the viewer, because a viewer zip carries no PDB); `Build-DebugOnHost.ps1` builds the Debug tools and the online-enabled Debug viewer on a
different host in its own tree, holding one of the host's build slot locks. Both take every path as a parameter.

## Owner test kits (not releases)

An owner test kit is built like a release but is local, unsigned and never published. Two rules (owner, 2026-11-03/04):

- **Engine:** the kit takes its engine only through `tools/release/kit_engine.py` (`stage_engine`: the pinned patched player and console exe from `tools/engine_pins.json`, fail closed on anything else). Run `python tools/release/kit_engine.py verify <kit folder or zip>` before handing the kit over. Never copy `godot/*` from an older kit.
- **Layout:** the kit is the player layout, the same file list as a release player folder: `EAWR.exe`, `EAWR Mods.exe`, `eawr_loadcheck.exe`, `eawr_mods_service.exe`, `godot/`, `launcher/`, `launcher-pin.json`, `project/`, `Revised/`, `runtime.json`, `SHA256SUMS`, `README.md` (plus `LICENSE`, `THIRD_PARTY_NOTICES.md`, `licenses/`). It is built by `tools/release/package_release.py --with-mods-manager` through `tools/release/pool_host/Package-OnHost.ps1`, from a root-tools build of the same commit (EAWR, EAWR Mods, the mods service, the load check and the tools). The old dev layout (`start_test.py`, `demo.py`, `start-*.cmd`) is not an owner kit. Compare the finished folder's file list with the last accepted kit before reporting it ready.

### Owner test kit (one job)

One pool job builds and packages a kit; no hand re-run of `package_release.py`:

```
python tools/offload/windows_build.py --ref <sha> --viewer --viewer-relay-only --preset windows-msvc-ninja-relay --require-symbols --no-test --no-probe-build --no-werror-probe --host <host> --package-on-host cfg.json \
  --target sim_headless --target sim_bench --target path_bench --target asset_scan --target asset_validate --target xml_scan --target scene_scan --target sky_scan --target unit_scan --target script_smoke --target hud_movie_prepare --target eawr_launcher --target eawr_mods_start --target eawr_mods_service --target eawr_loadcheck
```

`eawr_mods_service` and `eawr_loadcheck` are not in the default `ALL` target, so the explicit target list is part of the recipe (`--target` is accepted beside `--viewer` when a root preset is given). `--viewer-relay-only` skips the plain viewer build (the player ships only the relay viewer, which overwrites the plain one in `apps/viewer/project/bin` anyway; about an hour saved on a cold tree). The whole remote command must stay under 8000 characters (`tests/ci/test_windows_build.py` measures 7761 on build host).

`cfg.json` (host paths): `version` (`<semver>[-<label>]`, label `[0-9A-Za-z.+-]`, e.g. `0.3.2m-f8bf25ed` or `0.1.0+4e2da633`), `godot` (the patched engine, checked by `tools/release/kit_engine.py`), `launcher_artifact` (the pinned manager zip), optional `viewer_tree`.

When the job cannot link with symbols (no `--require-symbols`, or a viewer reused from a job without them), add `"symbols": false` to `cfg.json` (`-NoSymbols` on Package-OnHost, `--no-symbols` on `package_release.py`). The kit then keeps no private symbol archive and its `README.md` and `update-manifest.json` notes say `symbols: none (test kit)`. `"allow_missing_revised_layer": true` (`-AllowMissingRevisedLayer`) packages without the private Revised layer, which is not on the pool hosts; the owner-kit finish step adds it locally. `--public` refuses `--no-symbols`. Without it the packager checks every retained binary for a PDB identity first and lists all that lack one ("rebuild with `--require-symbols` (and `--viewer`)"). `Package-OnHost.ps1 -BuildDir` defaults to `out\build\windows-msvc-ninja-relay` and fails at once when that directory or an expected binary is missing.

## What a release does not contain

- **No game data.** Players need their own installation of Empire at War with the Forces of
  Corruption expansion; the tools and the viewer read it with `--eawr-game-root` (or
  `EAWR_EAW_GAME_ROOT`). No models, textures, sounds, fonts, scripts or shader source from the
  original game are ever included.
- **No Godot in the tools and viewer zips.** The player and debug zips bundle the pinned runtime
  (`tools/release/fetch_pinned_godot.ps1` downloads and checks it). With the viewer zip, download the
  pinned patched Godot 4.7.2 binary (or the stock official 4.7.2 for the viewer alone) and install Python
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
