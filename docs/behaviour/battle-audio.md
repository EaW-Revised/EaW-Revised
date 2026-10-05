<a id="space-battle-audio-p2-21-84"></a>

<a id="space-battle-audio-p2-21"></a>

# Space battle audio

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

### Moving ambience

| ID | Rule | Evidence |
|---|---|---|
| BA-80 | A live craft with `SFXEvent_Ambient_Moving` initializes a uniform delay from its current logical frame. The authored `SFXEvent_Ambient_Moving_Min_Delay_Seconds` and `_Max_Delay_Seconds` default to 5 and 10 seconds (150..300 frames); positive, ordered values are required. When due, only a team's current leading live craft services the cue. A locomotor with movement remaining requests the event attached to that craft; idle orbiting alone does not qualify. Reschedule from the current frame even when stationary or playback is refused. | SND-46; fresh debug-build path/idle/combat cross-check |
| BA-81 | Moving ambience uses the ordinary event's finite play count, random sample/pitch/volume, instance admission and 32 spatial voice limit. Its attachment follows the interpolated height-adjusted craft pose; ongoing local fog or hidden arrival sets gain to zero. Removal stops the attached voice and retires its timer. Timers and random delay state are presentation-only, bounded by live authored craft; movement presence is published during the existing snapshot pass, outside canonical state. | SND-11, SND-46; BA-05/08/09 |

Stock X-wings and TIEs author flyby events rather than normal idle/moving engine loops.
Their moving event uses `Preset_EGB`: priority 5, two instances, volume 70%, saturation
distance 400, pitch 95..105% and one play. TIE events override pitch to 100..110%.
Cinematic-focus loops use a separate retail path and do not replace ordinary flybys.
The report's `battle_audio.ambient` records timer work, due ticks, movement eligibility,
follower skips and attachment updates. Delay randomness is independent of voice admission.

The attached-event retail overload admits hidden cues and applies ongoing zero gain;
the viewer still uses its generic start-time hidden refusal (SND-04). Story-cinematic
visibility bypass and explicit attached-audio silencing remain outside this live path.
Audio clips verify audible output; XML and admission counts alone establish no retail mix parity.

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

Not modelled: the negative-feedback sound FoC plays for a press on an ability that is recharging (or
that no selected unit has); the press is refused here without a sound. The ability-active loop
(`SFXEvent_Special_Ability_Loop`) is not authored by any M2 ability.

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
| BA-74 | Speech files resolve from the queued language's speech directory. Authored production speech can name PCM WAV files as well as MP3; the stream route, not the filename extension, determines speech ducking. The current viewer supports English and preloads reachable production speech at preparation. Text/movie presentation and other languages remain unimplemented. | SND-E26; SND-62/69; XML/sample header SA-E01 |
| BA-75 | The inspected music start/update routes apply authored track volume, fade level and music/master sliders without a speech-active predicate or 0.3 ceiling. The shared category-gain getter only multiplies its slider by master. The sourced speech cap belongs to SFX; this change does not add a music cap. | debug build SA-E03 |

The viewer reads existing logical visibility from snapshots; no new simulation facts or
canonical bytes are needed. Stable observation uses the sourced 30-frame fog-service cadence
with one presentation phase for the roster. The original per-object phase and accelerated
service while the fog fade moves are not reproduced by this observer; this timing difference
remains a fidelity gap. Type/enemy eligibility never resets on hiding or re-reveal.
The speech queue has 256 preallocated entries; saturation increments a report counter and
drops the new request. This bound is viewer policy, not an original-game cooldown.
The report includes queue completion/failure/overflow counts and capped SFX start/update counts.

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

## Not modelled (fidelity list)

- Assist responses (`SFXEvent_Assist_Move`, `SFXEvent_Assist_Attack`), chained after the speaker's
  line in FoC from a random selected unit of another type;
  nebula/asteroid move lines, and the negative-feedback
  sound of a refused ability press (BA-52).
- Engine loops (`SFXEvent_Engine_*_Loop`, `SFXEvent_Ambient_Loop`), health warnings, HUD and
  advisor speech and the ambient map beds.
- Pre/post samples, predelay and `Chained_SFXEvent` (no battle event of the M2 roster uses them).
- A volume slider UI (the defaults of BA-45 play) and the superweapon music override.
- BA-24's leader resolution reads `LiveSessionView::squadron_members()`, built once from the
  start's tick-zero squadrons (battle world UI and squadron selection): a squadron a spawner launches after tick zero (space-fighters
  FL rules) is not in that map yet, so selecting one plays no response line until the viewer tracks
  launched squadrons the same way the selection and world UI already need to (shared gap, not new
  here).
