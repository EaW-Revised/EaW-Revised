# Data-assumption audit: where the code assumes the data is FoC's

Audit for (legacy EAWR-2861) (mod fidelity, under (legacy EAWR-1286)). Owner rule, 2026-10-10: mod support is why the game
is still played, so everything a retail-valid mod does should work here. This audit lists every
place we found where the code **structurally** assumes the mounted data is stock Forces of
Corruption. "Structurally" means a fixed name list, a forced path, a bound picked because FoC never
exceeds it, a closed set of accepted values, a decoder limited to what FoC ships, a fixture pin,
a capacity sized by FoC's asset count, or a fallback to FoC content.

It is an audit only: no runtime code changed, and no issues were filed. The coordinator files the
groupings at the end after review.

## Scope and method

- **Base:** `Thom-Ernst/Skirmish` at `c43f0fcb` (2026-10-10). All `file:line` references are
  against that tree.
- **Areas read:** `src/` (all modules), `include/eawr/`, `apps/viewer/src/`, `apps/sim_headless/`.
  `apps/viewer/project` has no `.gd` scripts at this commit (only the scene, project, extension,
  export settings and camera config files). `tests/` and `tools/` are out of scope, except
  `tools/fonts`, which the viewer's runtime font cache depends on.
- **Not repeated:** single literals that copy one XML value. The hard-coded XML sweep (legacy #1858-#1864)
  covered those.
- **Method:** grep and reading. We searched for:
  - file-name literals and `data/` prefixes;
  - `std::array`/`constexpr` name tables;
  - name comparisons against faction, object, shader and script names;
  - `max`/`clamp`/`kMax` bounds;
  - `fixture`/`pin`/`retail`/`FoC` checks in runtime code;
  - comments such as "every FoC ...", "FoC never ..." and "FoC ships".

  We then followed each hit to whether a retail mod could legally differ.
- **Retail rules:** where a rule is sourced, the table cites the rule ID (mostly the mod-loading
  walk, `docs/behaviour/walks/mod-loading.md`, `ML-nn`). Where retail behaviour is not
  established, the entry says **retail rule unsourced**, and the fix ticket must source it first.
- **Clean room:** the doc uses the project's own words, rule IDs and FoC's public XML/Lua names
  only. It contains no debug-build addresses, no decompiled code and no mod file contents.

### How to read the tables

- **Cat**: the (legacy EAWR-2861) categories.
  1. Fixed name list.
  2. Path assumption.
  3. FoC-derived bound or strict validation.
  4. Closed enum or converter set.
  5. Format assumption.
  6. Fixture pin or identity check.
  7. Capacity or residency.
  8. Fallback to FoC content.
- **Consequence**: *crash*, *battle fails* (the whole battle or session refuses to start, or stops
  mid-battle), *silent skip* (content ignored, maybe with a diagnostic), or *wrong* (runs, but does
  not do what the data says).
- **Severity for mods**: **blocks load**, **feature** (breaks a feature) or **cosmetic**.
- **Ticket**: an existing issue that already covers the finding, where one exists.

## Summary

The audit found 111 findings: 36 that block load, 60 that break a feature and 15 cosmetic. They
fall into a small number of shared root causes:

1. **The skirmish start and AI are seeded from stock names.** Examples: the M2 fixture (Coruscant
   hash, six fleet units, Rebel/Empire slots), the 17 AI XML files, the 25 plan scripts, the
   default freestore, Rebel/Empire/Underworld filters, and the three-value HUD faction enum. A mod
   that renames, adds or removes factions or units hits these first.
2. **Session-create validators reject the whole battle over one row.** Unit, ability, motion,
   combat, economy and game-constant validators each fail the session on a single out-of-range
   value. The bounds were chosen from FoC's data, not from a retail rule.
3. **Paths and lookups are re-implemented in many places instead of once.** About ten separate
   copies force `data/art/models/` or `data/art/textures/`, try only a fixed set of extensions, and
   disagree with each other and with ML-39/40/41. SFX samples are forced under `data/audio/sfx`.
   Speech and text are forced to English.
4. **The VFS and XML merge differ from retail precedence.**
   - Every layer must have a `MegaFiles.xml`.
   - SFX and patch archives are a fixed list.
   - Parent mods rank level with base FoC.
   - Duplicate objects are last-wins, where retail keeps the first.
   - Empty or `TBD` tags in a variant wipe out the inherited value, contrary to ML-24.
5. **Decoders accept only what FoC ships.** This covers WAV (16-bit PCM only), music (MP3 only),
   DDS/TGA variants, particle V2 and blend modes, the shader set (about 21 `.fx` names), ALO/ALA
   chunks, MEG v1 only, TED v2.1 only, and precompiled Lua.
6. **Stock Lua object scripts are replaced by native stand-ins chosen by script name.** Point
   defense, IG-2000, power-to-shields, Krayt and MC30 are selected this way. Their constants are
   built in, and any other object script is never run.
7. **Residency is sized for FoC.** There is no GPU texture sharing, compressed arrays are expanded
   to RGBA8, every type is composed eagerly, and the setup scene stays resident. On top of that,
   fixed effect, slot, squadron and fog pools are sized by stock counts.

## Findings

IDs are grouped by area. A finding lists its main lines. Where the same root cause shows up in
several areas, the later entries point back to the first.

### V: VFS, archives and manifests

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| V-01 | 1/8 | `src/vfs/vfs.cpp:269-277`, `411-414`, `455-494` | Every layer, mod roots included, has its own `MegaFiles.xml` with at least one `File` entry. Base and expansion manifests are always mounted beneath the mod. | Ship loose files only, with no manifest; or ship a manifest that intentionally omits stock archives. ML-07/ML-09: retail reads the single winning `Data/MegaFiles.xml` plus the fallback game's manifest. | Battle fails (no or empty manifest). Wrong: archives the mod dropped come back. | blocks load | |
| V-02 | 1/2 | `src/vfs/vfs.cpp:416-424` (dedupe at `306`) | SFX archives are exactly `SFX2D_English`, `SFX2D_Non_Localized` and `SFX3D_Non_Localized`, mounted under a forced `data/audio/sfx` prefix. | Run a non-English install (ML-48 mounts the selected-language 2D archive), or ship its own SFX MEG. A manifest-declared SFX MEG gets no prefix, so its bare-name entries never resolve. | Silent skip (missing samples). | feature | (legacy EAWR-1708) |
| V-03 | 1 | `src/vfs/vfs.cpp:425-429` | `Patch.meg`, `Patch2.meg` and `64Patch.meg` are mounted for every layer and stack. | ML-07: retail opens the current game's slots through the file service, so a mod's `Patch.meg` replaces FoC's. | Wrong precedence. | feature | |
| V-04 | 3 | `src/vfs/vfs.cpp:333-406` | `MegaFiles.xml` is strict: matching UTF encoding label, no DOCTYPE, exact-case `Mega_Files` root and `File` children, no empty entry. | ML-15: retail takes each root child's non-empty value without checking tag names. A Latin-1 or windows-1252 label is legal XML. | Battle fails (mount fails). | blocks load | |
| V-05 | 3 | `src/vfs/vfs.cpp:617-618`; `src/vfs/vfs_archive.cpp:86-93`, `149-177` | Every declared archive parses cleanly: no case-duplicate names and no `..` entries. | ML-08: retail diagnoses an unavailable archive and continues without it. | Battle fails (one bad MEG fails the mount). | blocks load | |
| V-06 | 5 | `src/vfs/vfs_archive.cpp:80-89`, `139-145` | Only MEG v1 exists; there is no header sniffing. | Use tools that write v2/v3 MEGs (retail acceptance unsourced). | Battle fails (the magic word is read as a count and trips the record limit). | blocks load | |
| V-07 | 2/3 | `src/vfs/vfs.cpp:159-174`, `212-255`, `554-581` | No symlinks in loose trees, no case-insensitive collisions in a layer, ASCII-only case folding. | Symlinked Workshop or mod folders (common on Linux/Proton; ML-11 uses the native file API, which follows links), copied case-variant duplicates, non-ASCII names (retail folding unsourced). | Battle fails (mount fails). | blocks load | |
| V-08 | 2 | `src/vfs/vfs.cpp:600-634` | Per-layer precedence: a layer's loose files, then its archives, then weaker layers. | ML-05: retail tries all mod loose files, then the native disk, then all archives globally, so a FoC loose file beats a mod archive. | Wrong file wins (edge case). | feature | |

### D: XML registry, merge and data readers

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| D-01 | 2/8 | `src/data/xml_registry.cpp:516-532` (`layer_rank`) vs `src/vfs/vfs.cpp:448` | Layer IDs are `mod`, `mod[N]`, `expansion` or `base`. The VFS names parent mods `mod-parent-N`, which rank 0, level with base. | Use any submod chain. | Wrong: FoC's (expansion) definition beats the parent mod's for the same ID. | feature | |
| D-02 | 4 | `src/data/xml_registry.cpp:643-655` | A duplicate object name resolves to the strongest layer, then the later definition. Hardpoints use the same generic order. | ML-21: retail keeps the first registered binding. ML-18: hardpoint includes parse in filename-CRC order. | Wrong definition selected. | feature | |
| D-03 | 3 | `src/data/xml_merge.cpp:121-137` | An empty or `TBD` tag in a variant overrides the inherited value. | ML-24: retail leaves the inherited value unchanged. `data::retail::is_placeholder` already implements the rule but is not used here. | Wrong; for example, an empty `Model_Name` drops the inherited model and the object is not drawn (`src/assets/map.cpp:39-46`). | feature | |
| D-04 | 4 | `src/scene/idle_tags.cpp:15`; `src/scene/space_population.cpp:25-30`; `src/data/ui/movie.cpp:119`; `src/presentation/godot/ui/minimap_view.cpp:80-94` | A boolean is true only for `yes`/`true`/`1`; the minimap view accepts only `Yes`. | ML-25: true is `1` or any value starting with Y or T, ignoring case. `data::retail::boolean` exists. | Wrong (flags such as `Is_Marker`, `In_Background` and `Loop_Idle_Anim_00` read false). | feature | |
| D-05 | 2 | `src/data/ui/command_bar.cpp:547` | Command-bar list entries are always `data/xml/` plus the entry. It does not use `registry_include_path`. | Write an entry with a `Data\XML\` prefix or a sub-path. | Silent skip. | feature | |
| D-06 | 1/4 | `include/eawr/data/ui/command_bar.hpp:40-44`; `src/data/ui/command_bar.cpp:113-119` | Command-bar `ALT0/1/2` are fixed to Empire, Rebel and Underworld; component types are a closed set of 5. | Add or rename factions (how retail picks the ALT index is unsourced). | Wrong HUD variant (see P-05). | cosmetic | |
| D-07 | 2/4 | `src/data/ui/dialog_catalog.cpp:355-388`; `src/data/ui/dialog_script_parser.cpp:33-51`, `405-411` | `GUIDialogs.xml` names and RC keywords are case-sensitive and a closed set. | Use different case (ML-28 suggests case-insensitive tag matching) or other RC statements (unsourced). | Silent skip (section or dialog dropped). | cosmetic | |
| D-08 | 3/5 | `src/data/ui/text_database.cpp:240`, `264`, `269`, `277` | Every text key is non-empty printable ASCII with a matching CRC, and there are no trailing bytes. | Text tools that write non-ASCII keys or mismatched CRCs (retail tolerance unsourced). | Silent skip of the whole database (every string shows its key). | feature | |
| D-09 | 3 | `src/data/ui/cursors.cpp:41`, `56-63` | Every listed pointer file exists and parses. | Ship one missing or malformed pointer (retail unsourced). | Silent skip of the whole cursor catalog. | feature | |
| D-10 | 3 | `src/data/xml.cpp:167-179` | A registry root name must match, or the catalog load fails. | ML-15/ML-30 suggest the shared retail reader does not check root names (unsourced for registries). | Battle fails. | blocks load | |
| D-11 | 7 | `src/data/xml.cpp:267-286` | Every effective `.xml` in the VFS is parsed at load, not just registry-admitted files. | Ship thousands of XML files. | Load time and memory (feeds diagnostics only). | cosmetic | |
| D-12 | 5 | `src/script/foc/ai_data.cpp:137-144` (used `262-279`) | `gameconstants.xml` values for the AI are found by naive substring search: the first literal `<Tag>`, case-sensitive, with no comment handling. | Keep a commented-out older value earlier in the file, or vary case or whitespace. | Wrong (a wrong value or the engine default is silently used). | feature | |
| D-13 | 5 | `src/script/foc/ai_data.cpp:66-120`; `src/script/foc/ai_equations.cpp:335-369` | The AI XML readers handle no entities or CDATA, keep only the `Name` attribute, and do not match end-tag names. | Use `&amp;` or CDATA (whether retail accepts them is unsourced). | Wrong names; inside an equation, battle fails (see S-06). | feature | |

### S: Script, AI and Lua

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| S-01 | 1 | `src/script/foc/ai_data.cpp:228-248` (`ai_xml_files`), `255-257`; `src/script/foc/tactical_ai_loading.cpp:235`; `src/skirmish/ai.cpp:184-193` | The goal system's data is exactly 17 stock files: 3 players, templates, 3 function sets, 2 goal files, 6 equation files, difficulty adjustments and game constants. A missing file is fatal. | Add player, template, goal, function-set or equation files, which retail reaches through data references. | Silent skip: an unknown player type is dropped (`ai_service.cpp:56-57`), so that faction runs freestore only. Unknown sets or equations are dropped (`ai_service.cpp:73`, `82`). | feature | (legacy EAWR-1700) |
| S-02 | 1/8 | `include/eawr/script/foc/tactical_ai.hpp:212`; `src/script/foc/tactical_ai.cpp:295` | Every AI player runs `BusyTacticalFreeStore.lua`; nothing reads the player type's freestore. | Name another space freestore per AI player type (AI-01). | Wrong (stock freestore runs). | feature | (legacy EAWR-1700); tag status todo (737) |
| S-03 | 1/2 | `src/script/foc/tactical_ai_loading.cpp:237-257`, `272-274`, search dirs `59`, `165`; `src/script/foc/tactical_ai.cpp:286`; `src/skirmish/ai.cpp:196-214`; `src/script/authoritative/bindings.cpp:535-587` | The stock scripts' `require` graph is the whole graph. Modules come from a fixed list and fixed folders, and `require` sees only the manifest. | Have a freestore, library or plan require a new helper (ML-34, ML-38), or keep helpers in another folder. | Battle fails (freestore or library); silent skip (plan dropped, `tactical_ai_loading.cpp:184-187`). | blocks load | (legacy EAWR-1429) |
| S-04 | 1 | `src/script/foc/tactical_ai_loading.cpp:259-270` (used `149-201`, `302`) | The AI's plans are exactly 24 SpaceMode plans plus 1 AI plan, by name. | Add, remove or rename plan scripts (retail discovery rule unsourced). | Silent skip (added plans); battle fails if a listed stock plan is missing (`src/skirmish/ai.cpp:206-207`). | feature | (legacy EAWR-1700) |
| S-05 | 4 | `src/script/foc/tactical_ai_loading.cpp:206-228`; `src/script/foc/ai_equations.cpp:182-187` | Equations use only the `GameObjectCategoryType`, `DifficultyLevelType` and `HardPointType` converters; an unknown converter or value is a parse error. | Use other converter enums such as `AIGoalCategoryType`, which FoC itself defines. | Battle fails. | blocks load | (legacy EAWR-1701) |
| S-06 | 3 | `src/script/foc/ai_equations.cpp:171`, `353-365`; `src/script/foc/ai_data.cpp:348-349`; `src/script/foc/tactical_ai.cpp:270-271` | Every equation in every loaded file uses the stock grammar: only `clamp()` as a function and plain `<Name>` start tags. One bad equation fails the set. | Include an equation retail tolerates or never evaluates (retail function list unsourced). | Battle fails. | blocks load | (legacy EAWR-1703) |
| S-07 | 4/6 | `src/script/foc/tactical_ai_loading.cpp:225`; `src/script/foc/ai_perception.cpp:763-768` | No hardpoint type matters: every `HardPointType` value maps to 0, and hardpoint health always reads 0. | Score goals on hardpoint health. | Wrong. | feature | |
| S-08 | 4/8 | `src/script/foc/ai_perception.cpp:460-493`, `532-615`, `618-660`, `690-705`, `771-774` | Only the stock space-skirmish perception tokens (AI-15) exist. The only `Script_` evaluator is native and returns a constant instead of running the evaluator Lua (AI-51). | Use other retail tokens, or add evaluator scripts. | Wrong (the equation fails and desire is 0); evaluator scripts are never run. | feature | |
| S-09 | 6 | `src/script/foc/ai_service.cpp:65-70`; `src/script/foc/ai_perception.cpp:610-613`; `src/script/foc/tactical_ai_bindings.cpp:672-673` | Difficulty is always Normal: only the Normal adjustment is read, `IsDifficulty` is true only for Normal, and `Get_Difficulty` returns "Normal". | Tune Easy/Hard adjustments, or branch on difficulty. | Wrong. | feature | (legacy EAWR-753) (related) |
| S-10 | 1/2 | `src/script/foc/tactical_ai.cpp:186-191`; `src/script/foc/tactical_ai_loading.cpp:249-255` | Only `ObjectScript_MillenniumFalcon` and `ObjectScript_VaderTieSquadron` are attached, at a forced `Data/Scripts/GameObject/<name>.lua` path (SOS-45). | Give any type a `Lua_Script`. | Silent skip (other object scripts never run). | feature | (legacy EAWR-1711) |
| S-11 | 8 | `src/script/foc/ai_taskforces.cpp:295-301`, `325-331`; `src/skirmish/ai.cpp:78-79`; `src/units/unit_abilities.cpp:76-82`, `328`; `src/sim/tactical/session_abilities.cpp:999-1004`, `1172`, `1182`, `1245-1249`; `include/eawr/sim/tactical/abilities.hpp:544-546` | `ObjectScript_PointDefense`, `ObjectScript_IG2000`, `ObjectScript_PowerToShields`, `ObjectScript_Krayt` and `ObjectScript_MC30` mean the stock scripts' behaviour. Native stand-ins are chosen by script name, with the stock constants (ranges, thresholds, periods) built in. | Edit those Lua files, or rename them. | Wrong (stock logic runs and the mod's Lua is ignored); silent skip when renamed. | feature | (legacy EAWR-1711) |
| S-12 | 6 | `src/script/foc/tactical_ai_bindings.cpp:195-203`, `391-395`, `488-493`, `720-722`; `src/script/foc/plan_bindings.cpp:695-697`; `src/script/foc/ai_perception.cpp:563-568`, `647`, `699-700` | Fixture content: no heroes (`Is_Hero` false), no garrisons, no minimum range, no start-location perception, no asteroid, ion-storm or nebula fields, fixture defender and base-level values. | Add space heroes, garrisoned or min-range units, and maps with fields or markers. | Wrong AI behaviour. | feature | (legacy EAWR-1705) (related) |
| S-13 | 3 | `src/script/foc/ai_perception.cpp:22-24`; `src/skirmish/ai.cpp:104-107` | "Every FoC squadron" is at most 7 craft against squad size 10/6/8, so the PG-08 squad-size power scale is skipped. | Author larger squadrons or a smaller `Max_Squad_Size`. | Wrong (AI combat power overstated). | feature | (legacy EAWR-1707) (related) |
| S-14 | 1 | `src/script/foc/ai_service.cpp:61-64` | A player type lists one Space template; only the first is used. | List several (retail selection rule unsourced). | Wrong. | feature | |
| S-15 | 5 | `src/script/sflua/sflua_sandbox.cpp:455-458` (reason in `docs/lua-sandbox.md:148`) | Scripts are always source ("FoC ships its scripts as source"). | Ship compiled `.lc` chunks; the retail path pattern includes them (ML-33). | Battle fails (freestore or library); silent skip (plan). | blocks load | |
| S-16 | 4 | `include/eawr/script/foc/tactical_ai.hpp:112-114`; `src/script/foc/host.hpp:454-455`; `src/skirmish/ai.cpp:45-47` | Projectile categories for defensive abilities are only `MISSILE`, `ROCKET` and `MPTL_ROCKET`. | Retail rule unsourced (whether categories are data-defined). | Wrong (other projectiles are never stopped by laser defence or missile shield). | feature | |
| S-17 | 4 | `src/script/foc/host.hpp:476-482`; `src/script/foc/tactical_ai_bindings.cpp:455-462` | Ability names outside the modelled `AbilityKind` set are treated as cut. | Give units other retail ability types. | Silent skip (`Activate_Ability` does nothing). | feature | (legacy EAWR-1706) |

### K: Skirmish start, lobby and maps

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| K-01 | 6/8 | `src/skirmish/start.cpp:768-781` (`m2_fixture`); `src/skirmish/inputs.cpp:314`, `346-356`; `src/skirmish/start.cpp:377` | Every start is built on the M2 fixture. Unset options (slots, seed, map) take its values: Coruscant, Rebel and Empire slots, and a fixed fleet of six stock unit types. | Rename or remove those factions or units. | Battle fails ("unit type is not in the unit tables" or "faction is not defined"). | blocks load | (legacy EAWR-1705) |
| K-02 | 6 | `src/skirmish/setup.cpp:87-93` | The map picker checks every map against the M2 lobby, i.e. the Rebel and Empire slots. | Drop, rename or disable either faction. | Every map is marked unavailable. | blocks load | (legacy EAWR-1705) |
| K-03 | 6 | `src/skirmish/inputs.cpp:370-372`; `src/skirmish/start.cpp:202-205` | `_mp_space_coruscant.ted` is byte-identical to stock (SHA-256 pin). | Override that map. | Battle fails on that map. | feature | (legacy EAWR-1705) |
| K-04 | 1/6/7 | `src/units/unit_tables.cpp:196-218`, `252-257`; `apps/viewer/src/live_session_prepare.cpp:48-62`, `100`; `apps/sim_headless/scenario_build.cpp:313`; `apps/sim_headless/cli_replay.cpp:110-112`, `166` | The unit-table load list is seeded from 12 pinned stock types (including the skirmish star bases) and 5 Coruscant obstacles. Expansion follows only production and faction links. | Use other station or faction roots, e.g. a third faction's star base. | Silent skip of mod-only types, then battle fails at start. Stock names are kept resident or reported unresolved. | feature | (legacy EAWR-1705) |
| K-05 | 1 | `src/skirmish/setup.cpp:110` | Team factions are filtered to Rebel, Empire and Underworld. | Add playable factions with their own station markers. | Silent skip (faction never offered; a map can drop below two teams, `setup.cpp:119`). | feature | (legacy EAWR-1705) |
| K-06 | 1 | `apps/viewer/src/skirmish_setup_mode.cpp:328`, `488`, `554` | The setup screen's faction combo lists exactly Rebel, Empire and Underworld, and a new row opposes the host by a Rebel check. | Add, rename or remove playable factions (the retail lobby rule is unsourced; (legacy EAWR-1705) asks for a decision). | Silent skip (a mod faction cannot be chosen or restored). | feature | (legacy EAWR-1705) |
| K-07 | 1 | `apps/viewer/src/lan_match.cpp:125-127`; `apps/viewer/src/live_session_economy.cpp:540` | LAN and replay setups map faction IDs back through `{Rebel, Empire, Underworld}`. | Add factions. | Battle fails (LAN: "faction is unavailable"); wrong (replay: empty faction name, so faction audio and scoring are lost). | blocks load | |
| K-08 | 3 | `src/skirmish/setup.cpp:120-121`, `166` | Every skirmish map sets the new-markers flag. | Ship older-style maps (retail rule unsourced). | Silent skip (map unavailable). | feature | |
| K-09 | 2 | `src/skirmish/inputs.cpp:332-336`; `src/skirmish/setup.cpp:50`, `77-79`; `src/sim/tactical/replay.cpp:113-114` | Maps are exactly `data/art/maps/<name>.ted` (no subfolder). Previews are only `data/art/textures/<stem>.tga/.dds`. | Use subfolders (retail rule unsourced). | Silent skip of maps; wrong preview. | feature | |
| K-10 | 1/2 | `src/skirmish/setup.cpp:55`; `apps/viewer/src/loading_screen.cpp:192`; `apps/viewer/src/battle_audio_prepare.cpp:572` | Text is always `ENGLISH`, and speech is always `data/audio/speech/english/`. | Ship or install another language only. ML-43: retail tries the requested language, then English, then the other slots. | Silent skip (keys shown instead of text, no speech). | feature | |
| K-11 | 3 | `src/skirmish/inputs.cpp:337-340`; `src/skirmish/setup.cpp:164-165`; `include/eawr/skirmish/setup.hpp:59`; `src/net/setup.cpp:56`, `170` | At most 8 players. | Author map capacity above 8 (retail limit unsourced; WSS-08 covers the local setup rows). | Battle fails. | feature | |
| K-12 | 1 | `src/skirmish/start.cpp:120-121`, `307-309`; `src/skirmish/setup.cpp:12-19`, `99`, `104`; `apps/viewer/src/live_session_prepare.cpp:84-86` | Start markers are named `Team_NN_Space_Station`, `Team_NN_Base_Position_Marker` and `Team_NN_Spawn_Point_Marker`. | Retail rule unsourced (probably an engine convention). | Silent skip or battle fails. | feature | |
| K-13 | 3 | `src/skirmish/inputs.cpp:186-192`, `208`, `240`; `apps/viewer/src/skirmish_setup_mode.cpp:174-180`, `194-197` | All nine `MP_Color_<name>` tags and every `MP_Default_*` tag are present. | Omit or rename one (retail default unsourced). | Battle fails (the setup screen refuses to open). | blocks load | (legacy EAWR-1703) |
| K-14 | 4 | `src/sim/tactical/victory.cpp:35-38`, `142-151`; `src/sim/tactical/replay.cpp:104`; `src/skirmish/inputs.cpp:535-536` | The space win condition is one of two stock values. | Name another engine win condition in game constants. | Battle fails. | blocks load | |
| K-15 | 6 | `apps/sim_headless/cli_options.cpp:78`; `apps/sim_headless/cli_skirmish.cpp:87`; `apps/sim_headless/cli_replay.cpp:187`, `195`, `212` | The headless tool accepts only `--skirmish m2` and always uses `m2_fixture()`, even with `--mod-root`. Replays without setup metadata assume M2. | Change any M2 content. | Battle fails (headless runs). | blocks load | (legacy EAWR-1705, EAWR-1712) |
| K-16 | 4 | `src/skirmish/inputs.cpp:248-251` | `Is_Named_Hero` and the superweapon flag are read only on six object types (Container, HeroUnit, Squadron, HeroCompany, UniqueUnit, UpgradeObject). | Set them on other types (retail rule unsourced). | Silent skip (lobby hero and superweapon gating). | feature | |
| K-17 | 8 | `include/eawr/skirmish/start.hpp:168-171`; `src/skirmish/inputs.cpp:541-549`; `apps/viewer/src/live_fog_view.cpp:123-127` | FoC fog values (cell 100, regrow 6 s) when the tags are absent, or when game constants cannot be read. | Omit the tags (retail default unsourced). | Wrong. | cosmetic | |
| K-18 | 8 | `apps/viewer/src/skirmish_setup_mode.cpp:498` | The map preview falls back to `generic_space.dds`. | Not ship it. | Cosmetic (no preview). | cosmetic | |

### U: Unit tables and session validation (sim)

`TacticalSession::create` (`src/sim/tactical/session_lifecycle.cpp:248-275`) and `session_content`
(`src/skirmish/content.cpp:251-270`) run the setup, sensor, durability, motion, squadron, fog,
combat, victory, ability and economy validators in turn. One out-of-range row in any table refuses
the whole battle. That chain is the root of (legacy EAWR-1703), and each row below is a separate trigger for it.

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| U-01 | 3 | `src/units/unit_durability.cpp:236-238`; `src/units/unit_combat.cpp:62-65`, `100-113`, `144-146`, `246`, `323-325`, `469`, `488-489`, `576-578` | Per-unit rows are "modelled" or the battle fails. Rejected cases: a destroyable hardpoint without Health; an unknown category in `Fire_Inaccuracy_Distance`; an ion stun with a radius; a ROCKET without curve fields; an unresolved death projectile; unusable turret bones; `Collision_Box_Modifier` above 256. | Author any of these on one unit; retail plays them; the real cases are listed in a mod-compatibility ticket (legacy EAWR-1703). | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-02 | 3 | `src/units/unit_support.cpp:573-574` with `src/units/unit_durability.cpp:134-164`, `src/units/unit_motion.cpp:111-143`, `src/skirmish/economy.cpp:59`; `src/units/unit_abilities.cpp:97-98` | About 55 game-constant tags are present and in range. | Omit a tag (retail default unsourced). | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-03 | 3 | `src/skirmish/inputs.cpp:460-471`, `511-513`; `src/skirmish/economy.cpp:75-82`, `118`, `142`, `161`, `168`; `src/skirmish/start.cpp:454-456` | Beacon lifetimes, garrison delays, per-faction unit caps, difficulty credit data and durations up to 86,400 s are all present and in range; `Special_Weapon_Index` is below 2. | Omit or vary them (retail unsourced). | Battle fails. | blocks load | (legacy EAWR-1703); unit cap tag todo |
| U-04 | 3 | `src/skirmish/start.cpp:709-727`; `src/units/unit_durability.cpp:254-257`; `src/units/unit_motion.cpp:372-375`; `src/units/unit_combat.cpp:592-595`; `src/units/unit_abilities.cpp:353-356` | No two type names share a CRC type ID. | Add enough types to collide (retail handling unsourced). | Battle fails. | blocks load | |
| U-05 | 3 | `include/eawr/sim/tactical/abilities.hpp:363`; `src/sim/tactical/abilities.cpp:341-343`, `370-371`; `src/units/unit_abilities.cpp:318-320` | At most two modelled abilities per type, and they must be distinct kinds. | Author three or more abilities (retail limit unsourced). | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-06 | 4 | `include/eawr/sim/tactical/types.hpp:218-248`; `src/sim/tactical/abilities.cpp:27-56`, `129-148`; `src/units/unit_abilities.cpp:143`; `src/units/unit_tables_profiles.cpp:377` | The ability and special-handler kinds are the set the stock space roster uses (28 abilities and 16 handlers). | Use any engine ability or handler type on a space unit (which work in space is unsourced). | Silent skip. | feature | (legacy EAWR-1706) |
| U-07 | 4 | `src/sim/tactical/abilities.cpp:500-504`; `src/units/unit_abilities.cpp:112` | Team containers only author `ION_CANNON_SHOT` or `BUZZ_DROIDS`. | Author another ability on a squadron container (unsourced). | Silent skip (loader); battle fails if one reaches the sim. | feature | (legacy EAWR-1706) |
| U-08 | 3 | `src/sim/tactical/abilities.cpp:386-416`, `436-440`, `456`, `494-497`; `src/units/unit_abilities.cpp:47`, `190`, `204` | Nested ability data looks like the stock unit that uses it. Examples: concentrate fire with no speed change ("stock Home One authors zero"), spawn z-offset 0, multipliers up to 4x, fixed expiration patterns, a buzz-droid type present. | Author speed-slowing concentrate fire, offset spawns, larger multipliers, other expiration combinations. | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-09 | 3 | `include/eawr/sim/tactical/fighters.hpp:409-411`; `src/sim/tactical/fighters.cpp:57-58` | Out-of-combat defense adjustment is in [-1, 1] ("FoC authors -1 on every craft"). | Author -2 or +1.5. | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-10 | 3 | `src/sim/tactical/economy.cpp:181`, `211-213`, `281`, `292` | Upgrades only add (0 to 8x); no free or instant units; arrival vulnerability in [-4, 0] (FoC -3). | Debuff upgrades, free or instant units, other vulnerability (unsourced). | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-11 | 3 | `src/sim/tactical/motion.cpp:153-155`, `328-337`, `378-389` | Speed and turn rates within stock-like ranges; bank angle at most 90 degrees; soft radius at least about 1.25 (the comment cites the smallest FoC corvette). | Author bank angles above 90 (full rolls), very slow units, tiny radii. | Battle fails. | blocks load | (legacy EAWR-1707, EAWR-1703) |
| U-12 | 7 | `src/sim/tactical/fighters_algorithms.hpp:33-35`; `src/sim/tactical/fighters.cpp:63-64`, `70-72`, `88-89`; `include/eawr/sim/tactical/fighters.hpp:122-123` | At most 64 members per squadron, 64 spawn entries and 255 bays; spin-away at most 60 s ("FoC's M2 craft give 2 s"). | Author larger squadrons or garrisons. | Battle fails. | blocks load | (legacy EAWR-1707) |
| U-13 | 3 | `include/eawr/sim/tactical/combat.hpp:199`; `src/sim/tactical/combat.cpp:219-235`, `265-270`, `284`, `293` | Collision geometry fits within ±4096 units, and no shot is faster than 4096 units per frame once any type has meshes. | Larger super-capital or station models, or a larger `Scale_Factor`. | Battle fails. | blocks load | |
| U-14 | 3 | `include/eawr/sim/tactical/durability.hpp:132`; `src/sim/tactical/durability.cpp:162`, `174`; `src/sim/tactical/damage.cpp:168-185`; `src/sim/tactical/combat.cpp:191-194` | Health, shield and damage values are at most 2^20 (a fixed-point product bound, not a FoC value). | High-HP stations or bosses, superweapon damage. | Battle fails. | blocks load | (legacy EAWR-1703) |
| U-15 | 7 | `src/sim/tactical/rocket_internal.hpp:11-12`; `src/sim/tactical/rocket.cpp:71`, `95`, `224-225`, `326-327`; `src/sim/tactical/projectiles.cpp:432-433` | A rocket's curve distance is large relative to its flight length (2048-point path budget), and never 0. | A long-range rocket with a short curve distance. | Battle fails **mid-battle**. | blocks load | |
| U-16 | 3/7 | `src/sim/tactical/fog_cells.cpp:18`, `163-166`; `include/eawr/sim/fog.hpp:24-26`; `src/skirmish/content.cpp:313-316` | The fog grid is at most 4096 cells per side (sim) and 512 per side (content, V-18). | A smaller cell size or a larger map. | Battle fails. | blocks load | |
| U-17 | 3 | `src/sim/tactical/session_impl.hpp:104-107`; `src/sim/tactical/session_economy.cpp:314-330` | One station per team ("stock skirmish has one station per team"), so upgrades die with it. | Maps with several stations per player. | Wrong (upgrades lost while another station stands). | feature | |
| U-18 | 2 | `src/units/unit_support.cpp:283-290` | Model paths are always `data/art/models/` plus the name plus `.alo`. | Author a prefixed or full path (ML-39). | Silent skip (no collision or bones). | feature | |
| U-19 | 1 | `src/units/unit_tables_profiles.cpp:305`; `src/units/unit_tables_decode.cpp:161`; `src/units/unit_abilities.cpp:166` | BARRAGE always targets an object named `Dummy_Barrage_Target`. | Retail rule unsourced. | Wrong or missing target. | feature | |
| U-20 | 4 | `src/units/unit_tables_profiles.cpp:488`, `678` | Tractor-beam exception keyed to the category name "Corvette"; redirect-blaster kept only on `SpaceUnit`. | Use these on other categories or types (unsourced). | Silent skip. | feature | |
| U-21 | 8 | `include/eawr/sim/tactical/motion.hpp:40-43`; `src/units/unit_motion.cpp:133-149`; `include/eawr/sim/tactical/abilities.hpp:329` | Missing tags mean FoC's value (re-evaluation 10 frames, guard range 750, stealth detection 30). | Omit the tags (retail default unsourced). | Wrong. | cosmetic | |
| U-22 | 3 | `src/units/unit_tables_decode.cpp:461` | Weaken-enemy duration is clamped to 3600 s. | Author a longer duration. | Wrong. | cosmetic | |
| U-23 | 8 | `src/skirmish/economy.cpp:235` | AI build time for ordinary units uses Normal's multiplier of 1. | Tune `Space_Build_Time_Multiplier`. | Wrong. | feature | (legacy EAWR-753) |

### P: Presentation, assets and formats

| ID | Cat | Where | Assumed | A mod may legally | Consequence | Severity | Ticket |
|---|---|---|---|---|---|---|---|
| P-01 | 7 | `src/presentation/godot/renderer_upload.cpp:74-81`, `107`; `src/presentation/godot/renderer_mesh_upload.cpp:48-95`; `apps/viewer/src/space_populate_compose.cpp:412-422`; `apps/viewer/src/space_populate_materials.cpp:100-107`, `142`; `apps/viewer/src/viewer_host_process.cpp:82`, `179` | FoC-scale residency. Each material/model key uploads its own GPU texture copy, including once per team-colour variant. Texture arrays expand BCn to RGBA8 at up to 4096 with regenerated mips. Every live slot is composed up front. The setup scene is only hidden, and a second VFS and catalog are mounted with the content cache off. There is no budget or eviction. | Ship many more types and textures. | Crash (out of GPU memory). | blocks load | (legacy EAWR-2856) |
| P-02 | 3 | `apps/viewer/src/space_populate_compose.cpp:489-494`, `753-757`; `apps/viewer/src/map_mode_ready_population.cpp:266-271` | Every surface of every composed model uploads. | Ship one model or texture the renderer rejects, or run out of memory. | Battle fails (where a per-unit skip would do). | blocks load | (legacy EAWR-1703, EAWR-2856) |
| P-03 | 1/4 | `src/scene/scene.cpp:41-64`, `198`; `src/scene/space_population.cpp:188-190`, `234`; `src/presentation/godot/renderer_contract.cpp:64-130`; `src/presentation/godot/legacy/registry.hpp:29-54`; `src/presentation/godot/renderer_materials.cpp:93-123`; `src/presentation/godot/renderer_fog.cpp:329-342`; `src/presentation/space/environment_scene.cpp:148-157`, `305-315`; `apps/viewer/src/map_mode_placements.cpp:213-221`; `apps/viewer/src/space_populate_compose.cpp:513-517`; `apps/viewer/src/space_environment_surfaces.cpp:124` | Materials name one of about 21 stock `.fx` files, each mapped to a built-in port; anything else is unsupported. A stock name always gets the built-in port, whatever the mod ships under that name. | Ship new or replacement shaders (ML-13 registers each mod root's shader directories). | Silent skip (surface or object not drawn, no team colour); wrong for replaced stock shaders. | feature | (legacy EAWR-1704) |
| P-04 | 4 | `src/presentation/ui/hud_shell.cpp:249-251`, `323-324`, `534-535`; `apps/viewer/src/land_look.cpp:371`, `431`, `488-490`; `apps/viewer/src/space_populate_compose.cpp:549`, `576`; `apps/viewer/src/space_populate_materials.cpp:204-219`; `apps/viewer/src/world_ui_prepare.cpp:336-337` | Role-specific closed shader sets: HUD meshes only MeshAlpha/MeshAdditive; land sky only Skydome/MeshAdditive; shield and cloak shells only `MeshShield.fx` with all textures present. | Use other shaders in those roles (unsourced). | Silent skip. | feature | (legacy EAWR-1704) |
| P-05 | 4/8 | `include/eawr/presentation/ui/hud_shell.hpp:49`; `src/presentation/ui/hud_shell.cpp:131-144`, `194-214`; `apps/viewer/src/map_mode_hud.cpp:116-117`, `196`, `475`; `apps/viewer/src/battle_input_special.cpp:27` | The HUD faction is a closed enum {empire, rebel, underworld}; unknown factions fall back to Rebel. The manual station-gun button is Underworld-only. | Add a faction with its own HUD variant, or a star base with a manual-target weapon. | Wrong (Rebel HUD and icons); silent skip (no special-weapon button). | feature | |
| P-06 | 1/8 | `apps/viewer/src/world_ui_prepare.cpp:185-194` | The selection ring texture is chosen by comparing the material name with the Empire's, defaulting to Rebel; `ShadowBlobMaterials.xml` is never read. | Name any material with its own textures. | Wrong (Rebel ring). | cosmetic | |
| P-07 | 5 | `src/presentation/audio/sfx.cpp:214-225`; `include/eawr/presentation/audio/sfx.hpp:82-89`; `apps/viewer/src/battle_audio_prepare.cpp:610-611`, `617`, `626` | Every SFX sample is a 16-bit PCM WAVE under `Data/Audio/SFX` (BA-30, a data survey). 8-bit, ADPCM, `WAVE_FORMAT_EXTENSIBLE` and more than 2 channels are refused. The path is always `data/audio/sfx/` plus the name, so `Data/`-rooted references are double-prefixed (ML-04 strips a `Data/` prefix). | Use other WAV variants (retail decoder set unsourced) or `Data/`-rooted paths. | Silent skip (silent sounds). | feature | (legacy EAWR-1708) |
| P-08 | 5 | `apps/viewer/src/battle_audio_prepare.cpp:574-596`, `642-652` | Music is always MP3; speech is MP3 or 16-bit WAV, chosen by extension. | Ship WAV or other music, or other PCM speech (unsourced). | Silent skip. | feature | (legacy EAWR-1708) (related) |
| P-09 | 5 | `src/presentation/godot/renderer_mesh_upload.cpp:10-24`, `97-119`; `src/presentation/godot/particle_texture.hpp:38-39` | 3D textures are FoC-style BCn DDS. bgra8/bgr8 upload without a red-blue swap, bottom-left TGA origin is ignored, a8 uploads at the wrong size, and any mip count above 1 is marked as a full chain (the last is unverified). Other code paths handle these correctly. | Use uncompressed DDS, TGA, alpha-only or partial-mip textures. | Wrong (swapped colours, flipped textures); silent skip (upload fails). | feature | (legacy EAWR-1710) (related) |
| P-10 | 5 | `src/assets/texture.cpp:194-196`, `241`, `249-259`, `268-278` | DDS is limited to DXT1/3/5, 24/32-bit, L8, A8 and DX10 BC1-5/7; there are no 16-bit, A8L8, DXT2/4, ATI1/2, cubemap or volume formats, and trailing bytes are fatal. TGA has no colour-mapped or 15/16-bit modes; BMP must be 32-bit. PNG data in `.tga` files is not handled. | Use any D3DX-loadable variant (retail acceptance unsourced). | Silent skip (untextured surface), or battle fails via V-11. | feature | (legacy EAWR-1710) |
| P-11 | 5/3 | `src/assets/mega_texture.cpp:122-126`, `139-153`, `176-180`, `205-219`, `246-250` | One backing page extension per atlas; one bad entry or trailing bytes fails the whole atlas. | A mod ships the other page extension than FoC; writers leave garbage in padding (unsourced). | Silent skip (HUD atlas missing). | feature | |
| P-12 | 5 | `src/assets/model.cpp:82-86`, `124`, `129`, `169`, `222`, `255`; `src/assets/animation.cpp:64`, `77`, `96-109` | ALO/ALA: any unknown chunk fails the file (`docs/asset-formats.md:20-21` policy); only two vertex layouts; at most 24 skin bones; light type at most 2; no duplicate track bones; no v2 scale blocks. | Ship exporter-written extra chunks or other layouts (retail tolerance unsourced). | Silent skip (model or animation not loaded). | feature | |
| P-13 | 3/5 | `src/presentation/animation/animation.cpp:268-279`, `289-296`; `src/presentation/animation/unit_clips.cpp:46-56`; `apps/viewer/src/space_populate_compose.cpp:365`, `379`, `402-404`; `src/scene/scene_build.cpp:337` | Tracks bind to bones by index and exact-case name with exact duration metadata. Clips are found only by FoC naming (`_idle_00` ... and enumeration stops at the first gap). Live clips need a whole-number frame rate. | Reuse an animation across models (`Anim_Override`), vary case, use fractional fps or other clip names (unsourced). | Silent skip (unanimated units). | feature | |
| P-14 | 5 | `src/presentation/particles/alo_particles.cpp:24-87`, `347-348`, `390`, `449`; `src/presentation/particles/render_plan.cpp:27-43`, `144-151` | Particle features are limited to the stock corpus. V2 systems never draw; blend selectors 4-9, 12 and 13 are refused; heat only with selectors 1, 2 and 10; terrain bounce/stick disables the emitter; one malformed emitter fails the system. | Author V2 systems, any engine blend, depth-sprite or kite emitters. | Silent skip. | feature | (legacy EAWR-1709) |
| P-15 | 3 | `src/presentation/godot/legacy/bindings.hpp:55-80`; `src/presentation/space/space_effects.cpp:113-143`, `166-171` | Material parameters are as clean as FoC's: no duplicates or non-finite values, only technique t0, fixed vertex formats for Planet/Nebula. | Ship exporter-produced ALOs with extra parameters or other vertex formats. | Silent skip (surface or backdrop not drawn). | feature | (legacy EAWR-1703) (pattern) |
| P-16 | 3/8 | `src/presentation/camera/constants_source.cpp:78-83`, `139-177`, `217-249`, `287-290`; `apps/viewer/src/map_mode_ready.cpp:817-833` | `tacticalcameras.xml` is strict (no DOCTYPE, duplicate tags or definitions, non-numeric values); on failure the viewer falls back to the overview camera. | Append duplicate definitions or vary formatting (retail handling unsourced). | Wrong (battle opens in the overview camera). | feature | |
| P-17 | 3/8 | `apps/viewer/src/map_camera.cpp:232-236`, `263`; `apps/viewer/src/map_mode_ready_population.cpp:98-100` | Maps declare positive extents. Fixed project values (`Distance_Min`, minimum scroll speed, `Pitch_Min`, overview clicks) replace the mod's camera XML. | Ship maps without extents, or re-tune `TacticalCameras.xml`. | Battle fails (no extents); wrong (mod tuning overwritten). | blocks load | |
| P-18 | 2 | `src/scene/scene_assets.cpp:48`, `59`, `103`; `src/scene/scene_build.cpp:288`, `334`; `src/scene/scene_internal.hpp:13-14`; `apps/viewer/src/map_mode.cpp:47-71`; `apps/viewer/src/space_environment.cpp:42-66`; `apps/viewer/src/space_populate_assets.cpp:19-45`; `apps/viewer/src/unit_mode.cpp:89-108`; `apps/viewer/src/unit_emitters_prepare.cpp:63-82`; `apps/viewer/src/world_ui_prepare.cpp:88-89`, `350-353`; `apps/viewer/src/battle_effects_prepare.cpp:129-150`, `500`; `apps/viewer/src/debris_props.cpp:100-118`, `137`; `apps/viewer/src/loading_screen.cpp:93-109`; `apps/viewer/src/live_session_view.cpp:76`; `src/presentation/ui/loading_screen.cpp:38-46` | About ten separate lookups force `data/art/models/` or `data/art/textures/`, try only `.alo` or `.tga`/`.dds`, and disagree: some drop subfolders, some try `.dds` first, some trim whitespace. | Reference with subfolders, a `Data\Art\...` prefix, or a name present as both `.tga` and `.dds` (ML-39/40/41: supplied name, search directories, then a `.dds` retry, with a lowercased, suffix-stripped cache key). | Silent skip or a different file per subsystem. | feature | |
| P-19 | 3 | `apps/viewer/src/battle_effects_projectiles.cpp:373-376`, `424-425`, `701-702`, `740-741`, `802-803`; `src/presentation/godot/particle_adapter.cpp:323-326`; `apps/viewer/src/map_mode_ready_population.cpp:434-436` | Every effect texture (laser, lightning, beam, tractor and laser-defence) resolves and decodes. | Name a texture it does not ship, or in an unsupported format. ML-40: retail returns a shared default texture. | Battle fails **mid-battle**. | blocks load | |
| P-20 | 1/3 | `apps/viewer/src/live_session_prepare.cpp:499`, `553-564` | A `Neutral` minimap colour exists, and every capture-point type has a readable model and neutral idle clip. | Rename the neutral faction, or ship one broken clip. | Battle fails. | blocks load | |
| P-21 | 3 | `apps/viewer/src/live_session_prepare.cpp:189-193`; `src/presentation/ui/production.cpp:15-35` | `ReinforcementOverlayGood/BadColor` are exactly 4 integers 0-255, with exact-case tags. | Use 3 channels, omit the tag, or vary case. | Battle fails. | blocks load | |
| P-22 | 3 | `apps/viewer/src/battle_scoring.cpp:93`, `98`, `101-110`, `132` | The scoring script name tag is exact-case and the module name is `[A-Za-z0-9_]`, forced under `Data/Scripts/Miscellaneous`. Every library `.lua` is loaded eagerly, and one unreadable file or unresolved squadron member fails scoring. | Vary these (unsourced). | Silent skip (no results screen). | feature | |
| P-23 | 7 | `apps/viewer/src/battle_effects_impacts.cpp:31-32`, `93-95`, `508`; `apps/viewer/src/debris_props.cpp:18`; `apps/viewer/src/battle_effects.hpp:112`; `apps/viewer/src/battle_effects_projectiles.cpp:124-149` | 4096 particles per effect, 256 live effects, 32 model projectiles per type (BP-62, a project rule). | Dense emitters, large battles, missile spam. | Silent skip (dropped effects). | cosmetic | |
| P-24 | 7 | `apps/viewer/src/live_session_fighters.cpp:94`, `109`, `157`, `191-192`, `290-302`; `apps/viewer/src/live_session_view.hpp:223` | A hangar launch count of at most 64, and at most 10 live purchases per type, are pre-composed. | Larger hangars, many cheap units. | Wrong (simulated but invisible units). | feature | (legacy EAWR-1707) (related) |
| P-25 | 5 | `src/assets/map_decode.cpp:561` (legacy plane around `407`) | TED format version is exactly 0x0201. | EaW-era map versions (retail acceptance unsourced). | Silent skip (map excluded from the list; no terrain). | feature | |
| P-26 | 3 | `apps/viewer/src/map_mode_particles.cpp:64-67`, `77-79`; `apps/viewer/src/map_mode_ready.cpp:917-924`; `apps/viewer/src/map_mode.hpp:60` | Every map particle placement resolves within a fixed 8192 budget (land and map view only). | Reference an absent particle ALO. | Battle fails (land view). | feature | |
| P-27 | 2/4 | `apps/viewer/src/map_mode_placements.cpp:255`, `266`, `334-348`; `apps/viewer/src/space_populate_materials.cpp:121`, `152`; `apps/viewer/src/map_mode_hud.cpp:729`; `src/units/unit_tables_profiles.cpp:908`; `src/presentation/space/fog_field.cpp:31` | Exact-case matches for shader names (`Grass.fx`, `MeshAdditiveVColor.fx`), `Space_Layer` `"Capital"`, and fog-field tags, while the main lookups ignore case. | Write a different case. | Wrong. | cosmetic | |
| P-28 | 1/4 | `src/scene/scene_assets.cpp:117-139`; `apps/viewer/src/map_mode_placements.cpp:159-160`; `src/presentation/ui/hud_shell.cpp:233-235`; `src/presentation/godot/ui/minimap_view.cpp:107` | Mesh and clip naming conventions: `_ALT0`-`_ALT9` damage states, `girder*` hidden, `_idle_0N` emblem clips, and a required pause-shell `_idle_00.ala`. | Use other naming (unsourced). | Wrong visibility; silent skip (the pause shell fails without its clip). | cosmetic | |
| P-29 | 1/8 | `apps/viewer/src/loading_screen.cpp:76-89`, `185-191`; `tools/fonts/extract_eaw_fonts.py:44`, `56-59` | UI fonts are four faces extracted from the FoC executable (name and hash pinned); mod fonts are never loaded. | Name other faces in dialog or theme data (unsourced). | Wrong (system fallback faces). | cosmetic | |
| P-30 | 3/7 | `apps/viewer/src/battle_effects_prepare.cpp:301-302`; `apps/viewer/src/debris_props.cpp:185-188`; `apps/viewer/src/land_look.cpp:71`, `238`, `243` | Lightning `Detail` 1-64 and bolts 0-128; a breakoff prop needs `Attachment_Bone` (FoC falls back to `Fire_Bone_A`); terrain blend tile and layer limits. | Vary these. | Wrong or silent skip. | cosmetic | |
| P-31 | 4/7 | `apps/viewer/src/world_ui_view.hpp:142-143`; `apps/viewer/src/world_ui_prepare.cpp:241-243`; `src/presentation/ui/world_ui.cpp:56-64` | At most two world-UI ability icons; `Ship_Class` strings map to bar sizes through a closed table. | Add a third ability or new classes. | Wrong. | cosmetic | |
| P-32 | 1 | `apps/viewer/src/battle_audio_prepare.cpp:129`, `170`, `255`; `apps/viewer/src/battle_effects_prepare.cpp:279`; `apps/viewer/src/world_ui_prepare.cpp:211`; `apps/viewer/src/world_ui_groups.cpp:220-221`, `442`; `apps/viewer/src/battle_input_commands.cpp:536-576` | `audio.xml`, `musicevents.xml`, `speechevents.xml`, `lightningeffecttypes.xml` and `MT_CommandBar.mtd` (with fixed entries) are loaded by fixed name; hotkeys are fixed. | Relocate or rename them (retail references unsourced). | Silent skip. | cosmetic | |

## Probe-only (not on the setup-to-battle path)

Listed for completeness. These are lower priority unless a probe becomes a runtime path.

- **`src/script/pglua.cpp`** (used by `apps/script_smoke` only):
  - a single pinned PGLua header (`18-26`);
  - two opcodes refused (`375-380`, `src/script/script_host_context.cpp:247-256`);
  - a retail identity pin with fixed archive SHA-256 values (`496-504`, `523-531`).
- **`src/script/foc/ai_goals.cpp:168-172`:** a report-only goal-name filter.
- **`src/skirmish/melee.cpp:27-31`, `129-130`:** the benchmark roster uses fixed stock unit names.
  The name-keyed roster gate (`src/skirmish/roster_gate.cpp`) is empty.
- **`src/assets/map_references.cpp:55-64`, `104-117`:** fixed art roots and suffixes
  (`asset_validate`, `sky_scan`).
- **The `remake` data profile is chosen for any mod** (`apps/sim_headless/cli_skirmish.cpp:20`,
  `src/data/xml_registry.cpp:133-150`). It affects only unknown-field warnings.
- **Headless install layout:** `corruption/Data` over `GameData/Data`, and scenario runs mount no
  mod chain (`apps/sim_headless/cli_skirmish.cpp:11-12`, `scenario_build.cpp:272-273`).
- **The camera config XMLs are pinned to Coruscant** by path and hash
  (`apps/viewer/src/map_camera.cpp:140-157`). They are read only with
  `--eawr-map-camera-config`.
- **Viewer dev defaults:**
  - Coruscant is the default map (`apps/viewer/src/viewer_host_config.cpp:111`,
    `apps/viewer/src/ui_gallery_mode.hpp:39`).
  - The setup GUI tests pin maps and slots (`apps/viewer/src/skirmish_setup_mode.cpp:126`,
    `607-615`, `793`, `866`).
  - Debug ship placement uses fixed marker and type names
    (`apps/viewer/src/space_populate_compose.cpp:160-171`, `240`).
  - The model preview pins an asset (`apps/viewer/src/model_preview.hpp:28-40`).
  - The primary-sky harness accepts MeshGloss only (`src/presentation/space/space.cpp:230-240`).
  - The non-live space particle budget is 131,072 (`apps/viewer/src/map_mode_assets.cpp:132`).
- **The effect, font, fog and input probe modes:**
  - `apps/viewer/src/effect_mode.cpp:134-187` has a closed profile set and needs the stock
    install layout.
  - `apps/viewer/src/font_mode.cpp:51-72` uses a fixed face list.

## Intentional, needs a sourced reason

These look deliberate: documented project policy, engine-defined names, or behind a tag-registry
status. The fix tickets should either cite the retail source in the code and docs, or move the
item into the findings above.

- **Behind a tag-registry status** (`docs/tag-coverage/statuses.json`):
  - `SpaceFreeStoreScript`: todo (737); see S-02.
  - `AIUsesFogOfWarSpace`: deferred (652); hard-coded off at
    `src/script/foc/ai_perception.cpp:29-31`.
  - `Reserve_Spawned_Units_Tech_0`: todo (legacy EAWR-651); forced to 0 at `src/units/unit_motion.cpp:330-333`.
  - `Space_Tactical_Unit_Cap`: todo (legacy EAWR-654).
  - `Space_Build_Time_Multiplier`: 737; see U-23.
  - `HardPoint_Target_Reticle_*_Texture`: todo (653); reticles are hard-coded per hardpoint type
    at `src/presentation/ui/world_ui.cpp:379-401`.
- **Project resource limits without a retail source:**
  - Lua quotas: 4M instructions per service, 64 MiB per instance, and caps on threads, events and
    timers (`include/eawr/script/authoritative/scheduler.hpp:140-149`). Exhausting a quota removes
    the instance, so a heavy mod freestore silently stops.
  - Parser limits: MEG record and name limits (`src/vfs/vfs_archive.cpp:11-12`), a 4 MiB manifest
    and 2 GiB reads (`src/vfs/vfs.cpp:22`, `284`), asset size, element and depth limits
    (`src/assets/asset_internal.hpp:12-14`), and an 8192² texture limit
    (`src/assets/texture.cpp:165`). These are listed under "Capacity qualification" in
    `docs/behaviour/walks/mod-loading.md`.
  - Net limits: a 1 MiB setup and batch, 1024 commands and 1,000,000 ticks
    (`include/eawr/net/lockstep_session.hpp:16-19`, `include/eawr/net/setup.hpp:22`). The 1 MiB
    setup holds every map object at tick 0 and has not been measured against large mod maps.
  - Replay format limits (`include/eawr/sim/tactical/types.hpp:22-26`).
  - Particle `max_skip_seconds` = 300 (`include/eawr/presentation/particles/particles.hpp:278`).
- **Fixed-point range bounds:** `max_combat_distance`, `max_motion_coordinate` (2^18),
  `max_motion_rate` 1024, and the 2^20 health/damage bound. These are documented as Q24 limits but
  still reject a whole battle when exceeded; see U-13 and U-14 for the fix direction.
- **Engine enums copied by name; completeness unsourced:**
  - SpaceLayer names (`src/units/unit_motion.cpp:24-32`);
  - hardpoint-type names (`src/units/unit_support.cpp:137-139`) and `HardpointRole`
    (`include/eawr/sim/tactical/durability.hpp:21-28`);
  - the 119-entry clip-type table (`include/eawr/presentation/animation/unit_clips.hpp:23-41`);
  - the Prim\*.fx blend table (`src/presentation/particles/render_plan.cpp:22-43`);
  - ability icons and hotkeys (`src/presentation/ui/ability_buttons.cpp:11-63`, AB-04/09/10);
  - ability and behaviour names compared in the viewer (for example `DUMMY_STAR_BASE` and
    `ION_CANNON_SHOT` in `apps/viewer/src/battle_audio_prepare.cpp` and
    `apps/viewer/src/live_session_prepare.cpp`);
  - the 64-bit category loop in `Is_Good_Against`
    (`src/script/foc/tactical_ai_bindings.cpp:363-367`).
- **Engine-fixed asset and event names:**
  - textures: `W_Laser_Pill.tga`, `p_particle_master.tga`, `w_galaxy_dot.tga` and the space fog
    grids (BP-03/06, FW-03/22);
  - `Engine/PrimAdditive.fx`;
  - SFX events: `GUI_Text_Hint_SFX` (WBF-10) and `SFX_Stealth_Reveal` (WST-14);
  - sub-object names: SHIELD, STEALTH, BASEMESH, buildup and charged (BP-17, WST-12, BP-74);
  - the button fallback `i_button_temporary.tga` (L-7);
  - fixed engine XML paths: game constants, tactical cameras, radar map, faction files.

  Each is presumably engine-fixed, but several cite no source in code.
- **Other documented constants:**
  - build queue length 5 (PU-14, `src/skirmish/economy.cpp:442`; (legacy EAWR-753));
  - `multiplayer_colour_count` = 9 (WSS-20);
  - `local_setup_rows` = 8 (WSS-08);
  - victory countdown 210 frames;
  - voice pools of 32 3D and 16 2D voices (BA-09);
  - SFX priority, probability, pitch and volume clamps (BA-01; the volume clamp is (legacy EAWR-804));
  - HUD slot limits;
  - a 256² cursor frame (a Godot limit);
  - Bink-1-only movies;
  - environment record 0 instead of retail's random pick (R-SEL-03);
  - the stock install layout (`GameData/Data` plus `corruption/Data`) required under any mod root.
- **Strict loading of the FoC executable's font cache** (`src/data/ui/sfnt.cpp`) validates only
  that cache, not mod data. It would reject CFF, TTC or large fonts if it were ever reused for mod
  fonts.
- **Fatal hardpoint include errors** (`src/data/xml.cpp:206-212`, `230-234`) cite C-06/R-10/R-13.
  Confirm the source covers release builds, since ML-19 says retail omits the include and
  continues.

## Proposed fix groupings

Ranked by how many mods each would unblock. "Unblocks" counts the blocks-load findings it clears
first, then the breadth of mods that would hit it: a renamed faction or one odd row is far more
common than a v2 MEG. Sizes are rough.

1. **Data-driven skirmish start, factions and roster (no M2 seeding).**
   - Findings: K-01, K-02, K-03, K-04, K-05, K-06, K-07, K-12, K-15, P-05, P-06, D-06, S-12.
   - Fix: derive the lobby factions, start slots, station markers and the unit-table closure from
     Factions.xml, map markers and faction data. Keep the M2 fixture only for tests. Give the HUD,
     the selection ring, the special-weapon button and LAN/replay faction decoding a
     data-defined faction with a stated fallback.
   - Impact: unblocks any mod that renames, adds or removes a faction or a fixture unit.
   - Extends (legacy EAWR-1705). Size M.
2. **Tolerant validation with counted diagnostics (one policy, every validator).**
   - Findings: U-01-U-05, U-08-U-11, U-13-U-16, K-13, K-14, S-06, P-02, P-15, P-17, P-19, P-20,
     P-21, P-22, D-08, D-09, D-10, V-04, V-05.
   - Fix: replace "one row fails the battle" with per-row skip/default/clamp plus a diagnostic.
     Keep FoC data strictly checked in tests. For each case, source the retail rule: the default
     for an absent tag, fractional factors, unknown enum names, a missing texture (ML-40 default
     texture), a missing include (ML-19), an unavailable archive (ML-08). Fixed-point bounds
     (U-13, U-14, U-16) need wider ranges or per-unit rejection rather than a session failure.
   - Impact: unblocks most real mods (every Remake binding failure is here).
   - Extends (legacy EAWR-1703). Size L; split by validator family.
3. **AI discovery from data: players, files, plans, freestore, modules.**
   - Findings: S-01, S-02, S-03, S-04, S-05, S-09, S-14, S-15.
   - Fix: walk player → templates → function sets → equations → plan categories from the mounted
     data. Select the freestore per player type. Admit Lua dependencies through the VFS (ML-34,
     ML-38). Accept precompiled chunks if retail does. Add the missing converter enums. Read all
     difficulty records.
   - Impact: unblocks a playing AI for any mod that touches AI files.
   - Covers (legacy EAWR-1700, EAWR-1701, EAWR-1429). Size L.
4. **One shared asset-name resolver (ML-39/40/41) for models, textures, maps, sounds and scripts.**
   - Findings: P-18, U-18, K-09, P-07 (path part), P-28, P-32, D-05, P-27, S-10 (path part).
   - Fix: replace the roughly ten local `data/art/...` probes with one resolver. It should:
     - accept `Data/`-rooted and sub-path references (ML-04);
     - use retail's candidate order and `.dds` retry;
     - lowercase cache keys;
     - match case-insensitively everywhere.
   - Impact: fixes silent misses across art, audio and maps.
   - Covers (legacy EAWR-1708)'s path part. Size M.
5. **VFS and merge precedence to the retail rule.**
   - Findings: V-01, V-02, V-03, V-07, V-08, D-01, D-02, D-03, D-04, D-13.
   - Fix:
     - read only the winning `MegaFiles.xml` plus the fallback game's (ML-07, ML-09);
     - mount SFX archives by selected language (ML-48) without a forced prefix for
       manifest-declared ones;
     - make patch slots replace, not stack;
     - follow symlinks;
     - rank all parent mods above FoC;
     - keep the first binding for duplicate objects (ML-21);
     - leave inherited values on empty or `TBD` (ML-24);
     - use the retail boolean rule (ML-25) everywhere.
   - Impact: unblocks submod chains and loose-file mods, and stops FoC content leaking through.
   - Size M.
6. **GPU residency: shared textures, compressed arrays, demand loading, setup release.**
   - Findings: P-01, P-02, D-11.
   - Fix: one GPU texture per cache identity; keep BCn in arrays; compose and upload on demand
     instead of every slot at start; free the setup scene and duplicate catalog.
   - Impact: unblocks any large mod on ordinary GPUs.
   - Covers (legacy EAWR-2856) (with (legacy EAWR-1497)). Size L.
7. **Native stand-ins for Lua object scripts → a per-object script host.**
   - Findings: S-10, S-11, U-19.
   - Fix: run the mod's `Lua_Script` for every type (with the Lua APIs those scripts need), and
     retire the name-keyed native stand-ins, or keep them only when the script body is
     byte-identical to stock.
   - Impact: unblocks station, hero and ability scripts in nearly every large mod.
   - Covers (legacy EAWR-1711). Size L.
8. **Open ability and enum sets.**
   - Findings: U-05, U-06, U-07, S-07, S-08, S-13, S-16, S-17, U-20, P-31, K-16.
   - Fix: source the engine enums and ability caps. Model or explicitly stub every engine ability
     and handler type. Read hardpoint and perception tokens, the squad-size scale, projectile
     categories and `Ship_Class` from data.
   - Impact: unblocks unit rosters with non-stock abilities.
   - Covers (legacy EAWR-1706) and part of (legacy EAWR-1707). Size L.
9. **Shader routing for mod shaders.**
   - Findings: P-03, P-04.
   - Fix: route by shader family; alias `.fxo` and case variants to reviewed adapters; register
     mod shader directories (ML-13); detect a mod-replaced stock shader rather than silently using
     the built-in port; write new adapters only from clean-room notes.
   - Impact: unblocks mod skies, planets and units with custom materials.
   - Covers (legacy EAWR-1704). Size M-L.
10. **Texture decoders and the upload path.**
    - Findings: P-09, P-10, P-11.
    - Fix: one normalisation step before upload (channel order, origin, a8, partial mips). Add DDS
      variants, TGA modes and PNG-in-TGA after checking real headers. Tolerate a bad atlas entry
      or another page extension.
    - Impact: unblocks textures from common mod tools.
    - Covers (legacy EAWR-1710). Size M.
11. **Audio decoders and language.**
    - Findings: P-07 (format part), P-08, K-10.
    - Fix: accept the WAV variants retail plays (8-bit, ADPCM, EXTENSIBLE) once sourced, plus
      non-MP3 music. Select the speech folder and text database by language with the ML-43
      fallback.
    - Impact: unblocks sound in most mods and in non-English installs.
    - Covers (legacy EAWR-1708)'s format part. Size M.
12. **Particles and animation.**
    - Findings: P-12, P-13, P-14, P-28.
    - Fix: tolerate unknown non-essential ALO/ALA chunks, bind tracks by name ignoring case, find
      clips through the type's tags instead of FoC naming, and draw V2 systems and the missing
      blend, depth-sprite and kite modes.
    - Impact: unblocks animated and effect-heavy mods.
    - Covers (legacy EAWR-1709). Size M-L.
13. **Capacities sized from data.**
    - Findings: U-12, U-15, P-23, P-24, K-11, P-26, U-17.
    - Fix: size squadron, garrison, hangar, purchase-slot, effect and rocket-path pools from the
      loaded data. Allow several stations per team. Source the player cap.
    - Impact: unblocks large garrisons and big battles.
    - Covers part of (legacy EAWR-1707). Size M.
14. **Smaller format and naming items.**
    - Findings: V-06, P-25, D-07, D-12, P-16, P-29, K-08, K-17, K-18, U-21, U-22, U-23, P-30.
    - Fix: MEG v2/v3 and older TED versions (after sourcing retail acceptance); case-insensitive
      dialog parsing; a real XML read for the AI's game constants; tolerant camera XML; mod fonts;
      retail defaults for absent tags.
    - Impact: low per item.
    - Size S each; one ticket or folded into the groups above.

Follow-up after this lands, deferred by the owner: a standing mod canary in the batch check.
