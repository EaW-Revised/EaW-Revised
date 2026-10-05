# Space ship movement: move, turn, turn in place and stop

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space, the ships of the M2
  fleet ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md) SK-22): `Corellian_Corvette`,
  `Tartan_Patrol_Cruiser`, `Nebulon_B_Frigate` and `Acclamator_Assault_Ship`. Ship movement
  and turning (legacy EAWR-70).
- Bounded question: how one ship given a move, face or stop order accelerates, turns, travels
  and stops, frame by frame, and how it banks while it turns (ship banking and sway).
- Source tags: **data** (a tag in the FoC files: `spaceunitscorvettes.xml`,
  `spaceunitsfrigates.xml`, `gameconstants.xml`; hashes under Sources in the M2 lock),
  **recording** (the fixed-force original-game traces of S-10 to S-14 and S-17,
  [tests/fidelity](../../tests/fidelity/README.md); three runs each, spread 0), **research**
  (the FoC debug build read under the [clean-room rule](../clean-room.md); evidence IDs
  E70-nn, E71-nn and E351-nn are opaque and their map stays private), **owner** (the owner's
  play knowledge of the original), **project** (a remake decision) and **inference**. Where
  research and a recording disagree, the recording wins.
- Avoidance (group formations and avoidance): how the path finder routes one ship around other ships of its
  layer and around stations and other static objects ([Avoidance](#avoidance-71)).
- Group moves (multi-select formation slots): how one move order for several ships gives each ship its slot,
  its planning speed and its planning frame ([Formations](#formations-344)).
- Out of scope: occupied destinations (blocked move-target clipping), hyperspace, the abilities' speed multipliers
  ([space abilities](space-abilities.md) AB-24, space ability implementation) and squadron craft (fighter spawning and simulation), whose fighter locomotor maps its turn to a roll its own way.

## How FoC moves a ship

research E70-01 to E70-11. A move order makes the space path finder plan a timed path once:
a list of nodes, each a frame, a position, a facing and a speed. The ship's locomotor then
follows that path every frame by interpolating between the two nodes around the current
frame; it does not steer. The finder is a weighted A* search over a small set of steps from
the ship's current state: a straight leg that brings the ship to full (or slow) speed, arcs
of a fixed angle to the left or right at full speed, a "match" arc to the tangent that points
at the target followed by a straight "end" leg onto the target, slow-speed and turn-in-place
("emergency") steps, and threat-avoiding steps. The search's cells are the forward step
length divided by 1.5; collisions with static and dynamic obstacles add cost. A finished
path then gets its end speed and the helper nodes where braking starts.

The remake runs that search (group formations and avoidance, [Avoidance](#avoidance-71)) whenever the motion table
carries avoidance rules, as the FoC tables do. The rules MV-12 to MV-17 below describe the
shape it takes in open space; a table without avoidance rules (the synthetic replay fixtures)
plans that shape directly, without the search. Threat steps are not implemented; blocked move-target clipping adds
destinations inside occupied space.

## Interface

- Content: the motion table (`sim::tactical::MotionTable`), built for M2 by
  `units::motion_table` from the space-unit data loading unit tables ([unit-data.md](../unit-data.md#motion-table)).
  Per ship type: maximum speed, acceleration and deceleration (units per frame, per frame
  squared), rate of turn (degrees per frame) and turn-in-place slowdown. Rules: the arc angle
  and the expansion distance; with group formations and avoidance the avoidance rules and the footprints (AV-05). Like
  the sensor and durability tables it is passed to the session, named by the content identity,
  and is neither replay data nor state.
- State: for each live unit whose type has a profile, its motion: at rest, following a path
  or turning in place, and the plan's inputs (start tick, start position, yaw and speed,
  target), and the plan's nodes. Without avoidance rules the nodes are a pure function of those
  inputs and the content; with them they also depend on the other ships' predictions, so they
  are hashed, and so is each tracking layer's window anchor (AV-03). The motion record is part
  of the state hash; so are the unit's position and rotation
  ([replay-format.md](../replay-format.md)).
- Inputs: the move (opcode 2), face (opcode 5, the Lua `Turn_To_Face`) and stop (opcode 1)
  orders of the unit's owner; attack, attack-move and guard orders plan through
  [space orders](space-orders.md) (attack approach, attack-move and guard). A move naming two or more tracked ships is a group move (FM-01).
- Outputs: each unit's position and rotation per completed tick, in the state and the
  snapshot transform.
- Cadence: every tick, before the tick's commands, each unit that follows a plan moves to
  the completed tick (MV-02). Units are independent; workers split them by stable ID. In a
  tick that plans a move with avoidance rules, workers then sample each tracked moving unit's
  prediction at the tracking windows' boundaries (the `tracking` phase), and the commands
  plan serially in command order (AV-15).

## Rules

- **MV-01** (data, research E70-10) A ship's maximum speed, `OverrideAcceleration`,
  `OverrideDeceleration` and `Max_Rate_Of_Turn` are its XML values times
  `Object_Max_Speed_Multiplier_Space` (1.2): the corvette's 3.1, 0.05, 0.05 and 1.25 become
  3.72 units per frame, 0.06 per frame squared and 1.5 degrees per frame (recording: S-10 and
  S-12 show exactly these). A missing acceleration or deceleration is the maximum speed. The
  turn-in-place slowdown is `TurnInPlaceSlowdownCorvette` (2), `-Frigate` (3) or `-Capital`
  (4) by `Space_Layer`. The arc angle is 360 / `MaxRotationsSpace` (15 degrees); the
  expansion distance is `XYExpansionDistanceSpace` (300).
- **MV-02** (recording S-10, S-11, S-13) An order accepted in the commands of tick t acts
  from frame t + 2: during frame t + 1 the unit still does what it did before (at rest, on its
  old path), and its new plan starts at frame t + 1 from where that frame left it. A move at
  tick 30 first moves the unit at tick 32; a stop at tick 120 still moves it at 121.
- **MV-03** (project, ship movement and turning blocked-path option B) An order is accepted but moves nothing when
  the unit's type has no motion profile (stations), when the target's XY is the unit's XY, or
  when the unit or the target lies outside ±262,144 units. Rejections are unchanged.
- **MV-10** (research E70-03, E70-06) A move plans one path at its start frame from the
  unit's position, yaw and speed there. The path's nodes carry a frame, a position, a yaw and
  a speed.
- **MV-11** (research E70-05; recording) A move stays in the unit's layer: the path keeps the
  unit's height and ignores the target's. The corvette stays at z = -20 in every recording.
- **MV-12** (research E70-04, E70-05) When the target lies in the path finder's start cell the
  path is two nodes: the start (current speed and yaw) and the target at speed 0, reached
  after length / maximum speed frames, facing along the move. The cell is square, anchored at
  the start and extends in +X and +Y only: the target's offset on each axis is at least 0 and
  below 2/3 of the forward step. The forward step is the expansion distance, or half the move
  length (at least 50) for moves shorter than twice it, and at least the maximum speed.
- **MV-13** (research E70-03; recording S-12) Otherwise, a unit not at its maximum speed first
  flies straight along its current yaw until it reaches it: |vmax² − v²| / 2a units in
  |vmax − v| / a frames. It does not turn before. In S-12 the corvette, ordered 90 degrees
  left from rest, flies straight for 62 frames and starts turning exactly when it reaches
  3.72.
- **MV-14** (research E70-02, E70-03; recording S-12) While the bearing to the target differs
  from the yaw by more than the arc angle, the path takes one arc of the arc angle toward the
  target at full speed, in arc angle / rate of turn frames (15 degrees in 10 frames for the
  corvette). The arc's node lies on the circle of radius v / rate of turn (in radians) beside
  the ship.
- **MV-15** (research E70-03) When the bearing difference is at most the arc angle and the
  target lies outside the turn circle on its side, the path turns on that circle to the point
  whose tangent points at the target, in |yaw change| / rate of turn frames, then (MV-16) flies
  straight onto the target. S-12's corvette ends its turn heading 97.93 degrees.
- **MV-16** (research E70-03, E70-04; project) The end leg flies straight onto the target. A
  target within 0.01 degrees of dead ahead gets the straight leg at once; retail may first
  take a detour (U-01).
- **MV-17** (project) A target inside the turn circle on its side: the ship flies straight
  until the target is 1/16 unit outside the circle, then plans on (MV-14, MV-15). Retail's
  search would use other steps (U-01).
- **MV-18** (research E70-06, E70-07; recording S-10) The path ends at speed 0 on the target.
  If the last leg is too short to stop (v² − 2dL > 0), it ends at √(v² − 2dL) instead. On a
  straight last leg longer than the braking distance plus one acceleration step, the ship
  keeps full speed until v² / 2d before the target (115.32 units for the corvette), then
  brakes at d. S-10's corvette starts braking exactly there and stops on the target.
- **MV-19** (research E70-07) Likewise a straight leg that must gain speed gains it over its
  first |v1² − v0²| / 2a units and then holds it.
- **MV-20** (research E70-09; recording S-11) A face order turns the unit in place, the short
  way, to face the target's XY direction: |yaw change| / rate of turn × slowdown frames, the
  yaw changing linearly with time. It does not move the unit and ends any path: a moving ship
  stops where MV-02 leaves it. The corvette turns 90 degrees in 120 frames (0.75 degrees per
  frame); an Acclamator in 450.
- **MV-21** (recording S-11) A turn in place ends exactly on its target yaw.
- **MV-22** (research E70-11; recording S-13) A stop drops the plan: the unit halts where
  MV-02 leaves it, without braking. S-13's corvette moves its full 3.72 units at tick 121 and
  none after.
- **MV-23** (research AT-01, E452-03; recordings S-26, S-27) An attack order hands the unit's
  movement to the approach of [space orders](space-orders.md) OR-02 to OR-08: a unit in range
  holds, one out of range plans a move to its approach slot and closes. A unit at rest turns in
  place toward an ordered target in range ([space weapon fire A-04](space-weapon-fire.md#attack-orders));
  the turn is MV-20's. Attack-move and guard orders (attack approach, attack-move and guard) plan as moves or approaches (OR-10 to
  OR-17).
- **MV-30** (research E70-01) Between two path nodes the position follows the cubic Hermite
  curve whose end tangents are each node's yaw vector times its speed times the frames
  between the nodes. A constant-speed straight leg is a straight line at that speed; an
  acceleration leg is exact constant acceleration; an arc bends slightly off the circle
  between its nodes (S-12's yaw changes by 1.5061, 1.5020, ... 1.5065 degrees per frame over
  each arc). The remake evaluates the curve exactly and rounds once.
- **MV-31** (research E70-01; recordings) A following unit faces along the curve's velocity,
  with a level heading (no pitch; the bank roll of BK-01 to BK-05 turns the ship about that
  heading and never changes it), while it moves faster than 0.0001 units per
  frame; slower, its yaw is kept. So the pitch the recorder stages on the corvette is gone
  from the first frame it moves or turns.
- **MV-32** (research E70-01) At or after the last node's frame the path is over: the unit
  stays where the last frame before it put it (it does not snap onto the last node) and is at
  rest.
- **MV-33** (project, space-hardpoints HD-11) A unit whose engines are lost plans with its
  maximum speed, acceleration and deceleration times the durability speed factor at the time
  it plans.

## Banking in turns

research E351-01 to E351-10; data; owner (2026-09-27, ship banking and sway: in the original both the Corellian
corvette and the Nebulon-B frigate sway into a small bank while they turn). FoC's space
locomotor rolls the ship about its own forward axis every frame it follows a path or turns in
place, and levels it again once it is at rest. The roll is part of the ship's facing, which
the game keeps as simulation state and draws from; nothing else in the movement reads it.

- **BK-01** (data, research E351-01 to E351-03) Two type tags drive it: `Max_Rate_Of_Roll`
  (degrees per frame) and `Bank_Turn_Angle` (degrees). A type without them keeps the engine's
  defaults, 2 and 70. The roll rate is multiplied by `Object_Max_Speed_Multiplier_Space` (1.2)
  exactly like the rate of turn (MV-01); the bank angle is not. The M2 ships all author a roll
  rate of 0.2 (0.24 per frame); their bank angles are 15 (`Corellian_Corvette`,
  `Tartan_Patrol_Cruiser`), 5 (`Nebulon_B_Frigate`) and 20 (`Acclamator_Assault_Ship`). A
  bank angle of zero never rolls. Every ship with a motion profile banks; the values come from
  the unit tables, not from the ship.
- **BK-02** (research E351-04 to E351-06) Each frame a plan moves the ship, the frame's turn
  d (the new yaw minus the old, wrapped to [-180, 180)) sets a target bank. Its depth is the
  bank angle times twice |d| over the rate of turn, capped at the full bank angle: a turn at
  half the rate of turn or more banks fully. A left turn (d zero or more, yaw growing) targets
  a negative roll, which lowers the ship's left side into the turn; a right turn the mirror.
  A frame without a turn (a straight leg, or a ship slower than MV-31's threshold) targets
  zero. The frame a path ends (MV-32) keeps the roll as it is; the last frame of a turn in
  place banks like the others, unless the turn took no time.
- **BK-03** (research E351-04) The roll moves toward that target by the roll rate times the
  remaining gap over the bank angle, but never by less than a tenth of the roll rate, and it
  stops on the target. So a bank eases in and out: the corvette's first turning frame rolls
  0.24 degrees, its second 0.24 x 14.76 / 15 = 0.23616.
- **BK-04** (research E351-08) A ship at rest (its path ended, a stop order, or never moved)
  levels by the full roll rate each frame, from the frame after its plan ended, and stops on
  zero.
- **BK-05** (research E351-07, E351-09; [R-ROT-01](p1-effective-environment.md)) The roll is
  the facing's first angle: the ship's orientation is its heading yaw, then the roll about its
  own forward axis (Rz(yaw) Rx(roll), right-handed), then the model's fixed quarter turn. The
  model, its hardpoints and everything attached to it follow the rolled orientation.

The remake keeps the roll as simulation state (project, from E351-07: FoC keeps it in the
synchronized facing it sets every frame, not in a display-only value). Each unit with a
motion profile carries its roll in degrees (Q24); the movement phase (partitioned by unit, like
the rest of MV-02) updates it from the unit's own copied state, so every worker count gives the
same roll. The unit's rotation stays the level heading, so yaw, planning and the traces'
forward vector are unchanged; the roll is hashed in its own block and published in the
snapshot's instance transform ([replay-format.md](../replay-format.md)). The viewer draws that
transform's roll, eased between two ticks like the position.

### Banking cases

The rule worked by hand (0.24 per frame roll rate):

| Case | Bank angle | Roll |
|---|---:|---|
| C-10: corvette turning in place 90 degrees (C-02, 0.75 per frame, full bank target) | 15 | -0.24 at tick 32, -0.47616 at 33, about -12.8 at the turn's last frame (151); level 54 frames later |
| C-11: corvette on a full-rate arc (1.5 per frame) | 15 | -2.2 after 10 frames, -5.8 after 30, -9.3 after 60 (S-12's 90-degree turn); 90 % of the bank after 143 frames |
| C-12: Nebulon-B on a full-rate arc (0.84 per frame) | 5 | -1.9 after 10 frames, -3.9 after 30, the full -5 from frame 68 |
| C-13: Nebulon-B turning in place (0.28 per frame, a third of its rate) | 5 | eases to 2/3 of the bank, -3.33, and levels 14 frames after the turn |
| C-14: Acclamator on a full-rate arc (0.6 per frame) | 20 | -10.3 after 60 frames, -15.3 after 120 |

`tests/replay/motion_tests.cpp` pins BK-01 to BK-05 on these values and C-10 in a session
(1, 2, 4 and 8 workers alike); the `tactical-motion` fixture's golden hashes were re-pinned
for ship banking and sway because its corvette and Acclamator now bank.

## Cases

- **C-01** (S-10 shape) A corvette at rest facing +X at (-1800, -1500, -20) is ordered at
  tick 30 to (0, -1500, 0). Tick 31: unmoved. Tick 32: 0.03 units. Full speed 3.72 from tick
  94. Braking from 115.32 units before the target. It stops within 0.1 unit of (0, -1500) at
  z -20, facing +X.
- **C-02** (S-11 shape) The same corvette ordered at tick 30 to face (-1800, 0, 0): yaw 0.75
  degrees at tick 32, 90 at tick 151 and after; its position never changes.
- **C-03** (S-12 shape) Ordered at tick 30 to (-1800, 500, 0): no yaw change up to tick 93;
  15 degrees at tick 103, 90 at tick 153; after the match it flies at 97.93 degrees.
- **C-04** (S-13 shape) Ordered at tick 30 to (2500, -1500, 0) and to stop at tick 120: it
  moves 3.72 at tick 121 and stays put from tick 122.
- **C-05** An Acclamator facing +Y turns in place to face +X in 450 frames.
- **C-06** (blocked path) A station ordered to move, a corvette ordered to its own XY and a
  corvette ordered 300,000 units away all accept the order and never move.
- **C-07** (clipped destination, AV-19) A corvette at (-1800, 1000) ordered onto the centre of
  a mining pad at (-600, 1000) stops at (-900, 1000): rings 0 to 5 (radius 0 to 250) lie
  inside the pad's 206.25 plus the corvette's 50.19, ring 6 is open toward the corvette. A
  corvette at (-1800, -1000) ordered onto a held corvette at (0, -1000) stops at (-100, -1000),
  ring 2 (the held ship reaches 17.76 along its facing, plus 50.19).

The tactical-motion and tactical-motion-blocked replay fixtures in `tests/replay` pin C-01
to C-06 in a session, and tactical-motion-clipped pins C-07 with the FoC avoidance rules;
`sim_headless --scenario` stages the recorded scenarios themselves
([traces.md](../traces.md)).

## Against the recordings

Remake traces against the first original run with the committed tolerances
(`compare_traces.py --report`, fixed-force traces private):

| Case | Result |
|---|---|
| S-11 | Every field within tolerance. |
| S-12 | Position within 0.0015 units (largest 24,371 raw against the 2,048 raw bound), fwd within 260 raw; alive, hull, shield, pos.z and fwd.z exact. |
| S-13 | Position within 0.001 units (16,366 raw); everything else within tolerance. |
| S-10 | Before the group formations and avoidance work up to 9.7 units and 15 degrees off from tick 94 (U-01); with the search, within 0.01 units at every tick ([Avoidance](#against-the-recordings-1)). |
| S-17 | Follows S-10 through tick 61; from 62 TURBO doubles the cruise to 7.44 as recorded. Retail stops 55.8 units short; the remake arrives (U-02). |

The S-12 and S-13 position differences are the original's own rounding: it evaluates each
leg as a binary32 cubic whose coefficients are as large as the leg (thousands of units), so
its positions carry up to about eight binary32 steps of noise on long legs. The remake's
exact evaluation follows the ideal curve, so these differences are expected and the
tolerances stay at the conversion bound.

## Unknowns

- **U-01** (resolved by group formations and avoidance) Retail's A* sometimes chooses another shape than MV-13 to
  MV-17. S-10's corvette, with its target dead ahead, turns 15 degrees right at full speed,
  15 degrees back and matches (a match needs at least 0.01 degrees of bearing difference and
  the path must finish with the end leg). The search reproduces it (AV-10 to AV-18).
- **U-02** TURBO (and ability speed multipliers) scale maximum speed, acceleration and
  deceleration (research E70-10; S-17: 7.44 and 0.12). Retail replans the move when TURBO
  starts, and S-17's corvette then stops 55.8 units short of its target; the remake replans too
  ([space abilities](space-abilities.md) AB-24) and arrives (AB-U2).
- **U-03** A move issued while the ship is moving plans from its current speed; no recording
  covers it.
- **U-04** Retail's end-of-move idle drift (the space idle movement switch) is off in every
  recording and not modelled.
- **U-05** Banking (BK-01 to BK-05) rests on the debug build, the owner's report that the
  corvette and the Nebulon-B sway in turns, and rig stills (rig recording, 2026-09-27, ship banking and sway): 28
  fog-off stills of a staged corvette and Nebulon-B, each ordered 90 degrees to its left from
  rest (capture, `-StagingProbe bank`; each still is labelled with
  the seconds since the order and the heading change so far). The corvette banks while it turns.
  The still 3.2 s into a left turn (heading +37 degrees) shows its right flank and underside
  where the same heading at rest shows its deck. So its left side is down in a left turn, as
  BK-02 has it (read by eye). The Nebulon-B shows no bank the eye can see, as a 5 degree limit
  would predict. The stills also give the turn pace: the corvette is at +37 degrees after 3.2 s
  and +96 after 5.1 s; the Nebulon-B at +42 after 3.9 s and +84 after 5.5 s. The depth of the
  bank is **not measured**: the stills come from the perspective tactical camera at varying
  screen positions, and the fixed-force traces record position and forward only, which a roll does not
  change. BK-01's values stay the debug build's.

<a id="heights-666"></a>

## Heights

Status: implemented for the skirmish start (companies, stations, map objects, squadron craft) and
the hangar launch. Reinforcements (simulation economy, build queue and arrivals, PU-34) raise the unit they create (LZ-01, PL-08); Lua and ability spawns do not exist in the sim yet and must raise theirs when they land. The rules come from the FoC debug build (evidence IDs E666-01 to E666-08,
private map) and the retail recordings. Space ships don't all fly on one plane: each type flies at
its own height, and that gap is what keeps ships of different layers, which never avoid each other
(AV-01), from meeting.

- **LZ-01** (research E666-01 to E666-04; recordings S-10 to S-14, S-32, S-33, S-34, S-51) An
  object is created at the point it is given, raised by its type's `Layer_Z_Adjust` (none: 0). FoC's
  object creation takes a flag for this, and every path that creates a battle object sets it: the
  starting forces and stations on their markers, map objects on their TED position (and the
  `SpaceProp`s, which the remake does not simulate), squadron craft, hangar launches,
  reinforcements, production, Lua spawns, ability spawns and projectiles (no FoC projectile type
  carries the tag). At the skirmish start the free-space
  search (PL-01 to PL-07) runs on the marker's plane and each company or craft is created at the
  point it finds, so it takes its height there (the origin of PL-07 included). The recordings
  agree: the Acclamator at z = -110 and the Nebulon-B at -90 (S-32, S-33), the MC80 at -290 and
  the Tartan at 0 (S-51), the Rebel star base (no tag) at 0 (S-34), the corvette at -20 (S-10 to
  S-14), each staged at height 0. The M2 heights:

  | Type | `Space_Layer` | `Layer_Z_Adjust` |
  |---|---|---|
  | `Tartan_Patrol_Cruiser` | Corvette | 0 |
  | `Corellian_Corvette` | Corvette | -20 |
  | `Nebulon_B_Frigate` | Frigate | -90 |
  | `Acclamator_Assault_Ship` | Frigate | -110 |
  | `Calamari_Cruiser` | Capital | -290 |
  | `Skirmish_Empire_Star_Base_1` | StaticObject | -150 (from `Empire_Star_Base_1`) |
  | `Skirmish_Rebel_Star_Base_1` | StaticObject | none |
  | squadrons and their craft | none | none |

- **LZ-02** (research E666-03, E666-04) A squadron's craft each take their own height; the
  squadron's team container is created without it (at the start it then stands at the centre of
  its craft, space-visibility V-03). A death clone is created without it
  too, so it stays where its unit was, at that unit's height (unit animation). So is a star base
  that replaces an upgraded one.
- **LZ-03** (research E666-05, E666-06; MV-11) Nothing in space changes a ship's height after
  that. A move keeps the unit's height (MV-11). FoC's direct position setters and its move and
  follow orders add the mover's own height to the point they are given, and its steering
  locomotor turns toward a point at the unit's own height, so no order, not even following a
  ship of another layer, takes a ship to another height. Only squadron craft leave theirs while
  they fight ([space fighters](space-fighters.md) FM-11, FM-21).
- **LZ-04** (project) `TacticalSession::stage_spawn` places a unit where its caller says: the
  scenario tool ([traces.md](../traces.md)) and `path_bench` raise the point by the type's height
  first. The scenario fixtures always did (LZ-01), so their traces and pins do not move.

### What reads the height

The height now reaches every rule that reads a position. Each was written to FoC's own test,
planar or 3-D, so none needed a change; until per-unit flight heights the 3-D ones only saw heights of 0.

| Reader | FoC's test | Rule |
|---|---|---|
| Target scan | a box on every axis around the unit | space-weapon-fire T-05, space-targeting CO-10 |
| Attack distance, turning toward a target | planar | space-weapon-fire A-04, A-07 |
| Aim point search and nearest hardpoint | 3-D | space-targeting R-11, space-weapon-fire W-05 |
| Weapon range at the firing attempt | planar | space-weapon-fire W-05 |
| Fire arcs | yaw and pitch in the fire bone's frame (3-D) | space-weapon-fire W-07, W-09 |
| Lead, scatter, projectile flight and hits | 3-D; scatter radius planar | W-10, W-11, space-damage DG-22, DG-24, DG-30, DG-36, DG-37 |
| Projectile travel | range plus the height between origin and aim point | space-damage DG-23 |
| Missiles | 3-D, pitch from the height | space-damage MS-02, MS-04 |
| Avoidance and the destination search | per layer, planar footprints; the arc cost counts the unit's height | AV-01, AV-04, AV-12, AV-19 |
| Group moves | 3-D distances; slots keep the target's height, paths the ship's | FM-02, FM-03, FM-05 |
| Attack-move, guard, approach | planar range; the meeting time is 3-D | space-orders OR-04, OR-07 |
| Sensors and fog | planar | space-visibility V-06, Q-03 |
| Dogfight grid | cell points at the craft's own height | space-fighters FD-03, FM-21 |
| Selection and picking | the ray meets the unit's volume where it is drawn | foc-battle-selection P-1 |
| Order point | the battle plane z = 0 | foc-battle-selection P-3 |
| Minimap camera outline | the mean start height of the players' units | foc-minimap MM-09 |
| World UI, icons and bars | follow the unit's drawn position | foc-battle-world-ui |
| The tactical camera | its own target and height | tactical-camera-input |

Not wired: `SpaceProp` heights ([rendering](../rendering.md#space-ambient-particle-allocation-238)
places props at their TED position; the tag's row belongs to movement tag coverage). The console flag
`FightersPreserveZ` belongs to the fighters (E457-09).

<a id="avoidance-71"></a>

## Avoidance

Status: implemented for single-ship moves when the motion table carries avoidance rules (the
FoC tables do; `units::motion_table`). The rules come from the FoC debug build (evidence IDs
E71-nn, private map) and the S-10 and S-14 recordings. They replace the open-space shortcut
of MV-12 to MV-17 and resolve U-01. A table without avoidance rules (the synthetic replay
fixtures) still plans with MV-12 to MV-17.

### How FoC avoids

research E71-01 to E71-24. FoC has no steering. Every move of a ship with a space layer runs the
path finder's weighted A* search at the start of the move, and the search's collision test is what
avoids other ships and static objects. Ships in other layers are invisible to it.

- **AV-01** (research E71-12, E71-13, E71-17) The tracking system keeps one collision layer per
  space layer: capital, frigate, corvette and super capital, plus a static-object layer. A ship
  is tracked in its own layer (a type may name a separate stationary layer; none of the M2 fleet
  does, so a held ship stays in its layer). Every `SPACE_OBSTACLE` object submits itself at
  spawn: the star bases, the mining pads (`Mineral_Extractor_Pad`), the defense satellite pads,
  the gravity well station and the merchant dock, all `Space_Layer` `StaticObject`. A type
  without `Space_Layer` is not tracked (`Orbital_Resource_Container`). A search queries only
  the ship's own layer and the static layer. So a corvette never avoids a frigate or a capital
  ship, and the reverse (recording S-14: the left corvette flies the no-search path exactly
  and passes 78 units from the held Nebulon-B's centre), but every ship avoids stations and
  pads.
- **AV-21** (debug build, research R850-01 to R850-03; data) A type starts with no moving
  or stationary space layer. The object's layer lookup retains those defaults unless the
  type authors a layer; the separate multiple-locomotor exception can select the corvette
  layer. FoC fighter types author neither layer nor that exception, so their absent
  `Space_Layer` means no collision layer, not an implicit ship layer. The remake preserves
  that default by excluding craft from ship motion profiles and tracked footprints. This
  is default behaviour, not evidence that FoC's parser ignores an authored `Space_Layer`.
- **AV-02** (research E71-12) Each tracked ship carries a prediction: its position for every
  frame from now to the end of its planned path (a ship at rest: its position, held). When a
  ship gets a new path, a turn in place or a stop, its prediction is rebuilt and its layer is
  marked for rebuilding. The remake's prediction of a frame is where the plan puts the ship at
  that tick (`sample_motion`), and where it stops once the plan is over.
- **AV-03** (research E71-13, E71-21) A layer answers queries through 45 consecutive time
  windows of 90 frames each, starting at the frame it was last rebuilt and rolling one window
  every 90 frames. A layer is rebuilt at the frame a ship in it submits a prediction or a
  ship joins or leaves it. In a window each tracked ship is a straight move from its predicted
  position at the window's start to its predicted position at the window's end, with a
  rectangular footprint (its hard half extents) facing along that move, or along its predicted
  facing at the window's end when it holds still. It counts as moving in a window where it
  moves and as static where it does not. An obstacle is a square of its soft radius. A query
  that starts before the layer's first window finds nothing; the next window's part of a query
  starts one frame after the previous window's end.
- **AV-04** (research E71-14, E71-20) A query is a straight move of the searching ship's
  footprint over a frame interval. Per window it overlaps, the query is clipped to the window;
  the tracked ship is placed at the query's own start and end frames within its window. The
  two footprints are projected onto their relative direction (a footprint's reach is its X
  extent where the direction is along its facing, its Y extent across, blended by the squared
  cosine) and the query hits the ship when the relative distance falls to the summed reach
  within the span. The searching ship itself is ignored. The remake decides this exactly on
  the Q24 values (a quadratic in the span's fraction, signs from exact 192-bit products).
- **AV-05** (research E71-15, E71-18, E71-19; measured) The footprint's half extents are the
  type's custom hard extents if set, else the half extents of its model's collision bounds (the
  union of its collidable meshes' boxes in the bind pose), times `Scale_Factor`. The soft
  radius (the search's occupation radius and an obstacle's square) is
  `Custom_Soft_Footprint_Radius`, else `Space_Obstacle_Radius`, else the larger half extent,
  times `Scale_Factor`. Measured on the FoC data ([unit data](../unit-data.md#motion-table)):
  the corvette 17.76 by 41.82, the Nebulon-B 23.48 by 109.95, the mining pad radius 206.25.

### The search

- **AV-10** (research E71-01, E71-03, E71-23) The search expands path components (position,
  yaw, frame, speed). Its cost is time: a step costs the frames it takes, scaled per step type
  (AV-12) and by collisions (AV-13). Its estimate is the straight distance to the target over
  the maximum speed. The open list is a binary heap on the total cost with no tie-break: a new
  entry rises while its parent is strictly more expensive, and after taking the cheapest the
  last entry sinks, to the left child when it is strictly cheaper, then to the right child when
  that is strictly cheaper still. Components are merged by a signature cell: position relative
  to the move's start in cells of 2/3 of the forward step, and yaw in 25 bins; a closed cell is
  not expanded again by the forward, arc and end steps, and a cell that already holds a
  cheaper path drops them. Cells of the target's cell are never recorded, so the end leg can
  still reach it. The search stops after 3,500 expanded children
  (`SpacePathfindMaxExpansions`).
- **AV-11** (research E71-02) Steps: straight forward (one forward step, or less onto the
  target), an arc of 15 degrees left or right at full speed, a match turn to the tangent toward
  the target (legal for a bearing difference from 0.01 degrees to the arc angle), the end leg
  onto the target (only after a match; with the default settings only an end leg finishes the
  search, which is why S-10's corvette turns away and back), a straight speed change to full
  speed, a straight slow-down to the wait speed (`WaitOperatorSpeedCoefficient` 0.2 of full
  speed), a wait leg (100 × 0.2 = 20 units at the wait speed, `WaitOperatorBaseFrameTime`), and,
  from the start only and only when blocked ahead, an emergency turn in place in 10-degree
  probes (the ship stops first). A ship not at full or wait speed can only change speed; after a
  slow-down it can only wait; after a wait it can wait again or speed up.
- **AV-12** (research E71-06) Step cost factors: match 0.99, end 0.98, arcs
  1 + 0.5 / max(1, distance to target / 400) (the distance counts the unit's height, a retail
  quirk), wait 0.8 (`WaitOperatorCostCoefficient`).
- **AV-13** (research E71-06, E71-07) A step whose move hits a tracked ship or object: a
  static hit is dropped unless the step starts within the turn radius plus 1.2 times the soft
  radius of the move's start (`OccupationRadiusCoefficientSpace`), and an end leg is always
  dropped; otherwise (a moving hit, or near the start) the step stays but costs 15 / 0.66 ≈
  22.7 times more (`MinObstacleCostSpace`, `CurrentPathCostCoefficientSpace`). An end leg's
  query is split into its constant-speed and braking parts.
- **AV-14** (research E71-11, E71-22) A failed search is retried up to six times
  (`SpacePathingTries`) with looser settings: no wait steps and any step may finish; then
  coarser steps (+0.5 to the rotation and forward coefficients) and 1.7 times the expansions;
  then an estimate weighted by 1 / 0.66 below a distance cutoff (the retail weight is the
  previous try's cost coefficient); then only static and moving obstacles; then half speed.
  When every try fails the unit stays (AV-U5).
- **AV-15** (research E71-11, E71-16; recording S-14) Orders are planned one ship at a time, in
  command order, and each planned ship submits its prediction before the next ship plans. S-14
  shows the order: the right corvette, ordered after the left one in the same tick, plans
  around the left one's path. A unit that an earlier command of the tick destroyed has left
  its layer (AV-03) before the next order plans. The remake plans serially in the commands phase for this reason;
  the per-unit prediction sampling that the planning reads is the partitioned `tracking` phase
  ([simulation](../simulation.md#phase-map)).
- **AV-16** (research E71-22) A move shorter than 40 units plans nothing: the unit stops where
  MV-02 leaves it (the retail formation finishes; AV-U5).
- **AV-17** (research E71-22, E71-24) Before the search, a short move lowers the planning speed:
  with R = v / rate of turn (radians), from the point where the ship would reach v, a target
  inside either turn circle, or behind and nearer than 2R, scales v by max(0.1, 0.9 d / 2R)
  (at least a tenth of the maximum); then, while the target is nearer than v² / a + R, v drops
  by 10 % (FoC reads the acceleration there). Repeated until v changes by at most 0.001. For
  the corvette the second rule starts below 373 units.
- **AV-18** (project) On the target a forward step has zero length and zero cost. In Q24 such
  steps repeat until the expansion limit; FoC's binary32 steps do not land exactly on the
  target, and the S-10 recording shows the search going on to its detour. The remake drops a
  zero-length forward step.

<a id="the-destination-266"></a>

### The destination

- **AV-19** (research E266-01, E266-02, E266-04) Before the 40-unit rule (AV-16) and the speed
  reduction (AV-17), every move's target goes through the nearest open position search, and the
  path then ends on the point it returns, in the unit's height (MV-11); the ship stops there.
  The test of a point is a query (AV-04) of a square footprint that does not move, half extent
  the unit's soft radius (AV-05) times `OccupationRadiusCoefficientSpace` (1.2), facing +X,
  from the current frame to frame 2^32, in the unit's own layer and then in the static layer;
  the unit itself is ignored and any collision bit blocks (a plain move's filter is all
  bits). Because the query spans every window, a point that another ship of the layer will
  cross later on its planned path is blocked too. Ships of other layers never block. The
  search tries up to 40 rings. Ring k has radius k × `DestinationSearchRadiusIncrementSpace`
  (50 in FoC's `GameConstants`; for a move the first ring has radius 0) and
  trunc(2π × radius / query half extent) + 1 points (one point when the half extent is 0).
  Point i of n lies at i / n of a turn counter-clockwise from the direction of the destination
  to the unit, at ((cos a + 1) / 4 + 1 / 2) × radius from the destination: the full radius
  on the unit's side, half of it on the far side. The first open point wins, so ring 0 keeps
  an open destination exactly. When the unit stands on the destination the direction is zero
  and every point is the destination. When no point is open the target stays as ordered.
  FoC bounds the point count by nothing but the soft radius (the object type's soft
  footprint has no floor; research E266-06), so a tiny radius would put billions of points on a
  ring. The remake (project, nearest-open-position path clipping review) rejects a footprint whose ring 39 would hold more
  than 2^13 points at unit-table validation, since each point is a collision query (for example, the
  corvette's soft radius of 41.822 puts 245 there; at 50 a radius under about 1.25
  is rejected), caps each ring at 2^13 points for a footprint that bypassed validation,
  and ends the search after ring 0 when the direction is zero, which returns what FoC's 40
  rings of the same point return.
- **AV-20** (research E266-01; project) The remake computes the rings in Q24 where FoC mixes
  binary32 and binary64 (the point count in binary64, the angles and distances in binary32)
  and normalises the direction exactly where FoC uses a fast inverse square root. A point
  within a rounding step of an obstacle's edge, or a ring whose 2π × radius / half extent lies
  within a rounding step of an integer, can therefore come out the other way. The plan's
  recorded target is the moved point.

Against the retail recordings (FoC debug build, fixed-force recorder, fog revealed, Coruscant):

| Case | Result |
|---|---|
| S-21 station | A corvette at (-1800, 50) sent onto the gravity well station's centre (a TED-placed object, soft radius 500) stops at (-828.9965, 51.2362); AV-19's ring 12 gives (-828.9995, 51.2362). Ring 11 at 550 lies 0.19 inside the blocked 550.19, as retail also decides. |
| S-21 mining pad | A corvette at (-1800, -430) sent onto a TED-placed pad's centre stops at (-1095.9852, -428.5975); ring 6 gives (-1095.9994, -428.5976). |
| S-20 held corvette | Retail clips: the corvette stops at -184.68, the end of its speed-up and five 300-unit forward steps, inside the search cell [-200, 0) of a moved target and not in the cell of the ordered one. Its first search try failed and a retry finished on a forward step (AV-14), so the exact moved point is not visible; the remake's first try succeeds and stops on (-100, -1700) (AV-U10). Three runs agree bit for bit. |
| S-20 mining pad | A pad spawned by the Lua `Spawn_Unit` blocks nothing in retail: the corvette flies onto its centre (AV-U9). The remake tracks it and stops the corvette at (300, -1500). |

### Against the recordings

`compare_traces.py --report` against the first original run, committed tolerances:

| Case | Result |
|---|---|
| S-10 | The retail detour (15° right, 15° left, a 0.34° match, the end leg) within 0.01 units at every tick; largest pos.x 106,953 raw (0.006 units) at the stop, fwd.y 10,386 raw in the last frames where the speed falls to zero. U-01 resolved. |
| S-11 | Every field within tolerance (no change). |
| S-12, S-13 | As before the group formations and avoidance work (the search takes the MV-13 to MV-16 shape): position within 0.0016 and 0.001 units. |
| S-14 left corvette | Within 0.00 units (display rounding) at every sampled tick: speed-up, match, end. |
| S-14 right corvette | Within 0.01 units: slow-down to 0.744, one 20-unit wait leg, speed-up, match, end. |
| S-14 frigate | The mirror image of the retail detour: the same arcs, straight leg at 30°, match and end, on the other side of the held Nebulon-B (AV-U8). |

### Not covered

- **AV-U1** (resolved by multi-select formation slots) Group moves are FoC's formation system: see
  [Formations](#formations-344). S-14 uses individual orders.
- **AV-U2** Threat-aware steps and final-facing (aligned) steps are out of scope for M2 moves.
- **AV-U3** (resolved by blocked move-target clipping, AV-19, and FM-05a) The nearest open position search runs for every
  space move and for the formation slot mapping (multi-select formation slots). Verified at runtime for single moves onto
  map objects (S-21); the slot mapping's calls have no recording (FM-U10).
- **AV-U4** The map-edge rule (off-map steps cost 20 times more or are dropped) needs the map
  bounds, which the session does not have; it is not modelled. For the same reason the
  destination search (AV-19) does not drop points outside the map (FoC's
  map-bounds test, E266-03), so a clipped point near a map edge can lie off the map.
- **AV-U5** (unverified) When every try fails, or a move is shorter than 40 units, the remake
  leaves the unit at rest; FoC finishes the formation and the locomotor's reaction was not read.
- **AV-U6** (unverified) Whether a path's end re-submits the ship's prediction (moving the
  layer's window anchor) was not read; the remake does not.
- **AV-U7** The collision bounds come from the ALO meshes' stored boxes; whether FoC
  recomputes them from the vertices was not checked. The measured boxes put each ship's long
  axis in the model's Y (the models are turned 90 degrees when drawn), while the collision test
  applies the X extent along the facing, so a ship's footprint is its width along its heading
  and its length across it. That is how the debug build reads (E71-15, E71-19), unverified at
  runtime.
- **AV-U8** S-14's frigate sees two exactly symmetric detours around the held Nebulon-B (target
  and obstacle on its axis). The Q24 search and its heap order take the right-hand one, FoC the
  left-hand one. The heap, its ties, the capacity (50,000) and binary32-rounded costs were
  checked and do not explain it; the cause is somewhere in FoC's binary32 arithmetic.
- **AV-U9** (recording S-20) A `Mineral_Extractor_Pad` spawned by Lua `Spawn_Unit` is not an
  obstacle in retail: neither the destination search nor the path avoids it, while the
  TED-placed pads are (S-21). AV-01 reads every `SPACE_OBSTACLE` as submitted at spawn; the
  step that leaves a script-spawned one out was not found. The remake tracks both. M2 places
  pads only from the map, so skirmish play is unaffected.
- **AV-U10** (recording S-20) A corvette sent onto a held corvette: retail's first search try
  fails and a retry finishes on a straight forward step at -184.68, 84.68 short of where the
  remake's first try ends with the S-10 detour and end leg. The clip itself agrees (the finish
  cell); why retail's first try fails near a held ship was not found (possibly the footprints,
  AV-U7).

<a id="formations-344"></a>

## Formations

Status: implemented for move commands that name two or more ships tracked in a dynamic layer,
when the motion table carries avoidance rules. The rules come from the FoC debug build
(evidence IDs E344-nn, private map) and are checked against the owner's capture of a rebel fleet
group move (mixed-fleet group-move capture); there is no recording of a group move yet (FM-U10).

### How FoC moves a group

research E344-01 to E344-13. A player's move of the selection makes one movement coordinator for
all selected ships, with FoC's default destination: delays allowed, top speed not enforced. Each
ship becomes a formation of its own, grouped by space layer. Per layer the coordinator maps a
destination for every ship (its slot), sorts the ships front first and staggers their path
searches two frames apart; every ship plans with a speed scaled so that the group arrives
together. A single ship's move takes the same route with one member, which is the single-ship
move above. There is no steering and no formation keeping while the ships fly: each ship
follows the path it planned (AV-10 on), and the ships that plan later go around the paths of
the ships of their layer that planned before them.

- **FM-01** (research E344-01; project) A move command whose accepted units include two or more
  ships tracked in a dynamic layer (a motion profile and a footprint in the capital, frigate,
  corvette or super capital layer) is one group move. With one such ship it is the single-ship
  move. Other units of the command (no motion profile, no footprint) plan as before. A table
  without avoidance rules plans every unit as before: the mapping needs the layers and the
  footprints.
- **FM-02** (research E344-02) The group's ships are taken nearest to the target first (3D
  distance; command order on ties). The layers are processed in the order capital, frigate,
  corvette, super capital, and each layer maps and plans on its own: a layer with one ship sends
  it to the target itself, whatever the other layers do.
- **FM-03** (research E344-05) Within a layer the ships are mapped in order of their 3D distance
  from the layer's centroid, nearest first (ties keep the order of FM-02). A ship's direction is
  its XY offset from the centroid, normalised; a ship on the centroid takes its XY offset from
  the target; a ship on both keeps the target and places no slot.
- **FM-04** (research E344-07) A ship's occupation radius is its hard radius (the diagonal of the
  hard half extents, AV-05) times `OccupationRadiusCoefficientSpace` (1.2) plus its turn radius
  at maximum speed (maximum speed over rate of turn in radians) times
  `WaitOperatorSpeedCoefficient` (0.2): 82.94 for the corvette, 185.33 for the Nebulon-B.
- **FM-05** (research E344-05, E344-06, E344-08) The first ship of a layer goes one occupation
  radius from the target along its direction (to the target itself when the layer has one ship).
  Each later ship takes the first free point on the ray from the target along its direction,
  trying the target and then a point every max(50, radius / 4) units outward, 30 points at most.
  A point is free when a square of half size the ship's radius, held there from the order's
  frame to the end of the tracking horizon, hits no tracked ship of its layer outside the group
  and no static object, and when no slot already mapped in the layer lies closer than the two
  ships' radii (3D). When no point is free the ship takes the nearest open position to the
  anchor (FM-05a). The slot has the target's height; the path keeps the ship's
  (MV-11). So each layer keeps its shape as seen from its centre, directions kept and spacing
  re-packed, and larger ships, with their larger radii, sit further out.
- **FM-05a** (research E344-05, E344-06, E266-01) The first ship's point and a later ship's
  anchor with no free point on its ray go through the nearest open position search, the plain
  move's search (AV-19) with the same arguments (no turn allowance, start radius 0, filter SCT_ALL) plus
  the layer's group as the ignore list and the slots mapped so far. FoC tests the mapped slots
  against the point it was given, not against each ring point: when the point lies closer to a
  mapped slot than the two ships' occupation radii (3D), every ring point is rejected and the
  point is kept. The first ship's slot is its open position; the later ships' rays start from
  the target shifted as the first slot was. A later ship's anchor lies about one first-slot
  radius from that slot, so its fallback keeps the anchor in practice. The slot keeps the
  target's height. Each ship's own path search then clips its slot again (AV-19), ignoring only
  itself, as FoC's path search does for every formation.
- **FM-06** (research E344-03) A ship's time to reach the target is its turn to face it
  (|bearing - yaw|, wrapped to ±180 degrees) times 0.5 over its rate of turn, plus its 3D
  distance to the target over its maximum speed (after lost engines, MV-33). The group's time is
  the largest of all its ships, across layers.
- **FM-07** (research E344-04) Within a layer the ships plan front first: in descending order of
  their offset from the layer's centroid projected on the direction from the centroid to the
  target; ties keep the mapping order of FM-03.
- **FM-08** (research E344-04, E344-10; data `SpacePathfindFrameDelayDelta` 2) The k-th ship of a
  layer plans 2k frames after the order's first frame (MV-02): the front ship of every layer at
  once, the next 2 frames later, and so on; each layer starts again at 0.
- **FM-09** (research E344-04, E344-09) A ship plans with its maximum speed times its time over
  the group's time (at most 1; at least `SpaceIdleMovementSpeed`, 0). That speed replaces the
  maximum speed of its whole search, AV-17's reduction included, so the ships arrive about
  together and the group's slowest ship flies at full speed. A speed of 0 plans nothing.
  (research E344-15) When the group's time is 0 (every ship already at the target) the scaling
  is skipped: each ship keeps its full maximum speed.
- **FM-09a** (research E344-14; project, group formation, planning and speed matching) A ship whose maximum speed with its modifiers
  (lost engines times `Engines_Disabled_Speed_Modifier` included) is 0 cannot join the group.
  FoC tests that speed before it creates the ship's formation or counts its time; at 0 it
  asserts and leaves the composition before any layer is mapped, so the ships nearer to the
  target keep an unmapped formation and that ship and the farther ones get none (FM-U11).
  Retail data never gets there through lost engines (0.4); a mod with a modifier of 0 would.
  The remake follows MV-03 for that ship instead: its order is accepted, it has no motion plan
  and stays put, and the other ships are mapped and speed-matched as a group without it.
- **FM-10** (research E344-04, E344-13) Until its planning frame a waiting ship keeps its current
  plan (a ship at rest stays), and its layer predicts it only up to that frame and holding there
  after; this rebuilds the layer like a submission (AV-03). At its frame it plans from where it
  is (MV-10) against the layers as they are then. A new move, face or stop order replaces the
  wait; an attack order keeps it (MV-23).
- **FM-11** (research E344-11) A group move to a position is mapped and planned once. FoC re-maps
  and re-paths only moves onto a target object, every `MovementReevaluationFrameCount` frames.
- **FM-12** (project) Waiting ships due in one frame plan before that tick's commands, in the
  order of their groups' commands and then each group's planning order; like every plan they
  submit their predictions before the next ship plans (AV-15).

### Cases

- **C-10** Three corvettes at rest in a column across the move, 100 apart, ordered 2000 units
  ahead: the middle one sits 82.94 short of the target and the outer two 150 to each side of it;
  they plan middle, then the two outer ones 2 and 4 frames later; the middle one plans at 3.7088
  (537.66 of 539.29 frames), the outer ones at 3.72.
- **C-11** Two corvettes in a line along the move: the front one plans first and sits 82.94 past
  the target, the rear one 100 short of it.
- **C-12** Two corvettes, two Nebulon-Bs, a held Nebulon-B and a held corvette in their way: each
  layer's front ship plans at once and the second 2 frames later; the Nebulon-Bs end 185.33 below
  and 200 above the target, outside the corvettes (82.94 below, 100 above); the corvettes fly at
  about 2.4, the Nebulon-Bs at 2.64; each of them flies around the held ship of its layer.

`tests/replay/formation_tests.cpp` pins C-10 to C-12, a slot that steps past a held ship of its
layer, a wait replaced by a stop, zero-speed and zero-time groups, and worker equality.

### Against the owner capture

The owner's capture of a rebel fleet group move (mixed-fleet group-move capture: corvettes, frigates, a destroyer,
fighters and bombers; a Nebulon-B in the way) shows the fleet keeping its arrangement as seen
from its centre while it flies, the larger ships on the outside, and the ships going around the
Nebulon-B in their way on the second order. FM-03 to FM-05 and FM-10 give that picture; the
capture has no coordinates, so slots and timing are not compared in numbers (FM-U10).

<a id="the-owners-order-613"></a>

### The owner's order

In the path-search performance parity eye-check clip some ships left the formation towards the rear after the order. The
clip replays `path_bench`'s `owner` staging, which is no player order: 20 ships in a block of
five columns by four rows 400 apart, 12 of them ordered as one group move and the other 8, which
stand between them, as single moves in the same tick. The ships that fell back were group
members, the frigate layer's front Nebulon-Bs (fleet indices 13 and 12):

- The front one plans first (FM-07) while every other ship of its layer still stands at rest
  (FM-10): a single-move Nebulon-B east of it, a single-move Acclamator 566 units down its
  line to the target, another south of it and a waiting group member west. Its search leaves
  the block to the north and turns towards the target beyond it, 403 units north of its start.
  The remake's search with FoC's timing (`path_bench --search-budget` large, every search in
  its frame) takes the same route: FoC predicts a ship that has not planned at rest as well, and
  a player's order does not stand eight other ships in its group's way.
- The next one planned while the Acclamator south of it waited for its sliced search to land
  (PC-08) and flew around it as if it would stay there, 239 units north of its start; with
  FoC's timing it reads that plan and has no such turn. PC-09 gives it the plan: its route is
  then FoC's timing's (1,012 against 1,017 units to the side of its line, 1.037 against 1.038
  times the straight distance), and it no longer goes north of its start.

The slots, the layer split and the planning speeds follow FM-03 to FM-09; the debug build was
read again for group-move formation fallback: the slot directions are the ships' offsets from their layer's centroid in
world space, with no turn to the move's heading and no rows.

`path_bench --selection owner-group` stages the same 20 ships and gives them one group move to
the same point, the clip's order since the group-move formation fallback work. No ship turns back: none goes north of its start,
and no path is longer than 1.06 times the straight line (1.05 with FoC's timing). `tests/replay/formation_tests.cpp` pins a block of 16 (eight corvettes, eight
Nebulon-Bs) against FM-02 to FM-09 and its approach (no ship falls back along the order or moves
away from its slot, with every search in its frame and with every search sliced), and PC-09 on
two Nebulon-Bs in a line.

### Not covered

- **FM-U1** (unverified at runtime) A waiting ship's dummy destination ends at its frame (FM-08);
  whether its path is found in that frame's service or the next was not pinned. The remake plans
  at the frame.
- **FM-U2** (resolved by blocked move-target clipping, FM-05a) The nearest open position search moves a layer's first slot out
  of occupied space; no recording checks it yet (FM-U10).
- **FM-U3** The map-bounds test of a slot is not applied (AV-U4).
- **FM-U4** A moving ship whose current path ends before its planning frame stops there; FoC lets
  it continue its last motion at its walk speed until then (an idle destination, not modelled like
  U-04).
- **FM-U5** Squadrons (no space layer) join a group move in FoC as a formation of their own that
  escorts the group's ships; fighter spawning and simulation owns squadrons, so they are left out.
- **FM-U6** When the first ship mapped in a layer is on both the centroid and the target, FoC's
  later ships start their rays at the map origin; the remake starts them at the target.
- **FM-U7** FoC sorts with its library's `std::sort`, which keeps the order of ties for up to 32
  ships (an insertion sort); the remake keeps ties for any size. Larger groups may order ties
  differently.
- **FM-U8** `SpacePathfindFrameDelayDelta` is the retail value in code (2), not read from
  `gameconstants.xml`: reading it would change the FoC content identity (fidelity list).
- **FM-U9** FoC reads the maximum speed of FM-06 and FM-09 with the object's modifiers; the remake
  uses the speed after lost engines (MV-33) times the abilities' speed multiplier (space-abilities AB-24).
- **FM-U10** No recording of a group move: a rig recording would pin the slots, the stagger and
  the speeds in numbers.
- **FM-U11** (unverified at runtime) A group with a zero-speed ship (FM-09a): the release build's
  outcome of FoC's failed composition was not recorded. The remake's rule (the ship stays, the
  others move as a group) is the least visible choice, not FoC's.
- **FM-U12** (project, group-move formation fallback (legacy EAWR-613); fidelity list) A group member whose search starts while the search
  of an earlier ship of its layer is still running in slices (PC-08) plans with that ship held
  where it stands; FoC's would read its plan (PC-02). PC-09 closes the case where the earlier
  search has ended; the rest is measured in [The owner's order](#the-owners-order-613).
- **FM-U13** (unverified at runtime; fidelity list) A ship's own search moves its slot out of
  the routes of the group members that planned before it (AV-19 spans every window), so in the
  block of 16 of `formation_tests` the rear ships stop up to 900 units short of their FM-05
  slots. No recording shows FoC's group arriving (FM-U10).

<a id="planning-cost-503"></a>

## Planning cost

Status: PC-01 to PC-04 come from the FoC debug build (read for large-selection order stalls) and the `gameconstants.xml`
data; PC-05 to PC-08 are the remake's (large-selection order stalls, path-search performance parity): the search's internals may differ from
FoC's where the routes stay close (owner, path-search performance parity), measured against FoC's search below. The battle paused for a moment when the owner sent every unit at
the top of Coruscant to the bottom: the remake computed that order's path searches in one
tick.

- **PC-01** (research) One path per ship, never one for the group: a group move gives each ship
  a formation of its own (FM-01), and each ship runs its own search (AV-10 on) to its own slot.
- **PC-02** (research; data `SpacePathfindFrameDelayDelta` 2) Space has no queue of path requests
  and no per-frame search budget. A ship's search, with all its retries (AV-14), runs to the end
  in the frame that services the ship's formation. The only spreading is FM-08's stagger: in each
  layer the k-th ship searches 2k frames after the order, so every layer's front ship searches
  in the order's own frame. The land game's per-frame quota (`DynamicLandComplexityQuota`) and
  its random move delay for team members (`Max_Move_Frame_Delay`) do not apply in space.
- **PC-03** (research; data) The space search has no coarse grid. Its closed set is keyed on
  signature cells of 2/3 of the forward step (200 units with `XYExpansionDistanceSpace` 300)
  and 25 yaw bins (AV-10), and its collision queries go through the tracking layers' 45 windows
  of 90 frames (AV-03). `Space_Collidable_Grid_Cull_Size` (500) sizes the object manager's grid
  for culling collidable objects, not the path finder.
- **PC-04** (research) Until its search a waiting ship keeps its current plan and a ship at rest
  stays; its layer predicts it up to its planning frame only (FM-10). There is no placeholder
  move while a search is pending: the search is never pending past its frame (PC-02). The
  remake's sliced searches (PC-08) land frames later; the ship waits the same way until then,
  and its layer predicts the search's plan as soon as the search has ended (PC-09).
- **PC-05** (project, large-selection order stalls) Searches due in one frame in different layers run side by side in the
  partitioned `plan-searches` phase ([simulation](../simulation.md#path-searches-503)): a
  search reads only its own layer and the static layer (AV-01), so it never sees a search of
  another layer, and the result is the one-at-a-time result of AV-15. Searches in one layer
  keep their order and each submits before the next.
- **PC-06** (project, path-search performance parity) The bounded search. FoC's first try finishes only on an end leg
  (AV-14); when a static object stands off the final approach it drops every end leg and the
  try spends all its 3,500 expansions on a flat fan before the second try finds the path. The
  remake changes three things and keeps FoC's tries otherwise:
  1. the first try stops after 500 expansions;
  2. the second try runs as FoC's (its settings do not depend on the first);
  3. from the third try on, the estimate (the straight distance over the maximum speed, AV-12)
     is weighted by 21/20 (1.05).
  The tries share the farthest reach that FoC's fourth try reads. A search whose first try
  ends within 500 expansions is FoC's search exactly; FoC's search stays in the code as
  `PathSearchMode::exact`, the reference the path cost test measures against.
- **PC-07** (project, path-search performance parity) Search lanes and a per-tick budget. A tick's searches (due waits,
  then the orders' single moves and each group's front ships) are queued in planning order and
  planned before anything else reads or changes a layer (a group's slot mapping, a face, stop
  or other plan, any command but a move) and after the commands. Each dynamic layer is a lane:
  its searches run in order, each submitting its prediction before the next (AV-15), and the
  next search of every lane runs together in the partitioned `plan-searches` phase (PC-05), so
  a single move no longer waits for another layer's. A search starts only while its layer has
  spent fewer than `search_budget` expansions (1,000; `AvoidanceRules`, not FoC data) in the
  tick, and may spend only the rest of it: a search that reaches the budget is given up, and
  the lane's searches after it do not start. Both run sliced instead (PC-08). The first search
  of a layer always starts, so a search that fits in the budget plans in its own frame as in
  FoC (PC-02); only a long search or a burst runs sliced.
- **PC-08** (project, path-search performance parity; the owner's suggestion to compute in the background and adjust the
  heading when done) Sliced searches with a fixed landing. A search PC-07 gives up or does not
  start runs again from where the ship's current plan puts it `search_delay` frames later (4,
  0.13 s at 30 frames a second), for the plan the ship would make then, and that plan lands
  then: the ship takes it in that frame's planning, in its lane's order, and submits it. Until
  the landing the ship keeps its current plan (PC-04), predicted as holding where it is at the
  landing frame, like an FM-10 wait, until its search has ended (PC-09). The search reads copies of its layer and of the static
  layer taken when it starts, after the lane's earlier submissions, so it is a pure function
  of that frame's state; its slices run side by side in the `plan-searches` phase of the
  frames up to the landing, `search_slice` expansions (1,500) each, and in the landing frame it
  runs to its end. A slice stops only between two parents, so a search in slices is the same
  search bit for bit, whatever the slice size or worker count. A new move, face or stop in
  between drops it (FM-10). If the ship would plan from anything else at the landing than the
  search predicted (its position, yaw, speed or limits; a change the prediction could not see,
  such as an engine hit), it plans at once instead. Differences from FoC, which plans every
  order in its frame (PC-02): a sliced search plans against its layer as it was 4 frames
  before the landing, and the ship moves on its old plan (or stays at rest) for 0.13 s longer.
- **PC-09** (project, group-move formation fallback) A sliced search that has ended before its landing publishes its
  plan at once: from the frame it ends, its unit's layer predicts the unit along its current
  plan (at rest when it has none) up to the landing frame and along the search's plan from then
  on. The layer keeps its windows' anchor: the unit's submission is still the landing (AV-02,
  AV-15), where it takes the plan. A search in slices is the same search whatever the slice
  size; the frame it ends is not, so the slice size can change what a later search of the
  layer reads before the landing, and with it that search's plan. A search that starts after that reads the plan, as FoC's search
  of a ship that plans later reads the plan of a ship that planned before it (PC-02). Without
  it, a group member whose frame came while a neighbour of its layer waited for its landing
  planned around that neighbour as if it would stay where it stood (group-move formation fallback). A search that has
  not ended when a later search of its layer starts is still read as holding there (FM-U12).

<a id="against-focs-search-520"></a>

### Against FoC's search

`tactical_path_cost_tests` runs 82 searches both ways: corvettes and frigates from three sides
of Coruscant to a 3 x 3 grid of targets, onto the stations and a pad (AV-19), across a field
of 40 obstacles, and out of a crowded block across ships crossing the map, on the M2 start's
static layer with held fleets in both layers. Each path is sampled every 10 frames and
compared with the exact search's:

| Measure | Bounded against exact |
|---|---|
| Identical plans | 70 of 82 |
| Expansions | 102,629 against 180,408; at most 4,703 in one search against 8,604 |
| Route length | at most x1.018 |
| Arrival | at most x1.034 of the exact travel time |
| Largest distance between the routes | 1,157 units |
| Passing an obstacle or held ship on the other side | 2 searches |

"The other side" is a winding test: the loop the two routes close around an obstacle's
centre. The test pins these bounds (at least 70 identical, at most 2 on the other side,
length x1.02, arrival x1.035, 1,200 units, 5,000 expansions in one search) and every search's
work counts in `fixtures/path-cost.work.csv` (`--update` re-pins them). Weighting from the
second try on instead expanded about half as many nodes but put 11 or 12 of the 82 on the
other side; a 1.1 weight gave routes x1.032 longer.

### Measured

The M2 start (`tests/skirmish/fixtures/m2-start.eawr-replay`, FoC tables) at tick 3000: the human
player's two ships (one in the corvette layer, one in the frigate layer) and seven squadrons,
ordered about 9,650 units to the far side of the map. Each ship's first try runs out of its
3,500 expansions and the second finds the path (AV-14), as in FoC. The order tick took 780 to
935 ms against about 5 ms for a normal tick, all of it in the two searches: every Q24 multiply
and divide went through a 192-step bitwise division. With exact fast paths in the fixed-point
arithmetic (identical bits, `math_contract_tests`) and PC-05, the tick takes about 32 ms on 4
workers (about 48 ms on 1), and a normal tick about 1.3 ms.

The owner's benchmark (path-search performance parity, `path_bench`, [simulation](../simulation.md#path-search-cost)):
20 capital ships of the four M2 types that move, 12 as one group move and 8 as single moves in
tick 600, across Coruscant through the centre's stations and pads. On the owner's PC with
other workers keeping it near 90 % busy (the benchmark at high priority; a plain tick takes
about 5 ms there), 4 workers:

| | Order tick | p99 of the 300 ticks after | Max |
|---|---:|---:|---:|
| Before the path-search performance parity work | 94 to 118 ms | 52 to 65 ms | 94 to 118 ms |
| Faster search, same bits | 28 to 35 ms | 19 to 21 ms | 28 to 35 ms |
| PC-06 and PC-07 | 12.5 to 17 ms | 12.5 to 15 ms | 20 to 25 ms |

What remained was one search per layer per staggered tick (FM-08): the group's members plan
through the group's own predicted moves, 3,800 to 4,800 expansions with 11 leaves and 3.3
exact tests an expansion, which a per-tick budget cannot split. PC-08 splits them: in the
same benchmark 14 searches are given up at the budget and 22 land sliced, at most 12 are in
flight after a tick (6 MiB of copied views and scratch; the bound is one search per ship, about
0.5 MiB each, not 12), and no call counts more than 1,502
expansions. Run alternately on the owner's PC under the same load (other workers' builds;
4 workers, two rounds each):

| | Order tick | p99 | Slowest tick with a search | Its plan-searches phase |
|---|---:|---:|---:|---:|
| PC-06 and PC-07 | 9.9 to 10.0 ms | 13.0 to 13.5 ms | 15.5 ms | 9.1 ms |
| PC-08 | 7.8 to 8.2 ms | 8.9 to 9.7 ms | 8.7 to 9.2 ms | 5.2 ms |

A plain tick there takes 2.2 ms (p50) and up to 6 ms (p99). The slowest ticks of both runs
(about 20 ms at tick 855, 11 to 14 ms at tick 894) have no path search. `path_bench_owner_work`
pins every search's and slice's work and checks the budgets (the CTest needs the game data).

Exceptions to the budget, as in FoC's single-frame search: a search that has not ended by its
landing frame runs to its end there, so a search whose tries all fail (a boxed-in target) can count
tens of thousands of expansions in that tick; and a landing whose inputs changed (an engine hit, a
speed change) plans at once, serially, outside the budget. A new order within the 4 frames drops the
search (FM-10) and starts its own delay, so a player who re-clicks a long move faster than every 0.13 s
sees the ship keep its old plan until they stop; FoC re-plans on every click. Landing must stay after
the search starts and before an approach's re-evaluation (`search_delay` 4 < `reevaluation_frames` 10);
`validate` checks each constant alone, not that pair.

<a id="placement-597"></a>

## Placement

Status: implemented for the skirmish start (`skirmish::find_free_space`, `src/skirmish/placement.cpp`;
`src/skirmish/start.cpp`). Evidence IDs E597-01 to E597-08 are FoC debug-build readings (private map).
A new object in FoC is placed by a free-space search around a point when it is created on the
battlefield or joins as a squadron member (PL-08 lists the arrivals that skip it); the skirmish
start runs it for each starting company, which before the free-space starting placements work the remake stacked on the spawn marker.

- **PL-01** (research E597-02, E597-05 to E597-07) A lobby player's starting companies are created in
  list order, each from its spawn marker's position with the marker's facing. A company that is
  one object is placed by one search. A squadron company runs the search once per craft, in
  `Squadron_Units` order, each from the marker on its own; the craft get no `Squadron_Offsets`
  slot at creation and form up afterwards (space-fighters FM-11). The squadron's team is built
  from the placed craft; the remake puts the team container at the centre of its craft's
  bounding box, where every tick keeps it (space-visibility V-03).
  Each object is created at its point with the marker's facing, so every later search sees it.
- **PL-02** (research E597-01) The searched box is the type's model bounding box, in model axes,
  not turned by the facing. A type without a model is created on the point, and so is a type in
  the `SuperCapital` space layer. The remake's box is the model's collision bounds (the union of
  its collidable meshes, [unit data](../unit-data.md#motion-table) AV-05) times `Scale_Factor`,
  in X and Y (PL-U1); a type without collision bounds is created on the point.
- **PL-03** (research E597-01, E597-05) The candidates lie on rings around the point, from radius 0
  in steps of 1.2 times the box's larger side (X or Y), while the radius is below 2500. On each
  ring the bearing starts at the marker's facing yaw less 45 degrees (measured from world +X, counter-
  clockwise) and goes counter-clockwise in 22.5-degree steps through a full turn, 17 bearings
  with the last repeating the first; radius 0 is tried once. The first free candidate wins.
  Evidence (debug build, the per-object battlefield placement, E597-05, re-read for free-space start placement): the call
  passes the facing's Z component less 45 as the start angle, and that facing is the Euler
  triple in degrees the object is created with, so Z is the yaw (the exit-door variant adds a door
  angle in degrees to it and turns (1,0,0) by the sum). It is not a constant -45: Coruscant's
  Rebel spawn marker faces 336 degrees and the Empire's 149, so the first bearings are 291
  (-69) and 104 degrees. A marker at yaw 0 gives -45, which is why a start with yaw-0 markers
  cannot tell the two readings apart (the tests now also place at 336 and 149).
- **PL-04** (research E597-01) A candidate is free when its box, moved to the candidate, overlaps
  no blocker. Height never matters: the box's height range is widened by 1000 up and down and
  each blocker is moved to the candidate's height before the test. The remake tests open
  intervals, so boxes that only touch do not block (PL-U2).
- **PL-05** (research E597-01) Every collidable object blocks with its world-space bounds (the
  axis-aligned box around its turned model box), except decorations, objects of the background
  scene, objects in the `SuperCapital` layer, land obstacles, two further behaviour kinds (PL-U3)
  and an object the caller names (the one the new object leaves from). In the remake's start the
  blockers are the map objects with a footprint ([unit data](../unit-data.md#motion-table)), the
  stations, and each ship and craft already placed (PL-U4).
- **PL-06** (research E597-01; Coruscant) Because the new box is tested unturned while the placed
  object is then turned (PL-02), turned footprints can clip at the edges. On the Coruscant start
  no two ships or stations overlap; with the remake's boxes (and the start angle of PL-03) an
  X-wing clips a Y-wing of the Rebel squadrons by about 2.1 units and nine pairs of TIE
  Interceptors clip each other by about 1.05. Which pairs clip depends on the start angle, so the
  pairs of an earlier constant-angle start (a TIE Interceptor against the Tartan by about 11) are
  gone. That such a clip can exist follows from FoC's rule, not from a remake deviation; whether
  FoC's own boxes produce these particular ones depends on PL-U1.
- **PL-07** (research E597-05) When no candidate within 2500 is free the object is created at the
  world origin: the start's placement does not check for a failed search.
- **PL-08** (research E597-04, re-read for free-space start placement) A space reinforcement searches only for the members
  of a squadron or a fleet breakdown: each is searched from the arrival point with a start angle
  of 0 (world +X), and a member whose search fails is put on the point itself; every member's
  height is then set to 0. A single ship (a type with no squadron members and no ground company
  members, such as a bought Nebulon-B) is created directly on the arrival point at height 0
  with **no search at all**, so it may overlap what stands there; a transport is likewise placed
  without a search. Bought units' arrival (simulation economy, build queue and arrivals, purchase UI and reinforcement placement) reuses `find_free_space` for the squadron
  and fleet members only, with the fixed start angle 0, the point as the fallback and height 0. The
  "height 0" is the arrival point's: the point is on the plane, and object creation then raises each created
  unit by its `Layer_Z_Adjust` (LZ-01). The sim does this in `execute_economy` (station purchasing and reinforcements); whether the reinforcement
  call itself sets the raise flag is PU-G26 in [space purchasing](space-purchasing.md).
- **PL-09** (research E71-01 to E71-24, AV-01 to AV-19) FoC does not push overlapping ships apart:
  its ships do not steer, avoidance happens only when a move is planned, and a ship at rest holds
  its position. FoC does have a resolver for stopped-against-stopped and moving-against-moving
  overlaps (infantry, vehicles, garrisons), but it lives in the collision system only the land
  game mode provides: the base mode and the space mode give none, and the resolver returns at
  once without one (debug build, reviewed for free-space start placement). The remake adds no separation. Craft in flight steer around ships and never around
  each other (space-fighters FD-10, FD-11, squadron dogfight pairing and chase); that steer is not a push-apart of units at rest.

### Placement unknowns

- **PL-U1** (unverified) Which box the model bounding box is (all meshes, or the collidable ones
  the remake uses) and whether it includes `Scale_Factor` was not read. A larger box spreads the
  start further and removes some PL-06 clips.
- **PL-U2** Resolved for FoC (debug build, the box overlap test, E597-01, re-read for free-space start placement): the
  test is inclusive, the distance between centres on each axis is at most the sum of the half
  extents, so boxes that only touch block. The remake's `find_free_space` keeps open intervals, so
  it lets touching boxes stand; the ring step is 1.2 times the box's larger side, so a touching
  candidate almost never arises and no start moves. Deviation on the fidelity list.
- **PL-U3** Two of the behaviour kinds a blocker may not have were not identified; neither the
  map objects nor the units of the M2 start are known to have them.
- **PL-U4** (unverified) Whether the pre-built stations exist before the starting forces, and
  whether squadron craft are in the collidable set, was not read. On Coruscant the stations are
  more than 1000 units from the spawn markers, so only the craft question can change a placement;
  the remake counts craft as blockers (the less visible choice: craft never start inside each other).
