# Space skirmish command bar audit

## Scope and evidence

- **HCB-65:** Shared text presentation takes `Font_Name`, `Font_Point_Size` and
  `Text_Color` from the selected command-bar component. `Text_Outline` and
  `Text_Emboss` select its outline and emboss style where that presenter supports
  them. This covers the existing shell text models and tactical HUD text/font
  construction, including the encyclopedia and native Resume button; it does
  not establish that every authored component is active. Evidence: effective
  command-bar XML (X), existing consumers in `hud_shell.cpp` and
  `tactical_hud_build.cpp`, and HCB-19/31/64's presentation scope.

Audit dated 2026-10-09, against `f58f8f3d6b09f71f74de1c0a4305800f2f8a815c`.
The effective `CommandBarComponents.xml` contains 846 named components. This is an
inventory of controls and their consumers, not a claim that every authored field
is applied. Numbered families below include every member of the stated range.
`implemented` means the control has a working presentation and action/data path;
`partial` means a related path exists but the row names a remaining gap;
`missing` means there is no live presenter/action; `not-in-space-skirmish` names
an excluded mode or unsupported content. Unknown retail activation stays explicit.

Evidence references:

- **X**: effective XML component names, types, tooltip keys and authored art.
- **R1**: inspected retail Coruscant still, `ability-icon-probe/retail-corvette-selected.png`.
  Six command icons in two columns; help, holocron, pause/speed, reinforcement,
  retreat, movie-camera, planet name, credits, population, minimap, selected card,
  card ability and encyclopedia are visible.
- **R2**: inspected retail still, `ability-icon-probe/retail-nebulon-defend-on-hovered.png`.
  Empty selection disables the six commands; world health/ability marks remain.
- **R3**: retail Coruscant space-skirmish capture on 2026-10-09, highest graphics,
  fog enabled: the tech label is absent next to the population readout.
- **O**: debug-build evidence recorded in [space orders](space-orders.md), OR-01..17.
- **W**: debug-build and retail evidence in [world UI](foc-battle-world-ui.md), WU rules.
- **S**: debug-build and retail evidence in [selection](foc-battle-selection.md), S/O/P/V rules.
- **P**: debug-build evidence in [purchasing](space-purchasing.md), PU rules.
- **B**: debug-build evidence in [battle flow](walks/battle-flow.md), WBF rules.
- **D1**: 2026-10-09 read-only debug-build status-component audit: bracket
  creation, marker predicates, toggle admission, bounty award and flytext
  projection/age/alpha; checked against effective unit and component XML.
- **R4**: inspected stock Coruscant space-skirmish action captures, 2026-10-09.
  Movie mode hides the HUD and frames live fighters with letterbox bands; Escape
  restores the tactical view. The retreat control is dark and its click has no effect.
  The enabled beacon button arms point placement; the world click produces a blue
  world marker and minimap cue, which are absent in the later capture. This was a
  single-player capture; ally and enemy admission are sourced from the debug build.
- **R5**: inspected stock Coruscant space-skirmish action captures, 2026-10-10,
  Highest graphics, fog enabled, local Rebel versus Empire AI, empty selection.
  The help control was clicked twice; neither the immediate click images nor
  the settled captures show a help panel or text. The holocron click shows only
  the ordinary **Mission Holocron** hover tooltip, with no objective panel or list.
  Raw click receipts confirm input was sent to the owned retail game window.

Retail stills and raw lookup receipts stay in ignored capture/research storage;
the evidence index accompanying this audit records their exact locations.
X alone establishes authoring, not that a control is enabled in a retail match.
Rows without a retail observation identify that limit instead of treating an
unseen control as proved absent.

## Command buttons

| Rule | Component / action | Status | Our consumer or gap | Evidence |
|---|---|---|---|---|
| HCB-01 | `c_button00`: Attack, A | implemented | `hud_shell.cpp` order buttons; `tactical_hud_build.cpp`; `BattleInput::command_click`; `OrderInput::world_command`; `AttackPayload` | X, R1/R2, O OR-01/02, S O-1..5 |
| HCB-02 | `c_button01`: Attack Move, T | implemented | Same presenter/input chain; `AttackMovePayload`; ships retain their path, craft may divert | X, R1/R2, O OR-10..13 |
| HCB-03 | `c_button02`: Move, M | implemented | Same chain; `MovePayload`; explicit Move can target a point over a hostile unit | X, R1/R2, S O-3 |
| HCB-04 | `c_button03`: Waypoint, no default key; Alt queue modifier | implemented | Sixth authored slot, persistent point-placement mode, cursor/cue, retained move queue and repeat segment; retained coordinator membership includes unselected units and OF-05 draws the selected owner's path/flags; sourced phase in HCB-59 | X, R1/R2, debug build; HCB-57..60 |
| HCB-05 | `c_button04`: Full Stop, S | implemented | Same chain; immediate `StopPayload` | X, R1/R2, S O-4 |
| HCB-06 | `c_button05`: Guard, G | implemented | Same chain; `GuardPayload`; Ctrl+Alt shortcut | X, R1/R2, O OR-14..17 |
| HCB-07 | Requested Patrol / loop / return | partial | No separate button or default binding; Waypoint closes a repeating segment when a new point is within 80 planar units of an older queued destination | X, R1, debug build; HCB-58 |
| HCB-08 | Scatter / selectable formation / stance | not-in-space-skirmish | No such command in X or the six-icon retail strip. Formation spacing, fighter diversion and ability switches are separate mechanisms, not evidence for additional buttons | X, R1/R2; broader retail availability unverified |

The six slots share XML normal/hover/selected/disabled art and `Click_Shift`.
The six controls use their authored shell anchors; Waypoint fills its existing gap without compacting or moving the other slots. No queue
must be simulated by issuing later client-only moves: it must survive replay,
checkpoint, synchronization and selection changes in the simulation.

## Tactical shell, panels and overlays

Paths in this table are relative to `src/presentation/ui/`,
`src/presentation/godot/ui/`, or `apps/viewer/src/` as applicable.

| Rule | Components | Status | Our consumer / remaining work | Evidence |
|---|---|---|---|---|
| HCB-09 | `i_main_skirmish` | implemented | `hud_shell.cpp`, `tactical_hud_shell.cpp`: faction shell meshes, anchors and reference scaling | X, R1/R2, S V-5b |
| HCB-10 | `b_option_t` | implemented | `tactical_hud_build.cpp` options button; viewer options/menu callback | X, R1/R2 |
| HCB-11 | `b_camera_t` | partial | Shared `HudShell` / `TacticalHud` button, authored letterbox, local moving camera and Escape restoration; project orbit stands in for the retail shot director and reset-before-movie behavior | X, R1/R4; HCB-11a..11d |
| HCB-12 | `b_beacon_t`, `b_special_weapon2` | partial | `b_special_weapon2` arms point placement through the ordinary human command sink; replayed per-issuer replacement/expiry, allied authored world model, minimap icon and SFX. The button keeps its blank and icon under the hover, selected and disabled art (HCB-12g). `b_beacon_t` has no shell anchor to draw at (HCB-12h). The authored radar-event model animation remains unwired | X, R4; HCB-12a..12h |
| HCB-13 | `b_special_weapon` | implemented | `hud_shell.cpp`, `tactical_hud_build.cpp`, `battle_input_special.cpp`: faction special weapon slot and targeted command | X; debug-build WSW rules in [weapons](walks/weapons.md); not exercised in R1 |
| HCB-14 | `b_reinforcement` | implemented | `production_view.cpp`: pane opening, admission/empty-pool state and notification flash | X, R1/R2, P PU-70/71 |
| HCB-15 | `b_retreat` | partial | `hud_shell.cpp`, `tactical_hud_build.cpp`: the authored slot, blank, first icon alternate, hover/selected/disabled art and lockout dial, disabled by default; skirmish never enables it (HCB-15a). `retreat_button.hpp` and `galactic_retreat.cpp` drive it from the campaign's published retreat view (HCB-15b). Negative-feedback and refusal-reason sounds remain unwired | X, R1/R4, owner scope decision; HCB-15a/15b |
| HCB-16 | `b_droid_help_tactical`, `text_tactical_help`, `help_back` | not-in-space-skirmish | In the observed stock standalone skirmish, clicking the visible help control opens no panel or text. Campaign/tutorial help admission remains separate | X, R1/R2, R5; HCB-16a |
| HCB-17 | `b_story_arc_t`, `objective_back`, `objective_header_text`, `objective_text`, `objective_icon` | not-in-space-skirmish | In the observed stock standalone skirmish, clicking the holocron opens no objective panel/list; its hover tooltip is not an objective. Authored story objectives remain separate | X, R1/R2, R5; HCB-17a |
| HCB-18 | `b_play_pause_t`, `b_fast_forward_t` | implemented | `tactical_hud_build.cpp`, time callbacks in `map_mode_hud.cpp` | X, R1/R2; battle time rules |
| HCB-19 | `Text_Planet_tactical` | implemented | `hud_shell.cpp` planet-name style and `tactical_hud` text setter | X, R1/R2 |
| HCB-20 | `Text_Credits_tactical` | implemented | `hud_shell.cpp`, `production_view.cpp`; live economy text in `map_mode_hud.cpp` | X, R1/R2, P PU-65 |
| HCB-21 | `Text_Pop_Cap` | missing | Main shell readout under time buttons absent. `r_pop_text` and economy population/cap are available but only shown in the reinforcement pane | X, R1/R2, P PU-21/67 |
| HCB-22 | `Text_Tactical_Tech` | implemented | Shared XML-authored text model and presenter; hidden in multiplayer space under HCB-61 | X, R3, debug build; HCB-61 |
| HCB-23 | `radar`, `radar_blip` | implemented | `minimap_view.cpp`, `tactical_hud_build.cpp`, blip projection in `map_mode_hud.cpp`; click look/move and warning marker | X, R1/R2; minimap behavior note |
| HCB-24 | `s_select_00..23`, `s_health_00..23`, `s_shield_00..23` | implemented | `hud_shell.cpp`, `unit_cards_view.cpp`, `battle_input_cards.cpp`: cards, counts, health/shields, selection | X, R1, S P rules |
| HCB-25 | `special_border_00..11`, `special_button_00..23` | implemented | `hud_shell.cpp`, `ability_buttons_view.cpp`, `battle_input_cards.cpp`: selection ability presentation and activation | X, R1/R2; ability UI rules |
| HCB-26 | `tqueue00..09` | implemented | `hud_shell.cpp`, `production_view.cpp`: production progress/cancel by queue identity | X; P PU-63; queue absent from R1 |
| HCB-27 | `i_build_buttons`, `Build_00..05`, `tactical_sell` | partial | `unit_cards_view.cpp`, `battle_input_production.cpp` and pad sale command implement build-pad actions; not a generic authored tactical popup/sell presenter for all objects | X; build-pad walk; not exercised in R1 |
| HCB-28 | `i_main_reinforce`, `r_close`, `r_0000..0403` (five rows, four columns), `r_pop_icon`, `r_pop_text` | implemented | `reinforce_pane` in `hud_shell.cpp`, `production_view.cpp`: reserve cards, close/drag and pane population | X, P PU-66..68; pane not opened in R1 |
| HCB-29 | `reinforcement_counter`, `garrison_respawn_counter` | implemented / land caller excluded | Shared XML-authored independent top-left timer indicators under HCB-62; live space free-garrison deadline publication. Reinforcement timer is a land transport cooldown, with its land caller outside space scope | X, debug build; HCB-62, WSL-24/29; counters not exercised in R1 |
| HCB-30 | `skirmish_upgrade` | partial | The retained tactical research dock binds the authored frame variants, build dial, scale, font and text styling; inherited sidebar flags gate pending/completed allied upgrade rows. Interdictor/corruption overlays remain deferred | X; production rules; tactical retail capture |
| HCB-31 | `pause_shell`, `text_attack`, `attack_button` | implemented | `battle_overlay.cpp`, `tactical_hud_build.cpp`: native pause shell and Resume action | X; battle time rules; pause not opened in R1 |
| HCB-32 | `Movie_tactical` | partial | Shared `movie_player.cpp` and gallery movie slot exist; no live tactical event/button routing through this HUD | X; live playback not observed in R1 |
| HCB-33 | `tutorial_text_back`, `tutorial_text`, `gui_dialog_tooltip`, `target_type_back` | missing | No generic tactical tutorial/dialog-tooltip/target-type panel consumer; general tooltip applicability must be captured before styling | X; not exercised in R1/R2 |

## Movie camera and retreat evidence

| Rule | Observed behavior | Evidence |
|---|---|---|
| HCB-11a | The movie-camera button activates automatic close-up shots of live battle objects. The tactical HUD and world selection markers disappear while letterbox bands are drawn; the battle continues. This is a local presentation mode, separate from tactical overview and pause. | R4; debug build, movie-mode initialization and letterbox activation |
| HCB-11b | Escape leaves movie mode and restores the tactical HUD and camera. A Space press in the observed movie mode leaves the mode active; it must not be treated as an exit command on that evidence. | R4 |
| HCB-11c | `Camera_FX_Manager_Letterbox_Height` controls each band's fraction of the screen height. Letterbox activation disables the game-mode GUI; the stock value is `0.085f`. The movie button can reset a tactical camera that differs from its defaults before attempting movie activation. | debug build, letterbox activation and camera button; effective constants XML |
| HCB-11d | Project presentation-only fallback: follow the first own visible object, or the first visible object when none is own, with a gentle orbit. Radius is half the tactical camera's planar offset, bounded below by four near-plane lengths; height is one fifth of that radius and yaw advances 0.1 radians per second. Freeze and retain the tactical frame, cancel held inputs, hide tactical controls/world marks, and consume Escape to restore it. End the mode when its subject disappears. Retail interesting-object selection, shot cuts and reset-before-movie remain fidelity follow-ups; this is not a reconstructed retail director. | project policy, permitted presentation-only fallback; HCB-11a..11c |
| HCB-15a | No retreat action is offered in the observed stock standalone space skirmish: the button is disabled and its click has no effect. This observation does not qualify campaign retreat, a multiplayer surrender flow or every authored scenario. | R4 |
| HCB-15b | The space command-bar update enables `b_retreat` when the tutorial permits retreat, the local player is a conflict participant with a playable faction, the battle is not multiplayer tactical and no victory is pending; during the opening lockout it is disabled and its build dial shows one minus the remaining share of `Space_Retreat_Allowed_Countdown_Seconds`, otherwise the dial is full. Its press toggles: a local retreat underway is cancelled, otherwise retreat starts directly or through the retreat dialog (WAR-65). Starting blinks the button; the blink stops when no retreat is underway. A local forced countdown keeps the enabled blink, but its press queues neither cancellation nor a new start: the cancellation request returns without an event for a forced retreat (WR-74). A press while disabled plays the GUI negative-feedback event; a refused start plays the faction's not-allowed or reason event. Prevention and superweapon refusals therefore leave the button enabled. Project choices: the blink uses the selected art with the pause button's additive half-second flash, and our escape phase disables the button. Retail appearance of a campaign countdown remains a capture qualification (GHUD-C14). | debug build, tactical space command-bar update, retreat eligibility, retreat toggle, cancellation request and start; X |

## Help and objectives evidence

| Rule | Observed behavior | Evidence |
|---|---|---|
| HCB-16a | In the observed stock standalone Coruscant space skirmish with empty selection, clicking the help control opens no help panel or text. A second click also leaves the tactical view without a help panel; no panel-closing interaction is established because none opened. The XML-authored help art and text components alone do not establish tutorial, campaign or custom-scenario admission. | R5; X |
| HCB-17a | In the same skirmish, clicking the holocron opens no objective panel or objective list. Hovering it displays **Mission Holocron**, an ordinary tooltip rather than objective text. No generic skirmish objective wording or panel-closing interaction is established. Campaign and authored story-objective admission remain outside this observation. | R5; X |

The effective XML authors separate help and objective backgrounds, text styles
and objective-state icons. Their presence does not require showing an empty
panel or inventing a destroy-enemies objective in this skirmish. The observed
controls need no new panel presenter for standalone space skirmish; reusable
campaign/tutorial panels require evidence from those modes.

## Player beacon evidence

The combined retail session establishes standalone button activation. Debug-build
observations qualify ally visibility, replacement and lifetime. Activation
and expiry service ordering are separate questions; no service cadence is inferred
from the authored lifetime.

| Rule | Observed behavior | Evidence |
|---|---|---|
| HCB-12a | The beacon button activates point placement when its placement-delay gate is clear. Clicking it again cancels placement and clears the gate. Activation initializes that gate from `BeaconPlaceDelay`; this is not the beacon's lifetime. | debug build, button activation and mode setter |
| HCB-12b | Executing a placement removes the issuer's existing multiplayer beacon objects before creating the new faction beacon at the transmitted position and facing. Different issuers retain independent beacons. | debug build, placement execution |
| HCB-12c | Beacon visibility uses the owner's ally relationship to the observer. Placement sound and the authored radar event are also admitted for the local observer only when allied to the beacon. Enemy observers do not receive those visual/audio cues. | debug build, visibility admission and placement execution |
| HCB-12d | The deletion deadline is the current logical frame plus `Beacon_Lifetime_In_Secs` multiplied by logical frames per second and rounded to the nearest integer. Stock beacon authoring uses 10 seconds; replacements obtain a new lifetime. | debug build, deadline calculation; effective beacon XML |
| HCB-12e | The stock standalone button enables placement without a selected unit. The world click produces the faction beacon model and a minimap cue. The later capture no longer shows either marker. | R4; no two-human retail observation |
| HCB-12f | Project integration: the human command carries only a fixed-point position and no unit list. Agreed setup stores each issuer's resolved faction beacon type and rounded lifetime. Ordered command application replaces that issuer's beacon; expiry compares the agreed command tick with its stored deadline. State and metadata are bounded by the player limit, with no entity-service scan. Local activation uses the authored millisecond delay on the presentation clock; right click or Escape cancels placement as a project input policy. World/minimap/audio admission uses allied snapshots even outside sensor coverage. | project protocol and input policy; HCB-12a..12e; exact retail expiry-service order is not claimed |
| HCB-12g | The beacon button is a Button: its `Blank_Texture_Name` backing, its `Icon_Texture_Name` ping icon, then the hover, selected and disabled art, drawn in that order over the icon like the ability and reinforcement buttons (AB-07, PU-70, TM-14). The stock hover and selected art (`i_button_heros_over.tga`) is a translucent frame and the stock disabled art (`i_button_disabled.tga`) a translucent dim, so the icon shows through each state, including the placement-delay window after a placement (HCB-12a). The special weapon button and the order buttons draw the same way. A component without `Selected_Alpha` (the order buttons) adds its selected art instead of blending it, as the reinforcement button does (PU-70), so its icon stays visible while the mode is armed; that armed look of an order button has no retail still yet (unverified). | debug build, button draw order (AB-07, TM-14); effective command-bar XML and its texture atlas alpha |
| HCB-12h | `b_beacon_t` is a text button with an empty `Icon_Texture_Name` and the minimap button's state art. Its activation is the same radar-ping click as `b_special_weapon2`'s, and no shell model has an anchor for it, so retail draws nothing for it over `b_special_weapon2`. It stays unwired. A mod shell that anchors it is not supported yet (fidelity list). | debug build, component logic table (both names run the same click); effective shell models and command-bar XML |

`Multiplayer_Beacon_Type` supplies the faction's object type. Its authored
`Space_Model_Name`, `Radar_Icon_Name`, `Radar_Icon_Size`,
`Beacon_Radar_Map_Event_Name` and `SFXEvent_Beacon_Placed` describe the separate
world, minimap and audio cues. The existing population, minimap and SFX presenters
apply the authored model, icon, size and sound. `Beacon_Radar_Map_Event_Name` remains
a fidelity follow-up: the authored radar-event model animation has no presenter.
No facing is transmitted by the ordinary point-placement adapter; the beacon keeps
its default facing. The empty-icon `b_beacon_t` component has no shell anchor and stays unbound (HCB-12h).

HCB-12f wire format: coordinator-allocated order kind 24 uses replay opcode 29,
three signed 64-bit fixed-point coordinates and an empty unit list. Opt-in header
extension 9 stores version 1, a 32-bit row count and sorted rows of player ID
(32-bit), lifetime frames (32-bit) and type ID (64-bit). The canonical `BECN`
block appears only with active beacons: version 1, 32-bit count, then sorted
rows of player ID, reserved zero (both 32-bit), type ID, three fixed-point
coordinates, placement tick, expiry tick and sequence (all 64-bit). Unavailable
rules reject with reason 20. No beacon-specific event kind is required.

## Tooltips and world controls

| Rule | Components | Status | Our consumer / remaining work | Evidence |
|---|---|---|---|---|
| HCB-34 | `tooltip_back`, `tooltip_name`, `tooltip_price`, `tooltip_icon`, `tooltip_left_text` | partial | `HudShellButton::tooltip` retains only the first key, without a generic runtime presenter. Unit cards and hardpoint tooltips have dedicated paths; command/build/world-unit tooltips need shared coverage | X, R1, W WU-51/52; generic command hover not captured |
| HCB-35 | `encyclopedia_back`, `encyclopedia_header_text`, `encyclopedia_text`, `encyclopedia_icon`, `encyclopedia_right_text`, `encyclopedia_center_text`, `encyclopedia_cost_text` | partial | `tactical_hud_build.cpp`, `unit_cards_view.cpp`, `unit_cards.cpp`: delayed card popup with name/description/class/matchups. Some fields/placement are bespoke; not a universal world/build/button popup | X, R1, S P rules |
| HCB-36 | `zoomed_back`, `zoomed_header_text`, `zoomed_text`, `zoomed_right_text`, `zoomed_center_text`, `zoomed_cost_text` | missing | No expanded encyclopedia presenter; popup activation needs retail observation | X; not exercised in R1/R2 |
| HCB-37 | `st_grab_bar`, `st_health_bar` | partial | `world_ui_view.cpp`, `world_ui_bars.cpp`, `world_ui_prepare.cpp`: squadron icon/group text/health. Some world art/layout uses WU settings rather than every component field | X, W WU-23; R2 world marks |
| HCB-38 | `st_bracket_small`, `st_bracket_medium`, `st_bracket_large` | implemented | `world_ui_bars.cpp`, `world_ui_view.cpp`: selected/hover brackets by world UI rules | X, W WU rules |
| HCB-39 | `st_health`, `st_health_medium`, `st_health_large`, `st_shields`, `st_shields_medium`, `st_shields_large` | partial | `world_ui_bars.cpp` shows loaded-world-rule bars; not all XML overlay/animation flags applied | X, R1/R2, W WU-10..15 |
| HCB-40 | `st_control_group`, `st_ability_icon` | implemented | `world_ui_prepare.cpp`, `world_ui_bars.cpp`: group fonts and active-ability marks | X, R2, W WU-43/44 |
| HCB-41 | `st_garrison_icon` | implemented | `world_ui_bars.cpp`: garrison flag with WU-37..40 provenance | X, W WU-37..40; R1 has no inspected garrison |
| HCB-42 | `st_hero_icon`, `st_hero_health` | missing | No separate hero world portrait/health component consumer; ordinary unit/card/ability marks do not implement these components | X; hero behavior not exercised in R1/R2 |
| HCB-43 | `st_power`, `garrison_slot_icon` | not-in-space-skirmish | Power template has no lookup consumer in the inspected debug-build component callers; garrison slots are occupied land transport/bunker slots, not carrier reserves | X, D1, HCB-63 |
| HCB-44 | `good_hero_frame`, `evil_hero_frame`, `hero_slot_good00..10`, `hero_slot_evil00..10`, `hero_hb_good00..10`, `hero_hb_evil00..10` | partial | `hud_shell.cpp#hero_frame`, `hero_tray.cpp#collect_heroes`, `hero_tray_view.cpp`: each side is its own centred manual shell (`i_main_hero_frame_*`, art variant per hero count), slots draw ring, portrait and health bar. Placement (retail capture 10-20: local team top right, opposing top left), order, fog and overflow are `HeroTrayPolicy`/`Placement` values pending the retail capture (U-12); clicks are a later change (WHE-47/48) | X; hero-rich retail still required |
| HCB-45 | `generic_collision` | implemented: nonvisual hit fallback | Existing shared shell alpha/component hit mask consumes anonymous HUD-art hits (UI-I2); this empty Icon template is an input sentinel, not collision feedback art | X, D1, HCB-63 |
| HCB-45 | `generic_flytext` | partial | Shared XML readout presenter, logical-age view and gallery probe; real bounty awards await their accounting/caller feature | X, D1, HCB-64 |
| HCB-45 | `bribed_icon`, `surface_mod_icon`, `remote_bomb_icon` | not-in-space-skirmish | Bracket predicates are shared, but stock activation comes from land bribe, terrain surface effects and saboteur bombs | X, D1, HCB-63 |
| HCB-46 | `pb_switch`, `pb_switch_recharge`, `sl_switch`, `sl_switch_recharge` | not-in-space-skirmish | Bombardment requires land mode; super laser requires space without tactical production (campaign) | X, D1, HCB-63 |
| HCB-47 | `deathstar_switch`, `DS_Countdown_Timer` | not-in-space-skirmish | Campaign superweapon controls, not ordinary space skirmish content; no presenter | X tooltip keys; campaign retail activation not exercised |
| HCB-48 | `lt_weather_icon`, `tooltip_icon_land` | not-in-space-skirmish | Land weather/land tooltip art; no space consumer | X |

| HCB-56 | `cs_ability_button`, `cs_ability_text` | missing | No generic world ability button/caption component presenter; space activation unverified | X; debug admission/capture required |

## Other XML components

The remaining names are excluded from this space-skirmish audit, not declared
implemented elsewhere. The exclusion index below lists exact singleton names and
numbered families from X so a component is not silently missed. GC implementations
must be checked on their own branch; absence here is not a GC verdict.

<!-- HCB-49 exclusion index is generated from the audited XML name set. -->

| Rule | Excluded component(s) | Status / mode |
|---|---|---|
| HCB-49 | `advanced_planetary` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Allegiance_A` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `attack_button_single` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `autoresolve_shell` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `b_camera_g` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_Create_00..b_Create_25` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_dial` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_droid_help` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_fast_forward` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_filters0..b_filters3` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_option_g` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_planet_left` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_planet_right` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_planet_tab` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_play_pause` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_quick_ref` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_scroll_left` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_scroll_right` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_space_tab` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_story_arc_g` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_underground` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_zoom` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `b_zoom_adv_map` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `back_button` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `balance_pip` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_0000`, `bm_0001`, `bm_0100`, `bm_0101`, `bm_0200`, `bm_0201`, `bm_0300`, `bm_0301`, `bm_0400`, `bm_0401` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_close` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_frame` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_tech_level` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_text_steal` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_title` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_title01` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bm_title_000`, `bm_title_001`, `bm_title_100`, `bm_title_101`, `bm_title_200`, `bm_title_201`, `bm_title_300`, `bm_title_301`, `bm_title_400`, `bm_title_401`, `bm_title_4010`, `bm_title_4011`, `bm_title_402`, `bm_title_403`, `bm_title_404`, `bm_title_405`, `bm_title_406`, `bm_title_407`, `bm_title_408`, `bm_title_409` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `bribe_display` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `build_pad00..build_pad79` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `building_cap_number` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `building_cap_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `c_filters0..c_filters3` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `c_option_g` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `choice_button_left` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `choice_button_right` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `corrupt_button_icon_a` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_button_icon_b` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_button_icon_c` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_close` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_cost_a` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_cost_b` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_cost_c` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_icon_00a`, `corrupt_icon_01a`, `corrupt_icon_02a`, `corrupt_icon_03a`, `corrupt_icon_04a`, `corrupt_icon_05a`, `corrupt_icon_06a`, `corrupt_icon_07a` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_icon_00b`, `corrupt_icon_01b`, `corrupt_icon_02b`, `corrupt_icon_03b`, `corrupt_icon_04b`, `corrupt_icon_05b`, `corrupt_icon_06b`, `corrupt_icon_07b` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_icon_00c`, `corrupt_icon_01c`, `corrupt_icon_02c`, `corrupt_icon_03c`, `corrupt_icon_04c`, `corrupt_icon_05c`, `corrupt_icon_06c`, `corrupt_icon_07c` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_planet_defiler_00`, `corrupt_planet_defiler_01` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_planet_title00`, `corrupt_planet_title01` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_text00a`, `corrupt_text01a`, `corrupt_text02a`, `corrupt_text03a`, `corrupt_text04a` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_text00b`, `corrupt_text01b`, `corrupt_text02b`, `corrupt_text03b`, `corrupt_text04b` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_text00c`, `corrupt_text01c`, `corrupt_text02c`, `corrupt_text03c`, `corrupt_text04c` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_title_a` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_title_b` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corrupt_title_c` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `corruption_shell` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Day_Counter_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `drage_drop_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_bounty_hunter` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_build` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_conflict` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_corruption_icon` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_corruption_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_credit_bar` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_enemy_hero` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_ground_icon` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_ground_level` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_ground_level_pips` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_ground_sell` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_hero` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_hero_icon` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_ability` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_fleet` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_land_forces` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_name` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_ring` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_planet_value` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_political_control` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_radar_blip` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_radar_view` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_smuggled` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_smuggler` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_space_icon` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_space_level` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_space_level_pips` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_special_ability` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `g_weather` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `graph_left` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `graph_left_resolve` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `graph_right` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `graph_right_resolve` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `ground_forces_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `h_0000`, `h_0001`, `h_0100`, `h_0101`, `h_0200`, `h_0201`, `h_0300`, `h_0301`, `h_0400`, `h_0401`, `h_0500`, `h_0501` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `h_close` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `h_name_00..h_name_11` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `h_price_00..h_price_11` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `h_title` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `hack_close` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `hack_cost` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `hack_hack` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `hack_shell` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `hack_text_00..hack_text_06` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `hack_title01` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `i_blackmarket_encycl` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `i_corrupt_encycl` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `i_main_commandbar` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `i_main_organize` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `i_planet_image` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `icon_fleet01`, `icon_fleet02`, `icon_fleet03` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `land_art_ALT0` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `land_fleet_00..land_fleet_19` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `map_overlay_shell` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `map_shell` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `map_window` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `market_00..market_04` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `market_close` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `market_frame` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `market_tech_level00..market_tech_level04` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `market_title00..market_title13` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Movie_galactic` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Pad_00..Pad_09` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `pending_battle_shell` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `planet_bonus` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `planet_info` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Planet_name_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Planet_structure_00..Planet_structure_08` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Planetary_Income_number` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `q1_0000`, `q1_0001`, `q1_0002`, `q1_0003`, `q1_0004`, `q1_0005`, `q1_0006`, `q1_0100`, `q1_0101`, `q1_0102`, `q1_0103`, `q1_0104`, `q1_0105`, `q1_0106`, `q1_0200`, `q1_0201`, `q1_0202`, `q1_0203`, `q1_0204`, `q1_0205`, `q1_0206`, `q1_0300`, `q1_0301`, `q1_0302`, `q1_0303`, `q1_0304`, `q1_0305`, `q1_0306`, `q1_0400`, `q1_0401`, `q1_0402`, `q1_0403`, `q1_0404`, `q1_0405`, `q1_0406`, `q2_0000`, `q2_0001`, `q2_0002`, `q2_0003`, `q2_0004`, `q2_0005`, `q2_0006`, `q2_0100`, `q2_0101`, `q2_0102`, `q2_0103`, `q2_0104`, `q2_0105`, `q2_0106`, `q2_0200`, `q2_0201`, `q2_0202`, `q2_0203`, `q2_0204`, `q2_0205`, `q2_0206`, `q2_0300`, `q2_0301`, `q2_0302`, `q2_0303`, `q2_0304`, `q2_0305`, `q2_0306`, `q2_0400`, `q2_0401`, `q2_0402`, `q2_0403`, `q2_0404`, `q2_0405`, `q2_0406`, `q3_0000`, `q3_0001`, `q3_0002`, `q3_0003`, `q3_0004`, `q3_0005`, `q3_0006`, `q3_0100`, `q3_0101`, `q3_0102`, `q3_0103`, `q3_0104`, `q3_0105`, `q3_0106`, `q3_0200`, `q3_0201`, `q3_0202`, `q3_0203`, `q3_0204`, `q3_0205`, `q3_0206`, `q3_0300`, `q3_0301`, `q3_0302`, `q3_0303`, `q3_0304`, `q3_0305`, `q3_0306`, `q3_0400`, `q3_0401`, `q3_0402`, `q3_0403`, `q3_0404`, `q3_0405`, `q3_0406` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `q1_fleet`, `q2_fleet`, `q3_fleet`, `q4_fleet` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `queue00..queue09` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `radar_galactic` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `radar_galactic_adv_map` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `resolve_button` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `Slot_00` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `space_Pad_02`, `space_Pad_1` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `story_pending_battle_shell` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `Text_adv_help` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Text_Allegiance_A` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `text_attack_choice` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `text_attack_single` | not-in-space-skirmish: campaign pending battle / autoresolve (X) |
| HCB-49 | `Text_Credits` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Text_Day_Counter` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Text_Day_Credits` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `text_fleet1`, `text_fleet2`, `text_fleet3` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `text_galactic_help` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `text_hero` | not-in-space-skirmish: hero recruitment panel (X) |
| HCB-49 | `Text_Land_Fleet` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Text_Planet` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `Text_Planetary_Income` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HUD-POP-01 | `Text_Planetary_Pop` | space-skirmish registered population/cap readout below pause; effective component/bone and paired retail stills, see [unit cards](foc-unit-cards.md) |
| HCB-49 | `Text_Tech_Level` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `unit_cap_number` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `unit_cap_text` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `unit_pad00..unit_pad09` | not-in-space-skirmish: galactic / planetary / campaign (X) |
| HCB-49 | `weather` | not-in-space-skirmish: galactic / planetary / campaign (X) |

## Shared reuse and follow-up boundaries

**HCB-63: status-component applicability.** These are stock effective-XML
activation findings, not a prohibition on a mod defining different content.
The debug-build lookup audit covered the fixed component getter's 179 caller
names (190 overload bodies), with no failed exports. Bracket creation, update
and rendering were inspected together. No retail capture was needed to settle
these code gates.

| Component | Applies in space? | What shows, trigger and evidence |
|---|---|---|
| `st_power` | no | Authored 11-level Bar template. No lookup of this template occurs in the audited callers; bracket creation/update/render uses health and shields, with no power bar. No stock space unit or specialist activation found. X, D1. |
| `garrison_slot_icon` | land-only | One half-scale occupancy dot per `Num_Garrison_Slots`, alternate art for occupied slots computed from available slots; hidden for a death clone. Authored by land bunker, Gargantuan, HAV Juggernaut, Gallofree HTT and F9TZ transport types. Not free-garrison/carrier reserves. X, D1, effective unit XML. |
| `generic_collision` | yes, nonvisual | Empty Icon template returned for an unnamed/unbound shell collision or a tutorial-denied GUI component while HUD input is enabled. No unit/specialist trigger and no authored art. Existing shared UI-I2 hit masks already stop world input over anonymous shell art; tutorial admission belongs to its caller. D1, X; existing `hud_tests.cpp` opaque/transparent and screen-placement contracts. |
| `generic_flytext` | yes, tactical bounty feedback | Signed local credit award at the destroyed unit's retained position. No stock space-skirmish specialist grants its modifier; campaign racketeering and Nal Hutta's authored space upgrade can activate it in space battles. The land palace bounty upgrade is another source. Shared presenter implemented under HCB-64; bounty accounting/callers remain separate. D1; effective bounty/faction/planet XML. |
| `bribed_icon` | land-only | Admitted bracket's tactical-bribed predicate selects the authored bribe icon. The only stock ability holders are land Tyber Zann and his prologue variant; his space ship has no tactical bribe. D1; effective ability-holder XML. |
| `surface_mod_icon` | land-only | Uses the combatant's nonempty terrain modifier icon, falling back to the first team member that has one. Replaces the template's placeholder art with `Surface_Settings/Status_Icon_Name`; stock terrain surface events/holders are land-only and space units author no `SurfaceFX_Name`. D1; effective surface and unit XML. |
| `remote_bomb_icon` | land-only | Shows on an admitted bracket with a remote bomb attached. The stock placement holders are Underworld Saboteur and its team/demo variants, all land content; no stock space placement holder. D1; effective ability-holder XML. |
| `pb_switch` + `pb_switch_recharge` | land-only | Visible only with valid local-player planetary bombardment in land mode. Ready uses alternate switch art and enables it; waiting uses ordinary art, disables it and displays progress to next bombardment. D1, X. |
| `sl_switch` + `sl_switch_recharge` | GC-only (space tactical) | Requires space mode **and no tactical production**, then a local SUPER_LASER holder (Eclipse or Death Star II). Ready uses alternate art/enables the switch; active/recharging disables it, and active forces recharge progress to zero. Death Star presence relocates this action to the first selected-card slot with the ability icon/dial. Hidden in stock space skirmish. D1; effective superweapon unit XML. |

- **HCB-64:** `generic_flytext` is local tactical bounty feedback. A positive
  bounty awarded to the local player produces a signed credit amount at the
  destroyed object's retained target position. The producer checks hostile
  final-blow ownership and the recipient's faction bounty modifier; an ally of
  the killer can receive its own modifier's award. It is not generic damage
  text, bribery feedback or the `generic_collision` component. Stock bounty
  sources are the land `UL_Bounty_Upgrade`, campaign racketeering and the
  `NalHutta_Planetary_Advantage` (authored for both space and land). No named
  stock space-skirmish specialist supplies a bounty modifier in the inspected
  effective XML. The shared presenter accepts prepared text, creation frame
  and a caller-projected retained position; the bounty accounting/caller
  remains a separate feature, tracked by the bounty tag registry.
  The XML authors its font, point size, colour, emboss, outline, text offset
  and justification. Stock uses yellow 7-point medium text with emboss and
  no outline. The text is centred at its projected point by default; authored
  left/right justification overrides that default. Relative logical age
  moves it up **70 screen pixels over 60 frames**, with no distance or UI-scale
  multiplier on that travel. It stays opaque through age 30, then its byte
  alpha falls linearly to zero at age 60; it is removed after age 60.
  Empty text and a projection behind the camera draw nothing. The presenter
  advances no clock and changes no game state. `hud_readout_style` and
  `HudIconTextRenderer` provide the same style/font/text-cell path as HCB-61;
  the flytext adapter adds measured text centring and caller alpha. Evidence:
  D1, effective component XML, bounty upgrade XML and `Space_Upgrade_Type`.

- **HCB-61:** `Text_Tactical_Tech` uses its shell bone, XML font, colour and
  left justification. The displayed value is localized `TEXT_TECH`, a space,
  then the highest displayed technology among allied players of the local
  faction. Multiplayer space hides it; Consortium campaign also hides it.
  Campaign displayed levels include the faction's
  `Displayed_Tech_Level_Adjustment`; multiplayer displayed levels are raw tech.
  The shared presenter accepts the caller's prepared displayed level and mode.
  The current space-skirmish caller supplies its published economy level and
  remains hidden. A future campaign caller must supply the allied maximum and
  authored adjustment. Evidence: debug build, R3, effective XML.
- **HCB-62:** The two `No_Shell` timer components are independent top-left
  indicators, not selected-unit-card overlays or authored carrier reserve counts.
  `garrison_respawn_counter` reads the local player's free-garrison waiting
  timer under WSL-24/29. Committed deadlines and authored duration are copied
  into immutable presentation snapshots at initial and completed-tick publication;
  this metadata is excluded from canonical snapshot bytes, state hashes and replay.
  The viewer filters by local player and derives progress at the presented tick.
  `reinforcement_counter` reads the elapsed fraction of
  a local land transport's reinforcement cooldown, and expires when that delay
  completes; it has no space reserve caller. The timer row starts at 30 reference
  units from the left and 60 from the top. The garrison indicator comes first;
  land transport indicators follow, separated by scaled texture width plus 5
  reference units. Both use their XML icon, blank art, build dial, scale and
  colour. XML blinking fields are retained in the shared style, but these
  inspected timer activation paths do not request blinking. Evidence: debug
  build creation, progress and space/land placement consumers; effective XML.

- **HCB-50:** GC can reuse `CommandBarCatalog`, shell anchors/reference scaling,
  atlas/texture resolution, component button states, `HudShellButton`, font/text
  providers, `EawrHudButton`, and the card/ability/minimap/production presenters.
  Keep the input adapter separate: tactical `TacticalIntent` is not a galactic command.
- **HCB-51:** A shared tooltip view should accept content/style/anchor data from
  callers. Existing card encyclopedia content and hardpoint names are useful inputs;
  neither proves command/build/world-unit hover coverage. Coordinate GC coverage
  with its WGUI and GHUD inventory rather than duplicating a tactical-only popup.
- **HCB-52:** HUD-POP-01 presents the published tactical economy count/cap through
  `population_text` and the authored `Text_Planetary_Pop` component. Effective
  command-bar XML and paired retail stills source the presenter; GC supplies its
  own population source and `Text_Pop_Cap` binding.
- **HCB-53:** Waypoint needs authoritative retained destinations, append/replacement
  admission, completion and cancellation, replay/checkpoint encoding and worker-count
  equality. Attack/guard/stop interactions must be sourced before implementation.
- **HCB-54:** HCB-11a..11d and HCB-12a..12h now source the camera and beacon
  integrations. Remaining fidelity work is retail shot selection/reset behavior,
  the beacon's authored radar-event animation, optional facing and a mod shell that
  anchors `b_beacon_t`. No unsupported retail hotkey is inferred from the art.
- **HCB-55:** HCB-16a/17a establish no help/objective panel activation in the
  observed stock standalone skirmish. Campaign/tutorial admission remains separate.
  Expanded encyclopedia, hero tray and specialized
  world marks each need a bounded activation capture and
  their own consumer. Unverified admission is explicitly retained in HCB-33/36/42..46;
  it must not become a guessed implementation.

The waypoint queue now consumes `MaxWaypointsPerPath`; its registry entry retains the unresolved formation/timing scope.


## Waypoint behavior and project integration

| Rule | Behavior | Evidence / limit |
|---|---|---|
| HCB-57 | Waypoint toggles its mode and clears the other armed command modes. Left world and minimap placement produces queued point movement, including over hostile objects; the mode stays armed. Right release cancels it unless Alt is held. Alt also queues ordinary point movement; Ctrl+Alt guard suppresses queuing. No default keyboard binding is authored. | debug build, effective command bar XML |
| HCB-58 | Admission first checks `MaxWaypointsPerPath` (effective value 6). The constant is read as a C integer (leading digits only, no sign or range check) into 32 bits and the queue count is compared with it as unsigned, so an authored `-1` (Alliance at War) is 4294967295, no practical limit, and `0` accepts no queued point; a fraction keeps its whole part and text without digits reads 0. At capacity it ignores the new destination; below capacity an exact duplicate of the newest is ignored. Search older queued points from newest to oldest: strict planar distance below 80 marks the matched point through the newest for repetition and adds no new point. Otherwise append the destination without replacing the current leg. Repeated completed legs return to the queue's end. Replacement move, attack, attack-move, guard and stop clear queued point movement; accepted queued moves still run ability cancellation. | debug build (constant parse and queue admission); existing order cancellation rules |
| HCB-59 | Project completion seam: partitioned readiness reads the post-movement snapshot, then a stable entity-order reduction advances each ready ship formation or squadron team independently before ordered planning of the next leg. Early completion evidence confirms all-member arrival is serviced after ordinary locomotion and targeting, with same-frame planning and no second movement. FS-37/38 and FC-66/67 define the arrival phase and per-formation boundary. Queue admission visits each existing movement group once, including unselected members; directly selected craft are promoted to their team for queued movement. Units without a group form a new one. Each per-unit route retains the original coordinator command identity, including after its points drain; a replacement movement order detaches its recipients. Ready legs are grouped by coordinator and point identity, without a selection-wide arrival barrier. A retail done-state retry checks its one-second logical-frame deadline, but that is not proof of path-arrival advancement cadence. | debug build; FS-37/38, FC-66/67, FD-61; queued group admission and destination propagation |
| HCB-60 | Project replay format: Move opcodes 2 and 20 accept an optional eight-byte little-endian tail after unit IDs: version 1 then flags 1 (append). Absent tail retains legacy bytes; unsupported values are rejected. Nonempty queues add optional canonical state block `WYPT`: version 1, capacity, sorted unit count, then unit ID, active flag, chronological points with destination, hazard/repeat flags and original player/tick/sequence. Empty queues add no state bytes. These are project formats, not original-game serialization claims. | project design; replay and state compatibility contracts |

WYPT version 2 stores each route's optional coordinator command identity before its
active flag and chronological points. Ordinary coordinated movement retains an
empty route for later subset-selection queue admission; this changes state hashes
without changing ordinary movement. No old-format reader is retained. Snapshot
waypoint points are presentation copies and never enter canonical encoding.
The existing publication pass derives the nonempty-route readiness gate, so
empty coordinator identities do not introduce a per-tick completion scan.
Command admission is exempt from partitioned tick work: its stable member scan runs once per queued command, never per tick.

Retail shows the HCB-30 tactical research dock in space 1v1, team skirmishes and land battles. This viewer adapter currently wires the dock into space battles only; the neutral ledger seam supports land, but land-battle wiring remains a follow-up (legacy EAWR-4419). It forms a single column flush to the right edge, about 60 reference units below the top at reference height 768. It shows local and allied pending research with percentage complete, retains completed research without a percentage, and removes a completed starbase level-up when its authored completion flag is false. Completed rows appear first, in observed completion order, with pending rows in observed start order; cancelling through the production queue removes its pending row. Completed-row hover uses the usual encyclopedia panel, while left and right clicks are inert. WPR-70 through WPR-77 record the retail capture observations; the initial view opened mid-battle seeds a deterministic ledger order because the snapshot has no historical start timestamp.

The captured anchor and 38-reference-unit vertical pitch are presentation policy. The component `Offset` (-19,309) and `Default_Offset` (50,-38) consumer is unknown and these fields are not applied by this dock; identifying that consumer remains follow-up work. Existing authored textures, `Scale`, `Size` icon scale, font and text styling remain changeable through the component catalog.
