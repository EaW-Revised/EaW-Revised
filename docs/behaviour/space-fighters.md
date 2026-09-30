# Space fighter squadrons: launch, flight, attack runs and losses

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space, the M2 fixture
  ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md)). Data comes from the FoC profile of the
  retail corpus: the carrier and station files (`Starting_Spawned_Units_Tech_0`,
  `Reserve_Spawned_Units_Tech_0`, `Spawned_Squadron_Delay_Seconds`, the `HARD_POINT_FIGHTER_BAY`
  hardpoints), `squadrons.xml` and the craft files.
- Bounded question: how carriers and stations launch fighter and bomber squadrons and replace lost
  ones, how a squadron's craft fly, how they attack a capital ship and another squadron, and how
  craft die one by one. P2-12 (EAWR-75).
- Source tags: **research** (the FoC debug build, read under the clean-room rule; evidence IDs
  E75-nn are opaque and their map stays private), **recording** (the P2-06 trace S-16 and the EAWR-75
  trace S-28, fog revealed, AI suspended, [tests/fidelity](../../tests/fidelity/README.md)),
  **data** (a tag or value in the FoC files), **project** (a remake decision) and **unverified**
  (not established; the remake picks the least visible behaviour).
- Related notes: the squadron's sensor and its team container ([space visibility](space-visibility.md)
  V-03), craft target choice and fire ([space weapon fire](space-weapon-fire.md)), priority sets
  ([space targeting](space-targeting.md) R-09), damage ([space damage](space-damage.md)), capital
  ship movement ([space movement](space-movement.md)).
- Out of scope: `DEPLOY_SQUAD` (a human player deploys by hand, FL-09), abilities (EAWR-76),
  presentation (EAWR-80), land garrisons and the AI (EAWR-79). The player's squadron orders are FO-01 to
  FO-03 (EAWR-424).

## Interface

- Content: `MotionTable::squadrons` (`SquadronTable`, built by `units::motion_table`): per craft
  type its flight (`CraftProfile`), per squadron type its craft and formation offsets
  (`SquadronProfile`), per `SPAWN_SQUADRON` type its spawn entries, delay and fighter bays
  (`SpawnerProfile`). An empty table behaves exactly as before EAWR-75: nothing launches and craft never
  move.
- State, hashed only when present ([replay format](../replay-format.md)): per squadron craft its
  roll, pitch, yaw, velocity and flip flag; per squadron its spawner, entry, roster, mode (idle or
  escort), anchor, target and next scan frame; per spawner its hangar (next service and spawn
  frames and, per entry, the squadrons alive and left).
- Outputs: launched craft and their team container enter the snapshot as new units with the next
  stable IDs; craft positions and rotations change every frame.
- Cadence: the craft phase runs every logical frame (1/30 s) in the movement phase; the squadron
  target scan runs before the targeting phase; the hangar service runs after the unit systems.

## Rules

### Launch and replenishment

- **FL-01** (research E75-01, E75-25) A spawner's
  `SPAWN_SQUADRON` behaviour is serviced every 30 frames. Its first service is its creation frame
  plus a synchronized draw of 0 to 29 frames. The remake draws it keyed by the seed, the frame and
  the unit (project, like the targeting draws of space-weapon-fire P-02).
- **FL-02** (research E75-02) The first service builds one entry per `Starting_Spawned_Units_Tech_0`
  row: at most its count alive at once, and its count plus the matching
  `Reserve_Spawned_Units_Tech_0` count (0 when absent) to launch in all; a negative reserve never
  runs out. The first service may launch at once.
- **FL-03** (research E75-03) A service launches only when the next-spawn frame has come and a
  fighter bay stands: a destroyed destroyable bay no longer launches, and a spawner without a
  standing bay launches nothing. A spawner with `DEFEND` active launches nothing (no M2 unit has it).
- **FL-04** (research E75-03, E75-05) The entry is chosen from a random start in cyclic order: the
  first entry with fewer squadrons alive than its count and some left to launch. Launching
  increments its alive count and uses one of its left (an unlimited entry stays unlimited).
- **FL-05** (research E75-04) After a service that could launch, the next-spawn frame is the frame
  plus trunc(`Spawned_Squadron_Delay_Seconds` x 30).
- **FL-06** (research E75-06 to E75-08) The squadron leaves a randomly chosen standing bay. Every
  craft starts at the bay's attachment bone, facing along the bay's spawn vector (the bone's X
  axis times `Fighter_Bay_Flyout_Distance`, default 1) and flying along it at its maximum speed.
  The craft are created in `Squadron_Units` order, then their team container.
- **FL-07** (research E75-08) A squadron from a mobile spawner (a ship) escorts it; one from a
  station holds the end of the spawn vector.
- **FL-08** (research E75-09) When a launched squadron leaves the session (its last craft died),
  its entry's alive count drops. If every entry up to and including that one was full before the
  drop, the next launch waits a full delay from that frame; otherwise the running delay stands.
- **FL-09** (research E75-04) A human player's spawner with `DEPLOY_SQUAD` waits for the player.
  No M2 spawner has the ability; the remake does not model it.
- **FL-11** (owner, SK-36) The M2 fixture ignores `Reserve_Spawned_Units_Tech_0`: every entry of
  the FoC table has a reserve of 0, so each spawner launches its starting squadrons once and a
  lost squadron is not replaced (m2-skirmish SK-23, SK-36; EAWR-179 Q2). The hangar keeps FL-02 and
  FL-08 for a table with reserves (starbase hangars, a later phase).
- **FL-10** (recording S-16, S-28) The Acclamator launches its first squadron 27 frames after tick
  zero and the second 150 frames later (FL-05 with its 5-second delay). In S-28 the TIE fighter
  squadron is lost at tick 417 and the reserve squadron leaves at tick 597, the first service after
  417 + 150 (FL-08). Every craft of a launch starts at one point, 15.4 units ahead of and 8.8 below
  the carrier's origin, nose 45 degrees down.

### Flight

- **FM-01** (research E75-10, E75-23) A squadron craft is flown by the fighter locomotor once per
  frame. Its `Max_Speed`, `Min_Speed`, `Max_Rate_Of_Turn`, `Max_Lift` and `Max_Rate_Of_Roll` are
  scaled by `Object_Max_Speed_Multiplier_Space` (1.2); `Max_Thrust` and `Strafe_Distance` are not.
  Recording: the TIE fighter cruises at 5.4 and the bomber at 3.6 units per frame (S-16).
- **FM-02** (research E75-11) The craft's facing is a roll, a pitch (positive lowers the nose) and
  a yaw in degrees. To head for a point the point is taken into the craft's frame (unyawed, then
  unpitched); the yaw and pitch toward it are clamped to [-180, 180).
- **FM-03** (research E75-12; debug build: the heading step, EAWR-479) Turning: the wanted roll is
  minus the yaw change clamped to `Bank_Turn_Angle`; the roll moves toward it by at most
  `Max_Rate_Of_Roll` times the unused share of the turn budget. The yaw turns by at most
  `Max_Rate_Of_Turn` per frame, and not at all while the craft still leans the other way. The yaw
  change is the whole heading change still to go toward the aim point, not the frame's step, so
  the bank is the size of the turn left, up to `Bank_Turn_Angle`: a wide turn banks fully and
  levels out over its last `Bank_Turn_Angle` degrees, a small correction banks by its own size,
  and a craft reversing its turn (an S-bend) rolls through level before it yaws the new way. The
  roll is part of the object's facing, which FoC draws as it is: the bank is simulation state, not
  a presentation animation. With the M2 values (FM-01) the X-wing, TIE fighter and TIE interceptor
  bank up to 40 degrees at 6 degrees a frame, the Y-wing 70 and the TIE bomber 30 at 3.6.
- **FM-04** (research E75-13) The pitch changes by at most `Max_Lift` per frame.
- **FM-05** (research E75-14) The speed moves toward the wanted speed by at most `Max_Thrust` per
  frame; the velocity is the speed along the facing, and the craft moves by its velocity.
- **FM-06** (research E75-11; debug build: the heading step and the flip service, EAWR-479) A craft
  that wants to turn more than 170 degrees outside idle flight (and has lift) pitches over instead
  of turning: it holds its yaw and aims its pitch 180 degrees nose up (nose down when the aim point
  lies below its nose), so it climbs at `Max_Lift` a frame. At the start of the first frame its
  pitch has passed 90 degrees, its facing is re-expressed turned about: yaw and roll plus 180
  degrees and the pitch mirrored about the vertical, less one `Max_Lift` step. That is the same
  rotation, one step further round the loop. The aim point now lies ahead, so the craft flies the
  top of the loop upside down while it pitches toward it and rolls upright at its roll rate
  (FM-03's wanted roll follows the small heading change left): an Immelmann turn, all of it
  simulation motion in the facing. With the X-wing's 4.8 degrees a frame it passes the vertical
  about 19 frames after the turn starts, its nose points back at about frame 31 (1 s), some 110
  units higher (the loop's radius, speed over rate, is 57 units), and the roll out that began at
  the vertical leaves it upright at about frame 48; it then pitches down to its layer (FM-08).
- **FM-07** (debug build: the locomotor's heading step, its speed step and the object's facing
  setter; recording S-28; EAWR-506) A craft travels where its nose points. Each frame it first turns
  (FM-03: the yaw by at most `Max_Rate_Of_Turn`, the roll leaning into the turn), then pitches
  (FM-04: at most `Max_Lift`), and only then sets its velocity to its speed along the new facing,
  pitch included (FM-05). There is no separate direction of travel, so a craft never slides or
  climbs sideways and never waits for its facing to line up before it moves: it flies on while it
  turns, on the arc its speed and turn rate give (FM-11's turn radius). The roll, pitch and yaw are
  the object's facing, and FoC draws that same triple, so a climbing or diving craft shows its
  pitch. The remake draws each craft's whole rotation and turns it between two frames along the
  shortest arc (`presentation::space::interpolate_units`). Before EAWR-506 the viewer drew only the yaw
  and the roll: in S-28 the launched craft climb to their layer at 0.5 units per frame, nose up
  60 to 89 degrees, and were drawn level and rising sideways, while the sim's velocity stayed
  within the trace's precision of the facing. Between ticks the viewer also turns an unpitched
  craft's roll the short way (EAWR-479), so a craft rolling upright through 180 degrees out of a loop
  does not spin a whole turn between two ticks.
- **FM-08** (debug build: the locomotor's height correction; space-movement MV-11; EAWR-506) In space a
  craft changes height only by pitching. FoC's direct height correction (keeping clear of terrain
  and water, at most `Max_Lift` per frame) runs only in land mode. A squadron reaches its
  `Layer_Z_Adjust` height (FM-11, FM-21, FA-07) by pitching toward it; a capital ship keeps its
  height and flies level (space-movement MV-11).
- **FM-10** (research E75-24) The squadron's first live craft leads it; member k keeps the k-th
  `Squadron_Offsets` slot for its life.
- **FM-11** (research E75-18) Forming up: a craft flies toward the leader's position plus a turn
  radius (360 / turn rate x speed / 2 pi) along the leader's velocity, plus its own slot minus the
  leader's turned by the leader's facing (FM-14), easing its height toward the wanted one.
- **FM-13** (research E75-18; debug build: forming up hands its slot point to the heading step;
  EAWR-506) A squadron keeps its formation by steering, not by placement: every follower heads for its
  slot point (FM-11) under the same turn, lift and thrust limits as any other flight (FM-02 to
  FM-05), scaled up only while its wanted speed exceeds `Max_Speed` (FM-12). Nothing moves a craft
  onto its slot directly, so a follower out of place turns and flies back into it nose first.
- **FM-14** (debug build: the team's service, its facing upkeep and its slot assignment; EAWR-479)
  The slots turn with the leader's whole facing. Every frame the squadron's team container takes
  its leader's facing (roll, pitch and yaw) and turns every `Squadron_Offsets` slot by it. A
  follower's slot point (FM-11), its formation error along the leader's travel (FM-12) and its
  idle trail point (FM-20) all use the turned slots, so the formation banks, climbs and loops with
  its leader: in a 40-degree bank the X-wing wingmen 15 units to either side hold slots 10 units
  above and below the leader's level, those 30 out 19 units, and climb or dive to keep them.
  Before EAWR-479 the remake turned the slots by the leader's yaw only, and a squadron turned as a
  flat plate (G-F4).
- **FM-12** (research E75-18, recording S-28) A follower's speed eases between half and one and a
  half times the wanted speed to close its error along the leader's velocity, once the squadron's
  error exceeds `Squadron_Formation_Error_Tolerance`. Ahead of its slot the share toward the slow
  bound is clamped to [0, 1]. Behind it FoC bounds the share toward the fast bound only below, at
  0, so a far-behind follower extrapolates past 1.5 times the wanted speed: in S-28 a follower
  reaches 12.0 units per frame behind a leader at 5.4. The EAWR-388 review asked for a cap at 1.5
  times; FoC has none, so the remake has none either.
- **FM-20** (research E75-15) Idle: the squadron holds a point. Within 90 units of it the leader
  circles it 60 units out; the followers trail the leader (its position plus 40 frames of its
  velocity plus three quarters of their slot offset, turned as FM-14 says). Outside that the craft fly at `Min_Speed`.
  Inside it every craft wants half its own distance to the point over the leader's (at least one
  unit), with no cap, and the leader aims 60 units out, turned by ten over its own distance in
  radians (debug build: the idle service). The remake divided by at least one unit there (EAWR-664): a
  leader that ended a move within a unit of the point then aimed across it every frame and came to
  rest on it, and a wingman far from the point wanted half its distance per frame and flew off at
  up to 1974 times its maximum speed (the nightly soak, EAWR-673). After a fight the leader now holds an
  idle cell's point (FM-24, EAWR-687).
- **FM-21** (research E75-08, recording S-16) An escorting squadron holds its carrier's position at
  the craft's `Layer_Z_Adjust` height, and drifts back to it at idle speed after a fight (S-16:
  the bombers return at about 1.1 units per frame). Once its leader is within a cell's size of the
  carrier it holds an idle cell around it instead (FM-24).
- **FM-23** (debug build; walk 1 WSQ-08; EAWR-687) Idle squadrons are kept apart by an idle grid: cells
  of 120 units, rows along y, odd rows shifted half a cell along x, a cell's point its centre at the
  craft's `Layer_Z_Adjust` height. A cell holds at most one squadron, of any side. FoC lays the
  grid from the map bounds' low corner; the remake lays it from the world origin, as the combat
  grid (foc-battle-world-ui WU-27).
- **FM-24** (debug build; walk 1 WSQ-09, WSQ-11, WSQ-14; EAWR-687) A squadron that reverts to idle (its
  move ends, FO-02, or its fight ends without a new target, FD-09) holds an idle cell's point
  instead of the point it was given. It keeps a cell it already holds; otherwise it takes the
  cell under the point when that is free, else searches rings around that cell, row by row from
  the low corner, and takes the free cell whose point is nearest the given point (squared, in the
  plane; the first strictly nearest). At most 64 cells are examined, the first one included; with
  none free it holds the given point. The claims run serially in ascending container ID. So two
  squadrons that go idle at one point hold cells at least a cell apart, and after a fight the
  leader is up to 85 units from its point, not on it (FM-20, EAWR-664). FoC's escort is a move to the
  escorted unit that ends in an idle cell (walk 1); the remake's escort (FM-21) claims a cell
  around the unit once its leader is within 120 units of it, and gives the cell up when the unit
  is more than 120 units from the cell's point (project).
- **FM-25** (debug build; walk 1 WSQ-02, WSQ-39, WSQ-49, WSQ-58; EAWR-687) A squadron gives its idle
  cell up when it moves, takes a target or gets any order but a face. A launched or tick-zero
  squadron and one given a stop hold their point without a cell until they next revert to idle.
- **FM-26** (debug build; walk 1 WSQ-50; EAWR-687) While a squadron holds an idle cell its team
  container stands on the cell's point.

### Attack runs

- **FA-01** (research E75-16, FS-01) While the leader is farther than `Strafe_Distance` plus the
  target's soft radius (in the plane) and the squadron's approach is over (FA-07), every craft heads
  straight for the target at maximum speed.
- **FA-02** (research E75-17) Within that radius the leader keeps heading for the target while it
  lies within 45 degrees of yaw and of pitch ahead of its nose.
- **FA-03** (research E75-17, E75-19) Once the target leaves that cone the leader forms up ahead
  on its own velocity: it flies on past the target until FA-01 turns the squadron around for the
  next run.
- **FA-04** (research E75-17) The followers always form up on the leader inside the strafe radius.
- **FA-05** (recording S-28) Recognisable result: the squadron closes at full speed, passes over
  the corvette, turns out and comes back; the corvette's shield falls first (S-28: 600 to 329 by
  tick 300) and it dies at tick 883.
- **FA-07** (research FS-01 to FS-03, EAWR-469; research E452-03, EAWR-497; recording S-28) A squadron that
  takes a new target, or is given a player attack order (the debug build's attack order plans every
  craft's move to the target, also when the target is the one it already had), first flies an
  approach: while its leader is beyond the FA-01 reach, every craft forms up (FM-11,
  FM-12) at its maximum speed at its `Layer_Z_Adjust` height, and the leader looks along the
  squadron's path instead of along its own velocity. The approach ends for good the first frame
  the leader is within reach (FoC then clears the squadron's path); a later run from beyond the
  reach is FA-01's straight dive. The craft are out of combat on the approach and in combat on the
  dive (DG-26: FoC's directed combat clears the adjustment and only the path approach sets it). The path
  is FoC's planned move to the target; the remake takes it as the straight line in the plane from
  the leader to the target, re-aimed every frame (unverified, G-F8). Recording S-28: the first
  wave closes level about 20 units above the corvette (the craft's layer is 0, the corvette's -20)
  in distinct formation slots, and passes 16 to 51 units from the corvette's centre; before EAWR-469
  the remake's craft dived straight at it, pairs of them merged into one position, and the
  followers passed 120 to 160 units off its sides after a loop at the reach.
- **FA-06** (research E75-21) Against a single craft outside a squadron the leader chases the
  target and the others form up. A squadron fighting a squadron dogfights instead (FD-01 to
  FD-12, EAWR-457).

### Dogfights (EAWR-457)

The combat cells are the grid of foc-battle-world-ui WU-25 (400-unit cells, odd rows shifted by
half a cell, from the world origin in the remake, WU-27). The sim owns them since EAWR-457 and
publishes each squadron's cell in the snapshot; the world UI draws the icons from it.

- **FD-01** (debug build: the fighter's directed-combat service) A squadron whose target is a
  squadron (a craft target stands for its squadron, FO-04) dogfights while its leader is within
  the FA-01 reach of the target (strafe distance plus the target's radius, in the plane) or while
  it records the same cell as its target. Otherwise it leaves its cell and flies FA-01 to FA-07.
- **FD-02** (debug build: the fighter-vs-fighter service) While the target squadron records no
  cell, the squadron records the WU-25a cell without joining it; its leader heads for the target
  at full speed and the others form up at the target's height.
- **FD-03** (debug build: the fighter-vs-fighter service and the cell point) Once the target
  records a cell, the squadron joins it. A craft within 400 / sqrt(2) of the cell point (in the
  plane) fights (FD-05 to FD-07); outside, the leader heads for the cell point, which lies at the
  craft's `Layer_Z_Adjust` height, and the others form up at that height.
- **FD-04** (debug build: the fighter-vs-fighter service; recordings S-97, S-98) Retaliation: once
  a craft of the attacking squadron is within reach of the cell point, the target squadron takes
  the attacker as its target, unless it is on a move or already targets a squadron. The debug
  build also skips a target using the ion cannon shot or lucky shot ability; M2 has neither. The
  cell must be the target's own, so a squadron attacking a ship (no cell) never retaliates: in
  S-98 the TIE bombers keep attacking the corvette while the X-wings shoot them down. An idle
  squadron turns on its attacker through its own scan (FT-02): in S-97 the TIE squadron, given
  no order, targets the X-wings 84 frames after their attack order.
- **FD-05** (debug build: the follow test and the follow step) A craft can follow another within
  its `Targeting_Max_Attack_Distance` (3D) and within 90 degrees of yaw and 90 degrees of pitch of
  its nose (the offset in the craft's frame, as FM-02). Following, it turns and pitches toward
  the other craft (FM-03, FM-04) and flies at that craft's current speed, or at its own
  `Min_Speed` once closer than its `Minimum_Follow_Distance` (X-wing 25, TIE fighter 50).
- **FD-06** (debug build: the fighter-vs-fighter service; the chase timer) Pairing: a craft within
  reach of its cell point whose chase timer has run out looks through the enemy squadrons joined
  in its cell, in the cell's order, each squadron's craft in roster order, and chases the first
  craft whose own chase timer has run out and which it can follow (FD-05). Both timers start at
  ten seconds (300 frames) and the chased craft drops any chase of its own, so a craft is chased
  by at most one other at a time and a chased craft does not chase for ten seconds. The remake
  lists each scanning craft's candidates in the partitioned `dogfight-chases` phase from the
  frame's start and commits them in ascending craft ID, re-checking both timers: FoC services the
  craft one after another, and the commit's re-check gives the same pairs (project).
- **FD-07** (debug build: the fighter-vs-fighter service) While its chase runs and it is within
  reach of the cell point, a craft follows its target when it can (FD-05), else heads for it at
  full speed (steering around ships, FD-10). A craft with no chase, or a chased one, flies as
  outside the reach: the leader to the cell point, the others form up at the layer height.
- **FD-08** (debug build: the chase timer) A chase ends when its timer runs out, and the craft looks
  again (FD-06). A chased craft that dies ends the chase, and the chaser's timer runs on.
- **FD-09** (debug build: the directed-combat dispatcher, the new-target step, the end of combat)
  When the target dies, the squadron attacks the first enemy squadron joined in its recorded
  cell; with none, combat ends: the squadron leaves its cell and, idle, holds where its leader
  will be next frame at its layer height. A target lost from sight ends combat as well.
- **FD-10** (debug build: the collision-avoidance step and its callers; research E457-07) A craft
  steers around ships and static objects (units with a space layer; asteroid fields, ion storms and
  nebulae excepted) while forming up, idling, closing, on a run and heading for a chased craft it
  cannot follow; never while it follows (FD-05). When its look-ahead segment (its velocity times
  two over its turn rate in radians, twice that for the leader) hits a ship and it is inside the
  ship's box grown by its speed, and did not start inside the ship, it heads away from the ship's
  centre at its minimum speed and then flies straight on for 15 frames. The remake tests the
  ship as a sphere of its soft radius; FoC tests the hull's collision shape (project).
  Its segment test never divides; the remake's nearest point on the look-ahead does, by the
  look-ahead's square. When a craft all but stopped has a look-ahead whose Q24 square rounds to
  zero, the share comes from the exact raw products, clamped to [0, 1], as the float division by
  the tiny square gives it (EAWR-615, project).
- **FD-11** (debug build: the craft-to-craft avoidance routine has no callers) Craft never avoid
  each other: squadrons may pass through each other, and two craft of a dogfight may overlap.
- **FD-12** (owner, 2026-09-28; recording S-97) A dogfight stays near its squadrons' layer
  height. In S-97 (X-wings against TIE fighters, both at layer 0) 90 % of the craft heights lie
  within 100 units of the plane and the highest and lowest craft reach 173 units; the fight's
  centre stays within about 100 units of one point in the plane for 1300 ticks. The height
  comes back because every craft without a running chase heads for the cell point, or forms up,
  at the layer height (FD-03, FD-07), and a chase lasts ten seconds at most (FD-06). The remake's
  S-97 on the FoC tables (the fidelity trace): 90 % of the heights within 127 units (132 before EAWR-607), the extremes
  at 290 (one TIE fighter's dive) and the craft's pitch spread as retail's (half the samples
  within 41 degrees of level against retail's 34, 99 % within 89 against 91). **The trace's
  limits are fitted to the remake, not to retail:** the test allows a band of 150 units
  (`BAND_LIMIT`, retail 100) and an extreme of 320 (`EXTREME_LIMIT`, retail 173) so that the
  remake's 127 and 290 pass. The gap is on the fidelity list (FD-12).

### Targets

- **FT-01** (debug build: the fighter's directed-combat service and the object fog test; EAWR-633) A
  squadron keeps its target while the target lives and the owner sees it. The fog test is FoC's
  forced one, so an AI owner's squadrons respect fog here (`AIUsesFogOfWarSpace` = `False` only
  relaxes the unforced queries, space-visibility V-09). A fogged target does not stop the squadron
  while it still flies the approach its attack planned (FA-07): FoC keeps flying the path and
  attacks once it sees the target. Only a squadron with no approach left, or one whose target is
  fogged when the approach ends, drops the target and idles where its leader flies (FoC: the
  leader turns the team's direct attack off and every craft reverts to idle at the leader's
  position plus its velocity, FD-09). The remake ends the approach at the FA-01 reach, where FoC
  ends it at its planned path's end (G-F8).
  Recording S-99 (EAWR-664): an idle X-wing squadron takes a TIE squadron that flies past it on a
  move and follows it about 4150 units across the map without turning back; so does the remake. The
  debug build's formation leash (the chase ranges, `Autonomous_Move_Extension_Vs_Attacker`) caps
  only an attack that diverts a squadron from a destination it was given, not an idle squadron's own.
- **FT-02** (research E75-22, project) Without one it scans once a second: the best hostile,
  visible, damageable unit within the chase range plus the leader's `Targeting_Max_Attack_Distance`
  of the point it holds, where the chase range is `Guard_Chase_Range` when escorting and
  `Idle_Chase_Range` otherwise.
- **FT-03** (research E75-22, space-targeting R-09) The best candidate has the lowest priority in
  the leader's `Targeting_Priority_Set` (a candidate with no priority is skipped), then is nearest
  the leader, then has the lowest ID.
- **FT-04** (recording S-16, S-28) The squadron engages a target about 1200 units from its carrier
  at once: the escort's chase range (1000) plus the craft's attack distance (500) covers it.
- **FT-05** (project) The squadron's target is every craft's direct target, so they fire at it
  with their own weapons (space-weapon-fire).
- **FT-06** (recording S-28) A launched squadron takes no target for its first second. In all three
  recordings every launch (ticks 27, 177 and 597) slows from its launch speed to its idle speed at
  the carrier and turns on the corvette 30 to 35 frames later. The remake scans first one second
  (30 frames) after the launch; the extra 0 to 5 frames are not explained (G-F5).
- **FT-07** (research E75-28, E75-29, E75-30) Without a squadron target a craft is idle: it holds
  no target, does not scan and does not fire; its weapon countdowns keep running. In retail a
  squadron craft's own scan hands what it finds to its squadron instead of keeping it, the craft
  takes its target only from the squadron, and a craft idling in formation has its weapons
  disabled (no FoC type sets `Can_Fighter_Fire_When_Idle`). This covers the launch idle (FT-06):
  no launched craft fires in its first second. The recordings agree without testing it: in S-28
  every launched craft stays at least 1160 units from the corvette through that second, beyond its
  500-unit attack distance.

### Losses

- **FC-01** (research E75-09, space-visibility V-03) Craft die one by one by the damage rules;
  the team container follows its live craft and leaves with the last one, which releases the
  spawner entry (FL-08). When the leader dies the next live craft leads (FM-10). A killed craft
  leaves its squadron at once even when its dead copy spins away (EAWR-447, space-fighter-deaths
  SP-02).
- **FC-02** (project; debug build, space-movement PL-01) Every squadron company of the start is its
  squadron's team container. Its craft enter at tick zero each on its own free point near the spawn
  marker (the start's free-space search, one craft at a time), not on their formation slots, and
  form up from there (FM-11). The container stands at the centre of its craft (V-03).

### Player orders (EAWR-424)

- **FO-01** (debug build: the fighter locomotor's move service and the team's move)
  A squadron takes a player's order as one unit through its team container. A move sends the team
  along its path: the leader heads for the destination at its current maximum speed and every craft
  forms up on it (the fighter locomotor's own move entry does nothing; the team moves its members). The
  move drops the squadron's target and its escort. While it moves the squadron does not scan for
  targets (project: FoC's team is on a directed move; unverified against a recording).
- **FO-02** (debug build: the fighter locomotor's move service) The move ends when the leader passes the half plane at the
  path's end: the line through the destination square to the move. Every craft then reverts to
  idle, holding the destination at its `Layer_Z_Adjust` height (FM-20). The remake's path is the
  straight line from where the squadron stood (project: no squadron path finding).
- **FO-03** (project, unverified; the attack order: debug build, EAWR-633) A stop holds where the
  squadron stands (idle there). An attack order makes the unit the squadron's target and starts
  its approach (FA-07), which flies to the target also while it is fogged (FT-01); once the target
  is gone the squadron idles where its leader flies. A face order changes nothing.
- **FO-04** (debug build: the attack order, the team-member choice, the team's nearest member
  and the turret's aim) An attack on a craft of a squadron targets the squadron's team container.
  Every attacker, a ship or another squadron's craft, fires at the squadron's live craft nearest
  itself (the least spatial distance, the first in team order on a tie). The craft is chosen afresh
  at every use: FoC stores the container as the attack target and keeps no chosen craft, so an
  attacker moves to another craft as soon as that one is nearer (C-12). The one exception is a
  shooter's special-ability attack target, which FoC keeps while it is a member of the team; M2
  has no abilities (space-damage DG-02). The target lasts while one of its craft lives and is seen.
  A squadron chasing a squadron heads for the team's position (the formation destination keeps
  the team's own position in space) and treats it as a craft target (FA-06; project,
  unverified).
- **FO-05** (debug build: the player move with the attack-on-path flag and the formation's
  diversion test; research E452-02, E452-14) An attack-move given to a squadron (EAWR-452) is the
  team's move with the attack-on-path flag. To a point, the squadron flies it as FO-01 and FO-02
  say, but it scans for targets all the way, from its leader, within `Attack_Move_Response_Range`
  (300 on the FoC squadron containers) plus the leader's attack distance (FT-02); FoC measures that
  range from the move's tether point on the path, which the remake takes as the leader. Once
  there it holds the destination with the same range (FoC's finished attack-on-path move keeps
  its flag). To a unit, the squadron follows it as an escort does (FL-07) and scans with the same
  range around it, without making it the squadron's target (space-orders OR-12). A found target is
  kept by FT-01; the squadron goes on with the move once it is gone. The order drops the previous
  target. Project, unverified: the leader as the tether point, and the diverted squadron keeping
  its target where FoC's tether may break the attack off.
- **FO-06** (debug build: the escort order and the formation's diversion test; research E452-04,
  E452-14, E452-16) A guard given to a squadron is the team's move with the escort flag. Of a unit,
  the squadron escorts it as FL-07 says, scanning within `Guard_Chase_Range` (1000) of it. Of a
  point, it flies there as FO-01 says and scans only once its leader is within `Guard_Chase_Range`
  of the point (FoC's escort diversion measures from the destination, and refuses it while the
  craft is farther than the range from there); then it holds the point with that range. When the
  guarded or attack-moved unit is gone, the squadron holds where it was with the idle range
  (space-orders OR-08, OR-17). The order drops the previous target.
- Which code picks the craft (EAWR-475): `CombatWorld::candidates` (space-targeting CO-03) is the
  broad-phase scan that finds attack-worthy objects in the first place, including a squadron's
  team container; it never resolves a container to one of its craft. `CombatWorld::resolve_target`
  is the only place a team container becomes a craft, run on the already-known attack target right
  before a weapon fires (`UnitCombat::attempt`, FO-04, C-10, C-12); it does not consult the
  collection trees. A ship's hardpoint opportunity scan (space-weapon-fire T-05) can still hand
  different hardpoints different objects because it walks the tree in CO-03 order (G-F7), but once
  an object is a squadron's team container, FO-04's nearest-craft rule is what fires.
- **FO-07** (debug build: the space movement coordinator's composition step; EAWR-552) One move of
  several squadrons forms them into formations. The squadron farthest from the destination (input
  order on ties) starts one; its box is its formation radius doubled, widened in the plane to at
  least a third of its distance to the destination, and every remaining squadron whose own radius
  overlaps that box, and whose speeds suit it (each one's `Min_Speed` at most 0.9 of the other's
  `Max_Speed`), joins it. The others repeat this. A squadron alone flies FO-01 to the destination.
  FoC takes the world bounds of the team container where the remake takes its formation radius
  (project). A move that also carries ships gives FoC's squadrons an escort of one of the ships
  instead (space-movement FM-U5); the remake still maps the squadrons among themselves.
- **FO-08** (debug build: the formation's row mapping and default extents, the team's formation
  radius; EAWR-552) A squadron's formation radius is its farthest `Squadron_Offsets` slot plus its
  craft's soft radius. In cells of 20 units a squadron is two radii wide and deep, and the
  formation's rows are the square root of the squadrons' summed areas times 1.1 wide.
- **FO-09** (debug build: the formation's row mapping, its order comparator and its two
  projection comparators; EAWR-552, EAWR-599) A row holds squadrons of one order class, one type. FoC
  orders the classes heroes first, then by the type's `Targeting_Max_Attack_Distance`, shortest
  first, then by type name; the remake takes the craft's attack distance and then the type ID
  (project). Within its class the frontmost squadrons (along the move, from the formation's centre
  to the destination) fill a row while their widths fit, the first always; the rest wait for the
  next row. The row runs across the move from its right to its left in the order its squadrons
  stand, so the rightmost squadron takes the rightmost slot, with equal gaps between and around
  them. Each later row lies behind the last by half their depths, and the block is centred on the
  destination. Both orders are kept: a squadron in front stays in front of the ones behind it and
  one on the right stays right of the ones on its left, so four squadrons in a two-by-two block
  keep their corners, and four moved together hold a two-by-two block one squadron's width apart
  where before EAWR-552 they converged on one point. FoC sorts both ways with an unstable sort, so
  squadrons exactly level or exactly abreast have no defined order; the remake keeps the
  command's order (project). FoC maps the block onto a form-up box on the path, level with the
  formation's frontmost squadron and at most 160 units along the path, facing along it (debug
  build: the oriented form-up box); the remake centres the same offsets on the destination
  (project, unverified: where FoC's final slots lie).
- **FO-10** (debug build: the fighter locomotor's waypoint and moving service, the formation's
  upkeep and initialisation; gameconstants.xml; EAWR-599) The squadrons of one formation fly one path,
  not each a line to its own slot. In space FoC hands every member the formation's one path and
  starts it at once; the form-up path to a member's stage point (FO-09's box) is built only on
  land, so squadrons do not assemble before they leave. Every frame each squadron's leader heads
  along the path's direction, one turn
  radius (FM-11) ahead of itself and shifted to the left of that point by its lane steer; its
  craft form up on it (FM-11 to FM-14). The lane steer is the squadron's side error, its slot's
  offset to the left of the path less how far left of the path its team container flies, less
  `FormationMinimumSideError` toward zero and over `FormationMaximumSideError` (0.1 and 30), and
  zero within the minimum. It is that many world units at a look-ahead of one turn radius: with a
  54-unit turn radius at 5.4 units a frame a squadron 100 units off its lane aims 3.5 degrees
  toward it and closes the gap over about ten seconds. So squadrons moved together fly side by
  side on parallel lanes and drift onto their slots; with FO-09's order kept, squadrons starting
  close to their lanes never fly through each other (C-27). A wide line is different: two rows share
  each lane, and while a squadron is far off its lane FO-11's steering speed-up holds it at its
  `Max_Speed`, so the rows form late and a squadron of one row can drift across the lane of one
  in the other row still level with it (S-57: four X-wing squadrons 200 units apart come within
  1.3 units of each other near the end of the flight). Retail does the same (owner capture EAWR-629,
  2026-09-29): four X-wing squadrons in a diagonal line, moved together, keep their line and its
  order for about 15 seconds with no row splitting off, the two middle squadrons then close in
  until their craft intermingle for some 8 seconds, and the four settle into a turned two-by-two
  block about 20 seconds after the order; four squadrons in a loose clump spread at once into a
  two-by-two block that keeps its corners for the whole flight, as C-27.
  A squadron's move ends where its leader passes the line through its slot square
  to the path (FO-02; project: FoC's per-member half plane was not read). The remake's path is the
  straight line from the formation's centre to the destination (project, as FO-02). A squadron
  moved alone still flies straight at its point (FO-01): its formation's lane steer, which pulls
  it onto its path line, is not modelled (fidelity list).
- **FO-11** (debug build: the formation's upkeep, forward offset and minimum speed; EAWR-599) The rows
  keep their distance by speed. Every frame a squadron's deviance is the mean, over the other
  squadrons of its formation at least 20 units off their places relative to it, of how far each
  stands ahead of it along the path less how far its slot lies ahead of its own. A squadron behind
  its place flies at the formation's maximum speed blended toward its own `Max_Speed` by its
  deviance over the largest deviance (at least 12); one ahead of its place blends the same way
  toward the formation's minimum speed, the largest `Min_Speed` among its squadrons. Steering
  aside, it flies faster by the square root of one plus its lane steer squared, never faster than
  its own `Max_Speed`, and every craft of the squadron forms up at that speed. Four squadrons
  starting level fall into their rows within a second: the back row slows toward `Min_Speed`
  until it is within 20 units of its place. Project: the formation's maximum is the slowest
  squadron's `Max_Speed` (FoC keeps its own value, not read); FoC also caps a speed-up at the
  maximum plus the square root of twice the deceleration times the deviance or side error, which
  the remake drops (it binds only when a squadron is faster than the formation, and the craft
  table has no deceleration); every moving squadron of the formation counts as a neighbour (FoC
  counts only a neighbour that is formation-moving too and whose current formation offset index
  equals the squadron's, taken from a per-object neighbourhood list; what that index and the list
  are was not traced, EAWR-696); FoC reads the team container's `Min_Speed` where the remake takes
  the craft's.
- Constants the space formation reads (debug build, EAWR-599): `FormationMinimumSideError` and
  `FormationMaximumSideError` (the upkeep's lane steer, FO-10), wired. Land only (EAWR-626):
  `MaxLandFormationFormupFrames` and the skip-land-form-up flag gate the stage-point form-up path,
  which space never builds.
- Tags near FO-07 to FO-11 that the remake does not wire: `BetweenFormationSpacing`,
  `FinalFormationFacingMinimumAngle`, `FinalFormationFacingDeltaCoefficient`,
  `Rotate_Formation_Facing_Moves`, `Max_Formation_Area` and `Short_Range_Attack_Formation_Coefficient`
  (gameconstants.xml): which of them the space formation reads was not traced here (EAWR-626 audits
  the formation constants); the row mapping and the form-up box read none of them.
  `MaxJiggleDistance` is authored only on land infantry and vehicles, so a squadron's jiggled
  offset is its base offset.
- The order's state is hashed only while a move lasts, and its diversion (FO-05, FO-06) only while
  it is not idle, so a session without squadron orders keeps its hashes (docs/replay-format.md).
  A craft's chase and avoidance state and a squadron's combat cell (FD rules) are hashed only
  while they are set, so a session without dogfights keeps its hashes.

## Cases

The fixtures in `tests/replay/fighter_tests.cpp` pin C-01 to C-06 and C-09 to C-16 on synthetic tables,
`tests/presentation/space/fighter_heading_tests.cpp` pins C-17, `tests/replay/flight_tests.cpp`
pins C-18 to C-21 with FoC's X-wing values, `tests/replay/dogfight_tests.cpp` pins C-22 to C-27;
`tests/fidelity/test_squadron_trace.py` runs S-28 on the FoC tables (C-07).

- **C-01** A spawner of entries A (1, reserve 2) and B (1, no reserve) and a 150-frame delay launches
  at its first service, again 150 frames later, and nothing more while both are alive.
- **C-02** Losing A's squadron while both are alive makes the next launch wait 150 frames from the
  loss; losing B's (no reserve left) launches nothing.
- **C-03** With its only bay destroyed a spawner launches nothing.
- **C-04** A launched squadron's craft start at the bay facing along the spawn vector at full speed,
  and their container has the next ID after them.
- **C-05** A squadron scans the priority-1 candidate over a nearer priority-3 one, and ignores a
  unit beyond the chase range plus attack distance.
- **C-06** A hash of a launch, attack, loss and replacement session is the same with 1, 2, 4 and 8
  workers.
- **C-07** S-28 on the FoC tables with 1, 2 and 4 workers writes one trace: the first squadron at
  the first service, the second 150 ticks later, every craft at the recorded bay point and
  facing, the corvette's shield gone before it dies, and its death within 20 % of the recorded
  tick 883 (the remake: 772, see G-F7).
- **C-08** A launched squadron with an enemy in reach takes its first target one second after the
  launch (FT-06); until then no craft holds a target or fires, and then they fire (FT-07).
- **C-09** A squadron ordered 1700 units away closes at full speed (over 4 units per frame) and then
  holds the destination; a stop during a second move holds where it stood; an attack on a frigate
  far beyond its chase reach makes the frigate every craft's target and the squadron closes on it;
  the session hashes the same with 1, 2, 4 and 8 workers (FO-01 to FO-03).
- **C-10** A ship ordered to attack a squadron fires first at its craft nearest the ship, and a
  squadron ordered to attack it fires at its craft; no shot goes to the team container; 1, 2, 4
  and 8 workers hash alike (FO-04). This is `CombatWorld::resolve_target`, run once a target is
  known; it never scans the collection trees (CO-03) itself, so it is unaffected by C-13's
  approach and by which candidate order found the target in the first place.
- **C-11** (EAWR-409, EAWR-469) A craft with a -1 out-of-combat adjustment carries it while idle and on
  the approach beyond its strafe reach plus the target's radius, and 0 on a straight dive beyond
  that reach after the approach and inside the reach; a type without the tag carries 0
  (space-damage DG-26).
- **C-12** A gunship attacking a squadron that flies past it fires every shot at the craft nearest
  it at that frame, so its shots move from one craft to the other; 1, 2, 4 and 8 workers hash
  alike (FO-04).
- **C-13** (EAWR-469) Beyond the strafe reach, a follower level with a leader and a target dead ahead
  dives straight at the target without the approach and turns toward its slot on the approach; the
  leader keeps level without it and climbs toward its layer height on it (FA-07).
- **C-14** (EAWR-452) A squadron attack-moving to a point engages a frigate 700 units off its way,
  on the way; the same move without the attack flag flies past it. Attack-moving to a friendly
  frigate, it follows that frigate and leaves an enemy 1300 units from it alone (beyond 300 plus
  500). 1, 2, 4 and 8 workers hash alike (FO-05).
- **C-15** (EAWR-452) A squadron guarding a friendly frigate engages an enemy 1300 units from it
  within two seconds (1000 plus 500). Guarding a point 2500 units away, it engages the same enemy
  once its leader is within 1000 of the point; a move to that point never does (the idle range
  is 200). 1, 2, 4 and 8 workers hash alike (FO-06).
- **C-16** (EAWR-497) A squadron ordered to attack a frigate 3000 units ahead and 400 below flies the
  approach: halfway there its leader still keeps its layer height, where a straight dive would be
  about 200 units down; the same after the order is given again. 1, 2, 4 and 8 workers hash alike
  (FA-07).
- **C-17** (EAWR-506) A synthetic carrier launches three craft nose down (FL-06) at a frigate above
  it. Over 300 ticks, every drawn craft's nose lies within 0.5 degrees of its travel over the tick
  that ends at the drawn frame (FM-07), half way through a tick it lies within half that tick's
  turn of the travel, and the craft pitch more than 20 degrees. A craft pitched from 80 to 95
  degrees (over the vertical, FM-06) is drawn half way over at mid-tick, a level unit draws no
  pitch, and a level craft rolling from 177 to -177 degrees is drawn upside down half way (EAWR-479).
- **C-18** (EAWR-479) An X-wing squadron (FoC's values and `Squadron_Offsets`) flies east, reverses and
  curves north: on every frame every craft moves by exactly its velocity, and the velocity lies
  along its nose within 0.01 degrees (FM-07).
- **C-19** (EAWR-479) In C-18 every yaw step leans into its turn, and the leader yaws at most
  `Max_Rate_Of_Turn` and rolls at most `Max_Rate_Of_Roll` a frame and banks the full 40 degrees in
  the curve. A lone X-wing facing a 135-degree turn rolls 6 degrees and yaws 3.6 on the first
  frame and banks 40 degrees by the seventh, left or right; a 5-degree correction banks 5 degrees
  (FM-03).
- **C-20** (EAWR-479) An X-wing flying east sent to a point behind it points its nose straight up,
  climbs over 80 units, heads west upside down 30 to 60 frames later and rolls upright within 40
  more; no frame turns it more than two lift steps plus a roll and a yaw step (FM-06).
- **C-21** (EAWR-479) A follower 15 units right of its leader climbs toward its slot when the leader
  banks 40 degrees left, dives when it banks right and stays level beside a level leader; in
  C-18's curve the squadron's heights spread over 5 units (FM-14).
- **C-22** (EAWR-457, FD-01 to FD-04, FD-06) Squadron 10 attacks squadron 20 700 units away: it records
  a cell, 20 turns on it, both join one cell, and craft pair off one chaser to one chased, only
  across teams; 1, 2, 4 and 8 workers hash alike. The craft keep within 200 units of their layer
  height (FD-12).
- **C-23** (EAWR-457, FD-06, FD-08) Every chase timer runs 300 frames, a chase ends when its timer runs
  out, and a craft whose chase ran out chases again.
- **C-24** (EAWR-457, FD-09) A fight ends when the target squadron dies and no other enemy is joined in
  the cell: the squadron leaves its cell and holds the idle cell where its leader was (FM-24).
- **C-25** (EAWR-457, FD-10, FD-11) A craft flying at a frigate's centre steers once it is within the
  frigate's radius plus its speed and passes further off the centre than through a frigate without
  a space layer; two craft on crossing moves pass through each other.
- **C-26** (EAWR-457, FD-04, S-98) A squadron attacking a frigate keeps it as its target while an
  enemy squadron engages it; the interceptors record a cell around it (FD-02).
- **C-27** (EAWR-552, EAWR-599, EAWR-687, FO-07 to FO-11, FM-24) Four squadrons moved together by one command
  hold four slots a squadron's width apart, and at arrival their centres stay at least a cell apart;
  moved one by one to one point they now hold separate idle cells too (FM-24). A squadron far from
  the others flies alone to the point. A two-by-two block keeps its corners, and a squadron of a
  shorter-ranged type heads its own row. In flight (while every squadron is on its move; after it
  each flies to its idle cell) the craft of different squadrons stay more than
  10 units apart and no two squadrons swap sides of the move while less than a squadron's width
  apart along it (before EAWR-599: 0.10 units, as moved one by one). A squadron 30 units off its lane
  steers (30 - 0.1) / 30 units aside, and one 60 units ahead of its row slows to `Min_Speed`.
- **C-29** (EAWR-599, S-57, FO-09 to FO-11) Four X-wing squadrons abreast, 200 units apart, moved 2400
  units by one command on the FoC tables: the trace is identical with 1, 2 and 4 workers; it
  reports the flight's closest craft of different squadrons and its lane crossings (1.3 units and
  none before the flight ends). Retail's capture of such a line (EAWR-629) shows the same late close
  pass of the two middle squadrons (FO-10).

## Unknowns

- **G-F1** (recording S-28) Resolved: runs 2 and 3 of S-28 bind all four bombers and all seven
  reserve fighters, as the debug build's spawn code and S-16 say; run 1 binds three and five, with
  the same launch ticks and deaths, so its missing slots are the recorder's slot binding. The
  remake launches every `Squadron_Units` member.
- **G-F2** Resolved by EAWR-457 (FD-01 to FD-12). The S-28 gap once filed under this ID, fighters
  circling at the corvette's sides, was the missing approach in formation (FA-07, EAWR-469).
- **G-F3** Resolved by EAWR-457: craft steer around ships (FD-10) and never around each other (FD-11).
  The out-of-combat defense modifier is space-damage DG-26 since EAWR-409.
- **G-F4** Resolved (EAWR-597, debug build): the start places each craft of a squadron company by its
  own free-space search (FC-02, space-movement PL-01). Whether formation offsets turn with the
  leader is resolved too (EAWR-479, debug build): with its whole facing, every frame (FM-14).
- **G-F5** How retail picks the squadron's target is only partly read: the formation's diversion
  ranges (E75-22) are, and so is the proposer (each craft's own scan, FT-07); the proposer's
  cadence and the squadron-side choice between proposals are not (FT-01, FT-02 project; FT-06
  takes its first delay from the recordings). A craft that already holds a target and finds it
  unsuitable rescans for itself in retail; the remake leaves that retarget to the squadron.
- **G-F7** (recording S-28) The remake's corvette dies at tick 772, 111 ticks before the recorded 883.
  History: 775 before the EAWR-388 review; 747 after it (the FM-12 cap alone; the other two fixes
  change nothing); 691 after EAWR-384's weapon arcs (with or without the cap); 685 with FT-06 and
  FoC's FM-12; 775 once craft fire only within their turret extents (space-weapon-fire W-09); 775
  after EAWR-409. EAWR-409 found why retail's hits on craft 640 to 710 units out come in pairs (22 then
  14): a craft out of combat takes twice the damage (space-damage DG-26), and the corvette's
  lasers now lead their targets (space-weapon-fire W-10, W-11). The first wave now dies on
  retail's schedule, shifted by the earlier launch: fighters at 154 and 175 against 172 and 190,
  bombers at 362 and 454 against 379 and 446, and every launched craft dies (before EAWR-409 one
  fighter lived to the end). EAWR-469 found the two gaps that kept the later craft alive: the
  corvette's hardpoints took their candidates in ID order, so every hardpoint took the same craft,
  where FoC's collection trees hand different hardpoints different craft (space-targeting CO
  rules); and the squadron dived straight at the corvette, where FoC's squadron approaches in
  formation (FA-07). With both, the fighters died at 157, 176, 234, 262, 298, 340 and 572 (retail 172, 190,
  270, 274, 321, 338 and 418; before 154, 175, 249, 552, 563, 614 and 658) and the bombers at 359,
  430, 454 and 544 (retail 379, 380, 446 and 465; before 362, 454, 533 and 567); the corvette died
  at 781 (before 775). **EAWR-536's per-mesh collision (space-damage DG-36, DG-37) moved these:**
  the fighters now die at 159, 160, 173, 259, 259, 559 and 572, the bombers at 354, 409, 453 and
  515, and the corvette at 772, 9 ticks earlier than before. The fighter that lived to 572 before EAWR-536 flew through the corvette unhit on its first
  run. Remaining differences: the keyed draws and the tree order differ from retail frame by
  frame (space-weapon-fire P-02, space-targeting G-01); from the last craft's death the
  Acclamator's lasers alone finish the corvette; the keyed service draw launches at 15 instead of
  27 (FL-01); and (FL-11) the M2 table launches no reserve squadron, while retail's reserve at 597
  finishes the corvette.
- **G-F6** The leader of a moving squadron follows a planned path in retail (E75-20); the remake
  has no squadron move orders yet, so squadrons only hold, escort and attack.
- **G-F9** (EAWR-506; resolved by EAWR-479, debug build) FoC's heading step adds or removes a whole turn
  from the wanted yaw change when it plus the yaw that undoing the roll amounts to (the roll over
  `Max_Rate_Of_Roll` times `Max_Rate_Of_Turn`) exceeds 180 degrees. The turn step then wraps the
  angle back into (-180, 180] before it uses it, so the adjustment has no effect: FoC turns the
  short way in yaw, as the remake does (FM-03).
- **G-F10** (EAWR-664) Resolved by EAWR-687 (FM-23 to FM-26): an idle squadron holds its idle cell's
  point, so a leader that ends a fight or a move is up to 85 units from it, not on it. On the
  nightly soak's 12 seeds that failed on speed (9000 ticks each, on the start before EAWR-605), the
  peak craft speed falls from 27.5 to 72 times the maximum (after the FM-20 fix) to 5.5 to 15
  times, and no seed fails on speed or slot crowding. Retail's S-97 and S-98 craft peak at about
  3 times; the rest of the gap is not traced.
- **G-F8** (EAWR-469) The approach path of FA-07 is not traced: where FoC plans the squadron's attack
  move and how the path runs past obstacles. The remake uses the planar line from the leader to the
  target. FoC also adds `Formation_Y_Offset_Coefficient` times the path's left normal to the
  leader's waypoint. EAWR-599 read its value (the lane steer, FO-10): for the approach's one-squadron
  formation it pulls the leader onto the path line; the remake adds it only on a group move.

## Fidelity list

- FT-01 (EAWR-633): a squadron keeps a fogged target until its approach ends at the FA-01 reach; FoC
  keeps it until its planned path ends (G-F8), and its path end is not traced.

- FD-06: the remake commits chase pairs in ascending craft ID after a partitioned scan of the
  frame's start; FoC services craft one by one, each seeing the flight of those before it.
- FD-10: ships are spheres of their soft radius for the avoidance test, not their hulls.
- FD-12: in S-97 the remake's deepest dive reaches 290 units off the plane where retail's
  reaches 173, and 90 % of its heights lie within 127 units where retail's lie within 100; the
  pitch spread matches. The trace's limits (band 150, extreme 320) are set above the remake's
  values, not at retail's, so the test passes with the gap and does not measure it.
- FD-13: the S-97 outcome still differs from the recording, and the cause is the chase, not the
  shots (EAWR-607). A shot log of both recordings (the debug build's object-weapon projectiles and every
  hit with its mesh, health before and after, and the hit's position) shows retail's fighters land
  as many of their shots as the remake's: 75 of 86 X-wing shots and 152 of 163 TIE shots in S-97,
  98 of 162 X-wing shots on the bombers in S-98, most of them on the craft sphere (space-damage
  DG-37: 121 of the 152 hits on X-wings, up to 48 units from the craft's centre); damage per hit
  matches (5, then about 3.1 with diminishing firepower). What differs is who fires: retail's TIE
  fighters fire 163 shots and its X-wings 86, while the remake's X-wings fire 165 and its TIE
  fighters 64 (after EAWR-607's W-06a burst rule and DG-37 world box). Over the fight retail's TIE
  fighters keep an X-wing in their cone about as often as its X-wings keep a TIE fighter (25 % of
  their live ticks each); the remake's X-wings 31 % and its TIE fighters 17 %, and the TIE fighters
  keep firing at the X-wing leader where retail's spread over the squadron. So the fight still
  ends one-sided: the remake loses no X-wing and all seven TIE fighters (the first at tick 448, the
  last at 1249), retail five X-wings from tick 706 and two TIE fighters. The chase geometry (FD-01
  to FD-06, the craft's speed in a dogfight) is the open cause. S-98 matches since EAWR-607: three
  bombers lost at 685, 1022 and 1030 (recorded 723, 929, 1110), 155 X-wing shots and 94 hits
  (recorded 162 and 98). `fidelity_dogfight_scenario_trace` holds both outcomes; S-97's limits sit
  at the remake's values and may only move toward the recording. The X-wings' shield recharge is
  not the cause: both logs show the same steps (3 points every 90 frames, 0.25 while the depletion
  cap holds, DG-13 and DG-15). Retail's X-wings take 30 or 31 hits each before they die, 0.8 to 2.0
  hits a second while under fire, 3.4 a second on the squadron, and recharge 4 to 13 points each
  (46.5 in all) against about 120 points of damage. The remake's X-wings take 44 hits in all, 1.2 a
  second on the squadron: 30 on the leader, 12 on one wingman, 2 on another. The leader recharges
  28.5 points over its longer spread of hits and ends with 4.7 hull, so the recharge saves that one
  craft; the other losses need the hits that are never fired.
- FO-07: the merge box uses the squadron's formation radius for the team container's world bounds;
  squadrons moved with ships are not handed an escort of a ship (space-movement FM-U5).
- FO-09: the classes are ordered by the craft's attack distance and type ID, not the team
  container's distance and type name, and there are no heroes; exactly level or abreast squadrons
  keep the command's order where FoC's order is undefined; the slots are centred on the
  destination, not on FoC's form-up box; the order comparator treats two types whose attack
  distances differ by less than a global equivalence radius as one class (the radius was not
  recorded), where the remake keeps every distinct type apart.
- FO-10: the path is the straight line from the formation's centre; each squadron's move ends at
  the line through its own slot. A squadron moved alone, and the FA-07 approach, fly without the
  lane steer. Retail's group move is an owner capture (EAWR-629, video, no trace): the remake's
  in-flight spacing is compared with it by eye, not measured tick by tick. Both show a wide line
  holding its order and forming its block late, with the two middle squadrons passing close.
- FO-11: the formation's maximum speed is the slowest squadron's; the deceleration caps are
  dropped; every moving squadron of the formation is a neighbour, where FoC counts only those with
  the same current formation offset index (EAWR-696); `Min_Speed` is the craft's. Also not modelled
  (debug build): a squadron in a moving-against-moving slowdown is skipped when the deviance is
  recomputed; a destination that enforces top speed sets every member to its own maximum, with no
  deviance at all; an escort's forward offset is averaged into the squadron's.
