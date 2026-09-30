# Unit animation: which clip FoC plays for which tactical state

## Applicability

The FoC debug build `StarWarsI.exe` (PDB names, read in GhidrAssist) and the FoC
effective XML and model archives. The question is bounded to the EAWR-81 states of a space unit:
idle, moving, firing, turret turn and death, for the M2 roster
(`plan/phase-2/m2-skirmish.md`, pinned unit table). Evidence IDs `UC-E1` to `UC-E9` are opaque;
the private map lives in the ignored `out/research/p2-81/`. Retail death references (owner
footage and rig stills, EAWR-81) are under [Retail references](#retail-references-81). The implementation is `presentation/animation/unit_clips.hpp` and the
live session's death clones (`apps/viewer/src/live_session_view.cpp`).

## Interface

Input: the tactical snapshot the live session publishes (instance transforms, the
`unit_destroyed` events) and the unit's object type. Output: the clip each drawn unit plays and
the clip position on the presentation clock. Nothing is written back to the simulation, so the
headless replay hashes do not change.

## Rules

- **UA-01 Clip names.** FoC names a model's clips by rule, not in XML. When it loads a model it
  tries, for every clip type in its type table, `<model>_<TYPE>_NN.ala` in the model's directory
  for NN = 00, 01, 02, ... and stops at the first missing index. `<model>` is the model's stem,
  or the stem of the object's `Land_Model_Anim_Override_Name` / `Space_Model_Anim_Override_Name`
  when one is declared. A file whose name is not `<base>_<TYPE>_NN.ala` for a type in the table,
  or whose base names no model or override, is never loaded. (UC-E1, UC-E2)
- **UA-02 The type table** has 119 entries in a fixed order: `IDLE`, `SPACE_IDLE`, `MOVE`,
  `TURNL`, `TURNR`, `ATTACK`, `ATTACKIDLE`, `DIE`, ... `DEPLOY` (25), `UNDEPLOY` (26),
  `CINEMATIC` (27) ... `HEAL` (118); `unit_clips.hpp` lists them all. A request for a type the
  model has no clip for leaves the model's pose unchanged. When several variants exist and the
  request does not name one, retail draws the variant from the synchronized random generator.
  (UC-E2, UC-E9)
- **UA-03 Moving.** No space locomotor (`SIMPLE_SPACE_LOCOMOTOR`, `FIGHTER_LOCOMOTOR`) requests a
  clip. A space unit plays no move or turn clip; its motion is its transform. (UC-E6)
- **UA-04 Firing.** The weapon behaviours request `ATTACK` (blend 0.1 s) or `ATTACKIDLE` only when
  the model has frames for that type. Hardpoint weapons of a capital ship request no clip.
  (UC-E6, UC-E7)
- **UA-05 Turret turn.** The turret behaviour requests only `POWERDOWN` and `POWERUP`. Aiming turns
  the turret and barrel bones procedurally from the `Turret_*` tags; it is not a clip. (UC-E8)
- **UA-06 X-wing S-foils.** `DEPLOY` plays when the `SPOILER_LOCK` ability is switched on and
  `UNDEPLOY` when it is switched off, at `Deployment_Anim_Rate`, blended over 1/30 s, starting at the
  frame that mirrors the current clip's remaining frames. Attacking does not move the S-foils.
  `DEPLOY` folds the S-foils shut from the open bind pose and `UNDEPLOY` opens them; the live view
  plays both from the snapshot since EAWR-76 ([space abilities](space-abilities.md) AB-31). (UC-E5)
- **UA-07 Death clone.** A destroyed unit whose type lists a `Death_Clone` is replaced by an
  object of the clone type. Retail picks the clone by the killing blow's damage type, then the
  `Damage_Misc` entry, then the unit's own type. The clone appears at the unit's position and
  facing, owned by the neutral player, coloured like the unit, and keeps the unit's velocity for a
  spin-away. (UC-E3)
- **UA-08 Death clip.** The clone plays `Specific_Death_Anim_Type` (default `DIE`) variant
  `Specific_Death_Anim_Index` (default: drawn) once, blended in over 0.5 s at speed 1 from frame 0,
  and holds its last frame. If the clip cannot start and the type has `Remove_Upon_Death`, the
  object is removed at once (unless it spins away); without `Remove_Upon_Death` it stays in the
  pose it has. Once the clip has ended, or at once when it never started, and
  `Death_Persistence_Duration` seconds (default −1: never) have passed, it fades out over
  `Death_Fade_Time` (default 0.25 s) and is removed. (UC-E4, UC-E4b, UC-E4c, UC-E9)
- **UA-11 Bone visibility.** Every frame a model draws each mesh only while the bone it hangs on
  (a skin's visibility bone) is visible in the pose; a clip's visibility keys hide pieces, and a
  held last frame keeps them hidden. A death clip that ends with every piece hidden leaves the
  clone drawn empty for as long as it persists. (UC-E10)
- **UA-09 Station death.** A clone type with `Should_Death_Clone_Play_Idle` starts on its idle clip
  when the dying station had no clip running; the death behaviour then blends to `DIE` as in UA-08.
  (UC-E3, UC-E4)
- **UA-10 Fighter death.** A fighter has no `Death_Clone` and no `DIE` clip, so its death is its
  death explosion; with `Spin_Away_On_Death` it first spirals away along a spline for
  `Spin_Away_On_Death_Time` with the chance `Spin_Away_On_Death_Chance` (a synchronized draw).
  (UC-E4) The rules are [space-fighter-deaths](space-fighter-deaths.md) SP-01 to SP-09 (EAWR-447).

### Project policy

- **UA-P1** The snapshot carries no damage type, so the view uses the `Damage_Normal` entry, then
  `Damage_Misc` (`death_clone_type`). All M2 types list only `Damage_Normal`.
- **UA-P2** The death clip's variant is the unit's ID modulo the variant count instead of
  retail's synchronized draw; every M2 clone has one variant.
- **UA-P3** The clone shows at the unit's last drawn pose, from the first frame the unit is gone,
  and only when the local player saw the unit on that frame. Its clip clock counts whole ticks of
  the presentation clock (30 Hz) from the unit's last tick.
- **UA-P4** A bone the clip marks invisible has its geometry collapsed to the bone's origin; the
  palette has no visibility of its own (UA-11 hides the mesh; the result on screen is the same).
- **UA-P5** The fade of UA-08 is shown as removal at the end of the fade time; the view then
  drops the clone and releases what it uploaded for it.
- **UA-P6** A `Specific_Death_Anim_Type` that names no clip type is read as `DIE`, and a declared
  `Specific_Death_Anim_Index` past the model's last variant as a clip that cannot start
  (both unverified). A clone kept in its pose because its clip cannot start plays the clone
  model's idle clip if it has one (unverified; no M2 clone lacks its death clip).

## The M2 roster

The FoC archives hold these clips for the pinned types (UA-01; the live-session report's
`unit_clips` rows list the same counts per run):

| Type | Model | Idle / move / attack / turret clips | Death |
|---|---|---|---|
| `Corellian_Corvette` | `rv_corvette` | none | clone `Corellian_Corvette_Death_Clone`: `rv_corvette_d` `DIE_00` |
| `Nebulon_B_Frigate` | `rv_nebulonb` | none | clone `Nebulon_Death_Clone`: `rv_nebulonb_d` `DIE_00` (`rv_nebulonb_d_death_00` is never loaded) |
| `Tartan_Patrol_Cruiser` | `ev_tartancruiser` | none | clone `Tartan_Patrol_Cruiser_Death_Clone`: `ev_tartancruiser_d` `DIE_00` |
| `Acclamator_Assault_Ship` | `ev_acclamator` | none | clone `Acclamator_Death_Clone`: `ev_acclamator_d` `DIE_00` |
| `Skirmish_Rebel_Star_Base_1` | `rb_station_01` | none | clone `Rebel_Star_Base_1_Death_Clone`: `rb_station_01_break` `DIE_00`, persists 34 s |
| `Skirmish_Empire_Star_Base_1` | `eb_station_01` | none | clone `Empire_Star_Base_1_Death_Clone`: `eb_station_break_break_01` `DIE_00`, persists 34 s |
| `X-Wing` | `rv_xwing` | `DEPLOY_00`, `UNDEPLOY_00` (UA-06); `CINEMATIC_00..03` unused in battle | none (UA-10) |
| `Y-Wing`, `TIE_Fighter`, `TIE_Interceptor`, `TIE_Bomber` | `rv_ywing`, `ev_tiefighter`, `ev_tie_interceptor`, `ev_tiebomber` | none (`rv_ywing_cinematic_00` unused) | none (UA-10) |

So an M2 unit plays no clip while it lives: no pinned model has an idle, move, attack or
turret clip, and the X-wing's S-foil clips need the `SPOILER_LOCK` ability, which the tactical
snapshot does not carry. The capital ships and stations play their clone's `DIE`
clip when they die.

## Cases

- **UA-C1** `rv_corvette_d.alo` has `rv_corvette_d_die_00.ala` and no `_die_01`: one DIE variant.
- **UA-C2** `nb_basepad_deploy.ala` has no two-digit index: never loaded (UA-01).
- **UA-C3** `ev_at-aa_turnr_half_00.ala` is `TURNR_HALF` index 0 of `ev_at-aa.alo`, not a `TURNR`
  clip of a model named `ev_at-aa_turnr_half`.
- **UA-C4** A clone with a 90-frame 30 fps death clip and `Death_Persistence_Duration` 34 is drawn
  until 3 s + 34 s + 0.25 s after it appeared (UA-08, UA-P5).
- **UA-C5** The M2 capital-ship clones declare `Remove_Upon_Death`; the station clones do not. A
  station clone without its death clip stays in its pose; a corvette clone without it is gone at
  once (UA-08).
- **UA-C6** `rv_corvette_d_die_00` hides its pieces by visibility keys as it plays; with the default
  persistence the corvette clone then stays, drawn with nothing visible, for the rest of the
  battle (UA-08, UA-11).

## Retail references (EAWR-81)

- **UA-R1** (owner footage, 2026-09-27: a retail Corellian corvette killed in battle, 30 fps
  video) The death is the `Death_Explosions` fireball and the breakup clone together. In the kill
  frame a fireball covers the ship. At 0.5 s the hull is already in several large pieces inside
  the explosion. The pieces drift apart along a line and keep burning, with fire and spark
  trails on them, through 3 s. At 5 s only small debris is left. This is UA-07 and UA-08 (the
  `Damage_Normal` clone playing its `DIE` clip) seen in play.
- **UA-R2** (rig recording, 2026-09-27: stills of kills staged at the local station through the
  capture mod's `-StagingProbe deaths`, fog off, each labelled with the seconds since the kill)
  The Tartan dies in a blue explosion (`Large_Explosion_Space_Empire`), its hull broken into large
  pieces with many small fragments around them (+0.0 s). The Acclamator is down to one burning
  fragment and a smoke trail at +2.1 s, and to smoke at +2.7 s. After a station kill the Tartan
  leaves nothing at +4.9 s. The station seldom finished its victims, so the probe killed the
  +0.0 s Tartan and both Acclamators by script. A script kill carries the debug-cheat damage type,
  so those stills may show UA-07's `Damage_Misc` or own-type fallback, not the `Damage_Normal`
  clone.
- **UA-R3** The remake draws the same explosion and clone pieces (live session, scripted kill,
  EAWR-387 clip route). It differs from UA-R1 and UA-R2 in three ways, all on the fidelity list and
  none changed here. Its pieces carry no fire or smoke after about 1 s. The corvette's pieces are
  nearly gone at 3 s, where retail's still burn. No cloud of small fragments flies out.
- **UA-R4** (rig recording, 2026-09-27, `-StagingProbe fighters`, station kills) An X-wing 0.3 s
  after its kill and a TIE fighter 0.4 s after theirs show only their death explosion (orange,
  blue): each craft was gone by 0.3 and 0.4 s after its kill. No still shows the kill frame
  itself, so they do not tell how much earlier it went; the removal rule is UA-08's. No still
  caught a spin-away (UA-10, a 0.2 or 0.4 chance per kill), so the spin-away itself has no retail
  reference yet.

## Unknowns

- A FoC rig capture of the Coruscant lobby start (fog off) shows the Rebel X-wings flying with
  their S-foils open and no ability used, which is the bind pose the view draws (UA-06). Deaths
  now have retail references (UA-R1, UA-R2); no rig recording shows a firing unit yet. Whether
  the corvette's wreck ends empty (UA-C6) is not visible in the footage: its pieces are gone by 5 s.
- Which damage type a projectile kill carries is unverified (UA-P1).
- The clone keeps its unit's position, height included: FoC creates it without its own
  `Layer_Z_Adjust` (space-movement LZ-02, EAWR-666).
- Spin-away deaths (UA-10) move the unit, so the simulation runs them (EAWR-447,
  space-fighter-deaths SP-P1); the fighter itself still leaves the session at once.
- The procedural turret aim of UA-05 is not modelled.
- 27 FoC clips outside M2 fail the strict track binding (the `binding_failed` rows of
  `tests/presentation/animation/corpus_association_foc.tsv`); whether retail binds them by a
  looser rule is unverified.
