# Player settings and the options screen

Status: built for the main menu wave (legacy EAWR-2927, EAWR-2923). The in-game menu does not
open the screen yet; the screen is written to be reused there.

The facts come from the FoC debug build (profile reading and defaults, each options dialog's
activation and button handling) and from the effective data's dialog resources
(`guidialogs.rc`: the controls and their `TEXT_*` captions). Nothing here quotes original code,
names or addresses; evidence cells say "debug build".

## 1. The record

One versioned record, `PlayerSettings` (`eawr/presentation/ui/settings.hpp`, engine-free), holds
the option set of the options dialogs. Its text form is `eawr-settings <version>` followed by
`section.key=value` lines; a file of another version, or one that is not a settings file, loads
as the defaults. No compatibility is kept before 1.0: change the format, bump the version.

| Rule | Behaviour | Source |
|---|---|---|
| ST-S1 | Loading never fails: an empty, unreadable or foreign file is the defaults; a bad line is dropped and a bad value keeps its default, each with a note; an out-of-range value is clamped with a note. | project |
| ST-S2 | The first line carries the version (1). Any other version loads as the defaults. | project |
| ST-S3 | Every field is an integer (percent, step, permille, 0/1), so a round trip is exact and a slider's stop is the stored value. | project |
| ST-S4 | Settings are UI state (UI-C2): they never enter a replay or a hash. A setting that changes a game's rules goes through the setup and command seams, not into the simulation from the screen. | project; UI-C2 |

## 2. What is stored, and its default

| Rule | Option (retail control) | Stored as | Default | Source |
|---|---|---|---|---|
| ST-A1 | Master, effects, speech and music volume sliders | percent, 0..100 | 75 each (0.75) | debug build |
| ST-A2 | Movie subtitles check | 0/1 | on | debug build |
| ST-V1 | Full-screen mode size, windowed size, windowed check | integers, 0/1 | 800 x 600 both, windowed off | debug build |
| ST-V1 | Vertical sync check | 0/1 | off | debug build |
| ST-V1 | Gamma slider | permille | 1000 (1.0) | debug build |
| ST-V1 | Widescreen check, hardware cursor check | 0/1 | off, on | debug build |
| ST-V3 | Basic detail slider | 0..3 | 2 | debug build |
| ST-V4 | Eleven advanced detail values (texture, shader, water, shadow volumes, soft shadows, heat distortion, bloom, particle, mesh, shadow, environment) | -1 or a value | -1: follow the basic level | debug build |
| ST-P1 | Render profile (project-authored; see section 4) | automatic, retail, enhanced | automatic | project |
| ST-G1 | Tactical and galactic game speed sliders | step 0..4 (five steps) | 2 and 2 | debug build |
| ST-G2 | Camera scroll speed slider | stop 0..9, a value 0..3 in ten stops | stop 3 (1.0) | debug build |
| ST-G1 | Absolute scrolling, delayed tooltips, right-button scroll, both-buttons rotate, alternate mouse controls, simplified faction colours, ability autofire, ask for auto-resolve | 0/1 | on, off, off, off, off, off, on, off | debug build |
| ST-N1 | Network port (1234 is "use the default port"), NAT negotiation, firewall bypass, UPnP | integer, 0/1 | 1234, on, on, on | debug build |
| ST-N1 | Quickmatch default, connection speed, preferred faction, lobby locale | integers | 1 (land), 0 (modem), 0 (random), -1 (the game language) | debug build |
| ST-K1 | Key overrides (command name to key text) | map | empty: the project's keys | project |

The retail anti-aliasing level is not stored: its slider drives the render profile (section 4).
The full-screen mode's refresh rate, the audio provider name and the profile's name, colour and
last-game fields are not options and are not stored here.

## 3. What each retail control does today

A control the build cannot honour is built disabled, with its reason as a tooltip (ST-O1). The
table of controls and reasons lives next to the record (`find_option_status`) and is a test.

| Rule | Control | Today |
|---|---|---|
| ST-A3 | Volume sliders | Heard at once on the run's shared audio buses (BA-45, section 5), with or without a battle; Cancel and Esc put the saved levels back; Accept saves; Defaults sets 75 and previews. The front end, a battle and the demo battle start at the saved levels. |
| ST-V2 | Resolution list | Windowed: applied to the window. Full screen: stored, and the window system keeps the display's own resolution. The list is the project's common sizes that fit the screen, the screen's own size, the window's current size and the saved sizes. |
| ST-V5 | Windowed check, vsync check | Applied to the window. Apply applies and stays; Accept applies and leaves; Cancel leaves without applying. Apply is disabled until something changes. |
| ST-P1 | Anti-aliasing slider | Off is the retail look, Max the enhanced one (section 4). |
| ST-G3 | Tactical game speed | The battle's speed step (TM-01) unless `--eawr-live-speed` chose one. |
| ST-O1 | Gamma, detail sliders, advanced page, widescreen, hardware cursor, subtitles, audio provider | Disabled: the detail levels and cursor kind are not built, the layout follows the window already. |
| ST-O1 | Galactic speed, auto-resolve | Disabled: the galactic game is not part of this build. |
| ST-O1 | Scroll speed, absolute scroll, right-button scroll, rotate with both buttons, alternate mouse | Disabled: the camera takes its controls from the project's bindings. |
| ST-O1 | Delayed tooltips, faction colours, ability autofire | Disabled: not selectable yet. |
| ST-O1 | Network page | Disabled: online play is not built. |
| ST-K1, ST-K2 | Keyboard pages 1 to 5 | Read-only. The edit boxes show the key the project's battle input binds to the command (guard, attack, move, stop, attack-move, select all, groups 0 to 9, tactical overview) and are blank otherwise. Rebinding is a follow-up: the battle input's keys are code, not a table. |

The retail difficulty radios appear only in a campaign game (debug build); this build has no
campaign, so they are hidden.

## 4. The render profile

`--eawr-render-profile retail|enhanced` is an internal switch for evidence runs and the render
profile has had no player setting (docs/rendering.md). The anti-aliasing slider of the video page
is that setting: Off applies the retail look (no MSAA, no screen-space AA, no anisotropic
filtering), Max the enhanced look. Until the player moves the slider the setting is `automatic`
and the run keeps its own choice (enhanced for a player's view, retail for captures and tests).
An explicit `--eawr-render-profile` still wins at start-up.

## 5. Where the settings apply

- **Start-up of a player's view.** When the settings file exists, the window mode and size, vsync
  and an explicit render profile apply before the first mode starts, and the file's volumes and
  tactical speed are kept for the battle. A capture, report or test run never reads the file, so
  evidence runs do not depend on the machine.
- **Audio buses.** One layout per run, made at the viewer's start and shared by the front
  end and BattleAudio (`apps/viewer/src/audio_buses.hpp`): `EAWR_SFX`, `EAWR_Voice`, `EAWR_Music`
  and `EAWR_Mix`, which feeds Master. A player sees: the master slider is the Mix bus, so it
  scales everything; the effects slider is the SFX bus (battle effects and the menu's button
  clicks); the speech slider is the Voice bus (localized speech); the music slider is the Music
  bus (the menus' music event and a battle's music). The slider levels live on the buses only; a
  sound's own volume stays on its player. The mapping of effects, speech and music to the three
  channel sliders is the retail category rule (SND-13); which slider retail drives the menu's
  button sounds is not captured, so they ride the effects slider (open point). The demo battle
  mutes no bus (MAT-18): it refuses its own 3D sound events and speech at the source, so the
  menu's clicks and the effects slider's preview stay audible on the shared buses under the saved levels.
- **The options screen.** Accept saves the file and applies the page's section at once; a
  slider moves the buses while it is dragged and Cancel puts the saved levels back.
- **Files.** `user://eawr-settings.cfg` in the user data folder, or `--eawr-settings-file <path>`.
  It is written to a sibling file and renamed over the target.

## 6. The screen

`EawrOptionsScreen` (`apps/viewer/src/options_screen.hpp`) builds the hub and the pages from the
dialog resources through the kit's dialog builder, so captions come from the text database and the
skin from the data, as every other screen. The hub is the main menu's options list
(`IDD_MAIN_OPTIONS_DIALOG`, full screen) or the in-game menu's (`IDD_IN_GAME_OPTIONS_DIALOG`,
centred, with the network page); the pages are centred dialogs. It is a modal layer (UI-I1), emits
`closed` when the hub is left and removes itself.

- `build(Options)` loads the data and the settings once; `show(parent, page)` opens it.
- `--eawr-options` is the standalone mode: `--eawr-options-page`, `--eawr-options-context
  main|game`, scripted steps `--eawr-options-do page:<page>`, `set:<IDC_ID>=<number>`,
  `press:<IDC_ID>`, `snapshot`, and `--eawr-settings-file`, `--eawr-report`, `--eawr-capture`.
- The GPU case is `tests/presentation/renderer/test_options_screen.py`: change a volume and the
  full-screen and vsync state, Accept, restart, and read the saved values back.

## 7. Open points (fidelity list)

- The main menu's own page placement (`IDD_MAIN_OPTIONS_DIALOG` full screen) and the pages'
  placement presets are unverified until the retail captures of the options pages are compared.
- The video page's resolution confirmation (a 20-second countdown that reverts the mode) is not
  built; a resolution applies at once and the window is resizable.
- The volume slider's test sound on release (effects and speech) is not played.
- Which slider drives the menu's button sounds in retail is not captured; they ride the effects slider.
- Full-screen resolution is not applied (ST-V2); the window system keeps the display's.
- The disabled reasons and the render-profile mapping are project-authored English strings and
  choices, not text-database keys.
- Rebinding keys (ST-K1), camera scroll speed and mouse options, tooltip delay, faction colours and
  ability autofire, the detail levels and gamma each need their own feature.
- `TEXT_MODS_OPTIONS` and `TEXT_SELECT_MOD_TITLE` are not in the text database. The retail executable
  registers "Mods" and "Select Mod" for them when the key is missing (debug build, observed in the
  retail captures); `TextLookup` carries the same two defaults. The hub shows the Mods button with
  that caption, disabled until the mods page (legacy EAWR-2945) exists, and hides it only when a mod removes
  `IDD_SELECT_MOD_DIALOG`.
- The keyboard pages' Accept stays disabled (no key can change yet, ST-K1), as retail disables it
  until a change.
- The in-game menu does not open the screen; the pause menu's Options button is a follow-up.
