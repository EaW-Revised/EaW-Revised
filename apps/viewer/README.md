# Production Godot viewer

With a FoC installation, map and effect modes mount `corruption/Data` over
`GameData/Data` and select the FoC catalog by default. `--eawr-profile eaw`
selects the base EaW files for regression fixtures; `foc` and `remake` are
also accepted explicitly (`remake` requires `--eawr-mod-root`).

`apps/viewer` is the production bootstrap for the RenderingServer renderer. Godot owns
the window, graphics device, swapchain, resize dispatch and shutdown. One `ViewerHost`
node owns the adapter; simulation entities are RenderingServer instance RIDs, not scene
nodes.

The project selects Forward+ on Vulkan (with D3D12 fallback on Windows).
`rendering/rendering_device/fallback_to_opengl3` keeps Compatibility as an
unpinned, untested emergency fallback for one milestone (EAWR-153). No viewer
capture or qualification result is pinned to Compatibility; the retained code
and its later deletion point are listed in [rendering.md](../../docs/rendering.md).

## Prop idle clips

A populated land or space map plays each animated placement's `Idle_Anim_00`
clip from its own start frame, a hash of its TED record, at the XML
`Idle_Anim_00_Rate_Mod` and loop setting (EAWR-145 space, EAWR-157 land,
`docs/asset-formats.md#idle-clip-playback`). With `--eawr-camera-interactive`
the clip clock follows real time. A fixed capture holds it at the particle-frame
count. `--eawr-map-idle-offset <ticks>` adds 30 Hz ticks to the clock, so a
series of captures at offsets 0, 30, 60, ... shows the Coruscant asteroids and
junk, or the Naboo comms array dish, turning second by second. It requires
`--eawr-populate`. The report's `populate.unit_animation` names each animated
land object type with its clip and playback. Map particles run on the same
clock, the attached effects on a space map (EAWR-186) and the land map emitters
(EAWR-196): one 1/30 s sample per frame in a fixed capture
(`--eawr-map-particle-frames` sets the count), real time in the live view.
`map_particles.clock` reports `held` or `live`. The environment's Planet and Nebula
effects take their `TIME` from that clock too, at retail's 0.03 s per tick (EAWR-185), so
the Coruscant nebula waves and drifts. `space.effect_clock` reports the clock, its
tick at capture and the effect time.

## Bloom

Land and space maps draw the retail `SceneBloom` pass with the selected environment
record's (default 0, see below) strength, cutoff and size (EAWR-201, `docs/rendering.md#bloom`), like retail at High
and Highest. `--eawr-bloom off` turns it off for a before/after comparison; the
report's `scene_bloom` records the status and values. Only a lit scene blooms: without
`--eawr-lighting` there is no bloom (`scene_bloom.status: lighting_off`) and
`--eawr-bloom on` is refused (EAWR-307). Unit, effect and model
modes and the space evidence harness never bloom. With bloom on, a land capture
also writes `<capture>.bloom_off.png`, the configured scene without bloom, and
its comparison captures and pixel evidence are unbloomed.

## Environment record

Retail draws one of a map's TED environments per battle at random (R-SEL-03 in
`docs/behaviour/p1-effective-environment.md`); the viewer uses record 0.
`--eawr-environment-record <N>` picks record N instead, so an eye check can match
the environment a retail capture drew (Naboo: 0 `Sunrise_Clear`, 1 `Noon_Clear`).
It selects the land skydome and, with `--eawr-environment map`, the lighting, the
shadow colour (`0x17`, [shadows](../../docs/rendering.md#shadows)) and the wind. A
record past the map's count fails the run; a space map takes only 0, as its view
composes record 0. The report's `lighting.environment` names the `record` and
`record_name`; `lighting.shadows.shadow_floor` is the RGB floor.

## Foliage wind

With `--eawr-environment map` a populated land map takes the wind of that TED
environment record (speed `0x2c`, heading `0x2b`), and its `Tree.fx` trees and
`Grass.fx` ground cover sway in it (EAWR-147, `docs/behaviour/vegetation-effects.md`
W-01..W-09). The scene clock is the idle clips' clock in seconds: held in a fixed
capture (so `--eawr-map-idle-offset` steps the sway and a capture repeats), real
time in the live view. Without a map environment there is no wind and nothing
sways. The report's `populate.wind` gives the wind, the clock and the number of
tree and grass surfaces; placements that sway have their evidence bounds widened
by how far they can move.

## Space hull shadow evidence

`--eawr-space-place-object <FoC SpaceUnit XML id>[@<spawn record index>]` is a
debug capture option for populated space maps. It creates one inert unit at a
catalog spawn marker through the normal space population upload. The default
index is Coruscant's player-one marker record 55, used by
`project/config/coruscant-space-map-camera.xml`; other maps need an explicit index.
A FoC StarBase id (`Rebel_Star_Base_1`) takes a `Team_NN_Space_Station`
marker record instead (Coruscant: 48 for team 0, 54 for team 1). Either stands
with its marker's own position and orientation, as a skirmish start places it
(R-ROT-04); the EAWR-199 station eye checks use the station form. No gameplay spawn or
simulation state is created. Without the option, the map composition is
unchanged.

`--eawr-space-place-at <FoC SpaceUnit XML id>@<x>,<y>,<z>,<yaw>` (EAWR-70, repeatable up
to 256 times) places inert units at explicit TED source positions and TED placement yaws (degrees) through
the same upload. The placement transform applies FoC's fixed +90 degree model turn (EAWR-288), so a
TED yaw is the heading drawn: a trace's facing yaw (0 along +X) is passed unchanged. It serves, for example, the samples of a movement trace
(`sim_headless --scenario`, [traces.md](../../docs/traces.md)) drawn as a trail. It needs
`--eawr-populate` on a space map and is not composed with `--eawr-space-place-object`; the
report's `populate.placed_ships` counts them.

`--eawr-live-session melee` (EAWR-601) plays the melee benchmark on the same map: both sides' ships and
squadrons within weapon range and set on each other, with no station or map object
([battle bench](../../docs/performance/battle-bench.md)). The viewer builds the fight exactly as
`path_bench --melee` does (`skirmish::build_melee`), `--eawr-live-melee s|m|l` (default `s`) and
`--eawr-live-melee-seed <n>` (default 601) choosing it, and then runs it as a replay session.

`--eawr-live-session skirmish` (EAWR-908)
plays any FoC `_mp_space_*.ted` with exactly two players, including maps authored
for larger lobbies. More than two **players** are refused; map capacity is not a
restriction. With no options, the map, lobby, SK-22 fleets and seed are M2's
unchanged defaults; `m2` remains the pinned fixture mode.

```powershell
& "<Godot console executable>" --resolution 1280x720 --path apps/viewer/project -- `
  --eawr-game-root "<game root>" --eawr-live-session skirmish --eawr-populate `
  --eawr-skirmish-map data/art/maps/_mp_space_bespin.ted `
  --eawr-camera-interactive --eawr-environment map --eawr-lighting sh --eawr-shadows on
```

- `--eawr-skirmish-map <logical path>` selects the map (also accepted as `--eawr-map`).
- `--eawr-skirmish-players <a>,<b>` selects two increasing player slot IDs, default `1,2`.
  Put it before per-slot flags when changing IDs; a third player is refused.
- `--eawr-skirmish-slot <slot>:<faction>:<team>:<human|ai>` changes a player's lobby
  choices, e.g. `1:Empire:0:human` and `2:Rebel:1:ai`. Factions must be playable;
  teams must have enough spawn/station markers. Player IDs select lobby colours;
  team IDs select `Team_NN` markers by the existing SK-03/SK-11 rules.
- `--eawr-skirmish-fleet <slot>:<unit>,<unit>,...` replaces that slot's SK-22 fleet;
  `1:none` removes the extra fleet. Faction changes keep the slot's fleet unless
  this flag changes it. Defaults are slot 1 `Y-Wing_Squadron,Corellian_Corvette,
  Nebulon_B_Frigate,Calamari_Cruiser` and slot 2 `Tartan_Patrol_Cruiser,
  Acclamator_Assault_Ship`. The data-driven free starting forces and pre-built
  stations still follow the selected faction.
  Selected fleet and faction types outside M2 load through the existing unit
  loader, including their craft, hangar, hardpoint and projectile dependencies;
  a type with unsupported or missing data is reported by the normal loaders.
- `--eawr-skirmish-seed <uint64>` sets the deterministic seed, default `67`.
  These flags require `--eawr-live-session skirmish`; `--eawr-live-ai off` disables
  AI controllers for a staging run. The first human is the local player unless
  `--eawr-live-player <slot>` selects another player.

Without `--eawr-map-camera-config`, a live start derives target bounds from the
map's declared extents and frames the centre of the local fleet placed around
its spawn (PL-01/PL-03), at distance 1200 and XML `Yaw_Default`, using the existing
owner camera overrides. Empty fleets frame the spawn. Explicit camera XML,
including `config/coruscant-live-session-camera.xml`, retains precedence; a
fixed capture camera still overrides the frame. Missing extents/spawn markers
produce a clear refusal. [SC-01/SC-02](../../docs/behaviour/skirmish-map-choice.md)
describe the project policy.

The effective FoC map placements and their XML definitions give this census.
**Native 1v1 maps:** Bespin, Endor, Geonosis, Kessel and Polus (two declared
players and two spawn markers). All 24 maps below contain asteroid fields;
there is no hazard-free map in this stock list. The three plain smoke maps are
Bespin, Kessel and Polus: asteroid fields but no nebulas, ion storms or mines.
Asteroid field models draw, but field collision/damage is not simulated.
Nebulas and ion storms are classified but neither drawn nor simulated; shield,
weapon and movement effects are absent. No placed mine type was found in these
maps; mine behaviour remains unsupported. Mineral extractor pads are not mines.
Some maps also use unsupported environment materials: Bespin has no primary
stars and its secondary skydome is currently reported as undrawn, so its battle
opens against the viewport background. Map selection does not add shader routes.

| Map (`data/art/maps/_mp_space_<name>.ted`) | Spawn markers | Unsupported hazards present |
| --- | ---: | --- |
| alderaan | 6 | asteroid fields, ion storms (nebula volumes) |
| bespin | 2 | asteroid fields |
| bothawui | 6 | asteroid fields, nebulas |
| coruscant | 6 | asteroid fields, nebulas, ion storms (nebula volumes) |
| dagobah | 6 | asteroid fields, nebulas, ion storms (nebula volumes) |
| dathomir | 4 | asteroid fields |
| endor | 2 | asteroid fields, ion storms (nebula volumes) |
| felucia | 9 | asteroid fields, nebulas |
| geonosis | 2 | asteroid fields, nebulas |
| hoth | 8 | asteroid fields, nebulas |
| hypori | 6 | asteroid fields |
| kamino | 9 | asteroid fields |
| kashyyyk | 10 | asteroid fields, nebulas, ion storms (nebula volumes) |
| kessel | 2 | asteroid fields |
| kuat | 6 | asteroid fields |
| naboo | 6 | asteroid fields, ion storms (nebula volumes) |
| polus | 2 | asteroid fields |
| ryloth | 6 | asteroid fields |
| saleucami | 24 | asteroid fields |
| shola | 6 | asteroid fields |
| tatooine | 6 | asteroid fields, nebulas |
| themaw | 6 | asteroid fields |
| utapau | 6 | asteroid fields |
| yavin | 8 | asteroid fields |

`--eawr-live-session m2` (EAWR-80 part A) runs the authoritative tactical session of the M2
skirmish (`plan/phase-2/m2-skirmish.md`) on `_mp_space_coruscant.ted` with `--eawr-populate`.
The session steps on its own simulation thread with the EAWR-276 worker pool
([simulation.md](../../docs/simulation.md#game-build)); the viewer draws its units from the
published snapshots through the placed-ship upload path, interpolated between the two newest
ticks, and hides the units the local player (`--eawr-live-player`, default the human slot)
does not see. The map objects the session owns leave the static scene. A squadron's container
has no model; its craft are drawn, those of the tick-zero squadrons and, from model slots
composed for every SK-23 launch, the craft launched later (EAWR-79: each takes the first free slot
of its type when it first shows; the report's `launch_slots` and `launched_drawn`; `populate.live_units` counts the slots apart from the session's units, as `launch_slots_drawn` and `launch_slots_not_drawn`). The units' own particle proxies follow them (EAWR-394, `unit_emitters` in the report): engine
emitters while their engines are online, a hardpoint's damage emitters once it is destroyed
([battle presentation](../../docs/behaviour/battle-presentation.md) BP-40 to BP-46);
`--eawr-map-effects off` turns them off. The FoC tactical AI (EAWR-79) commands the Empire, the
non-human lobby player: its retail space freestore script runs beside the world and its orders
join the replay ([FoC tactical AI](../../docs/behaviour/foc-tactical-ai.md#79-host)); the
report's `ai` lists its players, Lua load and the engine calls it could not make. Options:

- `--eawr-live-deploy-overlay on|off` (default `off`, EAWR-563): draws the fog plane as FoC's red deployment overlay
  (space-fog-presentation.md FW-22, FW-23) instead of the white fog: fogged cells and the map's border in
  `SpaceReinforceFOWColor` on the reinforcement tile. `live_fog.deploy_overlay` records it. FoC shows it only in the
  reinforcement pane; M2 enables it while the pane or a reinforcement drag is active. The switch also exposes it
  (`FogField::set_deployment_overlay`'s blocked-point function).
- Unit fog fade (EAWR-535, FW-16 to FW-21, always on except with `--eawr-live-reveal on`): the report's
  `live_session.fading_units`, `fading` (every drawn unit's opacity now) and `fading_log` (one row a tick a unit is
  below full opacity) read it.
- `--eawr-live-ai on|off` (default `on`): the FoC AI of `m2` and `skirmish`. A `replay` session never runs it;
  the recorded AI commands replay as they are. Passing it with a session kind other than `m2` or `skirmish`
  has no effect: it prints a warning and the report's `live_session.ai_flag_ignored` is `true`.
- `--eawr-live-reveal on|off` (default `off`, a viewer debug aid): draws every unit and effect
  (engine and damage emitters, breakoff props) regardless of the local player's fog, so the
  report's `live_session.hidden_units` is `0`; the minimap and the HUD's unit list
  (`--eawr-live-player`'s `visible_units`) follow it too, and every currently-drawn unit not owned
  by the local player lists in `live_session.hostile_units`. It is presentation-only: with
  `--eawr-live-session m2` it never touches the live session's sensor state, so the FoC AI's
  decisions, its commands and the session's hashes are unchanged (`live_session.reveal` records
  it in the report either way); with `--eawr-live-session replay` it instead gives every unit type
  of the recorded setup the revealed sensor range before the replay runs, as the fixed-force
  recordings stage fog revealed (a replay only replays recorded commands, so this does not change
  what the recording does). Selection and order input still read each unit's ownership, not
  reveal, so a revealed enemy cannot be selected or ordered (EAWR-82,
  [battle selection](../../docs/behaviour/foc-battle-selection.md)).

- `--eawr-live-order <tick>:<move|face>:<unit>@<x>,<y>,<z>`, `<tick>:stop:<unit>`,
  `<tick>:damage:<unit>@<amount>[,<hardpoint>]`, `<tick>:attack:<unit>@<target unit>` or
  `<tick>:<attack_move|guard>:<unit>@<x>,<y>,<z>` / `@<unit>` (EAWR-452, a point or the unit to
  approach or guard) (repeatable): the debug hook injects an order, as the unit's owner, into the next-tick
  command path at that tick. `damage` is the scripted damage of HD-30, to the hull or to
  the hardpoint of that `HardPoints` index; `<unit>` may be `2+3+4`, one command naming several
  units as a selection's order does (EAWR-552); a capture destroys a unit with it to show its
  death clone, or a hardpoint to show its breakoff prop. Unit IDs are those
  `sim_headless --skirmish m2` lists.
- A destroyed unit whose type lists a `Death_Clone` is replaced by that clone at its last drawn
  pose, playing the clone model's `Specific_Death_Anim_Type` clip, `DIE` by default ([unit
  animation](../../docs/behaviour/unit-animation.md)). A clone whose clip cannot start is removed
  at once when its type has `Remove_Upon_Death` and otherwise keeps its pose; a clone with a
  `Death_Persistence_Duration` leaves after it and its fade, and its resources are released.
  The report's `live_session` lists the clips of every start type (`unit_clips`), the clones set
  up (`death_clones`), shown now (`death_clones_shown`) and gone (`death_clones_retired`).
- A destroyed hardpoint whose XML names a `Death_Breakoff_Prop` throws that prop off (EAWR-391,
  [battle presentation](../../docs/behaviour/battle-presentation.md) BP-30 to BP-36): it appears
  at the hardpoint's attachment point with its ship's facing, drifts by `Debris_Movement_Vector`
  and tumbles by `Debris_Facing_Rotate_Vector` every logical frame, carries its
  `Debris_Attached_Particle` fire, and after a lifetime of whole seconds in
  [`Debris_Min_Lifetime_Seconds`, `Debris_Max_Lifetime_Seconds`] (drawn from the event, so every
  run shows the same) shows its `Death_Explosions` and leaves. The report's `breakoff_props`
  lists the props set up (`prepared`), thrown (`spawned`) and gone (`expired`), the events
  whose ship the player did not see (`not_seen`) or whose tick had left the snapshot history
  (`unknown`, EAWR-401), and the fires and explosions that ended before a frame reached them
  (`effects_skipped`).
- `--eawr-live-purchase-slots <N>` (1 to 64, default 10; EAWR-530 test hook): the model slots composed per
  buyable unit type. A slot whose unit is gone goes to the next unit of its type (the report's
  `slots_released`), so a small N shows the reuse without buying past ten squadrons. An
  `--eawr-live-late-orders on` lets an `--eawr-live-order` name a unit the start does not hold (a
  bought one, with an ID above every start unit's); it is given as the local player.
- `--eawr-live-death-anim <TYPE>`, `--eawr-live-death-persistence <seconds>`: test hooks that
  give every death clone this `Specific_Death_Anim_Type` or `Death_Persistence_Duration`.
- `--eawr-live-follow <unit>` (EAWR-447, eye-check clips): every frame the tactical camera looks at
  where the unit was last drawn, live or spinning away after its death, so a clip tracks it
  smoothly; once it is gone the camera holds its last spot.
- The report's `live_session.spin_away` lists each spin-away the frames reached (`unit`, the
  ticks of its `started` and `ended` events), `drawn_max`, the most spinning craft one frame
  placed, and `ships_max`, the most of those with a model (launched craft have none yet) (EAWR-447,
  docs/behaviour/space-fighter-deaths.md).
- The report's `live_session.ion_shots` (EAWR-862) lists each squadron whose `ION_CANNON_SHOT` the
  frames saw on (`switched_on`, `first_on`, `last_on`, `on`) and each unit they saw ion-stunned
  (`first`, `max_frames`); `battle_effects.ability_shots_fired` and `ability_shot_frames_drawn`
  count the ion bolts by the projectile type drawn (docs/behaviour/battle-presentation.md BP-66).
- `--eawr-live-capture-ticks a,b,...` also captures the frames that show those ticks, as
  `<capture stem>_tNNNN.png`; each must be a multiple of the step, which a frame shows exactly.
  `--eawr-live-ticks n` keeps the run going until tick n.
- `--eawr-live-follow-group <tick>:<entity>[,<entity>...]` (repeatable, ticks ascending): from that
  tick the camera eases towards the centre of the named units still alive (a 20-tick time
  constant) and holds when none is left. The EAWR-449 eye-check clips follow a plan's TaskForce with
  it; the entity IDs come from the headless plan timeline (`foc_plan_tests`).
- `--eawr-live-step s` (default 0.5): ticks per frame after the warm-up frames. A capture run
  paces the session from the frame count, so a frame shows the same tick on any host; the
  interactive view (`--eawr-camera-interactive`) runs it in real time.
- `--eawr-live-audio-pace on|off` (default off, capture runs only, EAWR-474): caps the render rate to
  `logical_frames_per_second / step`, so a frame's real duration matches the battle's own tick
  rate on every host. Which tick a frame shows is already host-independent (the step above), but
  how much real time that frame takes is not, and Godot's audio engine genuinely mixes in real
  time: a host that draws frames faster reaches more ticks, and asks BattleAudio for more sound,
  in the same real second, so sound a slow host spreads over real seconds instead overlaps in the
  mixer on a fast one. A test that reads `battle_audio.levels` needs this; one that only reads the
  deterministic counts (`requested`, `results`) does not.
- `--eawr-live-stall <tick>:<ticks>`: a test hook for a presentation stall in a capture run:
  the first frame past `<tick>` presents `<ticks>` ticks later than its step says;
  `start:<ticks>` stalls before the first frame, which then presents tick `<ticks>`. The frame
  after the stall still fires every hit and death of the skipped ticks: the battle effects and
  death clones read the session's event log (`LiveSession::events_after`), not its 64-snapshot
  history. The log keeps only the hits and destructions the view presents, for ten minutes of
  ticks and at most 16 MiB of records (`LiveEventLog`; below 22 MiB with the heap's overhead,
  whatever the event rate). Should it ever drop ticks before a frame reached them, the view
  logs it and lists them in `live_session.event_gaps`. Each late effect is aged by the ticks
  since its own; one whose lifetime is over by then is skipped and counted in
  `battle_effects.expired`.
- `--eawr-live-fault-tick n`: a test hook that throws on the simulation thread before tick n.
  The session stops stepping; the view keeps the last poses and shows the error, the report
  carries it in `live_session.error`, and a capture run fails with exit code 2.
- When the simulation fails (EAWR-615), the viewer writes the session's replay through the failed
  tick (the command log, its setup and the failed tick's commands) beside Godot's `godot.log`:
  `user://logs/eawr-live-failure-<unix seconds>-tick<N>.eawr-replay` (on Windows
  `%APPDATA%/Godot/app_userdata/EAWR Viewer/logs/`), always, without `--eawr-live-replay-out`. The
  error panel and `live_session.failure_replay` name the file; `sim_headless --replay <file>
  --game-root <install>` replays it into the same failure.
- `--eawr-live-particle-workers n` (EAWR-638; default a quarter of the hardware threads, 1 to 4, the
  main thread included): the pool the battle's particle systems (unit emitters, battle effects,
  breakoff props) step and build their streams on; 1 runs them on the main thread alone. The
  streams are the same with any count.
- `--eawr-live-workers n` (default hardware threads minus two), `--eawr-live-hashes <csv>`,
  `--eawr-live-replay-out <file>`: the pool size, the per-tick state hashes and the recorded
  replay. At the end a driven run replays its recording headless and reports
  `live_session.headless_hashes_equal`; `sim_headless --replay <file> --game-root <install>`
  does the same outside it. That replay takes about as long as the battle did, so an interactive
  (real-time) session skips it (`headless_hashes_equal` is null) and closing its window exits at
  once, writing the report (`status: space_environment_closed`); `--eawr-live-verify on|off`
  overrides the default either way. The console build prints `EAWR shutdown +<ms> ms <step>` lines
  for the quit path.
- `--eawr-live-input <tick>:<click|dclick|rclick|hover>:<unit=N[+@dx,dy,dz]|@x,y,z|screen=x,y>`,
  `<tick>:<hover|click|dclick|rclick>:icon=N`, `<tick>:box:@x,y,z/@x,y,z`, `<tick>:box:screen=x,y/screen=x,y`
  or `<tick>:key:<name>`, each with optional `+shift`, `+ctrl`, `+alt` (repeatable, EAWR-82): a player
  gesture replayed through Godot's input queue when the view shows that tick, aimed at a unit's
  projected centre (a squadron craft's own, EAWR-424), squadron N's icon, a source point or a
  viewport pixel; `unit=N+@dx,dy,dz` aims at a source-space offset from unit N's position (EAWR-665).
  A plain `unit=N` on a unit with a collision mesh aims at the centroid of the mesh triangle nearest
  its box centre, not the box centre itself, since that can fall in open space the pick misses (EAWR-665).
  `hover` moves the pointer there and leaves it. The report's `battle_input` says
  what it did and `world_ui` what the frame drew. Run pointer gestures on a GPU host: a window
  manager that resizes the local window (FancyZones) moves the injected window-space events off
  the pinned capture viewport.
  `<tick>:<click|hover|press|release>:hud=<pause|fast_forward|resume|quit>` aims at a HUD control (EAWR-459, EAWR-453),
  as do the production panel's `b_reinforcement`, `r_close`, pane slots `r_RRCC` and queue slots
  `tqueueNN` (EAWR-530). To deploy a completed purchase, open reinforcements, hold the left button on
  its reserve slot, drag the green/red model preview to a clear revealed point, and release. Red
  releases keep the unit in reserve; victory closes the pane and cancels placement (WR-07, WR-13..15).
  Script a drag with `510:press:hud=r_0000`, `515:hover:@x,y,0`, `520:release:@x,y,0`;
  the pointer reaches it one frame before the click. `f<frame>` in place of `<tick>` fires on
  that frame after the warm-up, since a paused battle shows one tick on many frames.
- `--eawr-live-capture-frames a,b,...`: captures by frame after the warm-up, as
  `<capture stem>_fNNNN.png` (EAWR-459: what a paused battle shows).
- `--eawr-perf-trace <csv>` (EAWR-601, [battle bench](../../docs/performance/battle-bench.md)): one row per
  drawn frame of the live session: the frame's wall-clock ms, the engine's process ms (the viewer's own frame work), the root viewport's measured render CPU
  and GPU ms, draw calls, objects and primitives (the renderer's numbers are the previous frame's), and
  the presented tick, the units the local player sees, the projectiles in flight and the particles of
  the battle effects and unit emitters, `particle_ms`, the main thread's ms in the frame's unit
  emitters, battle effects and breakoff props, and `ticks` and `tick_ms`, the simulation ticks
  completed since the previous row and their summed cost (EAWR-638), and `submit_ms` and `pieces`, the main
  thread's ms in the space view's snapshot builds and renderer submit and the pieces submitted, and `sent`, the transforms of those that reached the engine (EAWR-888: only
  the pieces that moved).
  Timers and counters only; the hashes do not change.
- `--eawr-live-speed 0..4` (default 2): the tactical speed setting, 10, 20, 30, 45 or 60 ticks
  a second ([time controls](../../docs/behaviour/tactical-time-controls.md) TM-01).

The time panel's pause and fast-forward buttons work as in FoC (EAWR-459, [time
controls](../../docs/behaviour/tactical-time-controls.md)): pause stops the simulation while the
camera and the interface keep going, shows "Game Paused" with a Resume Game button, and queues
orders for the next tick; fast forward (120 ticks a second) toggles on the press and is disabled
while paused. No key drives them, as in FoC. A driven capture run advances its presented tick by
the step times the rate over 30. Every change enters `live_session.time.track`, and
`--eawr-live-replay-out <file>` also writes `<file>.time.csv`; the replay and the hashes are the
same as without the changes. When the battle is decided the HUD shows "WE ARE VICTORIOUS!" or
"WE HAVE BEEN DEFEATED!" (EAWR-453, [battle end](../../docs/behaviour/battle-end.md)); at `end_tick`
the session halts and an end panel with Quit Game opens (`live_session.battle_end`). A capture
run then keeps drawing the frames it would have taken to reach `--eawr-live-ticks`.

The player controls the live battle as in FoC (EAWR-82, [battle
selection](../../docs/behaviour/foc-battle-selection.md)): left click, Shift+click, Ctrl+click,
double click and left drag select; right click moves or attacks, Ctrl+right click attack-moves
and Ctrl+Alt+right click guards an own unit or a point ([space orders](../../docs/behaviour/space-orders.md)
OR-01); S stops, A, M, T and G arm the attack, move, attack-move and guard modes; 1..9 and 0 recall control groups (Ctrl stores, Shift adds the group, Alt adds the
selection, twice within a second focuses the camera); many wheel clicks out at the farthest zoom,
or Insert, step into the tactical overview and the map overview. The arrows and the screen edge
pan (`space-live-camera-bindings.json`); the middle button and wheel work as on the map camera.
A fighter or bomber squadron selects and takes orders as one unit, also through its icon, and FoC's
selection circles, shield and health bars, squadron icons and hovered ships' hardpoint reticles are
drawn in the world (EAWR-424, [battle UI in the world](../../docs/behaviour/foc-battle-world-ui.md)).

For EAWR-25, `Star_Destroyer` is the shipped FoC Imperial Star Destroyer. Its
idle bind pose at record 55 takes that marker's yaw (149 degrees; before EAWR-288 it
was forced to 90) and is lit by the TED environment sun. Use `--eawr-populate
--eawr-map-effects on --eawr-lighting sh --eawr-environment map
--eawr-shadows on` and the Coruscant camera config for the evidence capture;
repeat with `--eawr-shadows off` at the same camera. With the debug option,
the directional light's shadow range fits the selected hull instead of every
distant placement; the view keeps the space shadow settings of
[rendering policies](../../docs/rendering.md#shadows) (four blended splits,
bias 2, normal bias 5, EAWR-150). Godot's directional shadow range is global to
this scene, so this evidence view also changes shadows on the other placed
objects while the option is active. The map-wide range used too few texels on
the hull. The capture report records the ship, spawn record, shadow range,
mode and bias and labels the range's scene-wide scope. The ordinary populated
Coruscant view keeps its own range, including after a debug view in the same
renderer.

The placed unit and every populated space object start with all hardpoints
intact (EAWR-136, [hardpoint state art](../../docs/asset-formats.md#hardpoint-state-art)).
`--eawr-space-hardpoint-state <HardPoint id>=<intact|damaged|destroyed>`, which
can be repeated, sets one hardpoint of the placed unit through the same hook
that EAWR-72 will drive (`SpacePopulation::set_hardpoint_state`). A destroyed
hardpoint drops its `Model_To_Attach`, shows its `Damage_Decal`, and starts its
`Damage_Particles` emitters. A damaged hardpoint has exactly the same visible
art as an intact one. A destroyed hardpoint also hides sub-objects below its
`Engine_Particles` bone when `Engine_Death_Hide_Engine_Particles` is set. The
hook re-plans the attached effects over the populated scene,
the placed unit included, and the map particle provider starts the newly
admitted emitters and releases the dropped ones. The option's states go through
the hook in order after the first plan, before the first frame.
The report lists the states under `populate.debug_ship.hardpoint_states`.
Every population report counts the damage emitters of intact and damaged hardpoints under
`populate.attached_effects.hidden_by_hardpoint_state`, and the admitted
emitters and those the aggregate budget turned away under `admitted` and
`capacity_exhausted`. Populated Coruscant's own emitters fill the default
budget (`--eawr-map-particle-capacity 8192`, 256 particles each), so the placed
unit's emitters start only with a larger capacity.

## Map frame budget

A map capture renders 30 warm-up frames, then 120 timed frames, and reads the
capture back after the last one. The report's `frame_time` averages the timed
frames. `--eawr-map-timed-frames <n>` (1 to 100000000) changes the timed count;
the Linux software render in CI uses 40 on lavapipe
(`tools/linux_software_render.py`). Map particles need warm-up plus timed frames
above `--eawr-map-particle-frames` (default 60), so keep `n` above 30 with
the default particle frames.

## Render profiles

An interactive run (`--eawr-camera-interactive` without a capture, self-test or
resize test) draws with the enhanced profile: 4x MSAA, SMAA and 16x anisotropic
filtering. Every other run keeps the retail look, with none of them.
`--eawr-render-profile retail|enhanced` forces either one, for example for an
enhanced capture or a frame-time comparison; it is an internal switch, not a
player setting. Mode reports record the applied settings as `render_profile`.
See [rendering policies](../../docs/rendering.md#render-profiles).

## Capture size

A window manager (PowerToys FancyZones, a tiling manager, a maximise) may resize
the Godot window once the first frame runs. A run with `--eawr-capture` pins the
root viewport before any mode starts, so Godot draws at the capture size and only
scales the image into the window. Every probe that reads the viewport back
without a capture pins it too when it activates: the tactical camera, atlas and
runtime exercises, the exploratory Hull preview, map runs (including fog paint
evidence and map-camera terminal and unlocked probes), and effect and unit modes.
Only an interactive run and an unlocked map-camera self-test, whose subject is
following real resizes, draw at the window size. Map and effect modes capture at their fixed
camera (1280x720, or the `--eawr-space-camera` size). Unit and frozen-scene
modes capture at the window size at start-up (`--resolution`). In a report,
`capture_identity.viewport` is the requested size and `capture_identity.png` is
the size of the PNG actually read back (for unit mode, `strip.png` is the strip).
In map, effect and unit modes a read-back of any other size fails the run. The test-only
`--eawr-window-resize-test <w>x<h>` resizes the window on the first frame, after
every mode has activated (`tests/presentation/renderer/test_capture_size.py`).

## Font cache (`--eawr-fonts`)

`--eawr-fonts` is a self-contained check of font provisioning (UI-05 EAWR-191). It mounts the font
cache that `tools/fonts/extract_eaw_fonts.py` writes ([game fonts](../../docs/build.md#game-fonts)).
The cache is `--eawr-font-cache <dir>`, else `EAWR_FONT_CACHE`, else `out/fonts` of the checkout
that holds this project. The mode loads every cached face into an engine font from memory.
It draws each face at 24 and 7 pt, plus six UI-F3 requests (a system face, an unknown face,
game data's lower-case spelling, Russian and Japanese), on the 2D canvas. Sizes follow UI-F1
at the capture height. It writes one `--eawr-capture` PNG. The `--eawr-report` lists:

- each face's bytes, SHA-256 and sfnt names, the names the engine read, and cache warnings
  (`EAWR-UI-0402`);
- whether the system has the Unicode face;
- per line, the face it resolved to, the source (`cache`, `system` or `engine_default`), the
  size, and the ink pixels drawn.

A missing cache is not an error; the lines fall back. `tests/presentation/renderer/test_font_mode.py`
runs a synthetic cache, an empty one and, with `EAWR_EAW_GAME_ROOT`, a cache it extracts on the GPU
host and deletes after the run.

## UI kit gallery (`--eawr-ui-gallery`)

`--eawr-ui-gallery --eawr-game-root <FoC install>` shows the UI kit (UI-06 EAWR-229) skinned from the
FoC data. It builds the theme from the `GUIDialogs.xml` skin, the `MT_CommandBar` atlas and the
font cache (found as for `--eawr-fonts`). Then it draws each kit control in its normal,
mouse-over, pressed and disabled states: frame, push button, a per-control button variation,
check box, radio button, slider, combo box, edit box, progress bar, text roles, list box and small
frame. It writes one `--eawr-capture` PNG.

`--eawr-ui-dialog <IDD_NAME>` builds that catalogue dialog instead, centred as UI-L4 places it.
`--eawr-ui-rules retail` lays it out by the retail rules, which gives the same pixels as a capture
of the original game at the same size. `--eawr-mod-root <mod>` mounts a mod over FoC, so the
kit takes that mod's skin. The `--eawr-report` lists:

- the atlas;
- the cached faces;
- the theme: slot origins, variation counts, built icons and fonts, the font of each role with
  its UI-F1/UI-F2 sizes, and diagnostics;
- every drawn control with its state, variation, rect and the distinct colours in it;
- for a dialog, each gadget's kind, variation and caption.

`--eawr-ui-movie <movies.xml name> --eawr-movie-cache <dir>` plays that HUD movie instead
(EAWR-237, [docs/ui/hud-movies.md](../../docs/ui/hud-movies.md)): the Theora entry the player made with
`tools/ui/convert_hud_movie.py`, looping, at twice its 200-unit HUD slot over a two-tone
backdrop. It needs no skin or fonts. The report's `movie` names the source, the cache key, the
decoded texture size, the distinct frames seen, the loops and whether it plays; a movie that
is not converted fails with EAWR-UI-0705 and one Godot cannot decode with EAWR-UI-0708.
`tests/presentation/renderer/test_ui_movies.py` covers both with a synthetic Theora fixture.

`tests/presentation/renderer/test_ui_gallery.py` runs the gallery at 1280×720 and 1920×1080 on a
GPU host, then the in-game menu by retail rules and the audio options dialog. With
`EAWR_REMAKE_MOD_ROOT` it also builds the Remake's space-battle load dialog. The faces come from
a cache the test extracts on the host and deletes afterwards.

## Tactical HUD shell (`--eawr-hud tactical`)

`--eawr-hud tactical` draws the space tactical HUD shell over a map (P2-20a EAWR-83,
[docs/ui/ui-layer.md](../../docs/ui/ui-layer.md) §1.3): the faction faceplate and help droid,
the radar's scan lines as the (still empty) minimap frame, the options button and the planet
name. `--eawr-hud-faction <empire|rebel|underworld>` picks the `_ALT` variant (default rebel),
`--eawr-hud-rules <aspect|retail>` the layout (default aspect, D4). A live session
(`--eawr-live-session`) draws the HUD by default, for its local player's faction; `--eawr-hud off`
leaves it out. Fonts come from
`--eawr-font-cache`, `EAWR_FONT_CACHE` or the checkout's `out/fonts`, as for the gallery. The
HUD stops the pointer only on component rects and opaque faceplate texels; every other click
reaches the world. The options button counts presses and opens nothing yet (P2-20e). The map
report gains a `hud` object: the shell model, the textures and where they came from, the planet
name and its source, the pixel rects and the diagnostics. `--eawr-hud-probe` pushes left clicks
at the options button, the sky, the minimap, opaque panel art and a transparent gap through
Godot's GUI dispatch and reports which reached the world.

In a live session the command bar shows the selection's unit cards (EAWR-425,
[docs/behaviour/foc-unit-cards.md](../../docs/behaviour/foc-unit-cards.md)): a card per unit or
squadron, grouped by ability, with health and shield bars, stacked into `x<n>` cards when the
selection overflows the 24 slots. A left click on a card selects (Shift deselects) as FoC does;
hovering a card names its type after 750 ms (where FoC opens its encyclopedia). `--eawr-live-input <tick>:<click|dclick|hover>:card=N`
drives a card by slot, and the report's `hud.unit_cards` and `battle_input.unit_cards` list what
was drawn and clicked.

Map captures are 1280×720. `--eawr-ui-gallery --eawr-ui-hud <faction>` draws the same HUD
over the gallery backdrop at the window size (`--resolution`), with `--eawr-ui-rules` and
`--eawr-mod-root` as for the gallery; `--eawr-ui-hud-map` names the map whose planet is shown
(default `_mp_space_coruscant`); `--eawr-ui-hud-cards <type>[*<count>][:<ABILITY>][@<health>][~<shield>],...`
fills its unit cards with a made-up selection. `tests/presentation/renderer/test_tactical_hud.py`
runs both on a GPU host.

## Performance overlay (`--eawr-perf-overlay`, F3)

A dev tool (EAWR-558, [docs/ui/perf-overlay.md](../../docs/ui/perf-overlay.md)): FPS, the frame time with a
graph of the last five seconds (a hitch is a spike), the simulation's cost per tick with the window's
worst tick, and the draw calls and units. **F3** toggles it in any map run; `--eawr-perf-overlay on|off`
(default `off`) starts a run with it shown, for captures. It only reads wall-clock timers, so the tick
hashes do not change, and it costs nothing while hidden. The map report always has a `perf_overlay`
object; shown, it lists `fps`, `frame_ms`, `tick_ms`, their averages and worsts, the tick's phases, the
draw calls and the overlay's own cost (`own_cost_ms`). `tests/presentation/renderer/test_live_session.py`
checks the flag, the key and the report on a GPU host.

## Input routing self-test (`--eawr-input-routing`)

`--eawr-input-routing --eawr-report <path>` checks input routing (UI-07 EAWR-304, rules UI-I1 to
UI-I3 and UI-C1 in `docs/ui/ui-layer.md`) through Godot's real event dispatch. It builds a
synthetic HUD, an edit box and a modal layer at a pinned 1280×720, injects scripted pointer and
key events and records what reached the world layer. It reads no game data and exits 0 when every
step held; the report lists the steps and the commands HUD and world input gave.
`tests/presentation/renderer/test_input_routing.py` runs it on a GPU host.

## Particle EffectMode proxy binding

Use `--eawr-effect <effect logical path>` with `--eawr-effect-proxy-host
<host ALO logical path>` and `--eawr-effect-proxy <proxy name>` to run a V1
EnhancedMesh effect from a named model proxy. Both proxy options are required.
`--eawr-animation <host ALA logical path>` optionally animates the host. The
existing `--eawr-effect-attach <host ALO logical path>:<bone>` option remains
available for ordinary effects. The attachment and proxy selectors cannot be
combined. A missing or duplicate named proxy, a proxy without an immediate
parent mesh, or an invalid mesh binding produces a report diagnostic before
the graphical registry creates resources.

The viewer chooses the first mesh in host model order attached to the proxy
bone's immediate parent. It copies ordered submesh vertices and triangle
indices, then samples one host pose per presentation time. The proxy bone
drives the emitter frame; the selected mesh's bone drives the mesh frame.
Both are source Z-up affine frames. The camera fit, fitted replay probe and
graphical run receive the same owned geometry and frame updates. The report's
`proxy_mesh` block records the selected mesh index and per-frame proxy and owner
origins.

The accepted pins remain Godot `4.7.2-stable` and godot-cpp `10.0.0-stable`. Configure
`EAWR_GODOT_CPP_ROOT` for a source checkout. To reuse an accepted compiled binding cache,
also set `EAWR_GODOT_CPP_LIBRARY` and `EAWR_GODOT_CPP_GENERATED_INCLUDE`; this avoids a
redundant godot-cpp build.

```powershell
cmake -S apps/viewer -B out/build/viewer -G "Visual Studio 17 2022" -A x64 `
  -DEAWR_GODOT_CPP_ROOT=<pinned-checkout> `
  -DEAWR_GODOT_CPP_LIBRARY=<accepted-lib> `
  -DEAWR_GODOT_CPP_GENERATED_INCLUDE=<matching-generated-include>
cmake --build out/build/viewer --config Release --target eawr_viewer
<godot-4.7.2-console> --headless --path apps/viewer/project -- `
  --eawr-headless-probe --eawr-report <ignored-report.json>
```

A visual frozen-scene run additionally supplies `--eawr-game-root`, `--eawr-mod-root`,
`--eawr-capture`, `--eawr-report`, and `--eawr-benchmark`. The report records the exact
scene and image hashes. Original assets and generated captures remain ignored output.

## Standalone package reproduction

`project/export_presets.cfg` selects only `main.tscn` and the public files under
`common/`; generated `out/` evidence is excluded. After building the extension, the
cross-platform helper exports one package, validates its sidecars/PCK allowlist and can
run the packaged headless probe. Before export it runs a bounded `--version` probe and
requires the pinned Godot `4.7.2.stable.official.ed1daf0bf` build, a matching
`export-templates.json` `godot_version`, and package/receipt roots that resolve inside
the repository's ignored `out/` tree. If a smoke receipt reports an engine version, it
must also match the pin. The matching template is selected from `export-templates.json`:

```powershell
python apps/viewer/tools/export_package.py `
  --platform windows-x86_64 `
  --godot E:/path/to/Godot_v4.7.2-stable_win64_console.exe `
  --smoke
```

From WSL/Linux use the same command with `--platform linux-x86_64`, the Linux Godot
binary and the Linux output package. `--platform linux-arm64` validates a cross-built
package but intentionally rejects `--smoke` unless a native ARM64 host is used.

The P1-01 receipts referenced in the qualification report were already produced with
the pinned binary and are preserved; this hardening adds preflight and output-root
guards without rerunning those platform exports. The focused synthetic checks cover
wrong-engine rejection, smoke-version mismatch and out-of-tree output rejection.

The PCK allowlist includes the project-authored, non-retail `config/camera-bindings.json`
that the presets' `config/*.json` filter has packed since P1-09; without it every export
from this revision failed validation.

## Packaged runtime qualification

`--eawr-renderer-runtime-test` is the public synthetic renderer exercise: a deliberately
uncompilable modern shader, live rejection of an unknown material route and an unknown
render pass (each must return one bounded `EAWR-RENDER-0001` diagnostic and leave the
resource registry and live instances untouched), the four-pass overlap/post capture,
shared references and 40 scene switches, then full RID release. The route/pass rejections
are folded into the existing `runtime_failed_upload_registry_clean` field, so the report
format (schema 1) is unchanged. Instead the exercise prints the stdout markers
`EAWR runtime: unknown material route and pass rejected; registry unchanged` and, just
before its report write, `EAWR runtime: all exercise checks passed; persisting report`.
The exercise then also rejects, live and with the registry untouched, an unknown legacy
family, an unsupported technique, an unsupported pass name and a legacy family in a render
pass its adapter lacks (each one exact `EAWR-RENDER-0001` naming that selector), and a
`canvas_item` shader that only mentions `shader_type spatial;` in a comment
(`EAWR-RENDER-0007` before any RID); it uploads and releases a spatial source whose leading
comment contains a semicolon. It prints `EAWR runtime: unknown legacy family, technique,
pass and render pass and a non-spatial modern source rejected; comment-led spatial source
compiled; registry unchanged`, which the source-project runtime test requires; it is not a
package-harness marker.
Every passing viewer mode now exits 2 when its required `--eawr-report` cannot be written;
a pass that was not persisted is a failed run.

`tools/qualify_package_runtime.py` runs that exercise from an **already exported**
package. It never exports or builds. It launches the packaged executable from the package
directory with an explicit `--rendering-driver`/`--display-driver`, under a timeout, and
writes a fresh evidence directory under `out/` (a non-empty directory is refused):

```powershell
python apps/viewer/tools/qualify_package_runtime.py `
  --platform windows-x86_64 --package out/viewer-packages/windows-x86_64 `
  --rendering-driver vulkan --display-driver windows `
  --package-receipt out/viewer-receipts/windows-x86_64/package-receipt.json `
  --probe-report-persistence
```

On Linux use `--platform linux-x86_64`, the Linux package and `--display-driver x11` or
`wayland`. The host must match the package platform natively. The run fails (exit 2) on:
a nonzero exit or timeout; a missing, stale, malformed or over-sized report; any report key
drift, false lifecycle field, runtime `scene_sha256` other than the exercise's fixed
identity, fewer than 40 scene switches or wrong pass order; a missing or repeated stdout
marker; an engine banner other than the package's pinned identity; a device banner that
does not match the report's backend; a PNG that is malformed, the wrong size or not the report's
`capture_sha256`; engine diagnostics beyond the expected shader-compiler pair, or any
leak/crash/shutdown text; executable, extension or PCK hashes (or the package file set)
changing during the run; or a `--package-receipt` whose hashes differ.
`--probe-report-persistence` also relaunches with a directory as the report target and
requires the passing-path marker followed by exit 2, so an earlier failure's exit 2
cannot pass. A package that cannot be launched still gets a failed receipt.
`receipt.json` records the command, cwd, timestamps, exit code, stdout/stderr hashes, host OS (including WSL), backend request and observed device, the
hashes before and after, and the verdict. It is written for failing runs too.

The engine identity is pinned per package. The Windows template is the official
`4.7.2.stable.official.ed1daf0bf`. The pinned Linux x86_64 and ARM64 templates are the
locally built P0 templates of the same commit. They report
`4.7.2.stable.custom_build.ed1daf0bf` plus their build timestamp, and the harness requires
exactly that. A host-specific engine warning can be waived only with an exact
`--allow-engine-warning 'WARNING: …'` line. The waiver is recorded in the receipt, and a
waiver that is not observed fails the run. `ERROR:` lines and leak, crash or shutdown diagnostics (such as
`WARNING: ObjectDB instances leaked at exit`) can never be waived. WSLg's
llvmpipe driver, for example, cannot change V-Sync, and a root WSL shell triggers Godot's
superuser warning.

The harness verifies `vulkan` (Windows and Linux) and `d3d12` (Windows).
The project selects Forward+, which Godot runs on those RenderingDevice drivers.
The device banner must match the requested driver:
`Vulkan <version> - Forward+ - Using Device #<n>: <vendor> - <adapter>`, or the same with
`D3D12 <feature level>`. If Godot falls back to OpenGL when a RenderingDevice
driver cannot start, the `vulkan` or `d3d12` qualification fails. The report's
`rendering_method` (`forward_plus`), vendor, adapter and API version must equal
the banner's. The synthetic tests in `tests/presentation/godot/test_qualify_package_runtime.py`
replace the executable with a Python stand-in. They prove the harness verdicts, not package
execution. `tests/presentation/renderer/test_runtime_renderer.py` runs the source project
through the editor binary under the same report, marker and engine-output checks. Set
`EAWR_ALLOW_ENGINE_WARNINGS` to exact newline-separated `WARNING:` lines to waive them there.
That run is also not package evidence.

### Submod chains

Pass a quoted, leaf-first chain with `--eawr-mod-root "<leaf>;<parent>;..."`.
Each entry accepts a mod directory or its `Data` directory. The inferred content
profile is `remake`; an explicit `--eawr-profile remake` uses the same chain.
Map, unit, effect, atlas and HUD loading search leaf, parents in listed order,
FoC expansion, then base. Loose files beat archives within each layer.
A leaf's `MegaFiles.xml` may name an archive present only in a parent: it is
activated at the supplying parent layer, whose id appears in source diagnostics
(`mod`, `mod-parent-1`, `mod-parent-2`, `expansion`, `base`). A single mod retains
its existing `mod` id. Without a mod root, the FoC mounts are unchanged.

The CPU tools `asset_validate`, `asset_scan`, `xml_scan` and `sky_scan` accept
`--profile remake --mod-root "<leaf>;<parent>;..."`. `sim_headless --skirmish m2`
accepts the same `--mod-root` value alongside `--game-root`; replay mode stays
independent of installed content. The opt-in HUD corpus uses
`EAWR_MOD_HUD_ROOTS="name=leaf;parent|other=leaf"` with `EAWR_EAW_GAME_ROOT` set.
For installed submods 2794270450 and 3229239424, list 1770851727 as their parent.

GPU lane launchers can set `EAWR_AUDIO_MUTE=1` to mute the master bus in every viewer mode,
including interactive live sessions and runs with `--eawr-audio on`. This keeps battle
audio events, voice scheduling and upstream mixing active; it only silences output.
