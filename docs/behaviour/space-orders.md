# Space orders: attacking a target out of range, attack-move and guard

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. P2 gap 1
  (EAWR-452, EAWR-73's G-W2): a ship
  ordered to attack a target outside its weapon range closes on it; attack-move and guard work as
  in FoC, for the player and for the AI (EAWR-79's `Attack_Move`, `Guard_Target`, `Attack_Target`).
- Bounded question: how a ship given an attack order on a unit, an attack-move or a guard order
  moves; where it stops; how it keeps station against a moving target; how the player gives each
  order. Weapon choice and fire are [space weapon fire](space-weapon-fire.md) and
  [space targeting](space-targeting.md); the moves themselves are [space movement](space-movement.md).
- Source tags: **research** (the FoC debug build read under the [clean-room rule](../clean-room.md);
  evidence IDs E452-nn are opaque and their map stays private), **data** (`gameconstants.xml`,
  `commandbarcomponents.xml` and the unit XML of the M2 lock), **project** (a remake decision).
  No original-game recording of these orders exists yet (OR-U4).
- Out of scope: squadron craft, which have no space layer and divert on their own (OR-13; a
  squadron's attack-move and guard are [space fighters](space-fighters.md) FO-05 and FO-06); waypoint queues (Alt), the compass facing
  of a right drag, abilities and hardpoint-targeted attacks.

## How FoC does it

research E452-03, E452-04, E452-18, E452-23. Every order that moves ships goes to a movement
coordinator: a move names a point; an attack names the target unit (after the attack itself has
set the ship's target); a guard names the guarded unit with an escort flag; an attack-move is a
move with an "attack on path" flag. A space coordinator maps a unit destination onto slots around
the unit (E452-07 to E452-09) and checks its ships every few frames (E452-10, E452-11). Whether a
moving ship may break off to engage something is a separate question the formation answers
(E452-14): in space the answer is always no for a ship with a space layer, so attack-move and
guard differ from a move only for craft.

## Interface

- Inputs: attack (replay opcode 3), attack-move (opcode 6) and guard (opcode 7) commands of the
  unit's owner ([replay-format.md](../replay-format.md#commands)); an attack-move or guard names a
  point and, when nonzero, a unit. The unit's `Targeting_Max_Attack_Distance` (last authored value,
  from the combat table), its motion profile and limits, the target's position, facing, current
  path and hardpoints. `MovementReevaluationFrameCount` (10) and `Space_Guard_Range` (750) from
  `gameconstants.xml`, carried by the motion table's rules.
- State: the order (kind, tick, point, unit) as before, and per unit its last approach mapping's
  prediction frame (state block `APPR`, only after a mapping planned an approach).
- Outputs: the unit's movement plans (MV rules), hence its positions.
- Cadence: the first mapping runs with the order's commands; afterwards the partitioned `orders`
  phase checks each approaching unit every reevaluation interval, after targeting and before the
  tick's commands, and the new paths plan serially in ascending unit ID after the attack turns
  (A-04), as every plan with avoidance does (AV-15).

## Rules

### Player input

- **OR-01** (research E452-02, E452-24, E452-26; data E452-25) A right click orders the selection:
  on a hostile unit an attack, anywhere else a move. With Ctrl held, a click on empty space is an
  attack-move and a click on a hostile unit is an attack (FoC marks it "attack on path", which
  changes nothing for a ship, OR-11). With Ctrl and Alt held, a click on an own unit (or an allied
  unit of community property) that is not selected guards it, and a click on empty space guards
  that point (OR-16); a hostile unit is still attacked. The command bar has Attack,
  Attack Move, Move, Waypoint, Full Stop and Guard buttons, whose default keys are A, T, M, (none),
  S and G; an armed mode does what its modifiers do, turning one mode on turns the others off
  (E452-01), and giving the order disarms it. Alt alone queues a waypoint (not modelled).

### Attacking a unit out of range

- **OR-02** (research E452-03, E452-23) An attack order replaces the unit's movement: its
  coordinator's destination becomes the target unit. This supersedes MV-23's "movement unchanged":
  a moving ship given an attack order stops following its old path (OR-05).
- **OR-03** (research E452-06) The approach distance is the unit's `Targeting_Max_Attack_Distance`
  plus the target's hard extent (the smaller of its X and Y extents, or its soft radius when a
  hardpoint is aimed at). *Project:* extents and soft radii are not loaded and count as zero, as in
  A-07 and P-04, so the approach distance is the attack distance. A unit without an attack distance
  has approach distance zero.
- **OR-04** (research E452-07, E452-11, E452-19) The unit is in range when its planar distance to
  the target's aim point is at most the approach distance, inclusive: A-04's test and aim point
  (the target's live targetable hardpoint nearest the unit, else its position). FoC also rejects a
  unit closer than `Targeting_Min_Attack_Distance`; no M2 ship authors it and it is not loaded.
- **OR-05** (research E452-07, E452-08, E452-09) A mapping of a unit: when it is in range it holds,
  dropping its path where it is like a stop (MV-22) and staying at rest if it is (*project:* a turn
  in place under way, A-04's, carries on; FoC's handling of it is not traced); otherwise it gets
  the approach slot: the point 0.9 × the approach distance from the target's predicted position
  (OR-07), on the planar line from that position towards the unit, and plans a move there (MV-10 to
  MV-19; with avoidance rules AV-10 to AV-15 and the destination search of AV-19 when the slot is
  occupied). A unit on the target's own XY gets the predicted position, which moves nothing (MV-03).
  FoC tries the slot on that line first and turns it about the target in 4, then 5 degree steps
  while it is blocked (OR-U1).
- **OR-06** (research E452-10, E452-11, E452-22) The first mapping runs with the order and plans
  from the next frame, like a move (MV-02). Afterwards the unit is checked every
  `MovementReevaluationFrameCount` frames (10) counted from the order's tick: it keeps its movement
  while it is in range now (OR-04), or while the end of its current movement lies within the
  approach distance of the target's predicted position (OR-07); otherwise it is mapped again
  (OR-05). The end of its movement (research E452-11, E662-01) is its tracked prediction past its
  last frame: where its path leaves it (the last frame before the path's last node, MV-32; AV-02),
  or where it is when it has no path. A ship flying a path to a slot in range is therefore never
  re-planned on the way: it plans once per mapping and flies the path it got, detours included. So a ship flies to its slot and stops
  there, inside its attack distance; when the target leaves that distance, the ship closes again
  within ten frames of the next check; when it arrives at rest in range, A-04 turns it to bring its
  weapons to bear. FoC starts each coordinator's checks at a frame given by its ID (OR-U2).
- **OR-07** (research E452-12, E452-13, E452-27) The predicted position is where the target will be
  at the prediction frame on its own planned path (its position when it has no path or the frame
  has passed). The prediction frame is the current frame plus the whole frames of t, where t solves
  |D + V t| = s t for the first nonnegative meeting: D is the target's position minus the unit's,
  V the target's facing (planar) times the top node speed of its current path (zero at rest or
  turning), s the unit's maximum speed after lost engines (HD-11). With |V|² − s² below 0.0001 in
  magnitude the equation is taken as linear. t is then reduced to t × (1 − approach / |D|), and to
  zero when |D| is less than the approach distance. With no meeting the prediction frame is the
  current frame. FoC clamps the predicted position to the map bounds; the remake has none there.
  The check of OR-06 uses the prediction frame of the unit's last mapping.
- **OR-08** (research E452-11; project) An approach ends when the order is replaced (A-03 extends to
  attack-move and guard), or when the unit's player-ordered target is dropped (T-01: dead, gone or
  fogged). A target fogged to the unit's owner is not followed while fogged. *Project:* when the
  approach ends the unit keeps its current movement; FoC's reaction to a lost coordinator target
  is not traced (OR-U3).

### Attack-move

- **OR-10** (research E452-02, E452-18) An attack-move is a move with the "attack on path" flag,
  for the player and for the Lua `Attack_Move` of a unit or task force.
- **OR-11** (research E452-14 to E452-17, E452-20) In space, a unit whose type has a space layer
  (capital, frigate, corvette, super capital) never breaks off a move for a target: the formation's
  diversion test refuses it unless the type sets `Disregard_Space_Layer_For_Guard_Attack_Move`,
  which no FoC XML does, so its targeting scan range stays its attack distance and a candidate out
  of range is not suitable. A ship on attack-move therefore flies exactly its move, firing at what
  comes into range through its own target choice and opportunity fire (T-05, W-04). It does not stop
  to fight and there is nothing to resume. The remake plans a ship's attack-move to a point as a
  move, group moves included (FM-01 to FM-10).
- **OR-12** (research E452-18, E452-07) An attack-move towards a unit (the AI's `Attack_Move(unit)`)
  approaches it as OR-02 to OR-08 do, with the "attack on path" flag, but without making it the
  unit's target.
- **OR-13** (research E452-14, E452-15; data) Craft, which have no space layer, do divert: within
  `Attack_Move_Response_Range` of their path on attack-move, `Guard_Chase_Range` when guarding and
  `Idle_Chase_Range` otherwise, each extended by `Autonomous_Move_Extension_Vs_Attacker` against a
  unit threatening them or the guarded unit. The remake diverts the squadron as one, by its FT
  scan with these ranges (space-fighters FO-05, FO-06); the extension against a threatening unit
  and each craft's own diversion are not implemented (the fidelity list).

### Guard

- **OR-14** (research E452-04, E452-06, E452-07, E452-11) A guard order gives the unit the guarded
  unit as its destination with the escort flag. Its approach distance is the smaller of the unit's
  attack distance (OR-03) and `Space_Guard_Range` (750); its in-range test is the planar distance
  to the guarded unit's position (not an aim point), inclusive; no minimum distance applies. It is
  mapped (OR-05) and checked (OR-06) like an attack: in range it holds; otherwise it plans towards
  the slot at 0.9 × that distance on the line to itself. Every M2 ship authors an attack distance
  of at least 800, so each follows the guarded unit to 675 units whenever it has drifted more than
  750 from it and its current movement does not bring it back within 750.
- **OR-15** (research E452-14) A guarding ship does not chase the guarded unit's attackers (OR-11);
  it fires at what comes into range. Its leash is the 750-unit guard range. `Guard_Chase_Range`
  (1000 on the M2 ships) applies to craft only (OR-13).
- **OR-16** (research E452-24, E452-14) A guard of a point is a move with the escort flag; in space
  a ship moves there (OR-11). The remake plans it as a move.
- **OR-17** (project; research E452-24) A guard or attack-move may name any live unit, own, allied
  or hostile, except the ordered unit itself, which is rejected with `target_is_unit`; FoC's input
  only offers units outside the selection. A named unit that is not live rejects every listed unit
  with `target_not_live`. The guard ends as OR-08 says when the guarded unit dies.

## Project choices

| Rule | Choice |
|---|---|
| OP-01 | Replay opcode 6 is attack-move and opcode 7 guard, each with an int64 point (x, y, z raw) and a uint64 unit: the unit is zero for a point order, and an order that names a unit keeps no point (its units store none, whatever the command carries) ([replay-format.md](../replay-format.md#commands)). They are additions to format version 2: files without them are unchanged. |
| OP-02 | Each unit is mapped and checked on its own, from its own position; FoC maps a coordinator's ships of one layer together, spreading them on an arc around the target from their centroid (OR-U2). Ships of one order that share a slot are separated by the destination search (AV-19). |
| OP-03 | The checks of OR-06 run in the partitioned `orders` phase: each approaching unit reads the moved units with this tick's targets and writes only its own slot. The new paths plan serially in ascending unit ID after the attack turns and before the tick's commands, because each plan submits a prediction the next one plans against (AV-15). Results are identical for any worker count. |
| OP-04 | The `APPR` state block holds each unit's last mapping frame only after a mapping planned an approach, so replays whose attack orders never close on a target keep their hashes. |

## Cases

- **C-01** (OR-05, OR-06) A ship with attack distance 800 and top speed 3 at rest at (0, 0) facing
  +X is ordered at tick 0 to attack a stationary enemy at (3000, 0): it plans a move to (2280, 0) from
  frame 1, stops there at rest and later fires; it never stands farther than 800 from the target
  once it has stopped.
- **C-02** (OR-05) The same ship ordered to attack an enemy at (500, 0) keeps its movement (at rest)
  and turns toward it as A-04 says, without moving.
- **C-03** (OR-06, OR-07) The enemy of C-01, once the ship has stopped, moves away along +X: within
  ten frames of it leaving 800 units the ship plans a new approach and closes again.
- **C-04** (OR-14) A ship guarding a friendly that moves 3000 units away follows it, and stops 675
  units short of the friendly's position at its prediction frame.
- **C-05** (OR-11) An attack-move to a point plans exactly the path of a move to that point.
- **C-06** (OR-17) A guard naming the ordered unit is rejected with `target_is_unit`; one naming a
  dead unit with `target_not_live`.
- **C-07** (OR-06; EAWR-662) The two Tartans of S-29, ordered to attack a stationary corvette 3000 units
  away, are each mapped once: their checks find the ends of their paths in range, so they fly the
  paths planned from the order to their slots without re-planning, and their headings swing no
  more than those paths' own detours do (no swing every check).

## Unknowns

- **OR-U1** FoC's slot search turns a blocked slot about the target (4 then 5 degree steps, up to a
  bound from the group's radii) and shrinks the ring when no slot fits; the remake plans to the slot
  on the line and lets the destination search of AV-19 move it.
- **OR-U2** FoC checks a coordinator's ships by layer (corvettes, frigates, capitals, super
  capitals) at frames staggered by coordinator ID, remaps a whole layer when any ship needs it, and
  staggers the new plans by `SpacePathfindFrameDelayDelta`; the remake checks each unit at its
  own order's interval.
- **OR-U3** What a ship does when its coordinator's target unit dies or leaves is not traced; the
  remake keeps its current movement (the least visible choice).
- **OR-U4** No rig recording of an attack on a target out of range, an attack-move or a guard
  exists; the rules rest on the debug build. A recording (fog off) of a corvette ordered to attack a
  frigate 3000 units away, and of a guard of a moving frigate, would confirm the stop distance and
  the check interval.
- **OR-U6** `MovementReevaluationFrameCount` (10) and `Space_Guard_Range` (750) are the motion
  rules' defaults, FoC's `gameconstants.xml` values; the unit tables read them at the next FoC
  identity re-pin, as `SpacePathfindFrameDelayDelta` (FM-U8), so the pinned content identity and
  the M2 start stay unchanged. The GameConstants audit (EAWR-626) takes all three in one re-pin (EAWR-663).
- **OR-U5** FoC aims the approach at its best target hardpoint (G-W3) and adds the target's soft
  radius; the remake measures to the nearest live hardpoint with no radius, as A-07.
