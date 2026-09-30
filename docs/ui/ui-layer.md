# UI layer: retail sources, architecture and plan (EAWR-155)

Status: design, 2026-09-25. There is no production code yet. The tickets are filed after review;
the owner decisions (section 5) are filed under EAWR-128.

The facts below come from the effective FoC VFS profile (`corruption` over `GameData`,
`Patch2.meg` winning) and from observing the retail game. The symbolised debug build was used
only for the layout, scaling, anchor and font-size rules. This note contains no addresses and no
retail code. Captures and extracted files stay under ignored `out/`.

## 1. Retail UI sources

### 1.1 Inventory

| Source | Location (FoC effective) | Format | Size | Tactical space HUD use |
|---|---|---|---|---|
| `GUIDialogs.xml` | `Patch2.meg` | XML with three sections: `Textures`, `Fonts`, `Tooltips` | `Textures`: `Default` (87 slots), 21 per-dialog sets (20 of 17 slots, 1 of 1) and 60 per-control sets. `Fonts`: `Default` (9 roles), 3 per-dialog and 287 per-control entries. `Tooltips`: 1 entry, whose control and key both do not exist. 33 entry names match no dialog or control, and 14 names repeat. 3 texture names (`underworld_logo_*`) resolve neither in `MT_CommandBar` nor as standalone textures. | Skin and fonts for the pause/options, battle-end and load dialogs |
| `Resources/GUIDialog/guidialogs.rc` + `resource.h` | `Patch2.meg` | Text of a Visual C++ `DIALOGEX` resource script, parsed by the game at start-up; `resource.h` supplies the numeric IDs | 111 dialogs. Controls: 954 `CONTROL` (718 `PETROGLYPH_DIALOG_IMAGE`, 155 `Button`, 51 progress, 28 trackbar, 2 IME), 630 `LTEXT`, 339 push buttons, 150 `EDITTEXT`, 124 `RTEXT`, 120 `COMBOBOX`, 93 `LISTBOX`, 80 `CTEXT`, 65 `GROUPBOX` | Same dialogs as above. Control captions are text-DB keys (`TEXT_*`): 891 resolve, 15 key-shaped ones are missing, and 841 are placeholders set at run time. Dialog `CAPTION`s are never keys. `resource.h` defines 2,336 symbols; one control id (`IDC_TEXT_AUDIO_3D_TECHNOLOGY`) is missing, and `IDC_STATIC` (−1) comes from the Windows headers. |
| `CommandBarComponents.xml` (listed by `CommandBarComponentFiles.xml`) | `Config.meg` | XML | 846 components (621 `TextButton`, 114 `Button`, 85 `Bar`, 18 `Shell`, 8 `Icon`), about 95 tags | The whole command bar: look, textures, fonts, SFX, tooltips and groups |
| Shell models | `Models.meg` | ALO; mesh and bone names are the component names | `i_tactical_controls.alo` has 153 meshes; `i_galactic_controls.alo` and 16 other shells exist; idle `.ala` animations | Tactical HUD layout (section 1.3). No XML references `i_main_tactical_space.alo`. |
| `MT_CommandBar.mtd` | `Textures.meg` | Mega-texture index (EAWR-31) | 1,107 entries on one 2048² page: use `load_mega_texture_atlas` to pair the same-stem `.tga`/`.dds`; `MT_CommandBarCompressed.dds` can be stale under mods and is not an automatic alternative | Every UI texture name resolves here; 219 XML references are unresolved (`plan/inventories/mtd-icons.json`) |
| Fonts | No font files ship | TrueType: four faces embedded in the executable and registered in memory at start-up (`EmpireAtWar-Bold`, `-Light`, `-Medium` and `-Stencil`; 65,684 to 67,632 bytes in the pinned FoC build, identical in the base EaW executable). Game data names them by their sfnt full name (name ID 4); their family names (ID 1) are `Empire At War Bold` and so on. The rest are Windows system faces (`Arial`, `Arial Bold`, `Arial Black`, `Arial Unicode MS`). | `GUIDialogs` usage: EaW-Bold 128, EaW-Medium 122, Arial Bold 20, Arial Unicode MS 13, Arial Black 4. Command bar: EaW-Medium 6 pt 247 times, 7 pt 38 times. Game constants define 13 name/size pairs. | All HUD text |
| Text DB | Loose file `corruption/Data/Text/MasterTextFile_English.dat` (not in a MEG); `CreditsText_English.dat` uses the same layout but is in display order (not CRC-sorted) and repeats its style keys | Binary format, rule UI-T1 | 19,224 entries; one key (`TEXT_END_OF_DATA`) appears 7 times, i.e. 6 excess duplicate records; longest value 769 characters | Every caption and tooltip |
| Cursors | `MousePointers.xml` + frame textures in `Textures.meg` | XML: `Base_Texture`, `Hot_X`, `Hot_Y`, `Anim_Frame_Delay`; frames are numbered `_00…` textures | 53 pointers, 83 frame textures. The names map to a fixed enum in the game, so entries can be added but never removed. | Select, move, attack, cannot, grab and similar pointers |
| UI sounds | `SFXEventsGUI.xml`, `SFXEventsHUD.xml`, `SFXEvents_Exp_HUD.xml` | SFX events (preset, samples, volume) | 90 + 298 + 191 = 579 `SFXEvent` declarations in the effective FoC data (`Config.meg`). Command bar references: `GUI_Highlight_Mouse_Over` 293, `GUI_Button_Press_2_SFX` 106, `GUI_Dial_Switch_SFX` 10 | Hover and click sounds; HUD speech events |
| Tooltips | Component `Tooltip_Text` (161 components; several keys give state variants) and the one `GUIDialogs` entry | Text-DB keys | `Tooltip_Delay` 330 (ms). Tooltip fonts: Arial 7, and Arial 5 for small text. | Command, ability and unit-card tooltips |
| Minimap | `RadarMap.xml`, plus per-object `Radar_Icon_Name` (99), `Radar_Icon_Size` (278) and `Radar_Icon_Scale_Land/Space` (74) | XML | 8 click and death events drawn as small ALO models; space backdrop `i_radar_map_grid`, FOW colour `25,66,120,100`; passability colour sets per environment | Minimap content |
| Per-object GUI data | Unit XML | `Icon_Name` (1,012), `Text_ID` (7,528), `Encyclopedia_*` (~1,000), `GUI_Bracket_Size` (308), `GUI_Bounds_Scale` (185), `GUI_Activated_Ability_Name` (130) | | Unit cards, brackets, names |
| `GameConstants.xml` UI values | `Patch2.meg` | XML | Countdown timers at (0.005, 0.10), spacing 0.05. Win/lose message: EaW-Bold 24 with colours. Hardpoint reticle 0.03 of the screen. `Health_Bar_Scale`. Radar selection colour. | In-battle messages and reticles |

### 1.2 Data versus engine code

| Part | Defined in data | Hard-coded in the engine |
|---|---|---|
| Command bar layout | Positions and sizes: shell-model meshes (section 1.3). Component look, fonts, SFX and tooltips: `CommandBarComponents.xml`. | Which component shows what: selection slot order, bar fill, ability mapping, button actions. The binding is by component name and `Group`. |
| Minimap | Placement: the shell `radar` mesh. Colours, backdrop and events: `RadarMap.xml`. Icons: object XML. | Rendering, view box and click handling |
| Unit cards / selection panel | Slots `s_select_00…23`, bars `s_health_*` and `s_shield_*`, textures | Grouping, ordering, paging |
| Menus and dialogs | `.rc` geometry and captions; `GUIDialogs.xml` skin and fonts | Screen placement preset per dialog (rule UI-L4), behaviour |
| In-battle text | `GameConstants.xml` fonts, colours and some positions | Message queue positions |
| Fonts | Face names and sizes (XML) | Face data embedded in the executable; pixel-size rule (UI-F1) |

### 1.3 Tactical space HUD (shell `i_tactical_controls.alo`)

Units are reference pixels (UI-L1), origin bottom-left, y up. Suffixes `_ALT0`, `_ALT1` and
`_ALT2` are the Empire, Rebel and Underworld variants.

| Element | Meshes | Rect (x, y, w, h) | Component | M2 |
|---|---|---|---|---|
| Faceplate | `Empire_Faceplate_ALT0`, `Rebel_…_ALT1`, `Underworld_…_ALT2` | −1, −1, 1078, 284; Underworld −1, −8, 1078, 291 | `MeshAlpha`, `i_galactic_dashboard_skirmish[_rebel\|_under]` | yes |
| Minimap | `radar` | 14.5, 10.5, 175, 175 | `Button`; `MeshAdditive` scan lines | yes |
| Unit cards | `s_select_00…23` | 50 × 50, 2 rows × 12, x 355…1010 | `TextButton`, `Tactical_Selection` | yes |
| Health / shield bars | `s_health_*`, `s_shield_*` | 50 × 8 per card | `Bar`, 10 levels, smooth | yes |
| Ability buttons | `special_button_00…23`, `special_border_00…11` | 24 × 25; borders 54 × 119 | `TextButton`, animated | yes |
| Orders | `c_button00…05` | 34 × 34 | Attack, Attack Move, Move, Set waypoint, Stop, Guard | yes |
| Options | `b_option_t` | 205, 7, 24, 24 | Opens the in-game options/pause dialog | yes |
| Pause, fast forward | `b_play_pause_t`, `b_fast_forward_t` | 2, 206.5 (34 × 35); 36.5, 206.5 | Toggles | yes (EAWR-459, [time controls](../behaviour/tactical-time-controls.md)) |
| Planet name | `Text_Planet_tactical` | 84, 228, 190, 22 | EaW-Bold 10, `255,212,33`, outline | yes |
| Credits, pop, tech | `Text_Credits_tactical`, `Text_Planetary_Pop`, `Text_Tactical_Tech` | about 20 high at y 206 | Text with icon | decision D7 |
| Retreat, reinforce, superweapons | `b_retreat`, `b_reinforcement`, `b_special_weapon[2]`; `*_switch` | 36 × 36 at x 200; switches 50 × 105 at x 350 | | no |
| Build queue, holocron, help, battle cam | `tqueue00…09`, `b_story_arc_t`, `b_droid_help_tactical`, `b_camera_t` | | | no |

Other space-battle UI outside the shell includes selection brackets and health bars over units
(`GUI_Bracket_*`), move and attack acknowledgement effects (`GUI_*_Ack_Effect`), hardpoint
reticles, tooltips, countdowns, in-game and event messages, the win/lose message and
`IDD_BATTLE_END_DIALOG`. The options button (`b_option_t`, tooltip "Main Menu") opens
`IDD_GAME_OPTIONS_DIALOG`, titled "Main Menu", with Save, Load, Options, Quit and Resume (§1.4).
Options leads to `IDD_IN_GAME_OPTIONS_DIALOG` and then the audio, video, keyboard, network and
gameplay pages. M2 needs only Resume and Quit.

As built (P2-20a EAWR-83, `presentation/ui/hud_shell.hpp`, viewer `--eawr-hud tactical`):

- The shell is the Model_Name of the `i_main_skirmish` Shell component, so a mod's shell wins.
  The shell draws, farthest z first, the variant's visible decorative meshes with a MeshAlpha or
  MeshAdditive shader and a texture: the faceplate and the help droid. It also draws the `radar`
  mesh's additive `i_scan_lines` over the faceplate's opaque black minimap well; that is the
  empty minimap frame. Textures are files under `Data/Art/Textures`. Button icons come from the
  command bar's `Mega_Texture_Name` atlas (`MT_CommandBar`), then from files.
- Planet name: the map's root field `0x09` (`assets::Map::context_name`, "Coruscant" in
  `_mp_space_coruscant`) names a `Planet` object. That object's `Text_ID`
  (`TEXT_OBJECT_STAR_SYSTEM_CORUSCANT`) is resolved in the text DB. Root field `0x08` holds the
  map's own name key (`TEXT_MAP_NAME_MP_SPACE_08`, "Coruscant Siege"), which the HUD does not
  show. Without an object, `Text_ID` or text entry, the context name is shown as written, with
  one EAWR-UI-0321 warning. EaW-Bold draws lower case as small capitals, so the text is not
  upper-cased.
- A shell button takes the pointer on its component's mesh, not on its drawn art (evidence
  HIT-1..HIT-7 in the ignored `out/research/p2-20a/`, static reading of the debug build).
  FoC finds what is under the mouse by casting the camera ray against the command-bar
  scene's models and mapping the hit mesh's name to its component; hover and click use that
  same pick. Button state art is an immediate textured quad outside the scene, so the ray
  never meets it. `b_option_t`'s 36 x 25 art therefore overhangs a 24 x 24 interactive rect;
  a click in the overhang goes to whatever else is under it. The viewer keeps the button
  control at its art quad and restricts its `_has_point` to the component rect, the rect the
  hit mask holds. A rig click in the overhang would confirm this at runtime; it has not been
  recorded.
- The HUD's hit mask is `HudViewModel::hit_test_screen`, so every bound component rect stops
  the pointer, including the slots later tickets fill.
- Button art keeps its texture's size (EAWR-349). In the debug build, each state quad of a button
  takes its size from its `MT_CommandBar` entry's texel width and height, times the component's
  `Scale` (default 1). The engine draws that quad centred on the component's position, and the
  position is the translation of the component's bone in the shell model, not the mesh centre.
  So `b_option_t` draws its 36 × 25 art around its bone at (217, 19), overhanging its
  24 × 24 mesh. The help button's bone sits at (19, 255), below the centre of a mesh that also
  spans the droid. The 1280×720 rig capture agrees to the pixel: the options button is drawn
  32 × 21 px inside a one-texel dark border. `ShellAnchor::origin` holds the bone translation,
  and `button_quad` computes the rect.
- The faction faceplate's texture draws divider lines across the time panel left of the planet
  name. FoC hides them under the panel's four buttons: help (`b_droid_help_tactical`), holocron
  (`b_story_arc_t`), pause (`b_play_pause_t`) and fast forward (`b_fast_forward_t`). Their
  opaque art covers the whole panel. P2-20a draws these four buttons as inert art in their
  normal (icon) state. Since EAWR-459 pause and fast forward are toggle buttons with their four
  state textures that drive the live battle ([time controls](../behaviour/tactical-time-controls.md));
  help and holocron stay inert art.

Unit cards as built (EAWR-425, part of P2-20b; rules in
[docs/behaviour/foc-unit-cards.md](../behaviour/foc-unit-cards.md)):

- `hud_shell` reads the card slots `s_select_NN` in order with their `s_health_NN` and
  `s_shield_NN` Bar components and the column borders `special_border_NN` (24 slots and 12
  borders in FoC). `presentation/ui/unit_cards.hpp` turns the selection into cards (squadrons fold
  into one card, groups by unit ability, stacks collapse only when the cards overflow the slots)
  and a card click into the new selection. The Godot `EawrUnitCards` draws them over the shell and
  is the only HUD control that takes the pointer on the slot meshes.
- The live battle (`--eawr-live-session`) rebuilds the cards every frame from its newest snapshot;
  a click goes back through `BattleInput::card_click` and never reaches a command. The gallery's
  HUD page draws a made-up selection with `--eawr-ui-hud-cards`.

### 1.4 Reference captures (rig, ignored `out/`)

Captured 2026-09-25 on the rig, FoC `coruscant-space` skirmish (Rebellion against Easy AI).

| Resolution | Location (worktree `out/ui/original/`) | Frames |
|---|---|---|
| 1280×720 | `coruscant-1280x720/` | `load_dialog`, `hud_default`, `hud_selected_unit`, `hud_stop_tooltip`, `ingame_menu`, `skirmish_setup_tooltip` |
| 1920×1080 requested | `coruscant-1920x1061/` | `main_menu`, `skirmish_setup`, `load_dialog`, `hud_default`, `hud_selected_unit` |
| 1280×720, earlier | `out/original/mp_space-coruscant/` | default, pan, zoom |

On the 1920×1080 desktop, Windows clamps a bordered 1920×1080 window to a 1920×1061 client, and
the game renders at that size. The taskbar covers the client below about y 1009, so the adapter's
export refused the size mismatch. These frames are client crops of step screenshots.

Measured results:

| Check | 1280×720 | 1920×1061 | Rule |
|---|---|---|---|
| Planet-name centre y (px from the top), measured against predicted | 495.5 / 495.9 | 731.0 / 730.8 | UI-L1, UI-L2 |
| Planet-name text width (px) | 95 | 144 (1.52×; height ratio 1.47×; font em 15 → 23 px) | UI-F1 |
| HUD width, minimap left edge | ≈ 995 px, 13 px | ≈ 1462 px, ≈ 20 px | UI-L1, UI-L2 (scale = client H/768) |
| Skirmish dialog 965 × 422 `.rc` units, measured against predicted | ≈ 1210 × 400 / 1206 × 396 | ≈ 1820 × 587 / 1809 × 583 | UI-L3 |
| `IDD_GAME_OPTIONS_DIALOG` 321 × 328, centred | ≈ 408 × 313 / 401 × 308 | n/a | UI-L3, UI-L4 |
| In-game menu open (Save, Load, Options, Quit, Resume) | Credits frozen for 2 min, so the skirmish is paused | n/a | UI-S1 |

Tooltips come in two forms. Order buttons show a small label at the pointer and a large panel
(icon, title, description) anchored above the droid at the HUD's top-left. Setup lists show only
the small label.

## 2. Rules a test should pin

| ID | Rule | Test |
|---|---|---|
| UI-L1 | The command-bar reference space is 1024 × 768. At aspect ≥ 4:3 the height is 768 and the width is 768·W/H. Below 4:3 the width is 1024 and the height is 1024·H/W. The pixel scale is H/768 (wide screens) or W/1024 (narrow). On a screen wider than 16:9 the UI lays out in a 16:9 safe area, 1365⅓ × 768 units, centred horizontally; otherwise the safe area is the whole screen (§3.4). Retail: the safe area is always the whole screen. | Table test at 1024×768, 1280×720, 1920×1080, 2560×1080, 3440×1440, 5120×1440 and 1280×1024 under both rule sets |
| UI-L2 | The tactical shell is placed at the safe area's lower-left corner at scale 1 in reference units. A shell wider than the safe area moves left only as far as keeps its right edge on screen. A shell wider than the screen keeps the retail corner and clips on the right (OD-1 option A, §3.4). Retail: the screen's lower-left corner. Component rects enclose vertices after the full bone-chain transform, including sampled ALA translation and rotation. Visible extent unions bound component rects with nonzero-alpha faceplate texels; faceplate UVs repeat. | Anchor test on synthetic and real shells; wide shells of 1352 units on 4:3 and 1379 units on 1920×1061 |
| UI-L3 | Dialog geometry is in `.rc` units of the 1024 × 768 reference and scales uniformly by the UI-L1 scale, min(W/1024, H/768), so a dialog is never stretched. Retail: x scales by W/1024 and y by H/768. | Rect test at the UI-L1 sizes plus a 1280×720 eye-check |
| UI-L4 | Dialog placement presets, relative to the safe area: centre (default); upper-, lower- and centre-left/right; upper- and lower-centre; full screen. Edge margins are 0.008 of the safe area's width and 0.006 of the height, plus the frame border on the left and top edges; right and bottom edges get the margin only. Upper-right has no margin on either edge but keeps the top frame border: it sits flush right with its top at the border. Upper-centre adds 0.02625 of the height below the top margin and border. A full-screen dialog's frame covers the viewport, and its gadgets are recentred in a 1024 × 768 area centred in the safe area. Retail: the safe area is the screen and sizes stretch per UI-L3. | Preset table tests: retail at 1280×720, aspect-correct at 2560×1080 |
| UI-L5 | Shell offsets snap to whole reference units. The retail half-unit D3D9 offset is not reproduced. | Snap test |
| UI-F1 | Font pixel height = ⌊⌊96·H/600⌋·pt / 72⌋. It is the em height; a `static size` font uses pt as pixels. H is the screen height; below 4:3 the aspect-correct rules take H = ⌊768·W/1024⌋ (960 at 1280×1024), so text keeps its size relative to the uniformly scaled dialogs. | Table test (720p: 7 pt → 11 px; 1080p: 7 pt → 16 px) |
| UI-F2 | `Stretch_Factor` ≠ 1 scales glyph height by the factor (rounded), keeping the unstretched average width. | Metric test |
| UI-F3 | An unavailable face falls back to the game's default Unicode face, and then to EaW-Medium. Russian raises the minimum to 7 pt; Russian and Japanese always use the Unicode face. | Fallback test (`tests/ui/font_tests.cpp`; viewer `--eawr-fonts`) |
| UI-F4 | `Top_Color` and `Bottom_Color` form a vertical gradient. `Outline` and `Emboss` are flags. | Golden capture |
| UI-T1 | `.dat` layout: u32 count; count × {u32 CRC-32 of the key, u32 value length in UTF-16 units, u32 key length in bytes}; all values (UTF-16LE); all keys (printable ASCII). The CRC is over the key bytes as stored (IEEE, as zlib). `MasterTextFile` records are sorted by CRC; `CreditsText` is not. | Synthetic fixture plus the corpus count |
| UI-T2 | Lookup is by exact key; it is case-sensitive, because the corpus holds keys that differ only in case. A duplicate key is reported. Which duplicate wins is open (first in file until observed). | Fixture |
| UI-T3 | A missing key renders the key text and records one diagnostic per key. | Fixture |
| UI-T4 | The language file is `MasterTextFile_<LANGUAGE>.dat` in the FoC text folder. | Path test |
| UI-C1 | UI never mutates the simulation. Every gameplay action goes through the same command sink as world input and is stamped (tick, player, sequence) by the scheduler. | Replay hash unchanged with and without the UI; UI-issued and world-issued orders give equal command streams |
| UI-C2 | Selection, hover, tooltips, open dialogs and cursor state are presentation-local and never enter replay or hash. | Hash test |
| UI-I1 | Controls see pointer and key events before world and camera input; world and camera handlers take only unhandled events. Order: modal dialog → HUD → world (EAWR-82) → camera. | Godot routing test |
| UI-I2 | "Over HUD" means over a component rect or an opaque faceplate pixel; transparent faceplate pixels pass through to the world. | Hit test on faceplate alpha |
| UI-I3 | A focused edit box suppresses hotkeys and camera keys. | Routing test |
| UI-R1 | UI draws on the 2D canvas after 3D, with HDR 2D off, in both Compatibility and Forward+. Texel colours display as stored and never pass the stored-value decode (FP-1). | Colour probe in both renderers |
| UI-R2 | Shell materials map `MeshAlpha` → alpha blend and `MeshAdditive` → additive. MTD subrects follow the EAWR-31 origin contract. | Golden capture |
| UI-S1 | The in-game menu (`IDD_GAME_OPTIONS_DIALOG`) pauses a single-player skirmish while it is open. Pausing stops tick advance, so the replay is unchanged. | Replay test: ticks and hash equal with and without a pause |

Open observations (fidelity list until observed):

- The duplicate-key winner.
- Which of two `GUIDialogs.xml` entries with the same name wins (first until observed). The override precedence is control → dialog → `Default`, and a font entry is taken whole (until observed).
- How retail recovers from a malformed `.rc` dialog (mods ship an empty field, a missing comma and a trailing `|`). Until observed, EAWR rejects only that dialog with one EAWR-UI-0201 error (the defect's line and the reason) and resumes after its `END`, or at the next `DIALOGEX` when the `END` is missing. `BEGIN`, `END` and statement keywords are never values, so a trailing `|` or `,` stops at the next statement.
- The `Anim_Frame_Delay` unit.
- Edge scrolling while the pointer is over the HUD.
- Whether the mouse wheel over the command bar zooms the camera. UI-07 stops it there, per UI-I2.
- Whether a HUD attack mode stays armed after a click on ground or a friendly unit. UI-07 keeps it
  armed; retail issues no action there.
- Whether retail hotkeys are suppressed while an edit box has focus (UI-I3 is a design rule; the
  code reading did not trace it).
- The idle animation of the tactical shell (P2-20a draws the bind pose).
- `Max_Text_Width` 110 on `Text_Planet_tactical`: its unit and what retail does past it. The
  HUD does not clip the planet name.
- `Click_Shift` and `Selected_Alpha` on the options button; P2-20a draws only its four state
  textures.
- UI-F3 on a PC without `Arial Unicode MS`: the rig's FoC skirmish capture shows list and combo
  text in a sans face (the GDI font mapper's substitute), where the chain lands on EaW-Medium.
- Where retail draws the `Scanlines` slot; the kit does not draw it.
- Project-authored until observed:
  - the emboss shadow (1 px, black at 75 %) and the outline (2 px black);
  - 50 % alpha for disabled checks, radios, sliders and captions;
  - the list's hover bar (the selection bar, white at 30 %, matches the rig capture);
  - the vertical centring of checks and sliders in their `.rc` rects;
  - the top colour only, not the gradient, on list, combo and edit text.
- GDI draws captions without anti-aliasing, so they look heavier than the engine's text at the
  same size.

## 3. Architecture

### 3.1 Layers and data flow

| Layer | Location | Owns | Engine-free |
|---|---|---|---|
| Sim | `src/sim` | Authoritative state and the command queue | yes |
| UI data | `src/assets/text_database.*`, `src/data/ui/` | Loaders into an immutable `UiCatalog`: text DB, `.rc`/`.h`, `GUIDialogs.xml`, command bar, shell anchors (existing ALO reader), cursors, SFX names | yes |
| UI model | `src/presentation/ui/` | Layout and scaling (UI-L*, UI-F*); HUD view-model built from the sim snapshot plus local selection; `UiAction` → `sim::Command` mapping | yes |
| Godot UI | `src/presentation/godot/ui/` | Control tree, theme, atlas textures, fonts, cursor, input routing | no |
| Viewer | `apps/viewer` | Mode and flags, hosting and captures | no |

Snapshot → HUD view-model → Controls. Control action → `UiAction` → command sink → sim queue
(UI-C1). This is the same sink EAWR-82 uses for clicks in the world.

### 3.2 Godot Controls versus a custom canvas renderer

| Criterion | A: Godot Control/Theme built from retail data | B: custom `RenderingServer` canvas renderer |
|---|---|---|
| Hit testing, focus, modal handling, keyboard navigation | Built in (`mouse_filter`, focus, `_gui_input`) | Must be written |
| Text: shaping, IME, RTL/CJK, wrapping, outline | TextServer/HarfBuzz, `Label` outline, `FontVariation` | Must be written, or GDI-style glyph caching rebuilt |
| Retail absolute layout | Anchors and offsets in pixels from the UI model; custom `_draw` for the 16-piece frame, 3-piece buttons and gradient text | Natural (retail draws quads) |
| Renderer independence (Compatibility and Forward+) | Canvas pipeline is shared | Shared |
| Tests | Control rects are queryable; golden captures | Own hit/draw test surface |
| ADR-011 fit ("prefer Godot UI behind project-owned adapters") | yes | Against the ADR |
| Cost of all screens (§4.3) | Lower: lists, scroll, edit, combo, slider exist | High: every widget is custom |
| Pixel parity with GDI text | Close, not exact | Close, not exact |

**Recommendation: A.** Keep an engine-free layout model fed by retail data and a thin Godot
builder:

- Custom-drawn leaf Controls handle what `Theme` cannot express: the 16-piece frame, 3-piece
  state buttons, top/bottom gradient text and bars.
- Shell decorative meshes are flat, so draw them as 2D canvas polygons, not 3D.

Reasons:

- Input, focus, text and IME come for free, and they are the parts B would underestimate.
- Retail layout is absolute and reduces to anchors and rects, which Controls express directly.
- Layout rules stay testable headless.
- The same path serves menus, lobby and galactic later.

### 3.3 Input routing and focus

- The viewer host handles camera input in `_input`, which runs before the GUI. UI-07 moves world
  and camera handling to `_unhandled_input`, so UI-I1 holds.
- The HUD root uses `mouse_filter` STOP only on component rects and on the opaque-faceplate
  mask (UI-I2). Modal dialogs cover the viewport and block the world.

As built (UI-07 EAWR-304):

- The policy is engine-free in `eawr/presentation/ui/input_routing.hpp`. After the GUI, a
  modal dialog blocks keys, presses, wheel and motion from the world. A focused edit box blocks
  every key, including the ones it does not consume. A button release always reaches the
  world, so a hold the world took always ends.
- Before the GUI, a world hold owns the pointer. While the world holds a mouse button, the
  host's `_input` passes motion and that button's release straight to the world, even over the
  HUD. Retail behaves the same way: a drag-select, map scroll or rotate keeps its state while the
  pointer is over the command bar.
- The world layer runs before the camera. EAWR-82's selection and orders plug into
  `ViewerHost::route_world_input` ahead of the camera path.
- `EawrUiHitMask` is the HUD root. Its `_has_point` is `HudViewModel::hit_test_screen`.
  `EawrUiModalLayer` is a modal backdrop; a visible one sets the policy's `modal_open`. Both turn
  off Godot's `force_pass_scroll_events`. By default Godot passes wheel events on through STOP
  controls, and the camera would then zoom under the HUD.
- HUD buttons never take keyboard focus (`FOCUS_NONE`, as in the kit), so arrow keys stay with
  the camera after a click. When a text control takes focus, held camera input is cancelled.
- A modal dialog opening or closing ends every world hold (`ModalHoldGuard`). The host checks
  the modal state before each event and each frame. On a change it releases the world's pointer
  capture and cancels held keys, buttons and edge scrolling, as on a window focus loss. Otherwise
  a key held when the modal opens would keep the camera moving, because the modal blocks its
  key-up. A held button would keep sending motion to the world past the modal backdrop. After
  the edge only a fresh press starts a hold, so no hold can stick.
- The command sink is `eawr/presentation/ui/command_sink.hpp`. HUD buttons, hotkeys and world
  clicks build a `TacticalIntent` (stop, move, attack; face and ability are refused until the
  rules have them) and call `CommandSink::issue`. `CommandScheduler` is the standard sink and
  the hand-off to the thread that steps the session:
  - `issue` is the producer side, on the Godot main thread. It validates the intent and stamps
    it at once with (open tick, local player, sequence). The sequence is the issue order. The
    open tick is the earliest tick not yet taken.
  - `take(next_tick)` is the consumer side. The thread that steps the session calls it at the
    tick boundary, before it steps `next_tick`; in the live session (EAWR-80) that is the simulation
    thread. It returns the due commands in sequence order and closes the tick. An intent issued
    after it returns lands in a later tick.
  - A mutex guards the queue and the open tick, so each command reaches exactly one `take` and
    none is stamped for a tick that already ran. The order within a tick is the issue order,
    whatever the thread timing. The replay records the tick each command landed in.
  `OrderInput` keeps the selection and the armed HUD move or attack mode. Both are
  presentation-local (UI-C2).

### 3.4 Resolution and aspect

Decision D4 (EAWR-166), implemented by UI-04b (EAWR-195) in `src/presentation/ui/layout.cpp`:

- The UI lays out in a safe area: the whole screen up to 16:9, and a centred 16:9 area on wider
  screens. The HUD sits at the safe area's lower-left corner at the UI-L1 scale. Dialogs scale
  uniformly (UI-L3) and their presets apply to the safe area (UI-L4). Fonts follow UI-F1.
- `LayoutRules::retail` keeps the original rules for comparison captures against the rig: the HUD
  in the screen's corner and dialogs stretched on both axes.
- There is no user UI scale.
- HudViewModel (presentation/ui/hud.hpp, MOD-3 EAWR-235) supplies the visible right extent to
  place_shell and uses that same placement for screen hit tests. Faceplate alpha masks are
  top-left and sample with nearest repeat, including negative UVs. Their texel areas are clipped
  against the transformed triangles for extent; transparent card margins never block input.
  Family capacity counts contiguous two-digit names from 00 in both shell and catalogue,
  independent of temporary visibility. The kit supplies decoded masks and a faction variant;
  an unavailable mask is diagnosed, without falling back to the faceplate's mesh bounds.

**Why a 16:9 safe area, not 4:3.**

- The retail HUD does not fit 4:3. The FoC tactical faceplate is 1078 units wide against 1024, so
  retail clips it on 4:3 screens. A 4:3 area would not hold the retail HUD or the widescreen mod
  HUDs (`mod-hud-survey.md` §2.2: Remake 1018, RaW about 1051, 3689306867 1352, and the Remake
  galactic faceplate 1379).
- Mods author wide HUDs for 16:9. UI-L1 scales by height, so a 16:9 screen is 1365⅓ units wide.
  The 1352-unit tactical faceplate of 3689306867 fits it, and so does every surveyed tactical HUD.
- On a 16:9 screen a 16:9 safe area is the whole screen. The HUD stays where retail puts it on the
  most common screens, and the 1280×720 rig captures (§1.4) check both rule sets. A 4:3 area
  would move the HUD 171 units (160 px at 1280×720) off the corner on every 16:9 screen, while D4
  asks for centring on ultrawide only.
- The rig's clamped 1920×1061 client is a little wider than 16:9, so there the aspect-correct HUD
  sits 12 units (17 px) right of the retail capture.

The survey's §5.3 proposal centred the HUD extent box itself. At 16:9 that would move the FoC HUD
144 units right of where retail and the rig captures put it. So the safe area is centred instead,
and the HUD keeps its retail position inside it.

**Shells wider than the safe area or the screen.** A shell that is wider than the safe area but
fits the screen moves left until its right edge meets the screen's (the Remake galactic faceplate
on 1920×1061). A shell wider than the screen is placed by `overwide_shell`, which alone holds the
answer to owner question OD-1 (EAWR-205, open). It implements option A: the retail corner at the
UI-L1 scale, clipped on the right. That includes FoC's own HUD on 4:3 and 5:4, as in retail.
Option B (scale down uniformly to fit) would replace that one function; it would also shrink
FoC's own HUD on 4:3 and 5:4 by 1024/1077.

**3D view.** The base game squashes models on ultrawide until the resolution is changed; the
viewer does not. The renderer sets a vertical-FOV perspective, and Godot takes the aspect from the
viewport every frame. The project sets no stretch mode, so an interactive viewport follows the
window. The capture pin keeps the aspect (letterbox). A resize republishes the camera frame with
its vertical FOV and pose unchanged. `tests/presentation/renderer/test_viewport_aspect.py` and the
controller's `test_viewport_aspect` pin this.

### 3.5 Text and localisation

- Captions and tooltips resolve through the text DB at widget build time.
- Language is a start-up setting (UI-T4). Changing it rebuilds the catalogue and the fonts
  (retail reloads fonts per language).
- Fonts come from decision D1. The size rules are UI-F1 to UI-F3.
- Font provisioning (UI-05 EAWR-191):
  - The player extracts the four faces with `tools/fonts/extract_eaw_fonts.py` (setup in
    `docs/build.md`). The cache holds `<face>.ttf` and a `fonts.json` manifest.
  - The UI mounts the cache as a loose VFS layer at `fonts/`, so the files are read as
    `fonts/<face>.ttf`.
  - The tool and the loader apply the same TrueType checks. A cached file whose name ID 4
    (or 6) is not its face is left out with a warning.
  - Embedded faces come only from the cache and other faces only from the system. GDI-style
    names such as `Arial Bold` resolve to a family and a weight. When nothing in the chain
    exists, the engine's own font is used; it is a project-authored fallback.
  - The default Unicode face is `Arial Unicode MS`, the only Unicode face the executable names.
    Stock Windows does not have it (Office installs it), and neither does the rig, so there the
    chain lands on EaW-Medium.

### 3.6 Theming

- Retail data is the theme source: `GUIDialogs` `Default` → Godot `Theme`; per-dialog and
  per-control overrides → theme type variations; command-bar components → HUD widget style.
- Project-authored values are limited to fallbacks: missing texture, font and colour defaults
  when data is absent. They are recorded with `provenance: project-authored`, as in
  `camera-bindings.json`.
- Mods override through the VFS like every other asset.

The UI kit (UI-06 EAWR-229) implements this. Code that builds screens from the kit relies on these
facts:

- **Theme types.**
  - The engine-free `ThemeModel` (`src/presentation/ui/theme.cpp`) becomes one Godot `Theme`
    (`src/presentation/godot/ui/theme_builder.cpp`).
  - The `Default` set is the type `EawrUi`. Each dialog or control entry is the variation
    `EawrUi__<entry>`. A control entry inside a dialog that has an entry becomes
    `EawrUi__<dialog>__<control>`, based on the dialog's variation.
  - Per item, a lookup therefore follows control → dialog → `Default`, as `DialogCatalog`
    resolves a control. `ThemeModel::variation(dialog, control)` names the variation to set.
  - Godot follows a `theme_type_variation` only when a theme declares it, so `EawrUi` is
    itself declared a variation of the empty type `EawrUiBase`.
  - Type names take identifier characters only.
- **Theme items.**
  - One icon per skin slot, named as the XML element (`Button_Left_Mouse_Over`). It is cut from
    the MTD page at texel resolution, with its size overridden to the UI-L3 scale. A slot
    written `none` holds an empty marker texture, so it still overrides its base.
  - Per font role (`Push_Button`, ...):
    - the font: the UI-F3 face as a `FontVariation` with `Character_Padding` and the UI-F2 width;
    - its glyph height in pixels;
    - the colours `<role>_top` and `<role>_bottom`;
    - the constants `<role>_emboss` and `<role>_outline`;
    - `<role>_cell_ascent` and `<role>_cell_descent`: the GDI text cell.
  - A control entry holds one font description, so it fills every role.
- **Draw rules, measured against the FoC in-game menu at 1280×720 by retail rules.** The frame
  lands on the same pixels, the captions are within 1 px and the title within 2 px.
  - The 16 frame pieces lie outside the dialog rect: corners at the corners, transitions inside
    the span next to them, edges stretched between. The frame background stretches over the
    rect; it has its own rim, so tiling it would draw a grid. The frame's bright line is the
    rect's edge.
  - A push button draws its pieces at their scaled texture height from the top of its `.rc`
    rect, whatever the `.rc` height. At 720 lines that is 27 px border to border for a 24-unit
    button. The caption is centred over the whole button.
  - The EmpireAtWar faces carry usWinAscent 921–959 against an hhea ascender of 688 (per 1000).
    GDI places text by the win metrics, so kit text uses that cell for its baseline and for
    vertical centring. With the engine's ascent the menu title sat 7 px high.
  - A closed combo box is as high as its text box texture; its `.rc` height includes the
    drop-down list.
  - A list's `Scroll_Tab` keeps its texture size at its proportional place; it does not
    stretch with the page. There are no lines between rows.
- **Texture files.** A slot's texture is looked up in the atlas, then as
  `Data/Art/Textures/<stem>.tga` or `.dds`, then by the name as written. The Remake skins
  `IDD_SPACE_BATTLE_LOAD_DIALOG` with a loose `LOADMENUBACK2.jpg`, so the retail reader takes
  JPEG. The asset loader reads TGA and DDS, so the kit decodes a JPEG through the engine.

### 3.7 Testing

| Layer | Harness | Gate |
|---|---|---|
| Loaders, layout, fonts, text | Root CTest, synthetic fixtures; opt-in corpus tests on `EAWR_EAW_GAME_ROOT` | Always (headless) |
| Command emission | Replay tests: UI-issued orders produce the same command stream; hashes unchanged at 1/2/4 workers | Always |
| Godot routing and rects | Viewer runtime test: Control rects equal the model; the event order in UI-I1 | Opt-in graphical |
| Look | `--eawr-ui <fixture>` capture at 1280×720 and 1920×1080; hashes pinned after FP-3; eye-check next to rig originals | Opt-in graphical plus owner eye-check |

## 4. Work breakdown

"Now" means it can start during M1.5 without touching `apps/viewer` or the serial viewer lane
(milestone 8). "After FP-1" means after EAWR-149 lands, or after EAWR-125 if Forward+ is declined.
Agent-days follow the scale in `docs/estimate.md`.

### 4.1 Foundation

| ID | Outcome | Acceptance evidence | Size | Days | Depends | When |
|---|---|---|---|---|---|---|
| UI-01 | Text DB loader | UI-T1 to UI-T4 fixtures; FoC corpus: 19,224 records, the 6 excess `TEXT_END_OF_DATA` duplicates reported | S | 0.14 | VFS | Now |
| UI-02 | Dialog catalogue: `.rc`/`.h` tokenizer and `GUIDialogs.xml` override resolution | All 111 dialogs parse; every texture and font resolves or is listed; captions map to text keys | M | 0.37 | UI-01, EAWR-31 | Now |
| UI-03 | Command-bar catalogue and shell anchors | 846 components typed; `i_tactical_controls` anchors equal §1.3; ALT variants | M | 0.37 | ALO reader | Now |
| UI-04 | Layout, scaling and font-size model | UI-L1 to UI-L5 and UI-F1 to UI-F2 table tests | S | 0.31 | UI-03 | Now |
| UI-05 | Font provisioning per D1, with fallback chain | UI-F3 test; faces load on Windows and Linux | S | 0.31 | D1, UI-04 | Engine part now; Godot part after FP-1 |
| UI-06 | Godot UI kit and theme builder | Frame, 3-piece button, gradient label, bar, list, combo, slider, check/radio, edit; a gallery capture | L | 1.00 | UI-02, 04, 05 | After FP-1 |
| UI-07 | Input routing, focus and command sink | UI-I1 to UI-I3 and UI-C1 to UI-C2 tests; camera unchanged | M | 0.44 | T5 EAWR-102; shared with EAWR-82 | After T5 (before EAWR-82) |
| UI-08 | UI test harness | `--eawr-ui` fixtures; pinned hashes. Rig HUD route with focus retry and selection, plus a second-resolution profile that is not clamped (a larger rig desktop or an admitted borderless launch) | M | 0.15 | UI-06; rig part none | Rig part now; Godot part after FP-3 |
| UI-09 | Cursors | 53 pointers load; hot spot and animation; state hook for select, move, attack and cannot | S | 0.31 | UI-06 | Loader now; Godot after FP-1 |
| UI-10 | UI sound events | Hover and click SFX via the EAWR-84 adapter | S | 0.31 | EAWR-84 | With EAWR-84 |

Days are midpoint agent-days by the calibrated method in `docs/estimate.md` (task count × class
median to × p80, worker-hours ÷ 24; recomputed in the EAWR-158 review). Foundation total: **3.7 agent-days**
(range 1.5–5.9; 36–141 worker-hours). Work that can run during M1.5: UI-01 to UI-04 and parts of UI-05,
UI-08 and UI-09, about 1.4 agent-days.

### 4.2 Phase 2 HUD (rescopes EAWR-82 and EAWR-83)

| ID | Outcome | Acceptance evidence | Size | Days | Depends |
|---|---|---|---|---|---|
| EAWR-82 P2-19 (unchanged scope) | Camera range; click, box, type and group selection; orders as commands; drag-box drawing | Existing checklist, plus orders flow through the UI-07 sink | M | 0.44 | EAWR-66, EAWR-70, EAWR-80, UI-07 |
| P2-20a (EAWR-83) | Tactical HUD shell: faction faceplate, minimap frame, planet name, options button | Eye-check next to the rig at 1280×720 and 1920×1080; UI-L1/L2 hold | M | 0.34 | UI-03, 04, 06 |
| P2-20b | Unit cards, bars, abilities with recharge, order buttons, tooltips | Selecting fills the cards; buttons emit the same commands as the world; tooltip after 330 ms | M | 0.71 | EAWR-82, EAWR-76, UI-06, 07 |
| P2-20c | Minimap: backdrop, blips, FOW, view box, click to pan or move | Blips follow units; clicks give commands or a camera pan | M | 0.47 | EAWR-67, UI-06, 07 |
| P2-20d | FoC team colours on the HUD and units (existing EAWR-83 core) | Existing EAWR-83 checklist item | S | 0.23 | EAWR-32 |
| P2-20e | Victory/defeat message, pause/options dialog with Quit, battle-end dialog (minimal) | End state visible; Quit returns cleanly | M | 0.47 | EAWR-77, UI-02, 06 |
| P2-20f | In-world feedback: selection brackets and health bars, move/attack acknowledgements, hardpoint reticles | Eye-check next to the rig | M | 0.22 | EAWR-82, EAWR-72 |

Phase 2 HUD total, including EAWR-82: **2.9 agent-days** (range 1.25–4.5). The calibrated Phase 2 estimate
(`docs/estimate.md`) already budgets a 1.07-day UI-foundation placeholder, EAWR-82 at 0.44 and EAWR-83 at 0.63.
Replacing the placeholder and EAWR-83 with the rows above adds about **4.4 agent-days** (48–165 worker-hours),
taking Phase 2 from 12.2 to about 16.6 agent-days. Decision D7 sets the M2 minimum.

### 4.3 Later phases (one line each)

| Phase | UI work | Rough days |
|---|---|---|
| M4 land skirmish | Land HUD variant (land faceplate, reinforcement pane, build pads) and land minimap passability colours | 3–4 |
| M3/M4/M6 menus | Main menu, options (audio/video/keyboard/gameplay), skirmish setup, loading screens from `.rc` | 4–6 |
| M3 galactic | `i_galactic_controls` shell, fleets/organize, planet info, production, galactic radar, story/campaign dialogs, save/load | 8–12 |
| M3 encyclopedia | `Encyclopedia_*` tooltips and pages, tech tree and summary screens | 3–5 |
| M6 multiplayer | LAN/internet lobby, staging, chat, IME edit | 4–6 |

## 5. Open owner decisions (file under EAWR-128)

Owner answers (2026-09-26):

| ID | Decision | Follow-up |
|---|---|---|
| D1 | Use the original faces, extracted from the player's own FoC executable by a local script; never committed or shipped; may be swapped later (EAWR-164). | UI-05 EAWR-191 |
| D2 | Decided by mod HUD compatibility: Godot Controls if they can represent how mods draw the HUD, otherwise a custom renderer (EAWR-165). | Mod HUD survey EAWR-192 (blocks UI-06) |
| D3 | A simple HUD for now; first learn how existing mods change the HUD and hook the same data (EAWR-165). | EAWR-192 |
| D4 | B: retail HUD with aspect-correct layout, centred on ultrawide, never stretched. Do not reproduce the base game's ultrawide squashing of models (EAWR-166). | UI-04b EAWR-195 (§3.4), UI-06 |
| D5 | A: English only in Phase 2; the loader stays language-agnostic (EAWR-166). | — |
| D6 | A: OS hardware cursor with frame swapping (EAWR-166). | UI-09 |
| D7 | Open (EAWR-167). | — |

The option table below is kept for reference.

| ID | Question | Options (recommended first) | Pros | Cons |
|---|---|---|---|---|
| D1 | Source of the four EmpireAtWar faces | A: load them at run time from the user's own FoC executable (located by an sfnt signature scan, kept in memory, never written or shipped) | Exact retail look; nothing redistributed | Reads data out of the executable; depends on the pinned build; licence of the embedded faces is unclear |
| | | B: ship an OFL look-alike | Clean licence, cross-platform | Visible difference in every caption |
| | | C: system fonts by name, falling back to Arial or Liberation | Trivial | Wrong look; Linux has no Arial |
| D2 | UI technology | A: Godot Controls with a retail-data adapter (§3.2) | Input, text and IME free; ADR-011 | Some custom leaf widgets |
| | | B: custom canvas renderer | Retail draw model | Weeks of widget work |
| D3 | M2 HUD fidelity target | A: retail layout from data, eye-check next to FoC | One path, no rework | Needs UI-02 to UI-06 first |
| | | B: simple project HUD now, retail skin later | Faster first playable | Throwaway work; two looks |
| D4 | Resolution and aspect policy | A: retail rules exactly (UI-L1 to UI-L4, stretched dialogs) | Parity, simple | Menus stretch on 16:9 and 21:9 |
| | | B: retail HUD, aspect-correct 4:3-area dialogs | Nicer menus | Deviates; more layout code |
| | | C: A plus a user UI-scale option | Accessibility | Extra test matrix |
| D5 | Localisation in P2 | A: English only; the loader stays language-agnostic | Least work | No early check of other languages |
| | | B: all `.dat` languages selectable | Early coverage | Font fallback cases (UI-F3) in scope |
| D6 | Cursor | A: OS hardware cursor with frame swapping | No latency, simple | Size limits, platform quirks |
| | | B: software cursor on the top canvas layer | Exact scale and animation | Frame latency |
| D7 | M2 HUD scope | A: §4.2 rows a–e; f reduced to brackets and health bars; credits, pop and tech hidden; pause/fast-forward shown | Playable and readable | About 4 agent-days over plan |
| | | B: full §4.2 | Closest to FoC | About 4.4 agent-days over plan |
| | | C: EAWR-83 as filed (command bar, cards, minimap only) | Plan holds | No pause/quit, end dialog or tooltips |
