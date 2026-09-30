# Space battle audio (P2-21, EAWR-84)

## Applicability

What FoC plays for a space battle: the sound events of shots, hits, deaths and hardpoint deaths,
the unit responses to the local player's selections and orders, and the ambient and battle
music. The viewer plays them from the live session's presentation events
(`--eawr-live-session`, EAWR-370), so the audio can never change the simulation: the session's hashes
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
| BA-13 | A sound's volume is its drawn volume times the object's visibility (0 when fogged or hidden). While a speech stream plays FoC caps it at 0.3; the battle plays no speech streams yet. | AU-13 |

### Battle events

| ID | Rule | Evidence |
|---|---|---|
| BA-14 | A shot plays at the new projectile's position: a hardpoint's `Fire_SFXEvent`, else its parent type's `SFXEvent_Fire`, when `Projectile_Appearance_Delay_Frames` is below 1 (no FoC hardpoint sets it); a unit's own weapon plays the type's `SFXEvent_Fire`. The firer's visibility decides BA-07. | AU-14, AU-15 |
| BA-15 | A projectile hit plays at the contact point: `Projectile_SFXEvent_Detonate_Reduced_By_Armor` when the hull armor multiplier was at most 0.75 and the projectile names one, else `Projectile_SFXEvent_Detonate`. A hit the shield took whole counts as multiplier 1 (BP-13) and plays the plain detonation. | AU-16 |
| BA-16 | A destroyed unit plays its `Death_SFXEvent_Start_Die` at its position. | AU-18 |
| BA-17 | A destroyed hardpoint plays its `Death_Explosion_SFXEvent` at the hardpoint's world position, else the `Death_SFXEvent_Start_Die` of its `Death_Breakoff_Prop` type. | AU-17 |
| BA-18 | Battle sounds are heard when the local player sees the unit, as the battle effects are drawn (BP rules of `battle-presentation.md`); an event fires on the first frame whose presented tick reaches its tick minus one. | viewer rule, mirrors EAWR-370 |

### Unit responses

| ID | Rule | Evidence |
|---|---|---|
| BA-20 | One unit speaks for a selection: the one whose first category in `GAMECONSTANTS.XML` `Unit_Command_Rankings_By_Category` comes first; a later unit with a lower `Ranking_In_Category` (default 25) takes over whatever its category, as FoC's loop is written. | AU-19, AU-20, AU-21 |
| BA-21 | A selection by click, drag box, double click (type on screen) or control group plays the speaker's `SFXEvent_Select`. | AU-19, AU-23 |
| BA-22 | A move order plays `SFXEvent_Group_Move` when more than one unit is selected and the type has one, else `SFXEvent_Move`; an attack order `SFXEvent_Group_Attack` or `SFXEvent_Attack` the same way. | AU-22 |
| BA-23 | Unit responses are 2D (`Preset_UR`: one instance, `Overlap_Test` per unit type), so a second response of the same type while one plays is refused. | data, BA-04, BA-05 |
| BA-24 (EAWR-499) | A squadron's team container carries no response sounds or ranking category of its own (every M2 squadron: the craft carry those, the squadron entry does not). Selecting or ordering a squadron therefore ranks and sounds as its leading live craft: FoC's category ranking (BA-20) and its select/move/attack sound both resolve a selected object that belongs to a team to that team's current leader before reading its own sound and ranking fields (debug build), so an X-Wing squadron speaks with the X-Wing craft's own lines. FoC keeps its leader slot live by reassigning it the instant the leader dies; the remake instead walks the squadron's fixed tick-zero roster order and skips a craft once it leaves the tactical snapshot, landing on the same craft in practice. The group-move/group-attack sounds are read from the resolved craft's own type with no further resolution (no M2 craft or squadron sets them, so BA-22 always falls through to the resolved single-unit sound for a squadron). | AU-32 |

### Ability sounds (EAWR-559)

An ability has two sounds in FoC and they come from two places (debug build, evidence AU-33 to AU-36):
the faction's *toggle sound*, played when the switch happens (the S-foils' "swhoong", the shields
rising), and the unit's *voice line*, played when the command bar's button or key is pressed (the
pilot shouting to lock the S-foils, the Nebulon-B's officer engaging the shields).

| ID | Rule | Evidence |
|---|---|---|
| BA-50 | **Toggle sound.** When an ability switches on or off, the owner's faction plays the `SFXEvent_GUI_Toggle_Non_Hero_Ability_On` or `_Off` entry it lists for that ability (`FACTIONS.XML`, one `<ability>, <event>` row per ability), once per switch however many units it carried (a squadron's craft, a selected group), as a 2D sound. The Rebel rows: `DEFEND` `GUI_Toggle_Shields_On`/`_Off` (`GUI_Shields_Up_1`, `GUI_Shields_Down_1`), `TURBO` and `SPOILER_LOCK` `GUI_Toggle_Turbo_On`/`_Off` (`GUI_Toggle_17`, `GUI_Toggle_18`); the Empire's `TURBO` and `DEFEND` rows are the same events. `POWER_TO_WEAPONS` (the row is `BARRAGE`, blank) has none. A sound plays only when the switch happened: a refused command plays none. When the switch is an enemy's (another team than the local player's) the faction's `SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_On`/`_Off` entry plays instead, and every one of those is blank, so an enemy's abilities are silent; an ally's are heard, fog or no fog. An AI or Lua `Activate_Ability` plays the same toggle sound as a button (the event it builds is started with the same sound flag as the button's in every caller traced; only the refusal sound is left to the button), so the local player's own script-driven `DEFEND` is heard. | AU-33, AU-34, AU-35, data |
| BA-51 | **A timed ability that runs out is silent.** FoC's expiry handler deactivates the ability and starts the faction's *enemy* off-event, not the plain one, for the local player's units; every enemy off-entry is blank in the M2 data. Only a switch-off (button, script) plays the off sound. The remake tells the two apart from the snapshot: a timed ability (one that has read frames left while active) whose last read, the final active tick's 0 included, had two frames or fewer left and that is then off ended by itself; an untimed ability's end always plays the off sound. A switch-off within the last two frames of a timed ability is taken for a natural end, silently. A switch that the engines or the shield end (AB-16, AB-17) is not told apart from a switch-off and plays the off sound (**unverified**: the debug build's deactivation for those was not traced). | AU-36 |
| BA-52 | **Voice line.** A press of an ability's button or key that reaches the simulation plays a voice line from the first pressed unit that has the ability (a squadron resolves to its leading live craft, BA-24): its `SFXEvent_GUI_Unit_Ability_Activated` when the press switches the group on (not every unit was on), its `SFXEvent_GUI_Unit_Ability_Deactivated` when it switches it off. The line is a plain 2D start of the event, not a unit response: it takes no part in the speaker ranking (BA-20) and no acknowledgement follows it. The events are `Preset_UR` ones (BA-23: one instance, `Overlap_Test` per unit type), so a line is refused while another line of the same type plays, and there is no cooldown beyond that. The M2 lines: the Corellian corvette's `TURBO` `Unit_Speed_Corvette`, the Nebulon-B's `DEFEND` `Unit_Defend_Nebulon`, the X-wing's `SPOILER_LOCK` `Unit_Ability_On_X_Wing` and `Unit_Ability_Off_X_Wing`, the Tartan's and the Acclamator's `POWER_TO_WEAPONS` `Unit_Barrage_Tartan` and `Unit_Barrage_Acclamator`; no other M2 ability has a deactivation line. Script and AI switches play no voice. | AU-33, AU-37, data |

Not modelled: the negative-feedback sound FoC plays for a press on an ability that is recharging (or
that no selected unit has); the press is refused here without a sound. The ability-active loop
(`SFXEvent_Special_Ability_Loop`) is not authored by any M2 ability.

### Music

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

### Viewer notes (EAWR-443)

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
- EAWR-474: a capture run paces which tick a frame shows from the frame count (`--eawr-live-step`),
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

## Not modelled (fidelity list)

- Assist responses (`SFXEvent_Assist_Move`, `SFXEvent_Assist_Attack`), chained after the speaker's
  line in FoC from a random selected unit of another type; `SFXEvent_Stop`, `SFXEvent_Guard`,
  hardpoint-targeted attack lines and the nebula/asteroid move lines, and the negative-feedback
  sound of a refused ability press (BA-52).
- Engine loops (`SFXEvent_Engine_*_Loop`, `SFXEvent_Ambient_Loop`), fly-bys
  (`SFXEvent_Ambient_Moving`), `Spin_Away_On_Death_SFXEvent_Start_Die`, health warnings, HUD and
  advisor speech and the ambient map beds.
- Pre/post samples, predelay and `Chained_SFXEvent` (no battle event of the M2 roster uses them).
- A volume slider UI (the defaults of BA-45 play) and the superweapon music override.
- BA-24's leader resolution reads `LiveSessionView::squadron_members()`, built once from the
  start's tick-zero squadrons (EAWR-424): a squadron a spawner launches after tick zero (space-fighters
  FL rules) is not in that map yet, so selecting one plays no response line until the viewer tracks
  launched squadrons the same way the selection and world UI already need to (shared gap, not new
  here).
