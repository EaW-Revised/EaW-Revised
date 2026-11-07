# GC planetary view: retail gap analysis (2026-10-31)

Scope: what the retail FoC galactic conquest shows once a planet is focused (zoom into a planet on the galactic map) against
what our viewer draws on `main` 8d96eccd. Presentation only; no sim, hash or lockstep change is proposed.

## Method and limits

- **Retail evidence:** the effective FoC `CommandBarComponents.xml` (`<workspace>/eawr-reference/foc-data/effective`, the
  retail base plus patches) for component names, types, textures, offsets and tooltip keys; the existing behaviour
  inventory [`gc-hud-inventory.md`](../gc-hud-inventory.md) (H-rows, GHUD-S/R evidence) and the GC walks for debug-build
  rules. No decompiled output was used for this document, so the two-context rule does not apply.
- **Ours:** a read of `apps/viewer/src/galactic` on `main`. `gc-hud-inventory.md` has a "Viewer" column written on 10-09
  that is badly out of date (the 10-22 ranking already says so); this table replaces it for the focused planet.
- **Not done:** no retail capture was taken and no layout pixel was compared. Layout claims below ("where it sits")
  are XML/model-derived, not seen. Where data cannot answer, the row says so and names the capture it would need.
- **Method for "ours":** grep of the viewer for each retail component name, then a read of the builder or updater.
  Status "done" means a builder and a per-frame updater exist; it does not mean pixel parity (that is the eye check).

## What the focused view is (retail)

Focusing a planet keeps the main command bar and swaps the fleet organization shell to the focused planet: ordinary orbit
fleet cards, the ground-force pads (`Pad_00..09`) and structure slots (`Planet_structure_00..08`, `space_Pad_1/2`), the
two production tabs with build choices and the queue strip, the planet name/allegiance/income header, `g_special_ability`
drop slots, and the encyclopedia/tooltip panel. A button (`advanced_planetary`) opens the **advanced planetary map**
(`map_shell`): a 512x512 `map_window` of the planet surface with an 80-position `build_pad` grid, a 20-slot `land_fleet`
force tray, unit/building capacity readouts and a header, with its own radar (`radar_galactic_adv_map`) and zoom.

## Gap table

Size is lane work to a mergeable presentation PR (S under 1.5 h, M 2 to 5 h, L over 5 h; classes from `docs/estimate.md`).

| # | Item | Retail evidence | Ours | Status | Size |
|---|---|---|---|---|---|
| 1 | Advanced map: `map_window` planet preview and `build_pad00..79` building grid | XML `map_window` (Icon, 512x512, scale 1.25, layer 2), `build_pad00..79`; H034/H036; GHUD-C05 | `build_radar_hud` builds the `map_shell` faceplate, `radar_galactic_adv_map`, `b_zoom_adv_map` and the `planet_bonus` icon only (`galactic_radar_hud.cpp:17`). No `map_window`, no grid | **missing** | L |
| 2 | Advanced map: `land_fleet_00..19` force tray, `unit_pad00..09`, `ground_forces_text` | XML names above; H035; WGUI-41/53..55 | A read-only tray exists but lists landed companies rather than orbiting units; unit pads are missing (GPD-14) | **partial** | M |
| 3 | Advanced map header and capacities: `Planet_name_text`, `Planetary_Income_number`, `Day_Counter_text`, `Allegiance_A`, `unit_cap_*`, `building_cap_*` | XML text components (Arial Bold / EmpireAtWar-Medium 7 pt, red name, green caps); H037 | Header and capacity labels exist; capacities show maxima only, rather than current/max (GPD-13) | **partial** | S |
| 4 | `advanced_planetary` open button (and `b_quick_ref`) in the focused view | XML `advanced_planetary` (TextButton, `advance_build_button.tga`, layer 3); H033/H034 | Advanced mode is reached by wheel state only (`galactic_mode.cpp:1162` help text); no button | **missing** | S |
| 5 | Previous/next planet buttons `b_planet_left`, `b_planet_right` | XML `b_planet_left/right` (swap textures, tooltips `TEXT_TOOLTIP_PREVIOUS/NEXT_BUTTON`); H042 | No reference in the viewer. Double-click/wheel focus works | **missing** | S |
| 6 | Planet info button and dialog (`planet_info`, `IDD_PLANET_INFO`) | XML `planet_info` (TextButton, `i_reinforcement_button` art, `TEXT_BUTTON_PLANET_INFO`); H030; WGUI-46 | No reference in `apps/viewer`; reference pages exist (`reference_pages*.cpp`) but not this dialog | **missing** | M |
| 7 | Weather icon `g_weather` / `weather` and planet weather text (legacy EAWR-653) | XML `g_weather` (Offset -20 -31), `weather` (layer 3), `lt_weather_icon`; `Encyclopedia_Weather_Info/Name` rows `todo` in `statuses-galactic.json` | Not drawn; `planet_bonus` (ability) is. Weather text absent from the encyclopedia | **missing** | S |
| 8 | Planet effect icons `g_bounty_hunter`, `g_smuggler`, `g_smuggled`, `g_corruption_text` | XML icons share `Default_Offset 0 13` with the status row; H027/H062; needs the effect state from the sim | Status row builds conflict/build/corruption-icon entries (`planet_hud.cpp` `planet_status_icons`); bounty/smuggler icons and the corruption text are not built. Sim publishes no effect state (H062 "A active effect missing") | **missing** (viewer) / blocked on a published fact | M |
| 9 | Stealth and raid alternates on the orbit presence icon (`icon_fleet01..03` alternates, `TEXT_STEALTH_FLEET`, `TEXT_RAID_FLEET`) | XML `i_icon_stealth_zoomed.tga`, `i_icon_raid_zoomed.tga`; GHUDL-16; ranking (legacy EAWR-4686) | Stack and best ship image done; GHUDL-16 records stealth/raid overlays "no producer" | **partial** | S |
| 10 | Focused encyclopedia variant (`zoomed_back`, `zoomed_header_text`, `zoomed_*_text`) | XML `zoomed_*` (No_Shell, layer 4, 262x14 header); H097 | `encyclopedia_*` ordinary panel done; no `zoomed_*` use | **missing** (low visibility; the ordinary panel serves) | S |
| 11 | Hero tray, enemy hero markers and respawn dial in the focused view | XML `g_hero`, `g_enemy_hero`, `hero_slot_good/evil00..10`, `hero_hb_*`; H054/H055 | `hero_map_bar.cpp` builds the map-hero bar and `g_hero_icon`; respawn dial and `g_enemy_hero` blink not found (H055 needs the schedule fact) | **partial** | M |
| 12 | Pathfinder slot on orbit fleet cards (`i_button_pathfinder`) | H050, ranking headline 1 | Slot reserved and drawn (`galactic_fleet_hud.cpp`); no command producer (legacy EAWR-2586) | **partial** (producer is sim) | n/a here |
| 13 | Ground production tab, space tab, build choices, queue strip, hammer, price/credits | CB `b_space_tab`, `b_planet_tab`, `b_Create_00..25`, `queue00..09`, `g_build` | `planet_hud.cpp`, `galactic_hud.cpp` (tabs, build palette, scroll controls, queues, activity icon) | **done** | |
| 14 | Planet header, income, level pips, political-control bar, ground sell | `Text_Planet`, `Text_Planetary_Income`, `g_ground/space_level_pips`, `g_political_control`, `g_ground_sell` | `galactic_hud.cpp` (GHUDL-13..15, `g_political_control` at line 824, `g_ground_sell`) | **done** | |
| 15 | Ground pads, structure slots, company drag/drop, launch onto orbit cards/slots, `b_underground` | `Pad_00..09`, `Planet_structure_00..08`, `Slot_00`, `space_Pad_*` | `ground_hud.cpp`, `orbit_hud.cpp`, `galactic_fleet_input.cpp` (GHUD-S68/S69, (legacy EAWR-4688) merged) | **done** | |
| 16 | Orbit fleet cards, stacks, drag/merge/split, overview presence icons | `q1..q3_fleet`, `g_planet_fleet` | `galactic_fleet_hud.cpp`, `fleet_drag.cpp`, `fleet_stacks.hpp`, `orbit_hud.cpp` | **done** (stealth/raid and per-object GUI index aside, row 9) | |
| 17 | Special-action slots (`g_special_ability`) | H053, H066, H070 | Destruction route only (`destruction_hud.cpp`); theft, spy, sabotage, corruption actions have separate dialogs (`corruption_hud`, `black_market_hud`, `hero_action_hud`, `weapon_action_hud`) but no shared drop slot | **partial** (sim-gated, ranking (legacy EAWR-4687)) | L |
| 18 | Selected planet bonus icon and ability encyclopedia | `planet_bonus`, `g_planet_ability` | `galactic_radar_hud.cpp:43`, hover in `galactic_hud.cpp:1483` (GHUD-S95/S96) | **done** | |
| 19 | Planetary radar and zoom | `radar_galactic`, `b_zoom`, `g_radar_blip`, `g_radar_view` | `galactic_radar_hud.cpp` (both radars, blips, view marker, zoom icons) | **done** | |

Rows 13 to 19 were checked by finding a builder and a per-frame updater. The existing `tests/presentation/renderer/test_viewer_galactic_*_runtime.py` files (not individually read for this document)
likely cover rows 13 to 16 and 18 to 19; I found no test for any missing row.

## Ranked list (what a player notices, then cost)

The advanced planetary map is the one visible hole: the shell opens and is empty. Rank is player impact first, size second.

1. **Advanced map header, capacity texts and the open button (rows 3 and 4, S+S, one PR).** Smallest change that stops the
   map shell being an empty frame, and it gives the player a way in. Data is already published: planet name, income,
   fiscal day, allegiance; unit and building capacity come from the planet's published base/slot facts (H037 says the
   filtered capacity values are not published, so the PR draws capacity only where the snapshot has it and otherwise
   leaves the readout blank rather than inventing a number).
2. **Advanced map `map_window` and build-pad grid (rows 1, L).** The core of the screen. Splits into: window and pad grid
   geometry from the XML offsets (M), then structure icons in occupied pads from the observer-filtered ground roster (M).
   Needs one retail frame of an opened map to pin the pad-to-surface placement (H036 "exact placement none"). File an
   owner-capture/rig ticket for that frame now; do not wait on it for items 1, 3 to 6.
3. **Advanced map land-force tray (row 2, M).** Reuses `ground_roster.hpp` and the pad drag code; the roster is already
   built for the focused pads.
4. **Weather icon (row 7, S).** The art, offset and planet weather field exist in the XML and planet data; presentation
   only, no producer needed. Weather text in the encyclopedia (legacy EAWR-653) rides with it.
5. **Previous/next planet buttons (row 5, S).** Pure input plus the existing focus transition; the order is the planet
   list order and the order rule is unverified (capture or the debug-build walk).
6. **Planet info button and dialog (row 6, M).** Static planet reference text from the catalog, plus the existing reference
   page widget; weather, native and map entries as in H030.
7. **Stealth and raid orbit icon alternates (row 9, S).** Needs the stealth/raid flag on the presence view; check
   `fleet_visibility.hpp` before sizing.
8. **Bounty/smuggler/corruption-text icons (row 8, M).** Blocked until the sim publishes effect state; do not start.

Not proposed (sim-owned or already tracked): pathfinder producer (legacy EAWR-2586), special-action slot (ranking (legacy EAWR-4687)), hero respawn
dial (needs the schedule fact, (legacy EAWR-1116)), focused encyclopedia variant (row 10, no visible gain over the ordinary panel).

## Open items needing the owner or a capture

- One retail frame of the opened advanced planetary map on a developed planet (pad placement, header positions, capacity
  colours): owner-capture/rig ticket, retail first.
- Whether previous/next steps through the planet list, the owner's planets only, or the nearby planets: not in the XML.
