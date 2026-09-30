# Walk: fighter and bomber squadrons, per frame

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. The subsystem walk of
  2026-09-29 (walk 1 of the coordinator's list): every rule the FoC debug build applies to a
  squadron and its craft each frame, in evaluation order, with the gaps against the remake.
- Sources: **debug build** (the FoC debug executable with symbols, read under the clean-room rule;
  evidence IDs EWSQ-nn are opaque and their map stays private), **recording** (the fidelity traces
  of [tests/fidelity](../../../tests/fidelity/README.md)), **data** (a tag or value in the FoC XML),
  **unverified** (not settled by either). Earlier evidence families (E75, E457, E452, FS) keep their
  meaning from [space-fighters](../space-fighters.md).
- Out of scope, recorded only as an interface: what targeting does inside its scan (walk 3,
  weapons, and [space-targeting](../space-targeting.md)); the formation and movement-coordinator
  code that plans a squadron's path, its escort and its diversions (walk 4, movement); hyperspace
  arrival; land bombing runs. The spawner side of launch is covered by FL-01 to FL-11 and is only
  compared here.

## Scope

- **Objects.** A squadron is a team container (the object `Squadron_Units` builds) plus its craft.
  The container carries the team service and a team locomotor; each craft carries the fighter
  locomotor, a targeting service and its weapons. A carrier or station carries the spawner
  (`SPAWN_SQUADRON`).
- **Cadence (debug build, EWSQ-02).** The craft's locomotor, the craft's targeting and the team
  service are each serviced every logical frame (1/30 s). The spawner is serviced every 30 frames
  (FL-01).
- **Entry points in the frame, in words.** Every object is serviced once a frame in the object
  manager's order, and each of its services in turn. A squadron's craft are created before their
  container (FL-06), so the craft are serviced before the container within a frame
  (**unverified**: the object manager's order was not read). Per craft, the locomotor service runs
  the rules WSQ-01 to WSQ-40 below; per craft, the targeting service runs WSQ-41 to WSQ-46; per
  container, the team service runs WSQ-47 to WSQ-53 and the team locomotor WSQ-54.

## Rules, in evaluation order

### The craft's locomotor service (every frame)

- **WSQ-01** (debug build, EWSQ-01) A craft in hyperspace is serviced by the hyperspace rules only
  (not M2). Otherwise the service first runs the flip upkeep (WSQ-24), then the grid upkeep
  (WSQ-02), then the state's service (WSQ-03).
- **WSQ-02** (debug build, EWSQ-01) Grid upkeep, for a craft in a squadron:
  - unless the craft's state is idle, the squadron gives up its idle-grid cell (WSQ-49);
  - unless the craft's state is directed combat, the squadron gives up its combat-grid cell
    (foc-battle-world-ui WU-25);
  - unless the leader's state is directed combat, the squadron's "in the combat grid" flag clears
    (the world UI's icon grid, WU-25).
- **WSQ-03** (debug build, EWSQ-01) The craft's state picks the service. States: none, moving,
  idle, directed combat, dead stop.
  - *None*: if the squadron's formation has an attack target, the state becomes directed combat
    and WSQ-13 runs; otherwise combat ends (WSQ-21), the state becomes moving and WSQ-10 runs.
  - *Moving*: the same test; with an attack target it switches to directed combat, else WSQ-10.
  - *Idle*: WSQ-04. *Directed combat*: WSQ-13. *Dead stop*: WSQ-38.
  - A craft is created idle, its hold point its container's position (WSQ-58).

### Idle

- **WSQ-04** (debug build, EWSQ-03) An idle craft is out of combat: its defense takes the type's
  `Out_Of_Combat_Defense_Adjust` (space-damage DG-26). The same holds while moving (WSQ-10) and in
  a dead stop (WSQ-38); directed combat sets it to 0 (WSQ-13).
- **WSQ-05** (debug build, EWSQ-03) A craft outside any squadron idles by itself: no turn, pitch
  back to level, speed toward the GameConstants `Space_Idle_Movement_Speed` (the FoC data does not
  set it; the code default was not read), or 0 where its position is not a legal space position.
  No M2 craft flies outside a squadron.
- **WSQ-06** (debug build, EWSQ-03; space-fighters FM-20) A squadron craft idles around the
  squadron's hold point, which is the leader's desired position (after WSQ-50, the centre of the
  squadron's idle cell). With `d_self` the craft's planar distance to the hold point and `d_lead`
  the leader's, floored at 1 (`d_lead` only; `d_self` is not floored, and a craft exactly on the
  hold point takes the offset (1, 0, 0) instead, so `d_self` is never zero):
  - if `d_lead` is under 90, the wanted speed is 0.5 x `d_self` / `d_lead` (every craft, the
    leader included: the leader crawls at 0.5 units a frame, a far follower flies faster, with no
    cap), and the leader aims 60 units out from the hold point along its own bearing from it,
    turned by 10 / `d_self` radians, so it circles;
  - otherwise the wanted speed is the craft's current minimum speed (WSQ-12);
  - a follower always aims at the leader's position plus 40 frames of the leader's velocity plus
    three quarters of its slot less the leader's slot (WSQ-51);
  - the debug console's preserve-height toggle keeps the aim at the craft's own height (off by
    default; not modelled).
  The craft then steers around ships (WSQ-35) or else heads for the aim (WSQ-25) as an idle craft
  (no loop, WSQ-26), and moves by its velocity (WSQ-40).
- **WSQ-07** (debug build, EWSQ-03) After it moves, an idle craft whose squadron's formation has a
  live attack target switches to directed combat for the next frame.

### The idle grid (the anti-stacking rule)

- **WSQ-08** (debug build, EWSQ-23, EWSQ-24) FoC lays an idle grid over the map's bounds: cells of
  120 units, rows along Y from the bounds' low corner, odd rows shifted by half a cell in X (the
  same layout as the 400-unit combat grid, WU-25). A cell's point is its centre at the craft
  type's `Layer_Z_Adjust` height. A cell holds at most one squadron, of any side: a squadron can
  take a cell only when nothing occupies it.
- **WSQ-09** (debug build, EWSQ-13, EWSQ-24, EWSQ-25) When a squadron reverts to idle (the end of a
  move, the end of combat, WSQ-11, WSQ-14) it claims an idle cell for its hold point:
  - a squadron that already holds a cell keeps it;
  - else it takes the cell under its desired position when that cell is free;
  - else it searches rings around that cell (the ring's border cells, and the start cell, row by
    row from the low corner), examining at most 64 cells in all. A candidate is a cell with no
    squadron in it. Each ring's candidates are scored by the squared distance from the cell point
    to the desired position; the first strictly lowest wins. A ring without a candidate widens the
    search; after 64 cells without one the squadron claims nothing and keeps its desired position;
  - the claimed cell's point becomes the desired position of the container and of every craft.
  So two squadrons that go idle at one point (two escorts of one carrier, two squadrons ending the
  same move) hold two different cells at least 120 units apart; they never stack.
- **WSQ-10** (debug build, EWSQ-08) *Moving.* The craft is out of combat (WSQ-04).
  - Without a path, it flies on along its velocity.
  - The leader advances every member's path segment once it is past the current segment's half
    plane. At the path's end, unless the craft is in directed combat, every member's desired
    position becomes the path's last point at the `Layer_Z_Adjust` height, its desired facing the
    leader's, and every member reverts to idle (WSQ-11, which claims the idle cell, WSQ-09).
  - Otherwise every craft forms up (WSQ-17) at the `Layer_Z_Adjust` height at its current maximum
    speed (WSQ-12), or at the formation's speed when the formation sets one (movement walk).
- **WSQ-11** (debug build, EWSQ-13) *Revert to idle* gives up the squadron's idle cell, clears the
  player's direct move and attack flags, sets the state idle, ends combat (WSQ-21), clears the
  craft's paths and claims an idle cell (WSQ-09).
- **WSQ-12** (debug build, EWSQ-09) A craft's current maximum speed is its type's `Max_Speed`
  (times `Object_Max_Speed_Multiplier_Space`, FM-01), except on an escort move with a target: then
  it is the formation's override maximum speed. Its current minimum speed is `Min_Speed`, on such an
  escort the lesser of that and half the override. How the formation sets the override belongs to
  the movement walk.

### Directed combat

- **WSQ-13** (debug build, EWSQ-10) *Directed combat.* The craft's out-of-combat defense is 0
  (DG-26). The target is the squadron's formation's attack target (with a hardpoint when it names
  one); when there is none, or it is being deleted, the squadron attacks the first enemy squadron
  joined in its combat cell (FD-09).
- **WSQ-14** (debug build, EWSQ-10) With no target, or a target the craft's owner cannot see
  (fogged):
  - a craft still on an unfinished path keeps moving (WSQ-10);
  - otherwise the leader clears the player's direct attack flag, sets every member's desired
    position to its own position plus one frame of its velocity at the `Layer_Z_Adjust` height,
    and reverts every member to idle (WSQ-11: each claims the idle cell nearest that point);
  - a follower does nothing else that frame: it flies on along its velocity. (FD-09 is the
    leader's part.)
- **WSQ-15** (debug build, EWSQ-10) The squadron fights at close range when it records the same
  combat cell as its target, or when its leader is within the strafe reach of the target:
  `Strafe_Distance` plus the target's soft radius, in the plane (to the hardpoint's position when
  the target names one). At close range the leader clears every member's and the container's
  paths; a target that is a squadron starts the dogfight (WSQ-19), anything else gives up the
  combat cell and runs WSQ-18.
- **WSQ-16** (debug build, EWSQ-10; FA-07) Beyond the strafe reach the squadron gives up its combat
  cell; a craft on an unfinished path flies the approach as a move (WSQ-10: out of combat, in
  formation, at the layer height); otherwise it steers around ships (WSQ-35) or heads straight
  for the target at its current maximum speed (FA-01).
- **WSQ-17** (debug build, EWSQ-06, EWSQ-07; FM-11, FM-12, FM-13) *Form up* at a speed `p` and a
  height `h`:
  - the wanted speed is the formation speed (WSQ-18a) between 0.5 `p` and 1.5 `p`; when it exceeds
    `Max_Speed`, the frame's turn, thrust and lift budgets are all scaled up by wanted over
    `Max_Speed`;
  - the aim point: the leader on a path looks along its path's current segment, a turn radius
    ahead (360 / `Max_Rate_Of_Turn` x speed / 2 pi, with the speed the greater of the wanted
    speed and the craft's own), plus the formation's Y-offset coefficient times the segment's left
    normal (G-F8); otherwise the aim is the leader's position plus a turn radius along the
    leader's velocity (its facing when it stands still);
  - the aim's height eases toward `h`: when the craft is closer to the aim in the plane than the
    distance it needs to level out (|pitch| / `Max_Lift` x speed), the height is interpolated by
    that share; then the craft's slot less the leader's (WSQ-51) is added;
  - it steers around ships (WSQ-35) or heads for the aim (WSQ-25).
- **WSQ-18a** (debug build, EWSQ-05; FM-12) *Formation speed* for a follower, from a wanted speed
  `w` and bounds `slow` and `fast`: only when the squadron's formation error (WSQ-52) exceeds
  `Squadron_Formation_Error_Tolerance` in size, and only by the craft's own error `e` along the
  leader's velocity (its offset from its slot):
  - `e` above the tolerance (ahead): the speed blends from the leader's speed toward `slow` by
    `e` over the distance to slow from `w` (clamped to 0..1);
  - `e` below minus the tolerance (behind): it blends toward `fast` by -`e` over the braking
    distance from the greater of `fast` and the craft's current speed down to `w`, bounded only
    below by 0, so a far-behind follower is asked for more than `fast`;
  - otherwise, and always for the leader, the speed is `w`.
  Nothing caps the result (WSQ-37). The braking distance grows with the craft's own speed, so the
  asked speed falls as the craft speeds up: with `Max_Thrust` 1 (the FoC craft), a follower
  2000 units behind a leader at 3.6 settles near 19 units a frame, 20 000 behind near 42.
- **WSQ-18** (debug build, EWSQ-11; FA-01 to FA-04, FS-04) *Against a ship* (any target that is
  not a squadron). The leader, beyond the strafe reach (in the plane), heads for the
  target at its formation speed between `Min_Speed` and 1.5 `Max_Speed` (for the leader that is
  `Max_Speed`), steering around ships; within it, while the target lies inside 45 degrees of yaw
  and pitch of its nose and within the reach in 3D, it keeps heading for the target. Otherwise, and for every follower, the
  craft forms up (WSQ-17) at `Max_Speed` at the target's height. **A lone craft target (not in a
  squadron) is a ship here**: FoC gives it the same runs.
- **WSQ-19** (debug build, EWSQ-12; FD-01 to FD-08) *Dogfight against a squadron.* As FD-02 to
  FD-08 say; in brief:
  - while the target squadron records no combat cell, the squadron records the WU-25a cell
    without joining it; its leader heads for the target at its actual maximum speed, the others
    form up at `Max_Speed` at the target's height;
  - once the target records a cell the squadron joins it; a craft farther than 400 / sqrt(2) from
    the cell point in the plane flies back (the leader to the cell point, the others form up at
    the layer height);
  - within that reach: retaliation (FD-04), then a running chase is followed (WSQ-33) or, when
    the chased craft cannot be followed, headed for at the current maximum speed with avoidance;
    a craft with no chase and a run-out timer pairs (FD-06); a craft with no chase and a running
    timer (a chased craft) flies as outside the reach.
  - **The dogfight's leash is the cell's reach**: a chaser follows its chased craft only while the
    chaser itself is within 400 / sqrt(2) of the cell point; past it, it turns back. A chase has
    no other distance limit.
- **WSQ-20** (debug build, EWSQ-16) *Pairing when a target is acquired.* When a squadron craft's
  targeting hands its squadron a squadron target while the squadron's formation still attacks
  something else, the team locomotor notifies every member. A member whose chase timer has run
  out immediately chases the first craft of the target squadron (roster order) whose own timer has
  run out, without the follow test (FD-05); both timers restart at ten seconds and the chased
  craft drops its own chase (FD-06's bookkeeping). So a squadron that turns on another squadron
  can pair its craft with the enemy's before it reaches the cell. The call site's gating is only
  partly read (**unverified** in detail: when exactly the formation's attack target differs).
- **WSQ-21** (debug build, EWSQ-13) *End of combat*: the squadron clears the player's direct
  attack flag and its attack target, and gives up its combat cell. A craft outside a squadron
  clears its own target.
- **WSQ-22** (debug build, EWSQ-14; FD-09) A dead target is replaced by the first live enemy
  object joined in the squadron's combat cell: the squadron is ordered to attack it (not as a
  player order).

### Flight (heading, turn, pitch, speed, move)

- **WSQ-23** (debug build; FM-02) A heading toward a point: the point in the craft's frame
  (unyawed, then unpitched), its yaw and pitch clamped to [-180, 180).
- **WSQ-24** (debug build, EWSQ-15; FM-06) Flip upkeep at the start of every service: a pitch past
  90 degrees re-expresses the facing turned about (yaw and roll plus 180, pitch mirrored less one
  `Max_Lift` step).
- **WSQ-25** (debug build, EWSQ-04; FM-02 to FM-05) *Head for a point* at a speed: nothing happens
  when the frame's turn or thrust budget is used up. A craft with less than 90 degrees of yaw to
  go stops flipping. The yaw step (WSQ-27), then the pitch step (WSQ-28), then the speed step
  (WSQ-29).
- **WSQ-26** (debug build, EWSQ-04; FM-06) A craft not idle that wants to turn more than 170
  degrees, and has lift, loops instead: yaw 0, pitch toward 180 degrees up (down when the point
  lies below the nose), and keeps looping until it is within 90 degrees of yaw.
- **WSQ-27** (debug build; FM-03) The yaw step banks into the turn up to `Bank_Turn_Angle` at
  `Max_Rate_Of_Roll` times the unused turn share, and yaws by at most `Max_Rate_Of_Turn`, not while
  still leaning the other way. The roll-unroll equivalence has no effect (G-F9).
- **WSQ-28** (debug build; FM-04) The pitch step: at most `Max_Lift` a frame.
- **WSQ-29** (debug build, EWSQ-04; FM-05, FM-07) The speed step: the speed (the length of the
  current velocity) moves toward the wanted speed by at most the frame's thrust budget; the
  velocity becomes that speed along the new facing, pitch included. There is no clamp to
  `Max_Speed` or any bound.
- **WSQ-30** (debug build; FM-01) Every flight tag above is the type's value, and the speeds, turn,
  lift and roll rates are scaled by `Object_Max_Speed_Multiplier_Space` (1.2, data).

### Following, avoidance and stops

- **WSQ-31** (debug build, E457-02) The follow test (FD-05): within `Targeting_Max_Attack_Distance`
  (3D) and 90 degrees of yaw and pitch of the nose.
- **WSQ-32** (debug build, E457-04) Chase timers are ten seconds (300 frames); run out at start plus
  count.
- **WSQ-33** (debug build, EWSQ-29) Following: turn and pitch toward the chased craft; speed toward
  the chased craft's current speed, or the craft's own current minimum speed within
  `Minimum_Follow_Distance` (data: X-wing 25, TIE fighter, TIE bomber and Y-wing 50). No
  avoidance, no loop. Unclamped: a chaser matches whatever its target flies.
- **WSQ-34** (debug build, EWSQ-17) A chased craft that leaves the game (dies, is removed) clears
  the chaser's chase; the chaser's timer runs on (FD-08).
- **WSQ-35** (debug build, E457-07; FD-10) Avoidance of ships and static objects (not asteroid
  fields, ion storms or nebulae) while idling, forming up, closing, on a run and heading for a
  chased craft it cannot follow; then 15 frames straight on.
- **WSQ-36** (debug build, E457-08; FD-11) Craft never avoid each other.
- **WSQ-37** (debug build, EWSQ-04 to EWSQ-06) *Speeds while fighting.* No service clamps a
  craft's speed. The asked speed exceeds `Max_Speed` only through the formation speed of a
  far-behind follower (WSQ-18a, self-limiting), the idle speed of a far follower (WSQ-06), a
  chaser matching a fast target (WSQ-33) and the escort override (WSQ-12); the speed then changes
  by at most the thrust budget a frame, and form-up scales that budget by the same ratio (WSQ-17).
- **WSQ-38** (debug build, EWSQ-17) *Dead stop* (an emergency stop: abilities and scripts, not the
  player's stop): out of combat; while the squadron's formation error exceeds the tolerance, or
  its bounds' diagonal half-length exceeds 1.33 times its formation radius, the leader flies at
  its minimum speed toward a point two frames of that speed ahead along its desired facing, and
  the others form up at their minimum speed at the layer height; otherwise each craft turns to its desired facing and
  brakes to 0. No M2 order causes it.
- **WSQ-39** (debug build, EWSQ-17; FO-03) *Stop* (the player's stop): the craft goes idle, drops
  its paths, and holds its container's position. It claims no idle cell until its next revert to
  idle.
- **WSQ-40** (debug build, EWSQ-18; FM-08) Every service ends by moving the craft by its velocity.
  The height correction toward terrain and water runs only in land mode: in space a craft changes
  height only by pitching.

### The craft's targeting service (every frame) — interface only

- **WSQ-41** (debug build, EWSQ-26) A squadron craft adopts its formation's attack target (and
  hardpoint) as its own and its weapons', when that target is suitable and in reach.
- **WSQ-42** (debug build, EWSQ-27; FT-02, FT-06, G-F5) Without a target the craft scans for one
  itself, around itself, at most once per scan period: each scan sets the next one 30 frames plus
  a synchronized random 0 to 15 frames later. The scan range is the craft's
  `Targeting_Max_Attack_Distance` plus the divert allowance its squadron's formation grants (the
  chase range of the formation's order: `Idle_Chase_Range`, `Guard_Chase_Range`,
  `Attack_Move_Response_Range`; the movement walk owns which applies). Players are tried from a
  random start; a priority-1 find ends the scan (space-targeting).
- **WSQ-43** (debug build, EWSQ-26) What a squadron craft's scan finds goes to the squadron, not
  the craft: the squadron takes it as its attack target (WSQ-47) and every member takes it.
- **WSQ-44** (debug build, EWSQ-26) A held target is dropped (the craft clears it) when it is in
  limbo, dead, being deleted, stealthed and the craft cannot target stealth, or fogged for the
  craft's owner.
- **WSQ-45** (debug build, EWSQ-26; G-F5) A held target that is unsuitable, or whose priority is
  not the best (1), is re-scanned for a better one on the scan period; a better find replaces it.
- **WSQ-46** (debug build, EWSQ-26) When a craft's target is not yet its formation's attack
  target, the craft's targeting either asks the movement coordinator for an attack move of its
  formation toward the target's squadron (the FA-07 approach path) or notifies the team locomotor
  (WSQ-20), depending on a pursue flag not read here. Movement walk.

### The team service (the container, every frame)

- **WSQ-47** (debug build, EWSQ-22) Setting the squadron's attack target: a craft of a squadron
  stands for its squadron; the squadron signals a move start; every member's targeting and every
  weapon hardpoint of every member takes the target (FT-05, FO-04).
- **WSQ-48** (debug build, EWSQ-19) The container's bounds are the union of its craft's model
  bounds; its position is their centre (unless its team locomotor holds it; hyperspace only), and
  its selection box scale twice the larger planar extent.
- **WSQ-49** (debug build, EWSQ-24) Giving up a cell (idle or combat) removes the squadron from the
  cell's list and clears its recorded cell.
- **WSQ-50** (debug build, EWSQ-19) A squadron holding an idle cell has its container placed on the
  cell's point every frame; a squadron in neither grid points its icon at its formation's centre.
- **WSQ-51** (debug build, EWSQ-20, EWSQ-21; FM-10, FM-14) *Slots.* The leader is the first craft
  of the roster. Slot k belongs to the k-th craft **of the current roster**: when a craft dies it
  leaves the roster and every craft behind it moves up one slot (the leader's death makes the next
  craft the leader and slot 0 its slot). A slot is the `Squadron_Offsets` row less the mean of the
  first n rows (n the live craft; the mean is kept by a running update that subtracts the wrong
  row on a removal, which has no visible effect since only slot differences are used), turned by
  the leader's facing (yaw, then pitch, then roll) every frame.
- **WSQ-52** (debug build, EWSQ-20) *Formation error*, every frame: for each craft after the leader,
  its offset from the leader plus the leader's slot less its own, along the leader's facing
  (forward axis); the squadron's error is the signed value of the largest in size. WSQ-18a gates
  on it.
- **WSQ-53** (debug build, EWSQ-19) The container's facing is the leader's.
- **WSQ-54** (debug build, EWSQ-28) The team locomotor copies its leader's state (idle, moving,
  directed combat) into the container, advances the container's path with the leader's, and drops
  the container's path when no member has one.

### Launch, recovery and losses

- **WSQ-55** (FL-01 to FL-11) The spawner's rules stand as documented.
- **WSQ-56** (debug build, EWSQ-21) Adding a craft to a squadron puts it last in the roster; the
  first craft added leads.
- **WSQ-57** (debug build, EWSQ-21; FC-01) Removing a craft (death): it leaves the roster at once
  (WSQ-51); the squadron's container is destroyed with its last craft, which releases the spawner
  entry (FL-08). The killed craft's dead copy spins away or explodes (space-fighter-deaths).
- **WSQ-58** (debug build, EWSQ-17) A newly created craft is idle and holds its container's
  position; no idle cell is claimed until its first revert to idle (WSQ-09).
- **WSQ-59** (debug build, EWSQ-17) A move order given to one craft is refused (the squadron's
  container takes orders, FO-01).

## The existing rules against this walk

| Existing rule | Verdict |
| --- | --- |
| FL-01 to FL-11 | same (spawner side, not re-read) |
| FM-01 | same (WSQ-30) |
| FM-02, FM-03, FM-04, FM-05, FM-07 | same (WSQ-23, WSQ-25, WSQ-27 to WSQ-29) |
| FM-06 | same (WSQ-24, WSQ-26) |
| FM-08 | same (WSQ-40) |
| FM-10 | **differs**: slots follow the current roster index, so craft move up a slot when one ahead dies (WSQ-51); FM-10 keeps each craft's launch slot for life |
| FM-11, FM-13 | same (WSQ-17); FM-11 omits the budget scaling, which FM-13 states |
| FM-12 | same formula; **differs** in the gate: FoC gates on the squadron's largest formation error along the leader's facing (WSQ-52), FM-12's text says "the squadron's error" but the remake gates on the craft's own |
| FM-14 | same (WSQ-51) |
| FM-20 | same (WSQ-06); **missing there**: the hold point is the squadron's idle cell (WSQ-08, WSQ-09) |
| FM-21 | **differs**: FoC's escort is a formation move toward the escorted unit (WSQ-10, WSQ-12; movement walk) that ends in an idle cell; FM-21 holds the carrier's exact position |
| FA-01 to FA-05, FA-07 | same (WSQ-15, WSQ-16, WSQ-18) |
| FA-06 | **differs**: a lone craft target gets ship runs (WSQ-18), not a chase |
| FD-01 to FD-08, FD-10, FD-11 | same (WSQ-15, WSQ-19, WSQ-31 to WSQ-36); **missing there**: the pairing at target acquisition (WSQ-20) |
| FD-09 | same, plus the idle cell claim at the end (WSQ-14) |
| FD-12, FD-13 | recording-based, not re-read |
| FT-01 | **differs**: FoC also drops a target that is in limbo or stealthed (WSQ-44) and re-scans an unsuitable or not-best one (WSQ-45) |
| FT-02, FT-03 | **differs**: each craft scans around itself on a 30-to-45-frame period (WSQ-42, WSQ-43); FT-02 scans once a second for the squadron from its hold point |
| FT-04 | same result (recording) |
| FT-05, FT-07 | same (WSQ-43, WSQ-47) |
| FT-06 | explained: the first scan comes 30 + 0..15 frames after the launch (WSQ-42), which covers the recordings' 30 to 35 (G-F5) |
| FC-01 | same (WSQ-57) |
| FC-02 | not settled (tick-zero placement, EAWR-597) |
| FO-01, FO-02 | same (WSQ-10, WSQ-59) |
| FO-03 | same (WSQ-39) |
| FO-04 to FO-09 | not re-read (orders and group moves: movement walk) |
| SP-01 to SP-09 | not re-read (space-fighter-deaths) |
| WU-25, WU-25a | same (WSQ-08 shares the layout) |

## Gaps against the remake

Our code: `src/sim/tactical/fighters.cpp` (the locomotor class and the squadron target scan) and
`src/sim/tactical/session.cpp` (the squadron frame, the dogfight and chase phases, the container
update).

| Rules | Ours | Verdict |
| --- | --- | --- |
| WSQ-01 to WSQ-03 | `Locomotor::run`: the mode is chosen per squadron (attacking, moving, idle) | same in effect; the dead-stop state is missing (not needed in M2) |
| WSQ-04 | `CraftStep` out-of-combat defense | same |
| WSQ-05 | none | missing, not needed (no teamless craft in M2) |
| WSQ-06, WSQ-07 | `Locomotor::idle` | same |
| **WSQ-08, WSQ-09, WSQ-50** | none: an idle squadron holds `SquadronState::anchor`, the exact point it was given | **missing** (EAWR-674: escorts of one carrier and squadrons ending one move stack) |
| WSQ-10, WSQ-11 | `Locomotor::move`, `squadron_move_arrived` | same, except the idle cell claim (WSQ-09) |
| WSQ-12 | none: the escort flies at `Max_Speed` | differs (escort override speed; movement walk) |
| WSQ-13 to WSQ-17 | `Locomotor::directed_combat`, `form_up` | same |
| WSQ-18a | `Locomotor::target_speed` | **differs**: gated on the craft's own error, not the squadron's largest (WSQ-52) |
| WSQ-18 | `directed_combat` with `target_craft` | differs for a lone craft target (FA-06); none in M2 |
| WSQ-19, WSQ-22 | `dogfight`, the session's dogfight and chase phases | same |
| **WSQ-20** | none | **missing**: no pairing when a squadron target is acquired |
| WSQ-21 | FD-09 in the session | same |
| WSQ-23 to WSQ-30 | `head_for`, `adjust_turn`, `adjust_pitch`, `adjust_speed`, `handle_flip` | same |
| WSQ-31 to WSQ-36 | `followable`, `follow`, `avoid` | same |
| WSQ-37 | no clamp, formulas match | same; the soak's 40x to 1974x runaways are **not** explained by any FoC rule: something else in ours (see the gap note below) |
| WSQ-38 | none | missing, not needed in M2 |
| WSQ-39 | FO-03 | same |
| WSQ-40 | FM-08 | same |
| WSQ-41, WSQ-43, WSQ-47 | `detail::squadron_target`, FT-05 hand-off | same in effect |
| **WSQ-42** | one squadron scan a second from the hold point (or leader), chase range plus attack distance | **differs**: per-craft scans around each craft on a 30 + 0..15 frame period |
| **WSQ-44, WSQ-45** | FT-01 keeps a live, seen target | **differs**: no better-target re-scan; no stealth or limbo drop (no M2 stealth) |
| WSQ-46 | FA-07 approach in the session | same in effect (movement walk owns the path) |
| WSQ-48, WSQ-53 | container at the centre of its craft's positions | same (points, not model bounds: negligible) |
| **WSQ-51** | `roster_offset`: the launch roster, a dead craft's slot stays empty | **differs** (FM-10) |
| WSQ-52 | none | missing (part of the WSQ-18a gap) |
| WSQ-54, WSQ-56 to WSQ-59 | squadron frame, FC-01 | same |

**The runaway note (EAWR-664, EAWR-634, soak EAWR-673).** FoC's speed rules (WSQ-37) let a follower fly
several times `Max_Speed` while it catches up, never 40 times: the asked speed falls as the craft
speeds up (WSQ-18a) and the idle ratio falls as it closes (WSQ-06). Ours uses the same formulas, so
a craft at 40x to 1974x of its maximum speed, 160 000 units off the grid, comes from something
FoC does not do: a bogus hold or slot point far away, a position set, or a Q24 edge. The EAWR-607
worker owns that bug (EAWR-664, PR EAWR-634); this walk files no duplicate.

### XML tags this subsystem reads

`Squadron_Units`, `Squadron_Offsets`, `Squadron_Formation_Error_Tolerance` (data: 25, one 35),
`Max_Speed`, `Min_Speed`, `Max_Thrust` (data: 1.0 on the FoC craft), `Max_Rate_Of_Turn`,
`Max_Lift`, `Max_Rate_Of_Roll`, `Bank_Turn_Angle`, `Strafe_Distance`, `Minimum_Follow_Distance`,
`Layer_Z_Adjust`, `Out_Of_Combat_Defense_Adjust`, `Targeting_Max_Attack_Distance`,
`Targeting_Priority_Set`, `Idle_Chase_Range` (data: 200 on most containers), `Guard_Chase_Range`
(1000), `Attack_Move_Response_Range`, `Autonomous_Move_Extension_Vs_Attacker`,
`Can_Fighter_Fire_When_Idle`, GameConstants `Object_Max_Speed_Multiplier_Space` (1.2) and
`Space_Idle_Movement_Speed` (not in the FoC data). The spawner's tags are FL-01's.

Marked `todo` in `docs/tag-coverage/statuses.json`: `Container/Squadron_Formation_Error_Tolerance`
and `Container/Squadron_Offsets` (EAWR-651; both are read through the unit tables, so the rows are
stale), `Container/Idle_Chase_Range` and `SpaceUnit/Idle_Chase_Range` (EAWR-653),
`Container/Guard_Chase_Range`, `Container/Attack_Move_Response_Range`,
`Container/Autonomous_Move_Extension_Vs_Attacker`, `Container/Max_Speed`, `Container/Min_Speed`
(EAWR-649), `Container/Targeting_Max_Attack_Distance` (EAWR-650), `SpaceUnit/Number_per_Squadron`,
`SpaceUnit/Squadron_Capacity`, `Squadron/Max_Squad_Size`, `Squadron/Is_Bomber`, `Squadron/Is_Escort`
(EAWR-651). None is `deferred`.

## Unverified, and what would settle it

- **U-01** The object manager's service order between a squadron's craft and its container
  (Scope). Ghidra: the object manager's service loop.
- **U-02** Whether a squadron that was never ordered (idle, no formation) has a formation whose
  divert allowance adds the chase range to its scan (WSQ-42). The EAWR-607 worker's retail S-99 shows an
  idle X-wing squadron chasing a passing TIE squadron 4150 units without turning back: FoC has no
  leash for an idle squadron. Ghidra: the formation's divert-for-attack test (movement walk).
- **U-03** The gating of the pairing at target acquisition (WSQ-20): the pursue flag and when the
  formation's attack target lags the craft's. Ghidra: the targeting interface's pursue getter.
- **U-04** The escort's override maximum speed (WSQ-12). Movement walk.
- **U-05** `Space_Idle_Movement_Speed`'s code default (WSQ-05). Not needed in M2.
- **U-06** A retail capture of two squadrons launched by one carrier idling side by side would
  confirm WSQ-09 on screen (two holds 120 or more units apart): the capture-mod Lua staging of
  EAWR-391 can spawn two squadrons at one point and let them idle.
