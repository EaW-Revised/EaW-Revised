# Main-menu demo attract battle

What FoC runs behind the main menu: a live, AI-only space battle on a stock demo map, cycled
by timer, stopped when the player starts anything else. Rule IDs `MAT-NN`; evidence is the
FoC debug build (`MAT-E` rows), the effective XML and Lua, and one retail capture of the menu.
The remake's own behaviour and gaps are in the last column. Tracked as (legacy EAWR-2937).

## Applicability

- Build: the FoC debug build, effective stock data (`gameconstants.xml`, `markers.xml`, the
  `demo_attract_*.ted` and `front_end_demo_attract.ted` maps, the object script
  `Demo_Attract_Controller`). Mods change every value below through the XML; the engine's own
  fixed choices are called out per rule.
- Not covered: the land demo controllers (`Demo_Land_Controller*`), the campaign demo launch
  of the main menu (a hidden button), the intro movie path.
- Related rules: the music rule SND-71 ([audio walk](walks/audio.md)), the story rule WST-13
  ([GC story walk](walks/gc-story.md)), the input rule WGUI-76
  ([GC UI walk](walks/gc-ui-sensors.md)), the pool rule WPR-23
  ([production walk](walks/production.md)).

## Data

| Tag (class `GameConstants`) | Value type | Stock value | Rule |
|---|---|---|---|
| `Main_Menu_Demo_Attract_Mode` | boolean | `Yes` | MAT-01 |
| `Demo_Attract_Maps` | list of map file names, resolved under `Data/Art/Maps/` | `Demo_Attract_Kamino.ted` (a commented-out alternative lists Kamino and Korriban) | MAT-03 |
| `Demo_Attract_Map_Cycle_Delay_Seconds` | list of seconds, one per map by position | `1200` (commented alternative `1000000, 1000, 1000`) | MAT-04 |
| `Demo_Attract_Start_Timeout_Seconds` | whole seconds | `3600` | MAT-05 |

Four more demo maps ship: `Demo_Attract_Korriban`, `Demo_Attract_Geonosis`,
`Demo_Attract_FocDemo` and `Front_End_Demo_Attract`. Only the first listed map is in use
stock, and the camera, forces and AI of every map come from the map file itself (MAT-08).

## Rules

| ID | Rule | Evidence | Remake |
|---|---|---|---|
| MAT-01 | The battle starts when the main menu activates, the staging screens activate or the Enter key is pressed on the main menu, provided `Main_Menu_Demo_Attract_Mode` is true and the attract is not already running. An attract that already runs is only given the front-end advance check of MAT-04; it is not restarted. The command line can start the attract before any menu. | debug build MAT-E01..E03, E07 | `AttractRunner::start(parent)`, called by `FrontEndMode` on the first menu or staging screen it shows (and again when they come back after a battle); `--eawr-attract` runs it alone |
| MAT-02 | Starting the attract initialises two clocks to now: the last-input time (idle clock) and the map time (time since the current map started). It does not itself load a map; MAT-08 does. | debug build MAT-E01 | same |
| MAT-03 | A start picks the map at index `(current + 1) mod count` of `Demo_Attract_Maps`; the current index starts before the first entry, so the first start uses entry 0. The list must hold one map with a non-empty name; the file is `Data/Art/Maps/` plus that name. | debug build MAT-E04 | same; an empty list means no attract |
| MAT-04 | Once per frame the map advances when the map time is older than the delay for the current index, in seconds. The delay is the entry at the current index of `Demo_Attract_Map_Cycle_Delay_Seconds`, or 120 s when the list is shorter. It runs only while the attract is active, no menu dialog is up and the game is not paused. With a dialog up (the main menu is one), the same test runs through the front-end check, which also needs the idle clock older than 8 s when the main menu is the active dialog (20 s for any other dialog) and the application in the foreground. | debug build MAT-E05, E06 | `AttractManager::service`; the front end says whether a dialog is up, whether it is the main menu and whether the window is in front |
| MAT-05 | Every frame the pause flag is set to "idle clock older than `Demo_Attract_Start_Timeout_Seconds`" and cleared otherwise; so after that long without input the battle freezes, and any input resumes it. The flag also suppresses the MAT-04 advance. | debug build MAT-E05, E10 | same (the runner holds the session paused) |
| MAT-06 | Input refreshes the idle clock: any key, a pointer move while the attract runs (or while it is initialised, not running, and the main menu is up), and a window activation. | debug build MAT-E08..E10 | `AttractRunner::key`, `pointer_moved`, `window_activated`, on the front end's input |
| MAT-07 | With the attract running and no dialog up: Backspace restarts the current map (the index steps back, then the advance steps forward), Enter advances to the next map, Escape stops the attract, minus skips the current map's remaining time (the map time moves back by that delay, or resets to 1). The key is consumed. With the main menu up the keys are not the attract's. | debug build MAT-E08 | same |
| MAT-08 | An advance tears the previous game down, then loads the map as a skirmish-mode tactical game that has no lobby: the player list is reset, HUD pieces, messages, command-bar components, story flags and every active speech, music and sound event are cleared, then the map is read as a saved-game style load with its own object placements. The forces are the map's placed objects, each owned by the editor player its owner index names; nothing is bought and no lobby start applies. The scheduled event queue is reset and its play events begin for the map. | debug build MAT-E04, E11 | `skirmish::placed_fixture` and `build_start` keep the placed forces under the editor players that own them (`UnitRole::placed`); no lobby, no station, no free forces; table types are loaded for the map's own placements |
| MAT-09 | AI control is assigned by editor player index, not by lobby: index 0 gets the player type named `BasicRebel`, index 6 the type `AI_Player_Underworld`; every other player keeps none. In the stock faction order index 0 is the Rebel faction and index 6 the Underworld, and both names equal that faction's `Basic_AI`. Everything else on the map (the Empire's units on the stock map, neutral objects) is uncontrolled. | debug build MAT-E04; XML | `skirmish::attract_ai_players()`: the same two indices and names (the engine's own fixed choice; each is the faction's `Basic_AI` in the stock data); other players run no AI and are not commandable |
| MAT-10 | The camera is the map's saved tactical-camera state: pitch, yaw, look-at point, distance and field of view stored in the map's root camera chunk. The camera controller's own per-frame service does nothing and the world rejects input (WGUI-76), so the view stays at that state for the whole map; it does not follow the fight. Owner 10-11: retail's attract runs FoC's battle camera (the cinematic camera, not built yet; (legacy EAWR-3005)); the static trace above is what the debug build showed, and the placeholder below stands in until then. | debug build MAT-E04, E12 | `skirmish::attract_camera` reads the chunk; the space view draws that frame at the window's aspect and takes no camera input; a map without the chunk keeps the default space view. Placeholder until the battle camera exists: `AttractCamera` (`attract_camera.hpp`, `update(dt, battle view) -> eye and look-at`) starts at that frame, then drifts the look-at toward the centroid of the ships, orbits at about 1 degree a second and lets pitch and distance breathe a little; presentation only, input stays blocked |
| MAT-11 | Colorization (faction tinting of models) is switched off in both object managers for the attract game. | debug build MAT-E04 | placed ships carry no team colour |
| MAT-12 | Every placed object that spawns squadrons has its garrison spawn switched on after the load. | debug build MAT-E04 | placed units start with garrison spawning on, as every map object does outside a lobby station |
| MAT-13 | The attract stops (a game-quit, the audio flags cleared, the demo flag of the game cleared, the idle clock state dropped) when: the player opens Credits from any hub, chooses Exit or presses Escape on the main menu, loads a recording, or presses Start on the skirmish staging screen (the staging screen first deactivates, then stops the attract, then starts the game). It also stops for the debug map and campaign hot keys. | debug build MAT-E13..E16 | `stop()` frees the session; `FrontEndMode` calls it when Exit/Escape quits and when Start is pending on a staging screen (the Credits, load-recording and hot-key paths have no front-end screen yet) |
| MAT-14 | The menu and staging screens keep the attract running. The battle stays visible behind the main menu, single-player menu, Multiplayer selector, single-player skirmish staging, options and load screens. The full LAN and Internet lobby/chat screens hide it with opaque backing. Selector Back and either lobby Cancel return to the main menu with the battle visible and running again. The staging screens start it when enabled and not running (MAT-01). | debug build MAT-E14, E17; owner; release-game capture `out/capture-menu/4428.md` in the `sk-capture-menu` worktree (2026-10-20) | The selector and shell returns clear the backing; LAN and Internet keep their opaque lobby backing. Without a running battle the staging keeps its flat backing; the dialog skin is unchanged. Lobby Cancel resets navigation to main; single-player staging Cancel keeps its menu chain. The GPU walk checks the selector and return frames, ongoing camera updates and the opaque LAN frame. |
| MAT-15 | Under the attract, every destruction of an object returns a type to its owner's skirmish reinforcement pool, at once and in destruction order. The object must not be a garrison unit (a hangar's or free garrison's) and not a team member (a squadron's craft). A team container returns its company type, once, when the team is destroyed. The type must carry `Build_Tab_Space_Units`. The owner must be a player. Nothing about the cause (hull loss, a script, a self-destruct) or the survivors flag matters. Outside the attract nothing is returned (WPR-23). The AI player's free store takes the entry unreserved, so any plan can pick it. | debug build MAT-E18 | does: the placed start carries the loaded content's `Build_Tab_Space_Units` types in its setup record (replay header extension 13, written only for an attract); the tick's destruction hook notes each destroyed object that is no garrison unit and no craft of a team, and the economy phase appends its purchase identity to the owner's pool with a fresh pool identity before that frame's production. A container emptied of craft returns its type once. Riders carried inside a destroyed carrier return nothing (no attract map carries any) |
| MAT-16 | A tactical AI plan's reinforce call normally refuses while the player has no allied star base standing ("AI players cannot reinforce after their starbase has been destroyed"). Under the attract that check is skipped; the reinforcements still come from the player's skirmish pool (MAT-15), the types being the pool entries the plan reserved. | debug build MAT-E19 | does, by absence: this project's reinforce call asks for a pool account and a position only, so the demo battle's players, which now hold accounts, reinforce with no star base. The retail refusal outside the attract is a gap of normal skirmish (fidelity list, not changed here: normal skirmish, LAN and replays stay byte-identical); when it lands it must skip when the setup carries the attract record |
| MAT-17 | Under the attract the end-of-battle callbacks are not run when a victory is pending, and no end screen opens; the battle just keeps running until the next advance. | debug build MAT-E20 | the placed start carries `VictoryCondition::none` |
| MAT-18 | The retail debug-build trace sets 3D sound-event gain to 0 while the attract runs; that gain path leaves 2D events unchanged. The music event manager is flagged, the main-menu music event keeps playing and the tactical ambient music is not started (SND-71). The story mode's event entry drops every story event while the attract runs (none is dispatched or delayed), so the scripted speech and announcement cues that story events drive never fire; the GUI toggle sounds of an ability mode switch and of a countdown ability's expiry are skipped too. No attract check was found in the speech manager itself, so an announcement a game system raises directly is not shown to be gated. The owner's report (10-12) settles the rule for the remake: no battle speech under the attract. A release-executable capture (2026-10-20) measured zero output during sampled large-ship breakups with music at zero; a music-on control was audible. This observation supports a silent demo battle without establishing every retail 2D dispatch path. | debug build MAT-E21; retail release capture 2026-10-20 | no audio bus is muted: menu music, clicks and slider previews share the battle buses. With `attract_silent`, SFX, queued speech and music share one admission policy. Every battle SFX event is refused independent of category or positional flags, production and briefing speech queues take nothing, and the music entry refuses direct summary stings and subsequent files/loops as well as director cues. No tactical music director is built. The attract report counts SFX refusals, admissions and starts in `audio_events`; the separate `speech` and `sfx_3d` blocks count their paths. A speech-class 3D event contributes to both refusal counts; queued speech refusals contribute only to `speech`. A non-positional death counter proves the regression case reached the previously audible path. |
| MAT-19 | Ship names are not allocated, radar events are not raised, the players-list overlay is not drawn, the profile's game speed is not applied and multiplayer frame scheduling is not used. | debug build MAT-E22 | no HUD, loading screen or battle input is built; the session runs at the normal step rate |
| MAT-20 | The map may carry a marker object whose object script (`Demo_Attract_Controller`) runs once: it reveals the whole map for the Empire, Rebel and Underworld players and sends every unit of those players, except a fixed list of types (hyper-velocity gun, ion cannon, the Rebel star base, StarViper, TIE Phantom, TIE Fighter, TIE Bomber, A-Wing, X-Wing, B-Wing), to attack-move to a random one of three `Story_Trigger_Zone_NN` objects of its faction. The stock default map (Kamino) has no such marker; Geonosis has one. | effective Lua and XML; map census | gap: object scripts are not hosted (SOS-03) |
| MAT-21 | The main menu's fade up from black waits while the attract runs and the game frame is below 4, so the first fade frames show the battle's first frame. | debug build MAT-E23 | front end (not the runner's) |

## Stock map content (census)

TED placements by owner index (0 Rebel, 1 Empire, 3 Neutral, 6 Underworld). Counts are
objects in the map, squadrons count one object.

| Map | Rebel | Empire | Underworld | Neutral | Marker |
|---|---|---|---|---|---|
| `Demo_Attract_Kamino` (stock) | 14 | 1 | 16 (with a star base) | 5 asteroid fields, backdrop, moon | none |
| `Demo_Attract_Korriban` | 1 | 15 | 14 (with a star base) | 6 asteroid fields, 1 huge asteroid, backdrop | none |
| `Demo_Attract_Geonosis` | 13 | 13 | 8 | 2 asteroid fields, backdrop, 9 trigger zones | `DEMO_CONTROLLER` |
| `Demo_Attract_FocDemo` | 1 | 8 | 13 (with a star base) | 4 asteroid fields, backdrop, moon | none |
| `Front_End_Demo_Attract` | 0 | 16 (with a star base) | 10 | 6 asteroid fields, 1 huge asteroid, backdrop | none |

## Remake classes and flags

| Piece | Where |
|---|---|
| Settings, map path, manager (clocks, cycle, keys), saved camera, AI names | `include/eawr/skirmish/attract.hpp`, `src/skirmish/attract.cpp` |
| Placed-forces start (`PlacedForces`, `placed_fixture`, `UnitRole::placed`); the pool-return record it sets (`AttractPoolReturn`) | `include/eawr/skirmish/start.hpp`, `src/skirmish/start.cpp`, `src/skirmish/inputs.cpp`, `include/eawr/sim/tactical/replay.hpp` |
| The demo players' pool accounts (no credits, no income) | `src/skirmish/economy.cpp` (`economy_rules`) |
| The pool return (destruction hook, container emptied, economy phase) | `src/sim/tactical/session_step_commands.cpp` (`destruction_event`), `src/sim/tactical/session_step_fighters.cpp`, `src/sim/tactical/session_step_economy.cpp` (`note_pool_return`, `economy`) |
| The battle (`MapMode::Options::attract`: no HUD, loading screen or battle input, muted, saved camera kept alive by `AttractCamera`) | `apps/viewer/src/map_mode*.cpp`, `apps/viewer/src/attract_camera.*`, `space_environment_view.cpp` |
| The runner (`start(parent)`, `process`, `key`, `stop()`) and the `--eawr-attract` mode | `apps/viewer/src/attract_runner.*` |
| Tests | `tests/skirmish/attract_tests.cpp`, `tests/skirmish/skirmish_start_placed_tests.cpp`, `tests/replay/fighter_attract_tests.cpp`, `tests/skirmish/attract_lively_tests.cpp` (installed game), `tests/presentation/renderer/test_attract.py` |

## Unknowns

- Which player the retail attract treats as the local player (fog of war for the view, the
  minimap): no human player exists. The remake draws every unit (presentation reveal) while the
  simulation's own fog still gates the AI's sensors. Marked unverified.
- Whether the retail attract applies the map's own victory conditions; MAT-17 only shows the
  end callbacks are skipped.
- The Empire player on the stock Kamino map owns one ship and no AI controller (MAT-09); the
  remake leaves it uncontrolled and non-commandable, which makes it an inert target at best.
- The credit and population rules of the AI players in the attract (the placed star base's
  build menu). The remake gives the players a pool account with no credits and no income, so no
  player buys anything and the pool is the only source of new units. The population cap is the
  faction's `Space_Tactical_Unit_Cap`, and a reinforcement arrives by hyperspace facing 0 degrees
  (the retail facing without a start marker is unverified).
- The heading of a craft pitched through the vertical when a squadron reinforcement searches for free
  space (PL-08): the remake places that craft's placement box unturned (yaw 0, the angle of a zero
  vector) instead of refusing the reinforcement. The long attract battle reaches it; the retail
  value is unverified.

- Which retail announcements besides the story-driven ones the attract would still raise: the debug
  build shows the story mode dropping its events and the 3D multiplier, and no attract check in
  the speech manager. The remake gates the sound classes directly (MAT-18) because the owner heard a
  hero announcement; whether retail raises none or relies on the demo game having no human seat is
  unverified.

## Gaps

- MAT-20: placed marker scripts are not run.
- The retail refusal of an AI reinforce without an allied star base (MAT-16 outside the attract)
  is not implemented in this project's skirmish; the attract needs nothing for it.
- A carried rider destroyed with its carrier does not return its type (MAT-15); no attract map
  carries riders.

The stock Home One and its death clone name `SFX_Death_Bombard` through
`Death_SFXEvent_Start_Die`. Its `Preset_INSANE` sets `Is_3D=No`, `Is_2D=Yes`,
priority 2 and volume 100; it has none of the speech/GUI category flags. This
non-positional effects event bypassed the former 3D/speech-only attract gate.
The gate now refuses all battle events, so modded death cues and other global
effects follow the same policy without any event-name or preset special case.
