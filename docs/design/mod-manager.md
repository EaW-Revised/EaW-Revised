# Mod manager and launcher: design

Status: design spec, 2026-10-25, updated 2026-10-26 with the owner's overnight decisions. It includes the
owner's addition from 2026-10-25: load order, file conflicts and an xEdit-style record view, all as first-class
features. Phase 1 is partly built; see "Status 2026-10-26" below.

## Status 2026-10-26

Built so far:

- **game repository:**
  - mod discovery in `src/mods` (legacy EAWR-5022);
  - chain resolution (legacy EAWR-5034);
  - `eawr_mods_service` and its protocol (legacy EAWR-5020, EAWR-5040, EAWR-5053);
  - profiles and the engine's `--eawr-mod-profile` (legacy EAWR-5035);
  - `EAWR.exe --mods` (legacy EAWR-5039, EAWR-5050).
- **launcher repository** (`EaW-Revised/eawr-launcher`): the scaffold, the manager screens, load-order editing, the
  `chain.check` flow, and preview release v0.1.2.

The owner resolved the open questions (section 8). By default the mod manager is **not part of game release 0.3**.
It ships as separate preview kits (launcher v0.1.2 now) until the owner decides to bundle it (MM-S2).

Licensing changed on 2026-10-26: the launcher repository is not clean-room (section 2, "Clean-room and licences").

Decision source: the owner's 2026-10-11 decision for an external mod manager (legacy EAWR-3009, EAWR-3008). Retail
chooses a mod on its in-game Mods page and restarts with it (MNU-18). We deliberately deviate: a separate manager
beside the game chooses mods, orders them and shows conflicts. The in-game page attempt is parked
(legacy EAWR-2983); this design reuses its discovery and choice-record ideas.

Owner rules this design keeps:

- the engine runs retail mods as shipped;
- mods are never edited;
- mod files never enter the repository;
- no owner paths or host names go into public files;
- everything retail lets a mod change through XML stays changeable.

## 1. Options and recommendation

### What was checked

| Question | Finding |
|---|---|
| Does MO2 support EaW/FoC today? | Yes, through a community "basic game" plugin in MO2's bundled basic_games collection: two Python descriptors of about 33 lines each (EaW and FoC, one community author). The FoC descriptor only names the game binary and the data folder (`corruption/Data`) and stores Steam app id 32470. It has no Workshop, `STEAMMOD` or `MODPATH` logic. |
| How does MO2 apply mods? | MO2's virtual file system (USVFS) hooks file APIs in the game process. It overlays every enabled mod into that one data folder, with a priority list and per-file conflict flags. USVFS is GPLv3, Windows-only and "alpha quality" by its own README. The README also warns that its hooking "may rub antivirus software the wrong way". Its hooks start during process initialisation, so DLLs loaded earlier may not see them. |
| Does that model fit FoC mods? | No. FoC stacks mods as ordered layers (`MODPATH`/`STEAMMOD`, leaf first). Each layer keeps its own `Data/MegaFiles.xml` and archive set, and inherited archive declarations resolve at the layer that supplies them (ML-05/ML-09, `resolve_manifest_chain`, `docs/vfs.md`). A merged view of one data folder keeps only the top layer's `MegaFiles.xml`. It therefore activates the wrong archives for any chain. The Remake chain 3229239424;2794270450;1770851727 ships its own data, with a manifest per layer, at each step. |
| Does MO2 know Workshop items and parent chains? | No. It has no reader for `steamapps/workshop/content/32470`, `appworkshop_32470.acf` or parent chains. In a sample of 8 installed Workshop items, only 2 ship a `modinfo.json` that declares dependencies. The rest record their chain only in their Workshop page and in community launch scripts (`STEAMMOD=<id>` lines). |
| Do we need USVFS? | No. Our engine already mounts ordered roots itself (`Vfs::mount`, `mod_chain_roots`), with stable provenance per file (`candidates`, `stat`). Hooking the Godot process would add a second, invisible resolution layer under our VFS. Our VFS's diagnostics and our match identity would then describe bytes we did not choose. |
| Platforms | MO2 and USVFS run only on Windows. On Linux and Steam Deck they run only under Wine/Proton, with community wrappers that pin MO2 versions. Our viewer is a Godot 4.7.2 build that also targets Linux. |
| Licence | MO2 and USVFS are GPLv3. Bundling MO2 obliges a bundled build to ship its source and keep it GPL-compatible. Writing a plugin is fine, but it ties our release to MO2's plugin API and Python runtime. Either way, no MO2 source or text may enter our repository (clean-room). |
| Multiplayer | EAWR-MATCH compares the content identity built from what our loaders read. MO2 profiles can differ invisibly, for example through an overwrite folder or a disabled file. Each player would then debug MO2 state that our handshake cannot name. |

### Player needs against the options

| Need (section 8 of the spec) | A. own manager | B. MO2 + EAWR plugin | C. own manager, MO2's model |
|---|---|---|---|
| 1. installed mods, Workshop, parents, library items | build | not covered (no Workshop) | build |
| 2. chain and profile with launch settings | build | profiles yes; chains only as a flat priority list | build, with MO2-style profiles |
| 3. conflicts and our diagnostics | build | file conflicts in one flattened folder only; none of our diagnostics | build, with MO2-style conflict flags on our layer list |
| 3b. record view: vanilla value, then each mod's value (owner, 10-25) | build on our catalog | not covered (that is xEdit's job, and xEdit has no FoC support) | build on our catalog |
| 4. launch, one click, no console | build (EAWR.exe exists) | MO2 window + its launch | build (EAWR.exe exists) |
| 5. MP chain identity, join picks profile | build | not covered | build |
| 6. read-only, offline, no credentials | yes | USVFS writes go to an overwrite folder; offline yes | yes |
| 7. Linux / Steam Deck | Godot UI runs there | Wine/Proton only | Godot UI runs there |
| 8. engine-readable profile, in-game Mods button | yes | no (state lives in MO2) | yes |

### Recommendation

**Choose C.** Build our own small manager. It borrows MO2's user model: named profiles, an explicit load order
(priority list), per-entry conflict flags ("overrides" / "is overridden") with a file-conflict view, and a profile
that carries its own launch settings. It also borrows the xEdit idea of a read-only record view, which shows one
XML object's vanilla value and then each mod's value in load order, with the winner marked. None of MO2's virtual file system is used.
In the game repository nothing is taken from either tool. Since 2026-10-26 the launcher repository may reuse MO2
and xEdit patterns, file formats and code under their licences (section 2, "Clean-room and licences"). The stack is the C++ core shared with the
engine plus typed GDScript screens (section 4). Steam Workshop integration goes through `steam://` links and
opt-in keyless lookups, with the Steam client staying the only downloader (section 3). MO2's strength is merging loose files into one
folder for games without layered loading. FoC already has layered loading, and our VFS already implements it with
provenance. MO2's one EaW plugin is a 33-line descriptor that flattens the layers we must keep apart. It knows
nothing about Workshop chains, and it would put a GPLv3 hooking DLL into a Godot process and keep Linux players on
Wine. Most of the work is ours under any option: Workshop discovery, parent resolution, our diagnostics, the match
identity, and a profile the engine reads. The existing first-run launcher (`EAWR.exe`, `apps/launcher`) already
parses Steam's VDF/ACF files and starts the game without a console. C is option A done with a proven UX model, not
a third architecture. MO2 users are not blocked: they can add EAWR as an MO2 executable, but we neither support nor
test that.

The owner's 2026-10-25 addition (load order, file conflicts, an xEdit-style record view) strengthens C. MO2's
conflict view works on one flattened folder, so for FoC it would answer the wrong question. A record view needs our
own loader anyway, because only our catalog knows the registry include order, the layer rank and the
`Variant_Of_Existing_Type` merge rules that decide the value the engine uses (`docs/xml-model.md`).

**When this would flip to B:**

- FoC players turn out to use MO2 widely, and an MO2 game plugin with Workshop and `MODPATH` chain support appears.
  Even then we would only add a plugin, not bundle MO2.
- The owner wants "Data-overwrite" mods as a first-class style. Those are mods that copy files into
  `corruption/Data` instead of using `MODPATH`.

**When it would flip to plain A:** if the owner prefers no profile/priority-list UX and only "pick a mod and play".

## 2. Architecture

### Processes

```text
desktop shortcuts: "EAWR" (play last profile)  "EAWR Mods" (open manager)
        |                                         |
        v                                         v
EAWR.exe (apps/launcher, native, no console) -----+
  finds the game (as today), reads <user data>/mods/state.json
  |-- manager wanted (shortcut, --mods, no usable profile, Shift)
  |      start: Godot --path launcher -- --eawr-game-root <root>   (GUI window, no console)
  |        manager (launcher repo) <--stdio JSON lines--> eawr_mods_service (game repo, no console)
  |      manager saves the profile via the service, writes launcher/next-action.json {launch-profile <id>}, quits 0
  |-- launch: Godot [--render-thread safe|separate] --path project -- --eawr-front-end
  |           --eawr-game-root <root> --eawr-mod-profile <profile file> --eawr-report <...>
  |      game writes launcher/next-action.json {open-manager} or {launch-profile <id>} when asked, quits 0
  |-- stay-open mode uses launcher/game-running.json to signal the running game to the manager
  '-- loops on next-action.json (deleted before each start); otherwise exits as today
```

- **One entry point, two Godot projects on one runtime.** The manager is its own small Godot project (`launcher\`,
  from the launcher repository, section 5). It runs on the same Godot runtime the game ships, not on a new toolkit,
  and runs on every platform the viewer runs on.
  It needs no Qt, .NET or web runtime. Like retail, the game process never swaps content while running. A
  different chain always means a fresh process.
- **`next-action.json` instead of exit codes.** The launcher already reports Godot's own exit codes, so new magic
  codes would collide. The file lives in the launcher's user-data folder and is written atomically (temp file,
  then rename). The launcher deletes it before every start, as it does with `front-end-report.json`.
- **The engine never needs the manager.** `--eawr-mod-profile <file>` works from any start script. The existing
  `--eawr-mod-root "<leaf>;<parent>;..."` stays as the low-level override for tests and tools, and wins when both
  are given (with a report line).

### Code layout

| Part | Where | Notes |
|---|---|---|
| `eawr_mods` core | `src/mods/` (new engine-free library, `sources.cmake`) | discovery, records, chain resolution, profile read/write, fingerprints, conflict model, chain code and shader translation policy (`src/mods/shader_translation.cpp`); portable C++20, no Godot |
| shared text formats | move `apps/launcher/src/text_formats.*`, `paths.*` and the Steam library part of `install_discovery.*` into `src/mods/steam/` | the launcher already parses `libraryfolders.vdf` and `.acf`; one parser, two users |
| launcher | `apps/launcher` | manager/launch loop, `next-action.json`, desktop shortcut for "EAWR Mods", `--render-thread` |
| manager UI | launcher repository Godot project: typed GDScript and scenes | screens in section 6, in typed GDScript (section 4); since the repository split (section 5) this row lives in the launcher repository and talks to `eawr_mods_service` |
| engine side | viewer host config + `sim_headless` | `--eawr-mod-profile`; the in-game Mods button |

`src/mods` keeps the VFS boundary. Discovery reads only directory listings, `.acf`/`.vdf`, `modinfo.json` and
(for chain hints) the text of a mod's top-level `.bat`/`.cmd` files. Asset bytes are read only through `Vfs`.
`tests/vfs/test_asset_io_boundary.py` gets `src/mods/` as a permitted native adapter for exactly those reads.

### Data model

All files are JSON. Each carries a `format` name and a `version`. Before 1.0 a format change bumps the version
and old files are rebuilt or dropped, with no legacy readers (owner rule, 2026-10-09). The schemas below
illustrate the fields; they are not final.

**Mod record** (built by discovery; cached in `mods/catalog.json`, rebuilt when fingerprints change):

```json
{ "key": "workshop:3229239424",            // workshop:<id> | mods:<folder> | path:<sha256 of the absolute path>
  "kind": "workshop",                       // workshop | mods-folder | manual
  "name": "…",                              // modinfo name, else known-chains name, else "Workshop item <id>"
  "alias": null,                            // the player's own name for it (manager state, never written to the mod)
  "root": "<absolute path>",                // local only
  "has_data": true,                         // a Data folder exists
  "metadata": { "modinfo": { "version": "…", "dependencies": [ {"modtype": 1, "identifier": "2794270450"} ] } },
  "role": "playable",                       // playable | parent-only | unknown
  "state": "installed",                     // installed | needs-update | downloading | missing
  "fingerprint": { "acf_manifest": "…", "time_updated": 0, "tree": "<sha256 of sorted relative path, size, mtime>" },
  "warnings": [ "EAWR-MODS-0003 parent 2794270450 not installed" ] }
```

**Chain**: an ordered list of record keys, leaf first, then FoC, then EaW (the VFS order). Each edge carries its
provenance so the UI can say why a parent is there: `player`, `modinfo`, `known-chain`, `launch-script`.

**Chain fingerprint**: a cache key derived from the ordered resolved layers and their content fingerprints, including the built-in layer and base game data. It changes when order or relevant content changes; the engine build id is an additional cache key. This is distinct from the planned chain code, which identifies the ordered mod identities rather than their content.

**Profile** (`mods/profiles/<id>.json`, one file per profile):

```json
{ "format": "eawr-mod-profile", "version": 1,
  "id": "remake", "name": "Remake",
  "layers": [ { "key": "workshop:3229239424", "root": "<absolute path at save time>" },
              { "key": "workshop:2794270450", "root": "…" },
              { "key": "workshop:1770851727", "root": "…" } ],
  "order_source": "chain",                  // chain (derived from the leaf's parents) | player (reordered by hand)
  "separators": [ { "before": 0, "label": "Remake" } ],   // display only, never affect loading
  "launch": { "mode": "front-end", "render_thread": "separate", "frame_rate_limit": 120 },
  "accepted": { "chain_fingerprint": "…", "load_check": "ok-with-warnings" } }
```

- **`layers` is the load order.** Index 0 has the highest priority, and the engine mounts the layers exactly in
  this order, followed by FoC and EaW (`docs/vfs.md`). Choosing a leaf fills the order from its chain
  (`order_source: chain`). The player can reorder by dragging, with Alt+Up/Down, or with the controller shoulder
  buttons, and can add or remove layers. Either sets `order_source: player` and keeps the order exactly as given,
  even after a Steam update. A layer placed above a parent it depends on, or a parent removed, shows a warning that
  the player may accept. The load order is saved per profile, and two profiles may hold the same mods in different
  orders.
- The engine resolves `root` first. If that path is gone, for example because a Steam library moved, it
  re-resolves `key` through the same discovery code before failing with a stable code. A profile with no layers
  has no user mod layers. The service also derives a `builtin:revised` row after user layers; its enabled setting and installed state determine whether Revised participates. With it disabled or missing, an empty profile plays unmodded FoC.
- **Matching when things move.** Layers match by `key` first. When a manual or Mods-folder layer's key no longer
  resolves (the folder was renamed or moved), the manager offers a match by display name or alias, and the
  player confirms it. The engine never guesses: a layer that does not resolve fails the launch with the missing
  key named. Installed mods that a profile does not list are simply off for that profile. The list is explicit,
  so a newly installed mod never joins an existing profile by itself.
- `launch.frame_rate_limit` and other values that already exist in the settings file are applied as a per-profile
  overlay (`video.frame_rate_limit`). They are not copied into the player's global settings.
- `render_thread` maps to Godot's `--render-thread` command-line option, which the launcher passes.

**File index** (the file-conflict view; built once per chain fingerprint, see "Indexes and cost"):

- one row per logical path that any layer supplies: the winner (layer, loose or archive, the archive's name) and
  every shadowed candidate in precedence order. It is built from `Vfs::enumerate_raw` and `candidates` on the
  profile's own mount, so it is exactly what the game will load, including FoC's rule that a mod archive beats an
  expansion loose file and a mod loose file beats every mod archive.
- per layer: the files it wins, the files it loses (and to whom), and the files only it supplies. This is MO2's
  "overrides / overridden" flag, computed on our layer list.
- per row, a byte digest per candidate, so "identical copy" (same bytes in two layers, a harmless conflict) is
  shown apart from a real override.

**Record index** (the content view; built once per chain fingerprint and once for vanilla FoC):

- the vanilla catalog: `load_catalog` on the FoC profile alone (no mod layers). This is the "original value"
  column. It is cached once per game install, keyed by the FoC data's registry hashes.
- the profile catalog: `load_catalog` on the profile's mount loads the VFS winner for each active include path. `find_all(id)` returns definitions from those winning files, including duplicates across active includes, but not definitions in shadowed same-path candidates.
- A loader-owned diagnostic pass (`load_shadowed_definitions`, added by MM-10 step A in draft PR (legacy EAWR-5480); not on main yet), opens `Vfs::candidates` in precedence order for each active include path and parses the shadowed candidates. It adds their definitions and source, layer, line/column and include order to the record index without changing the effective catalog. The view groups definitions by layer; effective occurrences and their provenance come from the catalog (`docs/xml-model.md`).
- per object: its type and category, defining layers, winning definition, `Variant_Of_Existing_Type` chain (derived to base), digest, source location and include order/rank for each definition, including occurrence order within a file. Values are read from the cached catalog or diagnostic sources when opened.
- whole-file XML outside the five registries, read by fixed name (for example game constants): use VFS raw candidates for that fixed logical path, parsed through the same `parse_document` parser used by `data::load_document` (`src/data/xml.cpp`); the diagnostic pass must adapt it to candidate bytes and provenance. The VFS winner supplies the whole file; the view compares vanilla and each mod candidate tag by tag.

**Load check report**: one engine run of mount, catalog, unit/type loaders and the skirmish content identity,
without starting a battle. The same stages already exist in `sim_headless --galactic` (`docs/gc-mod-bringup.md`).
It records the stage reached and diagnostic rows by code (`EAWR-VFS-*`, `EAWR-UNITS-*`, and the rows of the
loader strictness audit (legacy EAWR-4994)), with counts and three examples each. It also records the profile content
code (below). It is cached by chain fingerprint plus engine build id.

**Launch record** (`mods/launches.json`, last 20): profile id, chain fingerprint, time, exit status, report path.

### Record view semantics (xEdit-style, read-only)

For one object, for example `SpaceUnit "Star_Destroyer"`, the view shows one row per tag and these columns:

| Column | Source | Meaning |
|---|---|---|
| Vanilla FoC | vanilla catalog, effective value | what FoC alone loads (EaW base + FoC expansion, variant chain resolved) |
| one column per mod layer that defines the object, lowest priority first | that layer's raw definition | the value this layer authors, blank when it does not author the tag |
| Effective | profile catalog, effective value | what the engine loads: same loader, same merge rules, same winner |
| How | effective occurrence provenance | `own` / `overridden` (by which layer) / `inherited` (from which base type) / `merged` (`Death_Clone`-style token merge) / `added` |

Rules the view must show rather than hide:

- **Same loader, no second resolver.** Which file wins comes from the VFS mount, the same override order behind
  the effective vanilla reference extraction (every archive unpacked in FoC's order). Effective values come from `load_catalog` and the catalog's own resolution; raw shadowed values come from the loader diagnostic pass. The manager never re-implements layer rank, archive
  order, include order or variant merging. If the view and the game disagree, that is a loader
  bug with one place to fix.
- **Whole-definition override across layers.** A stronger layer's definition of an object replaces the weaker
  definition as a whole. Tags are not merged across layers. Per-tag merging happens only along the
  `Variant_Of_Existing_Type` chain (`docs/xml-model.md`). The view therefore names the winning definition. A tag
  that a weaker layer or vanilla authored is `DROPPED (unset)` only when absent from the winner's effective occurrence set after resolving the variant chain. A base tag inherited by the winner is `inherited`.
  The per-layer columns show what each layer authored, and only the effective column shows what loads.
- **Registry replacement.** A mod's `GameObjectFiles.xml` replaces vanilla's include list for the whole profile
  (`docs/xml-model.md`, "Active registries"). A vanilla definition whose file the mod no longer lists is shown
  greyed out as "not loaded in this profile". The vanilla column is therefore not the lowest layer of the profile
  catalog. It comes from a separate vanilla load.
- **Variant chains.** A row whose effective value is inherited names the base type it came from and the layer that
  authored it there. "Show chain" expands the derived-to-base chain, with one sub-column per type, so a mod that
  changes a base type and thereby changes every variant is visible on the variant.
- **Duplicates in one layer.** Two definitions in one layer are shown as two sub-columns, with the later one
  marked as the winner, together with the catalog's duplicate diagnostic.
- **Empty overrides.** An empty element is a real override (`docs/xml-model.md`), shown as `(empty)`, not as blank.
- **Nested elements** (hardpoint lists, ability blocks) stay attached to their winning occurrence and are shown as
  an expandable sub-tree, not flattened.
- **Engine use (owner: yes, nice-to-have).** The service supplies the column in each result row, mapping the current `docs/tag-coverage/statuses-*.json` values (indexed by `statuses.json`): `applied` = applied, `partial` = partially applied, `todo` = not yet implemented, `deferred` = deferred, `by-design` = intentionally excluded, `foc-ignores` = ignored by FoC, `land-or-galactic` = land or galactic scope, `multiplayer` = multiplayer scope, and `presentation-later` = presentation deferred. Unknown or missing status is shown as unknown. These are coverage classifications, not a blanket claim that every context reads the tag.
- Read-only. Each cell can show its source: the definition's XML text with the line marked, read through the VFS.
  This works for files inside `.meg` archives too. A loose file also offers "show in folder". There is no editing
  and no "fix" button.

Navigation: a search box over object names (all categories) and over tag names ("every object whose
`Max_Speed` a mod changes"), filters "changed by any mod", "changed by <layer>" and "conflicts: several mods
author the same tag", and a link from a file-conflict row to the records defined in that file.

### Indexes and cost

The checked Remake candidate set has 687 GameObject includes (`docs/behaviour/xml-registry-edge-cases.md`); its total file count is unmeasured here. The plan is: index once, cache, open records
lazily.

- **Build.** The first open of the conflict or record view for a chain fingerprint runs one VFS mount, one profile
  catalog load and, once per install, one vanilla catalog load. This is the same work the load check already does,
  so the load check builds both indexes in the same pass. It runs on a worker thread with progress and Cancel.
- **Cache.** `<user data>/mods/cache/<chain fingerprint>-<engine build>/` holds `files.idx` (path, candidates,
  digests) and `records.idx` (object id, category, layers, winner, variant chain, definition digests, source
  locations and per-definition include and occurrence order), in a versioned compact binary format. A Steam update changes the fingerprint and invalidates the
  cache. The cache is bounded (LRU, 512 MB default) and can always be deleted.
- **Resolved entry locations.** `files.idx` also stores, for every candidate, where its bytes live: loose path,
  or archive plus entry offset and size. The conflict view, the source pane and the targeted record reload (MM-11) can then
  read one entry without parsing every archive header again. The cache is only a shortcut. Each archive's size,
  modification time and header digest are stored with it and rechecked, and a mismatch drops the cache. The
  MEG safety bounds in `docs/vfs.md` still apply to every read. Whether the game itself should use such an
  index at start-up is a performance question for the engine, outside this design.
- **Open.** Opening a record needs values. The manager keeps the most recently opened profile's catalog in memory
  while the view is open. Otherwise it reloads only the includes that define the object and its variant chain,
  named by `records.idx`, through the same parser and resolver, with the full catalog as the only fallback.
- **Measure, don't guess.** The first content-view ticket records deterministic counts (files mounted, includes
  parsed, definitions indexed, bytes read) and the wall time of a full Remake index build on a documented test host. Budgets
  come from that measurement, following the performance rules: counted work, not wall-clock tests.
- **Low-end.** Index files are streamed and memory-mapped. The record list is virtualised. The view never holds
  two full catalogs on a machine below a RAM threshold set from the measurement.

### Discovery

1. **Steam libraries.** Discovery reuses the launcher's `libraryfolders.vdf` reader. For every library with
   `steamapps/workshop/appworkshop_32470.acf`, the installed items come from `WorkshopItemsInstalled`, with size,
   `timeupdated` and `manifest`. Folders come from `steamapps/workshop/content/32470/<id>`. An item listed but
   without a folder (or with `NeedsDownload`) is `downloading`. A folder not listed is `installed` with a warning.
   An item is never deleted, moved or "repaired".
2. **The game's `Mods` folder.** Every folder is listed (retail's MNU-18 rule from the parked in-game page (legacy EAWR-2983): folders only, cut at
   2048, ASCII case-insensitive order).
3. **Manual folders** the player adds with Browse. Discovery stores the path, never copies the folder.
4. **Parents**, in this order, each recorded as edge provenance:
   - the player's own chain in the profile;
   - `modinfo.json` dependencies. The reader is written from the public eaw.modinfo format description:
     `dependencies` list, `modtype` 0 = Mods folder path and 1 = Workshop id, an optional leading resolve-layout
     string, and variant files `<name>-modinfo.json` that each make their own entry;
   - a planned `known-chains.json` in the launcher repository (ids and display names only, for example
     `3229239424 -> 2794270450 -> 1770851727`). It is reviewed like code and contains no mod files;
   - `STEAMMOD=<id>` / `MODPATH=<path>` tokens in a mod's own top-level launch scripts. Only the tokens are read,
     never executed, and they are shown as a hint the player confirms.
5. **Library-only items** (role `parent-only`) come from `known-chains.json` or `modinfo`. They stay hidden from the
   "Play" list but are shown in "All installed" and as parents.
6. **Missing parent**: the profile cannot launch. The banner names the missing id and whether Steam lists it at
   all. As a heuristic, the load check flags dangling object names that usually mean a missing layer
   (`docs/gc-mod-bringup.md`, "Submod chains").
7. **Steam updates**: a changed `manifest`/`timeupdated` or tree hash marks the record "updated since last
   launch". It invalidates the conflict and load-check caches and changes the chain fingerprint. A profile whose
   item is `downloading` refuses to launch until Steam finishes.

### Multiplayer identity

- **Chain code.** A short code for people to compare (10 base32 characters), computed as SHA-256 of
  `eawr-mod-chain`, version 1, then the ordered layer list as `(kind, id)`. Workshop items use their id. Mods-folder
  and manual items use their folder name plus tree fingerprint. It answers "do we have the same mods in the same
  order?" before anyone connects.
- **Profile content code**: a short form of the map-independent domains of the skirmish content identity (rules,
  AI data, script manifest, numeric ABI, sandbox), taken from the load check. It answers "will EAWR-MATCH accept
  us?". It reuses `compute_match_identity` over the existing domains and adds no new wire domain, so replay pins
  and the handshake do not move. The handshake stays authoritative: EAWR-MATCH-0001 still refuses a mismatch,
  which the code only predicts.
- **Lobby and join** (later phase): the lobby listing carries **the chain code only**, not Workshop ids (owner
  answer to question 7). A joining player whose running chain differs is offered the local profile with the same
  chain code. On accepting, the game writes `next-action.json {launch-profile <id>, then join <session>}` and
  restarts. When no local profile matches, the dialog says that the host runs a different mod set and shows the
  host's chain code. The host can share the profile's layer list by other means. The game never subscribes or
  downloads.

### Shader conversion hook

The local mod-shader conversion (legacy EAWR-3824) runs on the player's PC. The manager is where it becomes visible: each
profile shows "shaders: converted / N pending / not needed", and conversion starts from the manager, not mid-game.
The engine-free `src/mods/shader_translation.cpp` already supplies translation policy; the manager reserves the status slot until a status API is available. The conversion itself stays in its own epic.

### Safety

- Mod roots are opened read-only. A contract test runs discovery, conflict and load checks over a read-only
  fixture tree and asserts that no write handle was opened.
- The manager writes only under the user-data folder (`<user data>/mods/`, `launcher/`).
- No Steam Web API calls, no credentials, no subscribe/unsubscribe; the only network use is the Steam title lookup (W2, on by default with a first-run note, cached 7 days). An optional "Open Workshop page"
  button (a `steam://` link handed to the OS) is an owner question.
- Nothing is deleted. "Forget" removes a manual folder from the manager's list only.

### Clean-room and licences

**Game repository (this one): clean-room, unchanged.**

- No MO2, USVFS or xEdit source, documentation text, icons, layouts or plugin code is copied into it.
- This covers `src/mods`, `apps/mods_service`, the VFS, chain resolution and the match identity.
- No GPL code enters this repository.
- We take only general UX ideas that many mod tools share (profiles, a load-order list, conflict flags, a
  per-record column comparison) and describe them here in our own words. The record view's values come only from our own loader
(`docs/xml-model.md`). The `modinfo.json` reader is written from the format description, not from the reference C#
implementation. Mod ids and display names in `known-chains.json` are facts, not mod content. No mod file, Workshop
description text or screenshot enters the repository.

**Launcher repository (`EaW-Revised/eawr-launcher`): not clean-room (owner, 2026-10-26).**

- **Licence.** The launcher is licensed GPL-3.0-or-later.
- **What may be reused.** MO2 and xEdit patterns, file formats and code may be reused, as long as their licences
  are followed:
  - `THIRD_PARTY_NOTICES` lists every reused file and its origin;
  - MPL-2.0 files keep their headers.
- **Porting.** There is no Pascal in the launcher. xEdit (TES5Edit) ideas and algorithms are ported to GDScript or
  C++ there.
- **The boundary stays one-way.** Nothing from the launcher repository moves into the game repository. The two
  meet only at the service protocol (section 5), a documented data interface.
- **Engine resolution stays in the game repository.** The launcher still never re-implements what must equal the
  engine (section 4).

## 3. Workshop integration

Owner follow-up, 2026-10-25: can the manager integrate the Workshop? The options below are listed from least to
most reach. Each was checked against Valve's public Web API reference, and the keyless calls were tried once
against a live item.

| Option | What it does | Needs | Limits and risks |
|---|---|---|---|
| W1. `steam://` links | "Open Workshop page" and "Browse EaW Workshop" hand a `steam://url/CommunityFilePage/<id>` link to the OS. The Steam client opens the page, and the player subscribes or unsubscribes there. Steam downloads into the normal `workshop/content/32470` folder, and discovery picks the item up from `appworkshop_32470.acf`. | a Steam client installed | Our code does no networking. We cannot subscribe on the player's behalf or know when the download is done, except by watching the `.acf` and the folder. |
| W2. Keyless Web API lookups | `ISteamRemoteStorage/GetPublishedFileDetails` (HTTP POST, no key) returns per item: `title`, `description`, `tags` (`FOC`/`EAW`, …), `time_updated`, `file_size`, `preview_url`, `visibility`, ban state. A test on 2026-10-25 for the eight installed ids returned all eight titles. `GetCollectionDetails` (no key) returns a collection's member items. | network, opt-in | It fixes the "Workshop item <id>" naming problem and gives "update available" (remote `time_updated` newer than the local `.acf`). It does **not** return dependencies ("required items"). `IPublishedFileService/GetDetails` with children is refused without a Web API key (checked: HTTP 401). The description is author text: we show it as plain text (BBCode stripped) and never parse it for chains. |
| W3. Dependencies via a keyed call | `IPublishedFileService/GetDetails` with `includechildren` lists an item's required items. | a Steam Web API key | A key cannot ship in a public binary, because the Steam Web API terms require the key to be kept confidential. Asking each player for their own key is poor UX. The only clean form is a small lookup endpoint on our own relay service that holds the key and returns ids → children. It is cacheable, but it is a new network service with its own privacy note. |
| W4. ISteamUGC in our launcher | Through the Steamworks client API: `SubscribeItem`, `UnsubscribeItem`, `DownloadItem`, item state and install info, and query results with children (dependencies) without a Web API key. | `steam_api64.dll` from the Steamworks SDK, initialised **as app 32470** (the game the player owns), with the Steam client running | We are not the Steamworks partner for app 32470. Running under another publisher's app id is outside what the SDK agreement grants us, and Steam would show the player as "playing" EaW while they use the manager. It would also add a Valve binary, under Valve's redistribution terms, to our unsigned release. **Not recommended** without explicit owner acceptance of that risk. |
| W5. SteamCMD | `workshop_download_item 32470 <id>` | for EaW (must-own), a logged-in account, which means the player's Steam credentials and Steam Guard | **Out.** It breaks need 6 (no credentials) and puts account secrets in our files. Using it anonymously fails for must-own items. |

Collections are a good fit with W2. "Import collection as profile" reads a collection's items (keyless) and
proposes a profile from the installed ones. Missing items get W1 "open page" buttons. The collection's order is
only a suggestion: chain resolution and the player decide the load order.

Updates: the Steam client stays the only updater. The manager shows "Steam lists an update" (the `NeedsUpdate`
flag in the `.acf`, or a newer remote `time_updated` from W2) and "Steam is downloading". It refuses to launch a
profile while an item is mid-download. It never downloads anything itself.

Offline: everything works offline from the local `.acf`, the folders, `modinfo.json`, `known-chains.json` and the
W2 cache (`mods/workshop-cache.json`, entries kept 7 days, refreshed on demand). W2 is a setting, "Look up
Workshop details online". It is **off by default** (owner, 2026-10-25), in line with the product rule "no network except what the game
already does". With it off, names come from the cache or the alias.

Antivirus: W1 and W2 are an OS link hand-off and plain HTTPS from the Godot process, with nothing unusual
about them. W4 adds a third-party DLL loaded under another app's id. W5 stores credentials.

**Recommendation:** ship W1 in Phase 1 (it is cheap and the Steam client does all the work). Ship W2 with
collection import in Phase 2, opt-in, as the fix for Workshop names and update badges. W3 is nice-to-have backlog (owner);
it is only worth it if the known-chains table proves too thin. Do not build W4 or W5.

## 4. Software stack

Repo facts (2026-10-25):

- the viewer is C++ as a Godot 4.7.2 GDExtension, and the repository has **0 GDScript files**;
- 740 tracked Python files, all tooling and tests;
- `EAWR.exe` was written as native C++ so that players need no Python or other runtime: no console window, no
  Python, static CRT (legacy EAWR-3962);
- `tools/check_presentation_boundary.py` (UI-07) already scans `.gd` files under `apps/viewer/project` for live
  session access, so GDScript is inside the boundary gate from day one;
- the parts that must equal the engine already exist in C++ (`src/vfs`, `src/data` catalog and merge,
  `match_identity`).

What must stay in C++, and what is cheap in a higher-level language:

| Must stay C++ (shared with the engine) | Cheap in a higher-level language |
|---|---|
| discovery that the engine reuses to re-resolve a profile; profile parse | screen layout, lists, filters, the first-run wizard |
| chain resolution, fingerprints, chain code | profile editing flows, renames, separators, drag/reorder |
| VFS mount, `candidates`, file index | Workshop HTTP (W2) and its cache, `steam://` hand-off |
| catalog load, record index, effective values and provenance | polling the `.acf`, progress and Cancel UI |
| load check, profile content code (match identity) | text, help and the record table's presentation |

If a second implementation of the left column existed in another language, the views would drift from what the
engine loads. That is the one failure the owner's record-view request rules out.

| Option | Effort | Agent iteration | Player packaging | Antivirus | Linux / Deck, controller | CI (zero hosted minutes) | Engine sharing |
|---|---|---|---|---|---|---|---|
| (a) all C++ in the viewer | UI pages at the measured viewer-UI rate (median 7.23 h, p80 10.36 h) | every UI change requires a viewer build and may wait for build capacity | nothing new | nothing new | Godot: yes; Godot focus navigation and joypad input built in | existing viewer tests on our hosts | direct |
| **(b) C++ core as GDExtension classes + UI in typed GDScript scenes** | core tickets at the model rate; UI tickets bounded by the viewer-UI rate (no GDScript sample yet) | UI edits need no compile: edit `.gd`/`.tscn` and rerun Godot headless; a pool build only when the core changes | nothing new: scripts ship in the project folder the release already packs | nothing new | same as (a) | headless Godot scene tests on our own hosts; a GDScript linter (Python, local) | direct, the same DLL |
| (c) Python UI (PySide6, Dear PyGui, Tk) over a JSON CLI | fast to write | fast | needs a frozen runtime (PyInstaller/Nuitka, 30-150 MB); our release dropped Python for this reason | frozen-Python exes are a common false-positive source | Qt runs on Linux desktop, poor in Deck gaming mode; little controller support | fine | via a subprocess CLI: a second process and a schema to keep in sync |
| (d) local web UI over the CLI | moderate | fast | a browser tab or a webview (WebView2 on Windows, WebKitGTK on Linux) | a local HTTP port can trigger firewall prompts and is an attack surface (other sites can reach localhost) | poor controller story | fine | via CLI, as (c) |
| (e) .NET/WPF, Tauri or Electron | new toolchain (C#, Rust or Node) | moderate | WPF: Windows-only, and .NET self-contained is 60-150 MB; Electron 150 MB+; Tauri is small but needs Rust and a webview | moderate | WPF none; Tauri/Electron desktop only | a new toolchain on the build hosts | via CLI, as (c) |

**Recommendation: (b).** Keep everything that must equal the engine in C++ (`src/mods` plus the existing VFS,
catalog and identity code). Expose it to Godot as a few GDExtension classes that return plain data: profile
lists, the file index page by page, record rows. Build the manager's screens in typed GDScript scenes. This is
the "easier than C++" path that costs nothing at packaging time. It needs no new runtime, installer, port or
toolchain. It runs on Linux and the Deck with controller navigation, and agents can change a screen without
waiting for a pool build. The price is a second language in the viewer project. Mitigations: typed GDScript only,
a linter, headless scene tests, and a rule that GDScript never computes anything that the left column of the
table owns.

**When it would flip:** to (a), if the owner wants a single language in the viewer more than the faster UI
loop; to (c), only if the manager had to exist before the viewer can start, which the EAWR.exe launch loop
already avoids.

Code layout under (b): the screens are `*.gd`/`*.tscn` in the launcher repository. On Linux, where `EAWR.exe` does
not exist, the manager scene also owns the launch loop via Godot's process API, so the Linux launcher shrinks to
a `.desktop` file. **Since the repository split (section 5)** the C++ core is reached through the local
`eawr_mods_service` process over stdio, not through GDExtension classes. The language choice is unchanged; only the
binding moved out of process.

## 5. Repository split

Owner, 2026-10-25 ~21:00: the launcher and manager become their own project in a separate private repository
(`EaW-Revised/eawr-launcher`), with its own lane. This section fixes the line between the two repositories. It
changes the binding in section 4: the screens still use typed GDScript, but they reach the C++ core through a
local service process rather than GDExtension classes loaded in-process.

### What lives where

| Launcher repository (`eawr-launcher`) | Game repository (this one) |
|---|---|
| the manager's Godot project: typed GDScript screens and scenes, theme, text | `src/mods`: discovery, records, fingerprints, chain resolution, `modinfo.json` reader |
| profile editing UX: create, copy, rename, reorder, separators, launch settings | the profile **file format** and its reader/writer, and the engine's `--eawr-mod-profile` |
| Workshop W1 (`steam://` hand-off) and W2 (keyless lookups, cache, collection import) | VFS mount, file index, catalog, record index, effective values and provenance |
| `known-chains.json` (reviewed ids and names) | load check, chain code, profile content code (match identity) |
| the GDScript client of the service protocol, plus a fake service for its tests | `eawr_mods_service`: the local service process and its protocol (below) |
| the Linux launch loop (`.desktop` entry, manager scene starts the game) | `EAWR.exe` (`apps/launcher`): the Windows entry point and launch loop, `next-action.json` |
| its own release artifact (project zip + manifest + SHA-256) | the in-game Mods button and the lobby's chain-code check |
| its tests, lint and release script | the player release: bundles a pinned launcher artifact |

The rule is the same as in section 4. Anything whose answer must equal the engine's stays in the game repository
and is reached only through the service. The launcher repository holds no VFS or XML resolution and no second
resolver. Any C++ it carries (for example ported xEdit algorithms) is under the launcher's GPL-3.0-or-later
licence and never moves into the game repository. It also never writes a profile file directly; it asks the service to save one, so the format has
exactly one owner.

`known-chains.json` is planned in the launcher repository because it is curation, not engine logic; no such file is on main yet. The service takes
it as an input (`resolve` accepts known-chain hints), so chain resolution itself stays in C++.

### How the launcher reaches the engine side: a stdio service

Three bindings were considered:

| Binding | For | Against |
|---|---|---|
| **local service over stdin/stdout (JSON lines)** | no port, no firewall prompt, no ABI coupling; the launcher repo needs no C++ toolchain; one process serves interactive paging (file index, records), progress and Cancel; the transcript is a natural test fixture | one more executable in the release; a protocol to version |
| GDExtension DLL shipped by the game repo | in-process, no serialisation | binary coupling to the game's build and Godot ABI; the launcher can only be tested with a built DLL; a crash in the core takes the manager down |
| one-shot CLI calls (`eawr-mods files --json`) | simplest | reloads the catalog on every call; the record view becomes slow or needs its own cache protocol anyway |

**Choice: the stdio service.**

- **The executable.** `eawr_mods_service` (game repository, `apps/mods_service`) is a GUI-subsystem executable,
  so it opens no console window. The launcher starts it once per manager session with Godot's process API,
  through its stdio pipes.
- **The protocol.** One JSON object per line, request/response with ids, plus progress events:
  - Protocol 1.8 implements `hello`, `shutdown`, `game.find`, `mods.discover`, `chain.resolve`, and `profile.list`, `profile.load`, `profile.save`, `profile.delete` (initial service methods); `chain.check` (1.2); `loadcheck.start`, `loadcheck.cancel`, `loadcheck.result` (1.3); `files.page` (1.4); `mods.changes` (1.5); `loadnotes.read` (1.7); and `settings.set_builtin_layer` (1.8). `hello` returns version, engine build id, release version and capabilities.
  - Planned for MM-10: `records.query`, `records.get`, and `source.get`. Planned for MM-14: `identity.codes`. Their shapes remain provisional.

- **Shape of the answers.** Responses carry finished rows: winner, `DROPPED`, inherited-from, the layer for each
  cell. GDScript only displays them.
- **Limits.** Requests and responses are bounded in size. The service reads only mod roots and the game folder,
  and writes only under the user-data folder.

### Versioning and compatibility between the repositories

- **Protocol version.** `eawr-mods-protocol` is `MAJOR.MINOR`. A minor change only adds capabilities; a major
  change breaks. The service reports its version in `hello`. The launcher declares the majors it supports and the
  minimum minor it needs, and shows a clear "this launcher needs a newer EAWR" message on a mismatch.
- **Profile file version.** The game repository owns it, and the service upgrades or rebuilds old files. Before
  1.0 there is no back-compat promise (owner rule, 2026-10-09).
- **Pins in both directions.**
  - The launcher's release manifest names the game release range it was tested against, for example
    `game >=0.3.0 <0.4.0`, plus the protocol range.
  - The game release pins one launcher artifact by version and SHA-256, in a small file in the game repository,
    and bundles it.
  - A player therefore always gets a pair that was tested together.
- **Mixed versions.** A launcher artifact newer than the pin can be tried locally by the launcher lane. Players
  only get pinned pairs.

### Build, test and release without hosted CI minutes

- **Game repository.**
  - The service builds with the existing presets on our build pool.
  - Its contract tests are ordinary CTest cases over synthetic fixture trees (no retail data): one per protocol
    request, a read-only assertion on the mod roots, and transcript fixtures.
  - The transcripts are written to a folder that the launcher repository consumes as recorded examples.
- **Launcher repository.**
  - No compile step. The lint (the GDScript linter, Python, local) and the headless Godot scene tests run against
    the fake service, which replays recorded transcripts.
  - They take seconds and run on any of our hosts, or locally.
  - They do not need the Windows build pool, because nothing is compiled.
  - An end-to-end pair test runs the launcher against a real `eawr_mods_service` taken from a pinned local game
    build, over a synthetic mod tree.
  - Hosted runners stay at zero minutes. A self-hosted runner, or the existing offload client pointed at a
    launcher checkout, runs the same scripts.
- **Release.** By default the mod manager is not part of game release 0.3 (owner, 2026-10-26). It ships as
  separate preview kits; launcher v0.1.2 is the current one. The bundling below is MM-S2, for when the owner
  decides.
  - The launcher lane cuts a release by zipping the project (scripts, scenes, text, `known-chains.json`), writing
    the manifest and the SHA-256, and publishing it to its own repository's releases.
  - The game release's packaging step (`tools/release/package_release.py`) fetches the pinned artifact, checks the
    SHA-256 and places it beside `project\` as `launcher\`.
  - `EAWR.exe` starts `godot\Godot_v4.7.2-stable_win64.exe --path launcher`, reusing the game's Godot runtime,
    so no second 80 MB runtime ships.
  - The release follows the release rules in force when it is cut.
- **Public-safety.** The same publish rules apply in the launcher repository from day one: no owner paths, host
  names or mod files. It is private now, and a later public release needs the same scan.

### Effect on the plan

The tickets move as follows (section 7 shows the updated tables):

- **Launcher repository:**
  - MM-0, now the repository scaffold: Godot project, lint, headless test runner, fake service, release script;
  - MM-5;
  - the view half of MM-6;
  - MM-8, MM-12, MM-13, MM-16, MM-17 and MM-18.
- **Game repository:**
  - MM-1, MM-2, MM-3, MM-4;
  - the runner half of MM-6;
  - MM-7, MM-9, MM-10, MM-11;
  - MM-14 (the code; display is in the launcher and the lobby) and MM-15 (in-game lobby).
- **New, game repository:**
  - **MM-S1** `eawr_mods_service` with protocol v1 (`hello`, discovery, resolve, profiles, load check with
    progress and Cancel), the transcript fixtures and the contract tests;
  - **MM-S2**, the release bundling of a pinned launcher artifact (pin file, SHA-256 check, layout, `EAWR.exe`
    start line);
  - later protocol additions (`files.page` in MM-7, `records.*` and `source.get` in MM-10/MM-11) ride on those
    tickets.
- **Dropped:** the GDExtension skeleton that was part of MM-0.

Cross-repository coordination between two lanes is not in any measured class. It is listed as a risk, not
padded into the numbers.

## 6. UI sketch

All screens are Godot UI in the viewer project, fully keyboard and controller navigable (focus order top to
bottom, left to right). They use our UI theme, honour the settings file's UI scale and use Godot's screen-reader
support (AccessKit) for labels and list rows. Status is never shown by colour alone: every flag also has a word or
icon.

### First run (no profiles yet)

```text
+-------------------------------------------------------------------+
| EAWR Mods                                                         |
|  Forces of Corruption: found (Steam)              [Change folder] |
|  Looking for mods... Workshop 8 items, Mods folder 1, manual 0    |
|                                                                   |
|  Suggested profiles (from what is installed):                     |
|   [x] Remake            3229239424 > 2794270450 > 1770851727      |
|   [x] Alliance at War   2191440256                                |
|   [x] Stargate          1145741406                                |
|   [ ] Republic at War   1129810972   (modinfo.json)               |
|   [x] Unmodded FoC                                                |
|                                                                   |
|  [Create selected profiles]   [Skip, I'll build my own]           |
+-------------------------------------------------------------------+
```

### Main screen: profiles and play

```text
+------------------+------------------------------------------------+
| PROFILES         | Remake                         chain code K7Q2-M9XA4F |
| > Remake      ok | Layers (leaf first)            why        state       |
|   Alliance at War| 1 FFT submod (3229239424)      player     updated *   |
|   Stargate   (!) | 2 CW Holdouts (2794270450)     known-chain ok        |
|   Unmodded FoC   | 3 Remake 4.0 (1770851727)      known-chain ok        |
|                  |   Forces of Corruption, Empire at War (always)       |
| [New] [Copy]     | Launch: Front end  Render thread [Safe v]  FPS [120] |
| [Rename] [Delete |                                                      |
|  profile]        | Load check: 3 warnings (last run 19:40)  [Details]   |
|                  | Conflicts: 412 files [View]  Records [Open] |
| ALL INSTALLED    |                                                      |
|  8 Workshop      |  [ PLAY ]                                            |
|  1 Mods folder   |                                                      |
|  [Add folder]    | * updated by Steam since the last launch: load check |
|                  |   will rerun before play                             |
+------------------+------------------------------------------------------+
```

- "Delete profile" deletes the profile file only, never a mod.
- In a non-chain stack the order can be edited (Alt+Up/Down, drag, controller shoulder buttons). Moving a layer
  above one of its declared parents shows a warning, which the player can still accept.
- PLAY runs the load check first if the chain fingerprint changed, then hands off to the launcher.

### Load order

This is the layer list on the main screen. Each row shows its conflict flags, in the spirit of MO2:
`+` wins files from a lower layer, `-` loses files to a higher layer, `=` only identical copies. Selecting a row
highlights the layers it overrides and the layers that override it.

```text
| Load order (top wins)                     flags   files  wins  loses |
| -- Remake ------------------------------  (separator)                |
| 1 FFT submod (3229239424)                 + -     2140   1810  12    |
| 2 CW Holdouts (2794270450)                + -      930    420  95    |
| 3 Remake 4.0 (1770851727)                 +       6220   3900   0... |
|   Forces of Corruption / Empire at War    (always last, not movable) |
```

### File-conflict view

```text
+--------------------------------------------------------------------------+
| File conflicts: Remake      show [only conflicts v]  type [all v]        |
| layer [any v]  role [winner|loser|either v]   [search path...          ] |
|  Path                                 winner         also in      same?  |
|  Data/XML/GameObjectFiles.xml         1 loose        3, FoC       no     |
|  Data/XML/Units_Space_Empire.xml      3 Remake.meg   FoC          no     |
|  Data/Art/Models/EI_Star_Dest.alo     2 loose        3            yes    |
|  ...                                                     12,480 rows     |
|  > selected: Data/XML/Units_Space_Empire.xml                             |
|    1. layer 3, archive Remake.meg (wins)                                 |
|    2. FoC, archive Config.meg                                            |
|    [Records in this file]                                                |
+--------------------------------------------------------------------------+
```

- The filters (layer, winner/loser/either, file type, path search, "only conflicts", "hide identical copies") can
  be combined and are saved per profile.
- The view is informational only. It explains, but never changes or edits files. For a chain it does not offer
  per-file "hide" switches, because retail cannot do that either.

### Record view (content level)

```text
+------------------------------------------------------------------------------------+
| Records: Remake    [search objects / tags...]   [changed by any mod v]             |
| SpaceUnit  Star_Destroyer   variant of: Generic_Star_Destroyer   defined in: FoC, 3, 1 |
|  Winning definition: layer 1 (FFT submod), Units_Space_Empire.xml line 812         |
|  Tag                    Vanilla FoC   3 Remake 4.0  1 FFT submod   Effective   How   |
|  Max_Speed              2.0           2.5           3.0            3.0         own 1 |
|  Tactical_Health        3000          4200          4200           4200        own 1 |
|  Shield_Points          1500          1500          -              (unset)     DROPPED|
|  Armor_Type             Armor_Hvy     -             -              Armor_Hvy   inh base|
|  > HardPoints (12)      [expand]                                                 |
|  [Show variant chain]  [Show source XML]  [Engine use column: on]                |
+------------------------------------------------------------------------------------+
```

- The winning cell is marked by bold type and a `>` marker, never by colour alone.
- "own N" names the layer whose definition won. "inh" names the base type a value came from. `DROPPED` marks a
  tag that vanilla or a lower layer authored but the winner's resolved variant chain does not contain. In this example `Generic_Star_Destroyer` also lacks `Shield_Points`. This is the most common
  surprise for modders, so it gets its own filter.
- With a keyboard, the arrow keys move between cells and Enter opens the source. A screen reader reads each row as
  "Max_Speed: vanilla 2.0, Remake 4.0 2.5, FFT submod 3.0, effective 3.0, own value of FFT submod".

### Load check details

The rows are grouped by code, each with a count, three examples and a link to the doc for that code. It also
shows the stage reached ("catalog ok, units: 2 errors"), the profile content code, and requirement checks: "needs
FoC" (always), "needs parent X", "made for retail version …" (only when `modinfo` says so).

### Low-end behaviour

- The manager opens with Godot's compatibility renderer, so a weak or broken GPU driver still shows it. This is to
  be confirmed against our project settings; if confirmed, the manager can also tell the player why the game did
  not start.
- Lists are virtualised (Workshop libraries can hold hundreds of items).
- The load check runs on a worker thread with a Cancel button and never blocks the UI.

## 7. Phased plan

### Estimate basis

Lane hours run from the first commit to the merge, for merged PRs in this repository. The samples were taken
2026-10-25 from the merged-PR list. p80 is by nearest rank. A PR may sit in two classes.

| Class | Sample PRs | n | median h | p80 h |
|---|---|---|---|---|
| engine-free model + contracts | (legacy EAWR-3018, EAWR-2126, EAWR-2053, EAWR-2929, EAWR-3972, EAWR-244, EAWR-3040) | 7 | 3.19 | 7.37 |
| viewer UI page/flow | (legacy EAWR-4042, EAWR-3056, EAWR-3040, EAWR-3018, EAWR-2982, EAWR-2978, EAWR-2974, EAWR-2965, EAWR-2958, EAWR-2957, EAWR-2929) | 11 | 7.23 | 10.36 |
| launcher / start tooling | (legacy EAWR-4432, EAWR-4066, EAWR-3972, EAWR-1004) | 4 | 4.92 | 7.65 |
| match identity | (legacy EAWR-4092, EAWR-2126, EAWR-2053) | 3 | 3.55 | 11.85 |

Not included: owner eye checks, Windows pool queue waits, and the parked in-game page's unmerged work, which is
reused, not counted (legacy EAWR-2983). The record view has no close analogue in the sample. Its model tickets are
priced as "model", and the measurement in MM-10 re-prices MM-11..MM-13 if the Remake index build turns out heavy.

**UI tickets under the GDScript stack (section 4).** The repository has no GDScript PR to measure, so every UI
ticket keeps the measured C++ viewer-UI price as its bound. The C++ samples include a pool viewer build per
iteration, and GDScript screen edits skip that step. The faster loop is therefore expected but not counted. The
first GDScript ticket (MM-5) is measured, and its lane hours re-price the remaining UI tickets (MM-6, MM-8,
MM-12, MM-13, MM-15, MM-17, MM-18). Python tooling PRs measured at a median of 0.14 h (legacy EAWR-4696, EAWR-3609, EAWR-3283, EAWR-1014, EAWR-551) are not a fair
analogue: they are small infrastructure fixes, not screens.

### Phase 1: usable by the owner (discovery, load order, profiles, launch, diagnostics)

| Ticket | Scope | Class |
|---|---|---|
| MM-0 | **launcher repo**: repository scaffold: Godot project, typed-GDScript rules, a local GDScript linter, a headless scene-test runner, the fake service replaying transcripts, the release script | launcher |
| MM-S1 | **game repo**: `eawr_mods_service` with protocol v1 (`hello`, discovery, resolve, profiles, load check with progress and Cancel), transcript fixtures, contract tests | model |
| MM-S2 | **game repo**: the release bundles a pinned launcher artifact (pin file, SHA-256 check, `launcher\` layout, `EAWR.exe` start line); deferred: not in release 0.3 by default, separate preview kits until the owner decides | launcher |
| MM-1 | **game repo**: `src/mods`: records, Steam library + Workshop + Mods-folder discovery, fingerprints; move the launcher's VDF/JSON parsers into it | model |
| MM-2 | **game repo**: chain resolution: `modinfo.json` reader, launcher-supplied `known-chains.json` hints, launch-script hints, missing-parent and update states | model |
| MM-3 | **game repo**: profile format with the explicit load order + engine `--eawr-mod-profile` (viewer and `sim_headless`), settings overlay, re-resolve by key | model |
| MM-4 | **game repo**: launcher: manager/launch loop, `next-action.json`, "EAWR Mods" shortcut, `--render-thread`, last profile | launcher |
| MM-5 | **launcher repo**: manager mode: first run, profile list, load-order editing (drag, keys, controller, separators, dependency warnings), launch settings, PLAY; Workshop W1 (`steam://` open page, `.acf` watch for new or updated items) | UI |
| MM-6 | **both**: load check runner in the service (game repo) + details view (launcher repo) (diagnostics, requirement checks, profile content code) | UI |

Phase 1 is 4 model, 3 launcher/tooling and 2 UI tickets: median 41.98 h, p80 73.15 h, midpoint about 58 lane
hours (2.4 agent-days). MM-0 (launcher lane) and MM-1 (game lane) run first and in parallel. MM-2..MM-4 can start
once MM-1 settles the record type. MM-S1 needs MM-1..MM-3. MM-5 needs MM-0 and the MM-S1 transcripts, not a
running service, so the launcher lane can build screens against the fake service. MM-6 needs MM-5 and MM-S1.
MM-S2 closes the phase.
With three workers this is about one to two calendar days. At the end the owner can replace the hard-coded kit
start files with profiles.

### Phase 2: file conflicts and the in-game button

| Ticket | Scope | Class |
|---|---|---|
| MM-7 | **game repo**: file index: candidates per path from `Vfs`, per-candidate digests, per-layer win/lose/only flags, versioned cache, built inside the load-check pass | model |
| MM-8 | **launcher repo**: load-order flags + file-conflict view with combined filters (layer, winner/loser, type, path, identical copies), link to records | UI |
| MM-9 | **game repo**: in-game Mods button: writes `next-action {open-manager}` and quits (retail's restart, MNU-18 deviation noted); close or park the in-game page PR (legacy EAWR-2983) | launcher |
| MM-18 | **launcher repo**: Workshop W2, opt-in: keyless item and collection lookups (titles, descriptions, tags, `time_updated`), cache, "update available" badge, "import collection as profile" | UI |

Median 22.57 h, p80 35.74 h.

### Phase 3: content-level record view (xEdit-style)

| Ticket | Scope | Class |
|---|---|---|
| MM-10 | **game repo**: record index: vanilla catalog + profile catalog, `find_all` plus loader-owned shadowed-definition diagnostics grouped by layer, winner / `DROPPED` / inherited / merged classification, registry-dropped vanilla files, whole-file XML tag diff; deterministic counts + one measured Remake build | model |
| MM-11 | **game repo**: targeted record reload (only the includes of one object and its variant chain), cache bounds, low-RAM mode | model |
| MM-12 | **launcher repo**: record view: per-tag table (vanilla, each layer, effective, how), variant chain expansion, nested sub-trees, source pane | UI |
| MM-13 | **launcher repo**: record search and filters: by object, by tag across objects, "changed by layer", "several mods author this tag", `DROPPED`, optional engine-use column | UI |

Median 20.84 h, p80 35.46 h.

### Phase 4: multiplayer and platforms

| Ticket | Scope | Class |
|---|---|---|
| MM-14 | **game repo**: chain code + profile content code in the manager and the lobby UI | identity |
| MM-15 | **game repo**: lobby listing carries the chain; join offers the matching profile and restarts into it | UI |
| MM-16 | **launcher repo**: Linux: the manager scene owns the launch loop (Godot process API), Steam library roots under the user's home, a `.desktop` file; Steam Deck: add as a non-Steam game, controller navigation check | launcher |
| MM-17 | **launcher repo**: shader conversion status slot (wired once the shader conversion has a status API) (legacy EAWR-3824) | UI |

Median 22.93 h, p80 40.22 h.

**Total: 21 tickets, median 108.3 h, p80 184.6 h, midpoint about 146 lane hours (6.1 agent-days)**:
midpoints of about 72 h in the game repository and 65 h in the launcher repository, plus 9 h for MM-6,
which is split between them. The Workshop and stack additions
added MM-0 and MM-18 and folded W1 into MM-5. The repository split adds MM-S1 and MM-S2. The first
usable step is Phase 1. File conflicts (Phase 2) come before the content diff (Phase 3), as the owner asked.
Phases 2 and 3 do not depend on Phase 4, so Phase 4 can run in parallel with them.

### Dependencies on loader work

- **Loader strictness audit (legacy EAWR-4994):** no hard dependency. The load check and the record view show whatever the loaders
  emit, and as that audit lands, mods fail where retail fails and pass where retail passes.
- **Real-mod compatibility scope check (legacy EAWR-1286):** the load check is the player-facing view of this scope check. Phase 1 is
  worth shipping before that check, because it makes the gaps visible per profile. The record view relies on the
  catalog loading the chain. Where that check finds a catalog gap, the view shows the diagnostic instead of values.
- **Shader conversion (legacy EAWR-3824):** only MM-17 waits on it.
- **Modded multiplayer smoke (legacy EAWR-4898):** MM-14/MM-15 reuse its Remake chain as the end-to-end case.

## 8. Risks, owner answers and open questions

Risks:

- **Workshop titles are not on disk.** Offline, an item without `modinfo.json` and not in `known-chains.json`
  shows as "Workshop item <id>" until the player names it. Mitigation: the known-chains table for popular mods,
  plus aliases.
- **Chains without metadata.** Most chains live only on Workshop pages. A wrong guess loads the wrong game. Every
  inferred parent is therefore shown with its provenance, and the player confirms it once per profile.
- **Record view truth.** The view is only as right as the catalog. The cross-layer whole-definition rule and the
  registry replacement rule come from `docs/xml-model.md`. If a retail capture ever contradicts them, the loader
  is fixed and the view follows, with no special-casing in the manager.
- **Index cost on big chains.** This is unmeasured today. MM-10 measures it before budgets are set.
- **Load check cost.** Big mods may take tens of seconds; file counts and load time need measurement. Mitigations: cache by
  fingerprint, a worker thread, and Cancel.
- **The profile content code depends on skirmish content loading.** If the identity domains change, the code
  changes too. That is intended, but the UI must say "for this game version".
- **Workshop lookups (W2).** Valve can change or rate-limit the keyless endpoint. The manager treats it as a best
  effort and works fully without it, and lookups are batched (one POST for all installed ids) and cached.
- **A second language in the viewer (GDScript).** Logic could creep into scripts. The rule "GDScript never
  computes what the engine computes" goes into the review checklist, and the service API returns finished
  rows rather than raw parts.
- **Two repositories, two lanes.** The protocol is the only contract. A change that needs both sides lands in the
  game repository first (a new minor version and capability), and the launcher adopts it after. The launcher never
  depends on an unreleased service. The coordination cost between lanes is unmeasured.
- **Steam library moves or Workshop folder renames.** Re-resolving by key covers moves. A manual folder that moves
  needs Browse again.

Owner answers, 2026-10-25 ~21:00. The design above already reflects them.

| # | Question | Answer |
|---|---|---|
| 1 | approve C; MO2-as-executable left unsupported but not blocked | yes |
| 2 | stack: Godot launcher, C++ core, typed GDScript screens | yes. The launcher is now its own repository and lane (section 5). |
| 3 | ship a reviewed `known-chains.json` | yes (it lives in the launcher repository) |
| 4 | W1 `steam://` links in Phase 1; W2 default | W1 yes; W2 opt-in, **default off** |
| 5 | Data-overwrite mods: warn or ignore | resolved by the owner (2026-10-26): the data-overwrite support is built and switched off until a retail file list exists |
| 6 | one shared settings file, per-profile launch overrides | yes |
| 7 | what the lobby advertises | **chain code only**, no Workshop ids |
| 8 | record view's engine-use column in player releases | yes, nice-to-have |
| 9 | record view for non-XML content later | fine as a later ticket |
| 10 | W3 dependency lookup via our relay service | nice-to-have (backlog) |
| 11 | never subscribe or download by itself (no W4/W5) | yes, for now |

The original questions, kept for reference (owner question list) (legacy EAWR-128):

1. Is C (own manager, MO2-style profiles and conflict flags, no MO2 code) approved, and is MO2-as-executable left
   unsupported but not blocked?
2. Is the stack approved: a mode of the game's own Godot project, launched by EAWR.exe, with the C++ core
   exposed through GDExtension and the screens in typed GDScript? These would be the repository's first GDScript
   files (section 4).
3. May we ship a reviewed `known-chains.json` with Workshop ids and display names of popular mods (Remake chain,
   AaW, Stargate, RaW, FotR)?
4. Workshop: is W1 (`steam://` links, Steam does the networking) fine for Phase 1? And for W2 (keyless online
   lookups of titles and update dates), should "Look up Workshop details online" default to on or to off?
5. Should Data-overwrite mods (files copied into `corruption/Data`) be detected and warned about, or ignored?
6. Do players keep one shared settings file with per-profile overrides only for launch settings (proposed), or
   get fully separate settings per profile?
7. Should the lobby advertise the chain's Workshop ids publicly (needed for "join picks the profile"), or only the
   chain code?
8. Should the record view's "engine use" column (whether this EAWR build applies a tag yet, from the tag registry)
   ship in player releases, or appear only in development builds?
9. Should the record view also cover non-XML content later, for example text strings from the master text file?
   That would be a later ticket; it is not in this plan.
10. Workshop dependencies (W3): is a small lookup on our own relay service, holding a Steam Web API key and
    returning required items per id, wanted? Or are `known-chains.json` plus the player's confirmation enough?
11. Can you confirm that the manager never subscribes or downloads by itself, so no ISteamUGC under app 32470
    (W4) and no SteamCMD (W5)?
