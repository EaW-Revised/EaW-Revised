<a id="mod-hud-survey-how-mods-change-the-foc-hud-192"></a>

# Mod HUD survey: how mods change the FoC HUD

Status: research, 2026-09-26. It decides D2 (UI technology) and makes D3 (simple M2 HUD) and D4
(retail HUD, aspect-correct, centred on ultrawide, never stretched) concrete. Design base:
[ui-layer.md](ui-layer.md).

Method. Each mod was mounted through the repo VFS (`resolve_manifest_mount`, the mod over
`corruption` over `GameData`) by a throwaway harness under ignored `out/`. The harness ran the
merged loaders (UI-03 command bar and shell anchors, UI-04 layout model) and the UI-01/UI-02
branches (text DB, `.rc` and `GUIDialogs.xml`) read-only. Vanilla FoC reproduces the UI-layer inventory counts:
846 components, 18 shells, 111 dialogs and 2,555 controls, 1,107 atlas entries and 19,224 text
records. Only data facts and identifiers appear here. No mod or retail content is copied, and no
mod file is committed.

## 1. The mods

| Id | Mod | Identified by | Mounted as | HUD data it ships |
|---|---|---|---|---|
| 1129810972 | Republic at War (RaW) | `modinfo.json` | leaf over FoC | `.rc`/`.h`, `GUIDialogs.xml`, command bar, 17 interface ALO/ALA, atlas, text, SFX, constants |
| 1770851727 | EaW Remake | installed as the Remake mod root | leaf over FoC | as RaW, plus 5 swapped shells, cursors (91 pointer textures) |
| 1976399102 | Fall of the Republic (FotR) | `modinfo.json` | leaf over FoC | as RaW, plus new dialogs and HUD buttons, 1,518 Lua files |
| 2794270450 | Remake Clone Wars (RCW) | submod: its `megafiles.xml` names the Remake's `RemakeMaps.meg`/`RemakeDisabled.meg`, and its command bar names shells that only the Remake ships | leaf over the Remake | own `.rc`, atlas, text, SFX and constants; its command bar parses to the Remake's catalogue |
| 3229239424 | A Remake submod: Web Way campaigns (`GC_WEB_WAY_*` keys) plus Clone Wars factions | Remake-style `megafiles.xml` with `Config_CW.meg`; atlas extends the Remake's (562 entries more, 5 fewer) | leaf over the Remake | atlas (8192 × 4096), text, SFX, constants; no `.rc`, dialogs or command bar |
| 3689306867 | A standalone EaW Remake build ("RE5") | Remake `megafiles.xml` under `Data/Megs/`; shells named `*_5`/`*_RE5`; text DB 29,543 records | leaf over FoC | as the Remake, plus a 36-slot, 1,352-unit-wide tactical HUD and dormant variant shells |

Mounted alone, RCW fails 5 of its 18 shells (EAWR-VFS-0002), because the shells live in the
Remake. It needs a mod chain. The viewer's `--eawr-mod-root` takes one root today (gap G4).

## 2. Per-mod HUD diff against vanilla FoC

### 2.1 Which HUD files a mod replaces

Every mod replaces these files; the two submods take some of them from the Remake:

- `guidialogs.rc`, `resource.h` and `GUIDialogs.xml`;
- `CommandBarComponents.xml`;
- `RadarMap.xml` and `GameConstants.xml`;
- `MT_CommandBar.mtd` and its page;
- the English text DB.

`CommandBarComponentFiles.xml` stays byte-identical everywhere, and no mod adds a second component
file. `MousePointers.xml` is replaced by all mods except RaW, and the GUI/HUD SFX files by all
except FotR. No mod replaces `MT_CommandBarCompressed.dds`, so it goes stale beside a modded MTD
(fidelity line F2). No mod ships a font file.

### 2.2 Command bar and shells

| | RaW | Remake (RCW and 3229239424 inherit it) | FotR | 3689306867 |
|---|---|---|---|---|
| Components (FoC 846) | 845 | 859: +23, −10 | 865: +21, −2 | 920: +77, −3 |
| Changed components; top fields | 79; `Mouse_Over_Texture_Name`, `Text_Color`, `Scale` | 336; `Scale` 243, `Icon_Alternate_Texture_Name`, `Text_Color`, `Text_Offset` | 156; `Scale`, `Text_Color`, `Size` | 350; as the Remake |
| New `Group` names | none | `Filter_Ground`, `Filter_Space` | 8 (`Documentation_*`, `FilterBuilds`, `Filter_Overlay`) | `Filter_Unit` |
| Shell `Model_Name` swaps | none | 5 (tactical, galactic, organize, planetary mode and overlay) | none | 5 (the same five, `*_5`/`*_RE5` galactic) |
| Tactical shell anchors (FoC 153) | 153; 150 moved, 3 renamed | 155; 49 moved, +`Movie_tactical`, +`text_tactical_help` | 153; 141 moved | 210; +57 (slots 24–35, `ds_countdown_timer`, `Movie_tactical`) |
| Tactical faceplate: mesh → visible extent (reference units) | 1146 × 1081 card; opaque part ≈ 1051 × 284 at the bottom | 1018 × 284 | 1146 × 1081 card; opaque part ≈ 1020 × 245 | 1352 × 284 |
| Galactic faceplate width | 1146 (card) | 1379 | 1146 (card) | 1379 |
| Indexed families: `b_create`, `s_select`/`s_health`/`s_shield`, `special_button`, `special_border` (FoC 26, 24, 24, 12) | unchanged | 41, 24, 24, 12 | 34, 24, 24, 12 | 41, 36, 36, 18 |
| Removed components | `i_planet_image` | `pad_03…09`, `text_fleet1…3` | `h_close`, `text_allegiance_a` | `text_fleet1…3` |
| Shell idle animations (`i_*.ala`) | 2 | 2 | 4 | 8 |

MOD-3 verification: the Remake-family catalogue has 41 build components, but the active
shells contain b_create_00 through b_create_39 and no b_create_40. The bound contiguous
capacity is therefore 40, while FoC/RaW bind 26 and FotR binds 34.

The data facts behind the table:

- **Faceplates as cards.** RaW and FotR draw the faceplate as one card taller than the 768-unit
  reference height. The texture is 2272 × 2048, and only its bottom quarter is opaque. So mesh
  bounds are not the visible HUD, and faceplate hit tests must use texel alpha (UI-I2). The UVs
  are linear with v running 0 → −1, so the texture must be sampled with wrap.
- **Widescreen HUDs.** The Remake family widens the galactic faceplate to 1379 units and the
  3689306867 tactical faceplate to 1352 units. That is the reference width of a 16:9 screen
  (UI-L1 gives 1365). 3689306867 also ships `I_Tactical_Controls_RE5_{Normal,Wide,Expanded}`,
  1018 or 1352 units wide, plus a `CommandBarComponents_RE5.xml` that the file list does not
  name. These are dormant, user-swapped variants.
- **Materials.** Shell meshes use only `MeshAlpha`, `MeshAdditive`, `MeshGloss`, `MeshCollision`
  and the `alDefault` placeholder, as in FoC. No mod adds an interface shader.
- **Animations.** Every shell idle animation checked (FoC, RaW, FotR, the Remake and six of the
  eight in 3689306867) moves at most one bone: the help-droid button, with translation and rotation.
  The four RE5 idles animate nothing.
- **Schema.** Every mod tag is in the FoC schema: no EAWR-UI-0307, and no invalid component.
  3689306867 defines `b_play_pause` and `b_fast_forward` twice (EAWR-UI-0304, the later one wins).

### 2.3 Dialogs and `GUIDialogs.xml`

| | RaW | Remake | FotR | 3689306867 |
|---|---|---|---|---|
| Dialogs parsed (FoC 111) | 110 | 108 + 2 rejected | 109 + 1 rejected | 109 + 1 rejected |
| Removed | `IDD_SELECT_MOD_DIALOG` | `IDD_SELECT_MOD_DIALOG` | `IDD_SELECT_MOD_DIALOG`, `IDD_TECH_TREE_BLACKMARKET[_SPACE]` | `IDD_SELECT_MOD_DIALOG` |
| Added | none | none | `IDD_TECH_TREE_HUTTS[_SPACE]`; `resource.h` gives both the same id (216) | none |
| Resized; dialogs with changed controls | 4 (main menus 512 → 1024 wide); 17 | 28; 48 (44 caption edits) | 6; 29 | 28; 49 |
| New control statements or classes | none | none | none | none |
| `GUIDialogs.xml` | unchanged values | 6 texture slots and 50 font entries changed, 20 font entries added | 5 fonts changed, 17 texture slots added | as the Remake, plus one `Default` slot |

The dialogs the UI-02 parser rejects:

- Remake, RCW and 3229239424: `IDD_MAIN_MENU_DIALOG` has an empty field (`215,,22`).
- The same three plus 3689306867: `IDD_TECH_TREE_BLACKMARKET` lacks the comma between the id and
  the class, so a `CONTROL` statement has too few fields.
- FotR: `IDD_TECH_TREE_EMPIRE_SPACE` has a `STYLE` that ends in `|` and runs on into `CAPTION`.

The mods run on the retail game, so the retail reader must tolerate these errors. How it recovers
is not observed (fidelity line F1).

Every font face in the mods (`GUIDialogs` and command bar) is already in the FoC set:
EmpireAtWar-Bold, -Medium and -light, and the Arial variants. Mods therefore leave D1 unchanged.

### 2.4 Atlas, cursors, text, sound and constants

| | RaW | Remake | FotR | RCW | 3229239424 | 3689306867 |
|---|---|---|---|---|---|---|
| `MT_CommandBar` entries (FoC 1,107) and FoC entries dropped | 1,630; −94 | 1,865; −28 | 2,644; −399 | 2,044; −28 | 2,422; −27 | 2,754; −28 |
| Page | 4096 × 2048 32-bit **BMP named `.tga`** | 4096² TGA | 4096 × 2048 **BMP named `.tga`** | 4096² TGA | **8192 × 4096** TGA | 4096² TGA |
| Text DB records (FoC 19,224) | 21,361 | 24,651 | 18,725 | 25,618 | 27,586 | 29,543 |
| Command-bar tooltip keys missing from the DB | 0 | 3 | 5 | 3 | 3 | 0 |
| Cursors (FoC 53) | unchanged | 23 changed (`Anim_Frame_Delay`, hot spot, texture) | `pointer_repair_hardpoint` **removed** | Remake's | Remake's | 23 changed |
| GUI / HUD / expansion HUD SFX events (FoC 90 / 298 / 191) | 66 / 143 / 168 | 107 / 315 / 201 | unchanged | 113 / 323 / 204 | 114 / 315 / 201 | 106 / 315 / 201 |

Every text DB loads, sorted by CRC and with no duplicates. FotR's DB is smaller than FoC's, so
vanilla keys can go missing (UI-T3).

`GameConstants.xml` UI values the mods change:

- countdown timer screen positions, spacing, font name and size;
- credits display font;
- hardpoint reticle textures and screen sizes;
- health thresholds;
- message colours;
- `Icons_Per_Column`.

`RadarMap.xml` changes one value per mod: `Space_FOW_Color` or `Land_Backdrop_Texture_Name`.

### 2.5 Scripted HUD changes

Mods run on the FoC executable, so every Lua call and story tag they use is FoC engine API. A few
are ones vanilla data never uses: FotR's `SFXManager.Allow_HUD_VO`, and story tags such as
`PAUSE_GALACTIC`, `REVEAL_ALL_PLANETS` and `DISABLE_AUTORESOLVE`. FotR's popups, planet guide and
message queue are Lua classes built on those calls. HUD-affecting calls in the mod scripts:

| | RaW | Remake | FotR | 3689306867 |
|---|---|---|---|---|
| Lua HUD calls (FoC scripts: 1,833) | 89 | 56 | 3,263 | 617 |
| Main functions | `Enable_Advisor_Hints`, `Set_Selectable`, `Game_Message` | `Add_Dialog_Text`, `Game_Message`, `Set_Dialog` | `Add_Dialog_Text`, `Fade_Screen_In/Out`, `Highlight`, `Letter_Box_In/Out`, radar blips, `Lock_Controls` | `Game_Message`, `Lock_Controls`, fades, radar blips, `Highlight`, letterbox, `Add_Objective` |
| Story events and rewards that target HUD components by name (`STORY_CLICK_GUI`, `FORCE_CLICK_GUI`, `FLASH_GUI`; RCW 72, 3229239424 36) | 0 | 36 (6 targets) | 77 (56 targets) | 222 (50 targets) |

**The mod pattern for new HUD buttons** (Remake build filters, FotR documentation/filter/save
buttons):

1. Add a component to the XML and a mesh with the same name to the shell.
2. Add a story `STORY_CLICK_GUI` event whose parameter is the component name.
3. Map that event to a Lua handler in `StoryModeEvents`.
4. The handler applies story rewards, e.g. `LOCK_UNIT` to hide build entries, or `FORCE_CLICK_GUI`.

The engine knows nothing about the new button beyond its name. Three FotR targets have no
component: `h_close`, which the mod removed, and `b_doc_filter06` and `b_disable_hints`, which exist
only as shell meshes. Such dangling references must stay harmless.

## 3. What mods change: classes

| Class | What | Mods |
|---|---|---|
| C1 Re-skin (data) | Atlas repack and larger pages, component textures, colours, font sizes, cursors, SFX, constants | all |
| C2 Re-layout (data) | Shell meshes moved or reshaped, faceplate replaced by a card or widened for 16:9, dialogs resized | all |
| C3 Shell model swap | Shell component `Model_Name` points at a new ALO; dormant alternates | Remake family, 3689306867 |
| C4 More slots in engine families | `b_create_NN`, `s_select/health/shield_NN`, `special_button/border_NN` grow; slots are found by name | Remake family, FotR, 3689306867 |
| C5 New components with script behaviour | New names and `Group`s wired through `STORY_CLICK_GUI` → Lua → story rewards | Remake family, FotR |
| C6 New, renamed or removed dialogs | `IDD_SELECT_MOD_DIALOG` removed everywhere; FotR swaps two tech-tree dialogs | all |
| C7 Script hooks | Lua and story calls that lock controls, hide the HUD in letterbox, fade, flash or highlight parts, show messages, objectives, story text and radar blips | all; mostly galactic and story |
| C8 Engine support beyond FoC | None: every change runs on the FoC executable. It relies on FoC engine tolerance: malformed `.rc`, BMP-as-TGA pages, larger atlases, extra slots, absent components | all |

<a id="4-coverage-by-the-155-architecture-and-the-loaders"></a>

## 4. Coverage by the UI-layer architecture and the loaders

| Class | Covered now | Gap → fix |
|---|---|---|
| C1 | Command bar, shells, text DB, the MTD directory and `GUIDialogs.xml` load for every mod | **G2** the page loader rejects BMP content (RaW, FotR): add content-sniffed 32-bit BMP. **G3** the 8192 × 4096 page (3229239424) exceeds the TGA pixel limit (16,777,216, the shared `detail::max_elements`): raise the page limit to at least 8192². **G5** the HUD atlas path must load pages through `load_mega_texture_atlas`, which already pairs the MTD with its same-stem `.dds`/`.tga` page, ignores a stale `*Compressed.dds` and rejects ambiguous siblings (`tests/assets/mtd_tests.cpp`); nothing to add in the loader |
| C2 | Anchors load for all 18 shells of every mod, with no new diagnostics. UI-L1/L2 hold in reference units | **G6** layout and hit-testing must not use faceplate mesh bounds: visible extent = union of bound component rects and the opaque faceplate texels (cards reach 1081 units). Wrap sampling for shell UVs. **G7** D4 placement rule for HUDs wider than 1024 (§5.3) |
| C3 | `shell_for_model` already resolves the shell through the component | none |
| C4 | Components load | **G8** the HUD view-model must count families from data (contiguous `NN` from 00 that exist in the catalogue and the shell), not hard-code 24/12/26 |
| C5 | Components load; `Group` is kept | **G9** a presentation event "component clicked or hovered (name)", and flash/force-click by name, as the future story bridge. The mechanism is in the script layer, not the renderer |
| C6 | UI-02 parses the dialogs it accepts | **G1** the parser rejects the whole `.rc` on the first bad statement (Remake, FotR, RCW, 3229239424, 3689306867): recover per dialog with EAWR-UI-0201 and keep the rest. Missing dialogs and unresolved dialog ids must be diagnostics, not failures |
| C7 | Not in §3 of the UI-layer design | **G10** a HUD state service fed by script: HUD hidden in letterbox or cinematic, control lock, message queue, objectives, story text box, radar blips, highlight/flash. M2 skirmish needs none of it; M4 campaign does |
| C8 | n/a | **G4** mod chains: the VFS already takes ordered layers; profiles and viewer flags must accept a submod over its parent. **G11** shell animation: apply ALA translation and rotation to component anchors (help droid, hero frames; EAWR-UI-0312 today). **G12** HUD movies (`Movie_tactical`/`Movie_galactic`, `COMMANDBAR_MOVIE`) need video playback: resolution, player-side conversion and a Theora player are in [hud-movies.md](hud-movies.md); the shell binding belongs to the tactical HUD |
| Cursors | UI-09 planned | **G13** a pointer missing from a mod's `MousePointers.xml` falls back to the default pointer (FotR drops one) |
| Tooltips and text | UI-T3 renders a missing key as its own text | none |

What a custom renderer (D2 option B) would need instead: textured polygon batches with alpha and
additive blending for shell meshes, atlas quads with state textures, bone-transform animation,
glyph rendering with gradient and outline, alpha-mask hit tests, focus, modal stack, keyboard
navigation, IME, and list, scroll, edit, combo and slider widgets for the dialogs. Godot's canvas
already does the drawing part inside a Control's `_draw`, so B would only add the widget work that
Controls already provide.

## 5. Recommendation

### 5.1 D2: Godot Controls (option A), built from data

Evidence that Controls can represent every mod HUD surveyed:

1. Mods draw the HUD only through FoC's data mechanisms:
   - flat shell meshes with the same five materials;
   - atlas sub-rects with state textures;
   - named fonts from the FoC set;
   - mesh-bound anchors.

   No mod adds an interface shader, 3D depth, a widget class or a control statement. The faceplate
   polygons (16–29 triangles) and cards are canvas `draw_polygon` calls with UVs.
2. The behaviours mods need map to Control features:
   - alpha hit tests → `_has_point` on the faceplate;
   - state textures → custom leaf Controls;
   - families and new components → Controls generated from the catalogue;
   - animated anchors → transform updates from the ALA tracks;
   - clicks by name → one signal per component.
3. The hard gaps are loader tolerance (G1–G4; G5 is only wiring) and the script bridge (G9–G10). Neither depends on
   the renderer.

Build rule: generate the HUD Control tree at run time from the shell anchors and the command-bar
catalogue. Never hand-author a `.tscn` per HUD. Then a mod's re-layout, swapped shell or extra
slots are data, as they are in retail. Leaf Controls:

- `ShellPolygon` (faceplates, frames);
- `AtlasButton` (state textures);
- `Bar`;
- `GradientLabel`;
- `Minimap` (later).

### 5.2 Minimal mod-ready M2 HUD (D3)

1. Take the tactical shell from the `i_main_skirmish` Shell component's `Model_Name`. Pick the
   faction's `_ALT<n>` meshes.
2. Draw decorative meshes as textured polygons (wrap sampling, alpha or additive). Hit-test the
   faceplate by texel alpha.
3. Bind components to anchors by name. Show unit cards, health and shield bars, abilities
   (`special_button`) and orders (`c_button`) as families counted from data.
4. Load the atlas as the MTD plus its same-stem page, decoded by content (BMP, TGA or DDS), with
   pages up to 8192 per side.
5. Show the planet name, the options button and the pause/options and battle-end dialogs from the
   lenient dialog catalogue. Captions and tooltips come from the text DB, with UI-T3 fallback.
6. Treat an absent component, dialog or pointer as an absent element plus one diagnostic, never a
   load failure.
7. Raise a named click/hover event for every component, even though M2 has no story listener.

### 5.3 D4 placement for mod HUDs (proposed rule)

HUD extent = the union of bound component rects and the opaque faceplate texels, in reference
units: FoC ≈ 1078, Remake 1018, RaW ≈ 1051, 3689306867 1352.

- **Screen wider than the extent:** centre the extent box horizontally, bottom-aligned, at the
  UI-L1 scale.
- **Screen narrower** (a 1352 HUD on 4:3): left-anchor as retail and let the right side clip.
  Owner decision OD-1 below.
- Never stretch.

UI-04b implements this with a centred 16:9 safe area instead of a centred extent box, so
the FoC HUD keeps its retail place on 16:9 screens (`ui-layer.md` §3.4).

## 6. Mod-compat test fixtures

Mod content is not ours to redistribute, so committed fixtures are synthetic and built in memory
or from tiny project-authored bytes. Real mods run only in opt-in corpus tests.

| Fixture | Kind | Pins |
|---|---|---|
| `rc_mod_tolerance` | Synthetic `.rc`/`.h`: `,,` field, missing comma before the class, `STYLE … \|` running into `CAPTION`, a dialog without an id, a removed dialog | Per-dialog rejection (EAWR-UI-0201), the rest parse (G1) |
| `atlas_bmp_page` | Synthetic 16 × 8 32-bit BMP named `.tga` plus a two-entry MTD | Content-sniffed decode (G2) |
| `atlas_large_page` | 8192 × 4096 RLE TGA generated in the test (about 1.3 MB) | Raised limit (G3) |
| `atlas_stale_compressed` | Modded MTD plus `.tga` beside a mismatched `*Compressed.dds` | The HUD path uses the existing page pairing (G5) |
| `shell_widescreen_card` | In-memory `assets::Model`: a 1352-unit faceplate, a card taller than 768 with an opaque bottom band, 36 `s_select` slots, a rotated hero-frame bone | Extent and D4 placement, alpha hit test, family count, animation (G6–G8, G11) |
| `commandbar_mod_edits` | XML: `Model_Name` swap, families extended to 35, removed `text_fleet1`, a new component with a new `Group`, one duplicate | Catalogue and binding tolerance (C3–C5) |
| `vfs_mod_chain` | Temp tree: leaf, parent, expansion, base. The shell exists only in the parent; the leaf lists a parent-only archive | Chained profile (G4) |
| `pointers_removed_entry`, `text_missing_tooltip` | Synthetic XML and `.dat` | Fallbacks (G13, UI-T3) |
| Opt-in corpus `EAWR_MOD_HUD_ROOTS` (`name=leaf[;parent]`, read-only, never committed) | RaW, Remake, FotR, 3689306867, RCW over Remake, 3229239424 over Remake | Per mod: component count, skirmish-shell anchors, family counts, MTD entries and page size, dialogs parsed and rejected, text records (§2 tables). Each mod covers a different class: RaW BMP page and card; Remake swap, `.rc` defects and filters; FotR new dialogs and components, BMP page and hooks; 3689306867 widescreen and 36 slots; the two submods chains and the 8K page |

<a id="7-owner-decisions-to-file-under-128-and-fidelity-lines"></a>

## 7. Owner decisions (to track in the owner-question list) and fidelity lines

| ID | Question | Options (recommended first) | Pros | Cons |
|---|---|---|---|---|
| OD-1 | A mod HUD is wider than the screen (a 1352-unit HUD on 4:3) | A: left-anchor and clip as retail; centre only when the screen is wider (§5.3) | Retail parity; mods ship narrow variants for 4:3 | Wide variants clip on 4:3 |
| | | B: as A, but scale down uniformly to fit | Every HUD fully visible; still never stretched | Deviates from retail; text gets smaller |
| | | C: always left-anchor as retail, also on ultrawide | Simplest | Contradicts D4's "centred on ultrawide" |
| OD-2 | Mod support in M2 | A: M2 loaders tolerate all six mods' HUD data (G1–G8, G11, G13) with the opt-in corpus test; script hooks and movies (G9, G10, G12) wait for M4 | Keeps the "hook the same data" goal testable now at loader cost | About a day of loader follow-ups in UI-02/03/05/09 |
| | | B: FoC only in M2; mods from M3 | No extra M2 work | Gaps found late, and the kit may bake in FoC-only assumptions |
| | | C: full mod parity, including story GUI hooks, in M2 | Mods playable early | Needs the script bridge (#M4 scope) now |

Fidelity list (one line each; observe on the rig with a mod before emulating):

- F1: retail recovery for malformed `.rc` statements (empty field, missing comma, dangling `|`).
- F2: which `MT_CommandBar` page retail samples when a mod replaces the MTD but not
  `MT_CommandBarCompressed.dds`.
- F3: whether retail binds a dialog by name or by `resource.h` id (FotR's renamed tech-tree dialogs
  have no id).
- F4: retail behaviour for a HUD wider than the screen (clip or scale).
- F5: FotR's replaced galactic shell does not bind the inherited FoC
  i_galactic_controls_idle_00.ala by bone index/name; observe retail clip association before
  changing the shared animation player's strict binding rule.
- F6: the sign and origin of a movie's `Commandbar_Offset`, and where retail draws a movie that
  has one (`Default_Incoming_Transmission`, [hud-movies.md](hud-movies.md)).
- F7: whether retail loops a `COMMANDBAR_MOVIE` portrait until `STOP_COMMANDBAR_MOVIE` or plays
  it once, and what the slot shows between movies.
- F8: retail's blend for alpha portraits (straight alpha assumed) and any tint or scanlines the
  shell adds over the movie.
