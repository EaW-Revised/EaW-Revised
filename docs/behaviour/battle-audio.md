<a id="space-battle-audio-p2-21-84"></a>

<a id="space-battle-audio-p2-21"></a>

# Space battle audio

BA-85: A countdown-spawned projectile enters ordinary death presentation and
requests `Death_SFXEvent_Start_Die` once at its own position, independently of
blast recipients. This route does not request `Projectile_SFXEvent_Detonate`,
even when authored: that cue belongs to ordinary projectile contacts (BA-15).
Consequently, adding countdown death audio leaves those contact cues unchanged
and does not double-play a projectile that authors both. Evidence: debug build
(countdown service, death initialization and death-effect selection); effective
projectile XML. Muted output retains the same request bookkeeping.

## Applicability

What FoC plays for a space battle: the sound events of shots, hits, deaths and hardpoint deaths,
the unit responses to the local player's selections and orders, and the ambient and battle
music. The viewer plays them from the live session's presentation events
(`--eawr-live-session`, live space-battle presentation), so the audio can never change the simulation: the session's hashes
are the same with or without it. Sources: the FoC debug build `corruption/StarWarsI.exe` read in
GhidrAssist (evidence IDs AU-01 to AU-31; the map with addresses is private under
`out/research`), and the FoC XML: `SFXEVENTFILES.XML` and the `SFXEVENTS*.XML` it lists,
`AUDIO.XML`, `MUSICEVENTS.XML`, `FACTIONS.XML`, `GAMECONSTANTS.XML` and the unit, hardpoint and
projectile files. Library behaviour that the binary hands to the Miles Sound System (MSS 6.6)
is marked as such.

## Interface

- `presentation::audio` (engine-free, `include/eawr/presentation/audio/sfx.hpp`) holds the
  registry, the voice rules, the falloff, the listener, the unit-response speaker and the music
  director. The viewer's `BattleAudio` owns the Godot players and reads the session.
- `--eawr-audio on|off` (live sessions only): `off` mutes the output while every rule and the
  report still run. Default: on in the interactive view, off in a capture run (tests).
- The report's `battle_audio` member lists every start request by reason and event, what each
  resolved to, the samples played, and every missing or undecodable sample and unknown event.

## Rules

### Sound events

| ID | Rule | Evidence |
|---|---|---|
| BA-01 | The SFXEvents load in `SFXEVENTFILES.XML` order (the presets file first). An event without the fields below is 3D, priority 3, probability 100, play count 1, one instance, volume and pitch 100 and saturation distance 300. Priority is clamped to 1..5, probability to 0..100, pitch to 50..200. | AU-01 |
| BA-02 | An event's fields apply in document order. `Use_Preset` copies the whole preset over what is there (the event keeps its name and is no preset); empty and `TBD` values are skipped. An integer field reads its leading integer, so the weapon events' `Probability` of `80, 20` is 80. | AU-02, AU-01 (a one-byte field) |
| BA-03 | Presets never play; a play count of 0 never plays. | AU-03 |
| BA-04 | `Max_Instances` 0 never plays. A 2D event with `Max_Instances` playing is refused; a 3D one goes on to BA-05. | AU-03 |
| BA-05 | A 3D event with `Max_Instances` playing plays only when it is not farther from the listener than the farthest of them, which it stops. While an event with the same `Overlap_Test` plays, the new one is refused. | AU-05, AU-03 |
| BA-06 | A `Probability` under 100 is a roll of 1..100 that must not exceed it. | AU-03 |
| BA-07 | A sound whose object the local player does not see (fogged) does not play. | AU-03, AU-13 |
| BA-08 | Volume and pitch are drawn uniformly from `Min_Volume..Max_Volume` and `Min_Pitch..Max_Pitch` (percent); pitch scales the playback rate. The sample is drawn uniformly, or the next in turn with `Play_Sequentially`. | AU-04 |
| BA-09 | There are 32 3D voices. With all busy, the candidate is the playing sound with the highest priority number, the farthest among equals (switching to a higher number keeps the distance of the one it replaced, as FoC's loop does). It is stopped for the new sound when its priority number is not lower and it is farther; otherwise the new sound does not play. The 2D pool's size is not modelled (16 voices; the battle's 2D sounds are the one-instance unit responses). | AU-07, AU-08 |

### 3D

| ID | Rule | Evidence |
|---|---|---|
| BA-10 | Space mode reads `Audio_Space_3D_Saturation_Distance_Mod` (1.5), `Audio_Space_3D_Rolloff_Distance_Mod` (2.0) and `Audio_Space_3D_Listener_Z_Pullback_Dist` (60) from `AUDIO.XML`. | AU-06, AU-09, AU-10 |
| BA-11 | A 3D sample's minimum distance is its `Volume_Saturation_Distance` times the saturation factor (at least 1), its maximum distance 15000, and the driver's rolloff factor the rolloff factor. The gain is Miles' inverse-distance law: 1 inside the minimum distance, then `min / (min + rolloff * (d - min))`. Past the maximum distance the sound is silent (library behaviour, unverified: MSS may hold the gain there instead; at 15000 it is already below 2 %). | AU-06, AU-09; MSS documentation |
| BA-12 | The listener stands where the camera's view line meets the plane z = 60, pulled back along the line to `|camera z - 60|` from the camera when that point is farther. It faces the camera's forward flattened onto the plane (FoC passes up (0, 0, -1) to Miles' left-handed frame; the viewer keeps screen left on the left). During a cinematic it would be the camera itself (not used). | AU-10, AU-11, AU-12 |
| BA-13 | A sound's volume is its drawn volume times the object's visibility (0 when fogged or hidden). While a speech stream is retained without an active GUI dialog, starting SFX gain is capped at 0.3 before category/master gain. Ongoing ordinary 2D and attached spatial gain writes apply the same ceiling; fixed-position spatial sounds keep their starting gain outside their separate fade path. WAV unit responses alone do not activate the cap. | AU-13; SND-61 |

### Battle events

| ID | Rule | Evidence |
|---|---|---|
| BA-14 | A shot plays at the new projectile's position: a hardpoint's `Fire_SFXEvent`, else its parent type's `SFXEvent_Fire`, when `Projectile_Appearance_Delay_Frames` is below 1 (no FoC hardpoint sets it); a unit's own weapon plays the type's `SFXEvent_Fire`. The firer's visibility decides BA-07. | AU-14, AU-15 |
| BA-15 | A projectile hit plays at the contact point: `Projectile_SFXEvent_Detonate_Reduced_By_Armor` when the hull armor multiplier was at most 0.75 and the projectile names one, else `Projectile_SFXEvent_Detonate`. A hit the shield took whole counts as multiplier 1 (BP-13) and plays the plain detonation. | AU-16 |
| BA-16 | A destroyed unit plays its `Death_SFXEvent_Start_Die` at its position. A craft that spins away instead plays `Spin_Away_On_Death_SFXEvent_Start_Die` attached to the moving dead copy; any remaining attached sound stops before the ordinary death cue plays at its final position when the spin ends (SP-03, SP-08). The attached voice's current position drives its falloff and voice culling. | AU-18; debug build |
| BA-17 | A destroyed hardpoint plays its `Death_Explosion_SFXEvent` at the hardpoint's world position, else the `Death_SFXEvent_Start_Die` of its `Death_Breakoff_Prop` type. | AU-17 |
| BA-18 | Battle sounds are heard when the local player sees the unit, as the battle effects are drawn (BP rules of `battle-presentation.md`); an event fires on the first frame whose presented tick reaches its tick minus one. | viewer rule, mirrors live space-battle presentation |

BA-16 distinguishes the start of death from the end of a persistent death copy.
The debug build requests `Death_SFXEvent_End_Die` when that copy shuts down, after
detaching its attached sounds. The field supplies an event, not a duration: a
persistent copy waits for its death animation to finish and then for
`Death_Persistence_Duration` before removal or fading. A completed fighter spin
instead requests the ordinary `Death_SFXEvent_Start_Die` (SP-08).
The effective FoC XML authors no nonempty `Death_SFXEvent_End_Die` for a
`SpaceUnit`, including inherited variants. Its authored `UniqueUnit` entries
belong to land death copies. The Krayt and Vengeance space death copies author
only `Death_SFXEvent_Start_Die` and `Remove_Upon_Death`; they have no end cue.
M2 therefore has no stock end-of-death event to schedule on this route.

### Moving ambience

| ID | Rule | Evidence |
|---|---|---|
| BA-80 | A live craft with `SFXEvent_Ambient_Moving` initializes a uniform delay from its current logical frame. The authored `SFXEvent_Ambient_Moving_Min_Delay_Seconds` and `_Max_Delay_Seconds` default to 5 and 10 seconds (150..300 frames); positive, ordered values are required. When due, only a team's current leading live craft services the cue. A locomotor with movement remaining requests the event attached to that craft; idle orbiting alone does not qualify. Reschedule from the current frame even when stationary or playback is refused. | SND-46; fresh debug-build path/idle/combat cross-check |
| BA-81 | Moving ambience uses the ordinary event's finite play count, random sample/pitch/volume, instance admission and 32 spatial voice limit. Its attachment follows the interpolated height-adjusted craft pose; ongoing local fog or hidden arrival sets gain to zero. Removal stops the attached voice and retires its timer. Timers and random delay state are presentation-only, bounded by live authored craft; movement presence is published during the existing snapshot pass, outside canonical state. | SND-11, SND-46; BA-05/08/09 |
| BA-82 | Ordinary space engine audio enables the authored idle loop on engine enable, then services the sampled walk speed against `SpaceIdleMovementSpeed * 1.1`. At or above the threshold enable moving before disabling idle; below it disable moving before enabling idle. Stock threshold is zero: stopped service writes zero and still selects moving. Engine disable switches both off. State changes survive missing events or refused admission; no per-frame retry. Hyperspace does not run ordinary locomotor service. | SND-45/67; fresh debug-build moving switch and stopped-service cross-check |
| BA-83 | Engine idle/moving events must be spatial loops. They share ordinary event admission and the 32 spatial voices, retain object attachment and use authored `Loop_Fade_In_Seconds` and `Loop_Fade_Out_Seconds`. Fogged attached starts are admitted at zero gain; ongoing fog/hidden arrival mutes them, reveal restores gain, and removal detaches immediately. Fades and source ownership cannot stop a slot stolen by another event. Presentation reads existing sampled speed published in the existing worker snapshot slots, outside canonical bytes. | SND-04/11/45; BA-05/08/09/13 |

The ordinary engine path consumes idle and moving loops for live space locomotors.
`SFXEvent_Engine_Cinematic_Focus_Loop` belongs to the separate cinematic-focus interface.
`SFXEvent_Ambient_Loop` has authored ship entries, often duplicating engine events, but its
getter cross-references only establish parse-time validation; continuous playback remains
unverified (SND-U05). It is not started in addition to the sourced engine loops.
Effective `Ambient_Map_Sounds` has seven land environments and no Space mapping (SND-47),
so stock space maps add no generic ambient bed. Custom space-bed execution remains unverified.

Stock X-wings and TIEs author flyby events rather than normal idle/moving engine loops.
Their moving event uses `Preset_EGB`: priority 5, two instances, volume 70%, saturation
distance 400, pitch 95..105% and one play. TIE events override pitch to 100..110%.
Cinematic-focus loops use a separate retail path and do not replace ordinary flybys.
The report's `battle_audio.ambient` records timer work, due ticks, movement eligibility,
follower skips and attachment updates. Delay randomness is independent of voice admission.

The attached-event retail overload admits hidden cues and applies ongoing zero gain;
attached starts use ongoing gain rather than a generic fog refusal (SND-04/11).
Story-cinematic fog bypass and explicit attached silencing are presentation interface facts.
Audio clips verify audible output; XML and admission counts alone establish no retail mix parity.

### Reinforcement feedback

| ID | Rule | Evidence |
|---|---|---|
| BA-84 | Local pane opening requests faction `Reinforcements_Selection_SFXEvent`; closing, an idempotent open and permission refusal add no selection cue. A valid pooled-type placement start requests `Reinforcements_Pick_Landing_Zone_SFXEvent`; an already active drag adds none. A valid arrival submitted to the local scheduler requests type `SFXEvent_Command_Fleet_Move`, else faction `Reinforcements_Enroute_SFXEvent`. Invalid drops and refused submission add no en-route cue. These are ordinary nonspatial starts, separate from authoritative cap refusal and frame-35 spatial arrival. Muting output leaves cue admission/reporting active. | WR-07/11/16; debug-build placement/drop cross-check |

`Reinforcements_Ready_SFXEvent` services a pending-garrison countdown, not ordinary space
skirmish pool completion; that caller remains outside this M2 path.
`Reinforcements_Cancelled_SFXEvent` is sourced on a placement-end request with its
cancel flag set and a selected type, plus a separate transport dialog. Successful
submission clears placement with that flag unset. The ordinary space drop callback
ends placement with cancellation set even after a failed placement or room check;
invalid drops and explicit right-click cancellation request the authored faction cue.
Ordinary overlap admission may refuse playback. Pane closure has no established
cancellation requirement, and `Reinforcements_Requesting_SFXEvent` runtime use remains
unverified.

### Unit responses

| ID | Rule | Evidence |
|---|---|---|
| BA-20 | One unit speaks for a selection: the one whose first category in `GAMECONSTANTS.XML` `Unit_Command_Rankings_By_Category` comes first; a later unit with a lower `Ranking_In_Category` (default 25) takes over whatever its category, as FoC's loop is written. | AU-19, AU-20, AU-21 |
| BA-21 | A selection by click, drag box, double click (type on screen) or control group plays the speaker's `SFXEvent_Select`. | AU-19, AU-23 |
| BA-22 | A move order plays `SFXEvent_Group_Move` when more than one unit is selected and the type has one, else `SFXEvent_Move`; an attack order `SFXEvent_Group_Attack` or `SFXEvent_Attack` the same way. | AU-22 |
| BA-23 | Unit responses are 2D (`Preset_UR`: one instance, `Overlap_Test` per unit type), so a second response of the same type while one plays is refused. | data, BA-04, BA-05 |
| BA-24 (squadron voice responses) | A squadron's team container carries no response sounds or ranking category of its own (every M2 squadron: the craft carry those, the squadron entry does not). Selecting or ordering a squadron therefore ranks and sounds as its leading live craft: FoC's category ranking (BA-20) and its select/move/attack sound both resolve a selected object that belongs to a team to that team's current leader before reading its own sound and ranking fields (debug build), so an X-Wing squadron speaks with the X-Wing craft's own lines. FoC keeps its leader slot live by reassigning it the instant the leader dies; the remake instead walks the squadron's fixed tick-zero roster order and skips a craft once it leaves the tactical snapshot, landing on the same craft in practice. The group-move/group-attack sounds are read from the resolved craft's own type with no further resolution (no M2 craft or squadron sets them, so BA-22 always falls through to the resolved single-unit sound for a squadron). | AU-32 |

| BA-25 | An attack aimed at a hardpoint first requests the speaker's repeated `SFXEvent_Attack_Hardpoint` entry for that target hardpoint's authored type. A missing entry falls through to the group or ordinary attack response (BA-22). This includes shields, engines, hangars and weapon kinds where the speaker authors a line. | SND-20, SND-21; XML data |
| BA-26 | Accepted stop and guard orders request the ranked speaker's `SFXEvent_Stop` and `SFXEvent_Guard`, respectively. A missing event is silent; arming or cancelling a mode alone does not request its response. Squadron speakers resolve through BA-24. | SND-23 |
| BA-27 | Command-bar attack, attack-move, guard and move presses request the corresponding Audio `SFXEvent_Command_Bar_*` only when their mode becomes armed. Disarming and executing the world order are silent on this route. Stop requests `SFXEvent_Command_Bar_Stop` after the stop action, separately from its unit response. The mouse and order hotkey routes share this transition. | Fresh debug-build button handlers; OR-01 |
| BA-28 | A left ability press with no eligible unit, or a mixed inactive/recharging group with no ready unit, requests Audio `SFXEvent_GUI_Negative_Feedback` and sends no ability. An all-active group may deactivate; one ready member permits activation without refusal feedback. Targeted abilities also refuse recharging presses before targeting. Autofire right clicks have their separate route. | Fresh debug-build ordinary and targeted ability handlers |
| BA-29 | An admitted group move or attack response may chain one other selected unit's `SFXEvent_Assist_Move` or `SFXEvent_Assist_Attack`. Draw uniformly from the original selection, excluding the speaker with at most 51 draws; test the resulting object's different type and lack of `EJECT_VEHICLE_THIEF` once. Both lines must be 2D. The assist starts at completed primary event end and re-enters normal admission; refused or explicitly cancelled primaries cannot release it. Sample stealing and backend failure still complete the admitted loop and preserve its chain. The chain retains the assist source independently of the primary's attachment; removing that source cancels it before completion. Stop, guard and single selections add no assist. | SND-08/09/18/19/22; fresh debug-build random-selection, completion and chain-detachment cross-check |

Space move destinations test live static obstacle circles using the bound motion footprint,
including authored scale and rotated, unscaled offset (WHZ-03/05). Asteroid containment wins
over nebula containment. The selected environment line precedes group and ordinary move;
a missing asteroid line skips nebula and falls through to those defaults (SND-24/25).
These checks use the presentation snapshot and add no simulation state. Runtime chains use
fixed voice slots and completion scratch storage, without frame-time storage growth.
The assist candidate's type and assist fields belong to the selected object itself;
the BA-24 leading-craft proxy is used for the primary speaker, without transferring
craft assist fields to a selected squadron container (debug build).

An invalid targeted ability action also requests BA-28 feedback. Right-click and Escape
cancellation remain silent on this route. Land garrison, galactic movement and other unsupported
refusal callers are outside the local space command bar. Waypoint arming has the same sourced
cue rule as BA-27, but its queued-order semantics remain deferred (legacy EAWR-1503).
Build-pad refusal checks are separate: a local request blocked by fog, non-allied ownership,
an occupied pad or insufficient credits plays feedback, whereas a pad cooldown is silent
(debug build). Those land-pad callers are outside this space implementation.
Stock Audio command-mode fields and M2 space assist fields are empty; test overlays use
existing events to prove the consumers without changing shipped data.

<a id="ability-sounds-559"></a>

### Ability sounds

An ability has two sounds in FoC and they come from two places (debug build, evidence AU-33 to AU-36):
the faction's *toggle sound*, played when the switch happens (the S-foils' "swhoong", the shields
rising), and the unit's *voice line*, played when the command bar's button or key is pressed (the
pilot shouting to lock the S-foils, the Nebulon-B's officer engaging the shields).

| ID | Rule | Evidence |
|---|---|---|
| BA-50 | **Toggle sound.** When an ability switches on or off, the owner's faction plays the `SFXEvent_GUI_Toggle_Non_Hero_Ability_On` or `_Off` entry it lists for that ability (`FACTIONS.XML`, one `<ability>, <event>` row per ability), once per switch however many units it carried (a squadron's craft, a selected group), as a 2D sound. The Rebel rows: `DEFEND` `GUI_Toggle_Shields_On`/`_Off` (`GUI_Shields_Up_1`, `GUI_Shields_Down_1`), `TURBO` and `SPOILER_LOCK` `GUI_Toggle_Turbo_On`/`_Off` (`GUI_Toggle_17`, `GUI_Toggle_18`); the Empire's `TURBO` and `DEFEND` rows are the same events. `POWER_TO_WEAPONS` (the row is `BARRAGE`, blank) has none. A sound plays only when the switch happened: a refused command plays none. When the switch is an enemy's (another team than the local player's) the faction's `SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_On`/`_Off` entry plays instead, and every one of those is blank, so an enemy's abilities are silent; an ally's are heard, fog or no fog. An AI or Lua `Activate_Ability` plays the same toggle sound as a button (the event it builds is started with the same sound flag as the button's in every caller traced; only the refusal sound is left to the button), so the local player's own script-driven `DEFEND` is heard. | AU-33, AU-34, AU-35, data |
| BA-51 | **A timed ability that runs out is silent.** FoC's expiry handler deactivates the ability and starts the faction's *enemy* off-event, not the plain one, for the local player's units; every enemy off-entry is blank in the M2 data. Only a switch-off (button, script) plays the off sound. The remake tells the two apart from the snapshot: a timed ability (one that has read frames left while active) whose last read, the final active tick's 0 included, had two frames or fewer left and that is then off ended by itself; an untimed ability's end always plays the off sound. A switch-off within the last two frames of a timed ability is taken for a natural end, silently. A switch that the engines or the shield end (AB-16, AB-17) is not told apart from a switch-off and plays the off sound (**unverified**: the debug build's deactivation for those was not traced). | AU-36 |
| BA-52 | **Voice line.** A press of an ability's button or key that reaches the simulation plays a voice line from the first pressed unit that has the ability (a squadron resolves to its leading live craft, BA-24): its `SFXEvent_GUI_Unit_Ability_Activated` when the press switches the group on (not every unit was on), its `SFXEvent_GUI_Unit_Ability_Deactivated` when it switches it off. The line is a plain 2D start of the event, not a unit response: it takes no part in the speaker ranking (BA-20) and no acknowledgement follows it. The events are `Preset_UR` ones (BA-23: one instance, `Overlap_Test` per unit type), so a line is refused while another line of the same type plays, and there is no cooldown beyond that. The M2 lines: the Corellian corvette's `TURBO` `Unit_Speed_Corvette`, the Nebulon-B's `DEFEND` `Unit_Defend_Nebulon`, the X-wing's `SPOILER_LOCK` `Unit_Ability_On_X_Wing` and `Unit_Ability_Off_X_Wing`, the Tartan's and the Acclamator's `POWER_TO_WEAPONS` `Unit_Barrage_Tartan` and `Unit_Barrage_Acclamator`; no other M2 ability has a deactivation line. Script and AI switches play no voice. | AU-33, AU-37, data |

Refused ability presses follow BA-28. The ability-active loop
(`SFXEvent_Special_Ability_Loop`) is not authored by the starting M2 roster.
Some expanded heroes author beam loops; those remain unimplemented. No default loop is supplied.

BA-50's faction effect and BA-52's command-bar voice have separate callers.
The debug build's successful faction-effect route does not check the UI-request
flag, selection, fog or camera distance. Lua activation retains its enabled
sound flag while clearing the UI-request flag; that flag gates refusal feedback
only. The issuing player's faction supplies the table, and the local player's
enemy relationship selects the enemy entry. A nonempty plain entry is required
before that selection, even if an enemy entry is authored. Arming autofire
changes its setting without starting an activation sound. These predicates do
not grant an AI or script activation the command-bar unit voice.

### Ability payload cues and superweapon scope

| ID | Rule | Evidence |
|---|---|---|
| BA-53 | A valid player confirmation of a targeted ability requests the first source object's `SFXEvent_Target_Ability`, attached to that source, rather than the target or terrain point. Arming, cancelling, deactivating and autofire changes do not request it. Invalid targeting uses BA-28. The event's authored 2D/3D mode, admission and sample rules remain in force. This acknowledgement precedes simulation success; it is not an activation notification for every affected unit or for AI/script beam commands. | debug build: targeted action acknowledgement; effective XML |
| BA-54 | A successful spawned-object ability also requests its source's `SFXEvent_Target_Ability` on the source object, including script/AI activation. Suppress this request if that same event is already playing anywhere, so player confirmation and the spawn do not double it. The viewer observes changes to the existing `started_tick` of HARMONIC_BOMB and WEAKEN_ENEMY through each retained logical snapshot; inactive instant slots still carry the birth. The zero-position fallback controls where the payload spawns, not where this cue attaches. | debug build: spawned-object ability execution; WHE-61; effective XML |
| BA-55 | No implemented M2 action invokes cluster-bomb, point-laser defense, stealth or maximum-firepower payload effects. Maximum-firepower authoring belongs to the land AT-AT; merely recognizing its nested type does not implement its action. Their effect cues have explicit deferred or land scope, rather than being treated as applied. None of the reachable nested concentrate/beam/ion payloads authors `SFXEvent_Activate`. | effective XML survey; implemented ability consumers in `src/sim/tactical/abilities.cpp` and `src/sim/tactical/session_step_commands.cpp`; debug-build payload type lookup |
| BA-56 | TSW button feedback belongs to a tactical superweapon stage request. On an in-range, ready, destroyable-target power-up transition, activation voice and weapon power-up start as unattached SFX, followed by the authored music override. Approach, 60/30/15/5-second warnings and arrival in range use the local faction's speech entries, selected for the owner or enemy relationship. Special-weapon readiness is separate: after its ready frame, with battlefield special weapons enabled, local/allied sources use their own `SFXEvent_Special_Weapon_Ready`, else the local faction's indexed `SFXEvent_HUD_Special_Weapon_Ready`; enemies use the indexed enemy entry. Stock space skirmish authors planetary ion-cannon and hypervelocity use upgrades at higher station levels, and land has its own weapons. M2 has the superweapon option and purchase filtering, but no implemented weapon firing/readiness or TSW stage. Existing upgrade-completion audio remains BA-60; a purchase never synthesizes TSW power-up or a second readiness cue. | debug build: stage request, power-up, approach speech and special-weapon service routes; effective XML; current space command consumers |

Reachable ability cue audit (effective XML, including the expanded M2 hero roster):

| Ability group | Authored separate cue and route |
|---|---|
| DEFEND, TURBO, POWER_TO_WEAPONS, SPOILER_LOCK | BA-50/52 only; no payload activation cue |
| ION_CANNON_SHOT | No target cue; weapon fire/detonation retains BA-13/14 |
| BARRAGE | Broadside `Unit_Barrage_Interdictor`, Marauder `Unit_Barrage_Corvette` would use BA-53; both actions are disabled by the current RG-03 release gate, so no confirmation is reachable in space skirmish. |
| INVULNERABILITY, REPLENISH_WINGMEN | No authored payload cue; no default |
| CONCENTRATE_FIRE | Home One `Unit_Barrage_Ackbar`; BA-53 |
| ENERGY_WEAPON | Accuser `Unit_Energy_Blast_Piett`; BA-53 |
| TRACTOR_BEAM | Accuser and tractor satellite `Unit_Tractor_Beam_Star_Destroyer`, Admonitor `Unit_Tractor_Beam_Thrawn`, Executor `Unit_Tractor_Beam_Vader_Executor`; BA-53. Generic destroyer target entries are blank. |
| HARMONIC_BOMB | Slave I has no authored target cue; BA-54 remains silent unless data authors one |
| WEAKEN_ENEMY | Sundered Heart `Unit_Energy_Flux_Antilles`; BA-53/54 |

Cluster-bomb and point-laser defense are authored on MC30 and Crusader respectively,
but their action consumers are unavailable. Space stealth on Vengeance, Phantom and
Peacebringer is also unavailable. Land payloads (force powers, grenade, repair, hack,
shield flare, maximum firepower) and galactic corruption/credit effects stay outside
this space audit. Existing hero beam-loop authoring does not justify a general loop.
The underlying barrage consumer does not override the disabled ability entries in
`data/skirmish/roster-gate.json`; its authored sound remains unavailable in this release.

The stock space upgrades `RS_Ion_Cannon_Use_Upgrade` and
`ES_Hypervelocity_Gun_Use_Upgrade` enable battlefield special weapons and are
listed at higher-level skirmish stations. Their build-complete and readiness tags
are separate even where both name the same availability line. The current build
does not apply their enable/firing/readiness state, so readiness entries are
deferred, rather than classified as land-only. Death Star TSW stage and range
speech likewise have no current playable state. Future state consumers must
publish their actual transitions before presentation can request these cues.

### Production and hyperspace

| ID | Rule | Evidence |
|---|---|---|
| BA-60 | An accepted local buy plays the type's build-underway speech, else `SFXEvent_Tactical_Build_Started`, else `SFXEvent_Build_Started`; local completion uses build-complete speech, then the corresponding tactical/generic complete events. Stock M2 types author SFX events rather than production speech. Pad completion keeps its separate spatial and local cues. Accepted types and completions are retained in immutable presentation metadata, outside canonical state, so a render stall does not infer purchases from queue length. | WPR-22/30; debug build WP-09/11; effective XML |
| BA-61 | Local tactical queue cancellation queues build-stopped speech, else plays `SFXEvent_Tactical_Build_Cancelled`, else `SFXEvent_Build_Cancelled`. The stock M2 path uses SFX fallback. A refused cancellation plays nothing. | debug build WP-A01, retained in ignored research; WPR-31; SND-38/44 |
| BA-62 | Each arriving ship or squadron craft requests its owner's faction `SFXEvent_Arrive_From_Hyperspace` at logical arrival frame 35, spatially on the object. Human and AI owners use the same route; the squadron container adds no extra sound. Retain the pose and visibility at frame 35 so late presentation still applies the ordinary fog and voice rules. | WR-37; WPR-40 |
| BA-63 | A local reinforcement command refused for lack of population room plays `SFXEvent_Tactical_Unit_Cap_Reached`. The local population becoming full plays `SFXEvent_Tactical_Pop_Cap_Reached` once until room becomes available again; the type-less room query costs one population. Buying or pooling a unit does not reserve population. | debug build WP-A02/A03, retained in ignored research; WR-02/05/16 |

### Announcements and streamed speech

| ID | Rule | Evidence |
|---|---|---|
| BA-70 | A logically visible type with `Play_SFXEvent_On_Sighting` is retained once per battle before testing the whole WAV 2D pool for busy voices. Resolve the local faction's first exact-type `SFXEvent_Unit_Type_Spotted`, else `SFXEvent_Generic_Unit_Spotted`. A matching blank does not fall back. Missing, busy or refused playback never restores the type. Requesting an authored type cue suppresses first-enemy handling for that service call only. This is not a radar edge, model fade threshold or per-unit first-reveal rule. | debug build SND-E37/38/54; SA-E02 |
| BA-71 | A logically visible enemy owned by a battle party can consume the battle's first-enemy flag. Set the flag before resolving the local faction `SFXEvent_Enemy_Spotted`; missing/refused playback never retries. This route has no whole-pool busy test. A type-cued enemy may trigger it at a later service while still visible. Projectiles are excluded by the caller; heroes are not a generic exclusion. | debug build SND-E39/54 |
| BA-72 | Standalone space skirmish does not automatically queue attacker/defender tactical intros or their conditions. The intro requires a parent campaign, a 10-second wait, no cinematic and an empty speech queue. Campaign conditions evaluate OR before AND, with the third object an exclusion. Automatic hero respawn and its local planetary-spawn sound are campaign behavior. Network-only victory-near and scripted/story speech remain separate interfaces. | SND-41/49/65; SND-E35/36/57; WHE-38 |
| BA-73 | Authored speech events append FIFO, including duplicates, and service only the front. Draw one file uniformly, use its volume, normal priority, no fade and centered output. Failed playback or a finished/interrupted stream removes the front without retry. One stream is independent of WAV response slots; lower numeric priority is more important, and equal/more important replacement retires the old stream before opening a file. Pause retains the stream and suspends queue cleanup. There is no additional speech cooldown. | debug build SND-E25/26/27/61; SND-44/48 |
| BA-74 | Speech files resolve from the queued language's speech directory. Authored production speech can name PCM WAV files as well as MP3; the stream route, not the filename extension, determines speech ducking. The current viewer supports English and preloads reachable production speech at preparation. Associated text/movie consumers are campaign/story scope under SND-69; other languages remain unimplemented. | SND-E26; SND-62/69; XML/sample header SA-E01 |
| BA-75 | The inspected music start/update routes apply authored track volume, fade level and music/master sliders without a speech-active predicate or 0.3 ceiling. The shared category-gain getter only multiplies its slider by master. The sourced speech cap belongs to SFX; this change does not add a music cap. | debug build SA-E03 |

The viewer reads existing logical visibility from snapshots; no new simulation facts or
canonical bytes are needed. Stable observation uses the sourced 30-frame fog-service cadence
with one presentation phase for the roster. The original per-object phase and accelerated
service while the fog fade moves are not reproduced by this observer; this timing difference
remains a fidelity gap. Type/enemy eligibility never resets on hiding or re-reveal.
The speech queue has 256 preallocated entries; saturation increments a report counter and
drops the new request. This bound is viewer policy, not an original-game cooldown.
The report includes queue completion/failure/overflow counts and capped SFX start/update counts.

#### Speech text and command-bar movies

SND-69 is a consumer interface, not automatic subtitles for every voice line. The
associated `Text_ID` is available only for a now-playing front speech event with
a valid selected-file index and equal `Files`/`Text_ID` list lengths. A pending,
failed or removed event supplies no text. The external hologram consumer displays
that indexed text in the command bar's tutorial/message text area, using
`Message_Text_Color`, only when its pending event is the playing event. Its text
has no elapsed-time expiry; the consumer removes remembered text when the
tracked event name is no longer queued or playing. A speech replacement completes the interrupted
front through the ordinary queue service; pause holds that service and the
retained speech stream (SND-13/44/48). Movies pause through the separate movie
hook (WBF-53). See the [speech text/movie scope audit](walks/audio.md#speech-text-and-movie-scope)
for the start, conflict and cleanup distinctions.

Effective stock XML authors `Build_Speech_Underway`, `Build_Speech_Completed`,
`Build_Speech_Stopped` and `Build_Speech_Countdowns` only on `Death_Star` in
`SPACEUNITSSUPERS.XML`. That object's `Tech_Level` is 99, above the stock
`MP_Default_Max_Tech_Level` of 5. The viewer's synthetic production-speech cases
exercise the existing stream interface; they do not establish stock M2 subtitle
or movie authoring. The faction `Tactical_Intro_Command_Bar_Movie_Name` belongs
to the parented briefing (SND-41); standalone skirmish marks that briefing done
without queuing speech. External hologram callers are galactic corruption
results and initiation, not ordinary space production or WAV responses.

Stock M2 therefore adds no speech subtitle, talking head or movie lifecycle.
The indexed-text query, explicit campaign/story text consumer and command-bar
movie playback remain work for those modes. No new text/movie tags are applied
by the M2 viewer on the strength of their presence in the global registries.

### Music playback

| ID | Rule | Evidence |
|---|---|---|
| BA-40 | A music event plays its `Files` in order from where that event last stopped (a per-event index from 0 at game start), at `Volume_Percent`, looping to the next file when `Loop` is Yes. | AU-28 |
| BA-41 | The battle's music comes from the local player's faction: `Music_Event_List_Ambient` and `Music_Event_List_Battle` entries for `Space`, one drawn at random. | AU-27 |
| BA-42 | Every weapon fire whose firer or target is the local player's starts the battle music unless it plays, and restarts the quiet count. | AU-25, AU-26 |
| BA-43 | Battle music returns to ambient once the quiet count reaches `Music_Space_Battle_To_Ambient_Peace_Seconds` (15) times 30 logical frames. The battle opens with ambient music (the call at tactical start was not traced; unverified). | AU-24 |
| BA-44 | A new track fades in over its event's `Fade_In_Seconds` while the previous one fades out over the new event's `Fade_Out_Previous_Seconds`. | AU-28 |

### Mix

| ID | Rule | Evidence |
|---|---|---|
| BA-45 | The volume sliders (master, music, speech, SFX) start at 0.75, and a channel plays at its slider times the master's: every sound at 0.5625 of its data volume, so the data alone balances the SFX against the music. The viewer has no slider UI yet and plays the defaults: the 3D SFX, the unit responses and the music each go to their own bus at 0.75 into a mix bus at 0.75. A hard limiter at -0.3 dBFS on the mix bus is a viewer choice, not FoC's (Miles' clamping was not traced), for the rare peaks of many shots over loud music. | AU-30, AU-31 |

<a id="viewer-notes-443"></a>

### Viewer notes

- Godot's 3D players find their listener only through a Camera3D of their world, and the renderer
  draws with a bare camera of its own. The listener therefore sits in a never-drawn viewport on the
  same world, with a camera that makes it a listener. Without it every 3D sound mixed to silence.
- Godot starts a player's stream on its next physics step. A voice asked to play counts as playing
  for 0.1 s before its player reports it, so a frame without a physics step cannot free the voice
  for the next sound to cut.
- The report's `battle_audio` meters each bus (`levels`: the run's RMS from a capture after the
  bus's effects and before its own slider; over the frames with sound the RMS of the per-frame
  peaks, the loudest peak and the clipped frames; a muted Master still meters the buses feeding
  it) and lists each start's distance and gain by reason and the voices' lifetimes in frames. At
  the duel camera the SFX run 3.7 dB above the music (RMS -15.8 against -19.5 dBFS); the SFX-to-music
  balance follows from FoC's data volumes, falloff and equal sliders, not checked against a retail
  recording (unverified).
- capture audio pacing investigation: a capture run paces which tick a frame shows from the frame count (`--eawr-live-step`),
  not from real time, so any host shows the same tick on the same frame. How much *real* time each
  frame takes is not pinned down, though, and Godot's audio engine genuinely mixes in real time:
  a host that draws frames faster reaches more ticks, and so asks BattleAudio to start more sound,
  in the same real second. Sound a slow host spreads over real seconds (each mostly finishing
  before the next starts, near FoC's data balance) instead overlaps far more in Godot's mixer on a
  fast one, reading louder against the music and, at the extreme, clipping the SFX bus itself
  (it has no limiter; only the mix bus does, BA-45). `--eawr-live-audio-pace on` caps the render
  rate to the tick rate (`logical_frames_per_second / step`) so a frame's real duration matches
  the battle's own, on every host; a test that reads `battle_audio.levels` needs it, one that only
  reads the deterministic counts does not. Without it, the RTX 4070 Laptop host read SFX RMS -12.0
  against music -24.3 dBFS (over the +12 dB tolerance) with 28 SFX-bus frames clipping at up to
  +3.3 dBFS; the rig (GTX 970) stayed within tolerance at its own, slower, natural frame rate. With
  `--eawr-live-audio-pace on`, three runs each: the rig read SFX RMS -14.27 dBFS against music
  -19.37 dBFS (+5.1 dB), the laptop -14.28 against -19.36 (+5.1 dB) -- the two hosts agree to a tenth of
  a dB, and both sit closer to the doc's own -15.8/-19.5 duel-camera reference than either host's
  unpaced reading did.

### Samples

| ID | Rule | Evidence |
|---|---|---|
| BA-30 | Every SFX sample in FoC is a 16-bit PCM WAVE (mono or stereo, 11025, 22050 or 44100 Hz) under `Data/Audio/SFX`; the viewer hands the PCM to Godot as it is. Music is MP3 under `Data/Audio/Music`, decoded by Godot's MP3 stream. A sample that is missing or does not decode is listed in the report and its voice freed; nothing is dropped silently. | data survey (12,007 WAV, 2,151 MP3) |

## Event playback lifecycle

SND-03/04/05/08/09/18 separate an admitted event from its current sample. Admission
returns a presentation identifier before decoding or allocating a voice. Delayed events
count toward the 2D instance and overlap limits; fading events do not. Repeating events
already attached to the same source are refused. Attached replacement detaches before
admission (SND-19), including queued events and runtime chains sourced by that object.

Every event loop draws volume, pitch, 2D pan and pre/post delays, then progresses through
predelay, optional pre sample, main sample, optional post sample and postdelay. A positive
predelay enters a waiting stage; initialization with zero predelay and predelay expiry
select main directly when no pre sample is authored (SND-08, debug build). The selected
sample starts on the following service, without a separate empty pre-stage wait.
`RHD_Battle_End` and `RHD_Defeated` inherit `Preset_HUD`, which authors neither predelay
nor pre samples; both therefore use zero predelay and start their main sample on that
next service. Their outcome request remains tied to the deciding frame (WBF-38).
Positive play counts count completed event loops; an infinite main sample loops continuously in
the backend. Pre and post samples remain finite. Sequential pre/main/post cursors advance
independently and equal-sized stage lists synchronize to the main cursor at admission.
Elapsed presentation milliseconds drive delays; spatial events and infinite 2D events
retain their stages through game pause. Each service makes at most one stage transition
per event, preventing a stalled frame from draining repeated empty or failed samples.

Natural event completion releases a runtime 2D chain ahead of the authored
`Chained_SFXEvent`. The destination re-enters ordinary admission. Cancelled events
cannot release a chain; a removed assist source clears its runtime chain before
completion. Stealing releases the former event's slot and completes its current loop,
retaining finite repeats and chain precedence without controlling the new slot owner.
Backend failure likewise completes the admitted loop and retains runtime-before-authored
chain precedence; admission refusal never creates an event to complete.
The report distinguishes admitted events, allocated samples and decoded starts.

SND-11 attached spatial events refresh their height-adjusted visible pose and logical
fog/arrival-hidden state while queued and playing. Explicit attached silence can be
supplied through the presentation interface. Story cinematics bypass fog, while explicit
silence and model hiding still mute. Removal detaches queued and playing events, including
engine loops and construction-owned continuous playback. Spinning dead copies retain
their published pose until their end notification. Fixed-position and ordinary 2D samples
do not inherit this attached gain multiplier. Fogged sources without a published visible
pose retain their snapshot position; exact hidden pose interpolation remains unverified.

The private cross-check refreshed SND-E02/E03/E21/E33/E34 and the spatial allocation
policy in the debug build. The effective XML includes pre/post speech and creature
samples; custom space overlays exercise these stages without redistributing retail assets.
The 2D panner uses the backend's pan effect per slot; its acoustic curve is not claimed
to match the original library.

## Base warning and construction unlock

SND-40 base warnings read the existing firing notifications, without requiring damage,
selection or fog visibility. A `DUMMY_STAR_BASE` or `DUMMY_ORBITAL_STRUCTURE` target
must belong to the local player, or be allied community property, and the attacker must
be hostile. One logical-frame cooldown covers all bases: the authored
`Delay_Between_Space_Base_Attack_Announcement_Seconds` is truncated after multiplying
by 30. The timer resets before the local faction's
`SFXEvent_Space_Base_Under_Attack_Announcement` is attempted. This is an ordinary WAV
SFX request, not a production speech stream. An authored event also places the
`radar_blip` component's `Icon_Texture_Name` (`i_radar_focus.tga` in stock) at the base's
notification position for four presentation seconds, even when sound admission fails.
The marker retains its texture colour without player tint; its alpha and size pulse
at the debug build's observed rate. No world highlight or object tracking is requested.
The platform captures base attack ownership, type and position in its bounded presentation
journal before snapshot eviction. Audio services the cooldown and consumes those immutable
notifications in logical tick order, so a render stall preserves the warning/reset tick.
The journal's existing tick/byte bounds and explicit gap reporting also cover these rows.

The viewer covers published weapon-fire notifications; attack paths without an existing
fire notification remain outside this presentation observer.

SND-68 is a qualified construction unlock, not a universal station-upgrade announcement.
The debug build requires requested unlock processing and notification, plus an affiliated
type with `Build_Initially_Locked` and `Build_Can_Be_Unlocked_By_Slicer`, below the
resulting tech level and absent from the player's unlocked list. Only a newly added type
allows local faction `SFXEvent_New_Construction_Options_Available`. The skirmish economy
has no slicer unlocked-list state; ordinary station or tech upgrades therefore retain
their own cues and stay silent on this route. The qualified unlock consumer belongs to
the galactic implementation (legacy EAWR-1644).

## Not modelled (fidelity list)

- Waypoint command semantics and unsupported land/galactic refusal routes (BA-27/28).
- Cinematic-focus engine loops and continuous `SFXEvent_Ambient_Loop` playback (SND-U05).
  Custom space map beds remain unverified; stock space has no configured bed (SND-47).
- Exact backend panning law, distance/clipping parity and an audible retail loop/chain comparison
  remain unverified (SND-U02); lifecycle tests establish timing and stage order.
- A volume slider UI (the defaults of BA-45 play) and the superweapon music override.
- BA-24's leader resolution reads `LiveSessionView::squadron_members()`, built once from the
  start's tick-zero squadrons (battle world UI and squadron selection): a squadron a spawner launches after tick zero (space-fighters
  FL rules) is not in that map yet, so selecting one plays no response line until the viewer tracks
  launched squadrons the same way the selection and world UI already need to (shared gap, not new
  here).
