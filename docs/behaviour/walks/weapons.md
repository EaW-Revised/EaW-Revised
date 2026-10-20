# Walk: weapons and projectiles, per frame

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. The subsystem walk of
  2026-09-29 (walk 3 of the coordinator's list): every rule the FoC debug build applies to a
  weapon (a weapon hardpoint, or a unit's own weapon) and to the projectiles it fires, each frame,
  in evaluation order, with the gaps against the remake.
- Sources: **debug build** (the FoC debug executable with symbols, read under the clean-room rule;
  evidence IDs EWW-nn are opaque and their map stays private), **recording** (the fidelity traces
  of [tests/fidelity](../../../tests/fidelity/README.md) and the retail dogfight outcome comparison retail shot log), **data** (a
  tag or value in the FoC XML), **unverified** (not settled by either). Earlier evidence families
  (TE, WA, AT, CF, PD, CT) keep their meaning from [space-weapon-fire](../space-weapon-fire.md),
  [space-damage](../space-damage.md) and [space-targeting](../space-targeting.md).
- Out of scope, recorded only as an interface: the ship-level target choice and the opportunity
  scan's inside (walk 2, capital combat, and [space-targeting](../space-targeting.md)); where a
  hit's damage goes once it reaches a unit (walk 2: shields, hull, hardpoints, DG-39 of hardpoint hit routing); how
  a squadron moves into range (walk 1); abilities that scale fire (walk 7); heights and layers
  (walk 4, per-unit flight heights). Presentation projectile models, trails and sounds (legacy EAWR-660) are not covered.

## Scope

- **Objects.** A *weapon hardpoint* (a `HardPoint` of type laser, missile, torpedo or ion cannon)
  on a ship, station or craft; a *unit's own weapon* (the type's `Projectile_Types`, used by the M2
  fighters); a *projectile* (a `Projectile` object with the `PROJECTILE` behaviour).
- **Cadence (debug build, EWW-01, EWW-07, EWW-10).** A weapon hardpoint is serviced with its parent
  every frame; it counts its recharge down by the frames since its last service. A unit's own
  weapon is serviced by the unit's weapon behaviour the same way. A projectile is an object of its
  own, serviced every logical frame (1/30 s).
- **Entry points in the frame, in words.** Every object is serviced once a frame in the object
  manager's order. For a unit, its behaviours (targeting, weapon, locomotor) and its hardpoints each
  run their service; a hardpoint runs WWP-01 to WWP-35, the unit's own weapon WWP-40 to WWP-55.
  Every projectile in the session runs WWP-60 to WWP-72. Which of a unit's services runs first,
  and whether a projectile created during the frame is serviced in the same frame, are not read
  (**unverified**, U-01, U-02); the remake's order is space-weapon-fire P-01 and space-damage DP-01.

## Rules, in evaluation order

The [ion inventory](../space-damage.md#inventory-of-the-starting-fleet-and-production-roster)
covers both the starting fleet and the expanded M2 production roster, including level-3/4/5
stations. Ordinary station ion cannons share the MC80's EN-07 drain path and do not stun;
`ION_CANNON_SHOT` overrides stun without draining. An ion projectile is not identified by
hardpoint class alone: B-wing laser hardpoints and TIE Defender torpedo hardpoints also fire
ordinary ions. Their temporary engine disable follows EN-08 and EN-09.

### Weapon hardpoint service (every frame)

- **WWP-01** (debug build, EWW-01) A hardpoint is not serviced during the setup phase or while its
  parent is being deleted. Only a weapon hardpoint whose weapon is enabled (an ability can switch
  it off) runs the weapon service.
- **WWP-02** (debug build, EWW-01) Before the recharge counts down, the service stops when weapon
  fire is off globally, when the hardpoint is destroyable and has no health, when the hardpoint is
  disabled, or when its parent is stunned, in limbo (not garrisoned), deploying or undeploying. So
  a stunned parent's hardpoints keep their recharge where it was.
- **WWP-03** (debug build, EWW-01) The recharge counts down by the frames since the hardpoint's
  last service (one a frame); while it is still above zero the service stops.
- **WWP-04** (debug build, EWW-01) A hardpoint with `Requires_Manual_Target_Assignment` fires only
  at the target a player assigned it, and drops that target after 300 frames of failing (with the
  negative-feedback sound for the local player). No M2 hardpoint sets the tag.
- **WWP-05** (debug build, EWW-01, EWW-02) The weapon-state gate, on the hardpoint's ordered target
  (none is allowed): the service stops for the frame, with no opportunity fire either, when that
  target is dead, being deleted or a death clone; when the hardpoint is not a weapon, is destroyed
  or is disabled; when the ordered target's category is in `Fire_Category_Restrictions`; or when an
  active unit-mode ability, a deployed state or an attack mode forbids this hardpoint to fire (its
  per-mode fire flags). No M2 hardpoint authors a per-mode fire flag.
- **WWP-06** (debug build, EWW-01) A parent with a locomotor whose type lacks
  `Can_Fighter_Fire_When_Idle` does not fire while its locomotor is in the fighter idle state (the
  recharge has already counted down; space-fighters FT-07).
- **WWP-07** (debug build, EWW-01) An ordered target's aimed hardpoint that is destroyed is dropped;
  the attempt then picks one itself (WWP-19).
- **WWP-08** (debug build, EWW-01) Without an ordered target, a hardpoint whose
  `Allow_Opportunity_Fire_When_Idle` is off does nothing. With an ordered target and that flag off,
  it fires at it only while one check of its parent's targeting holds (not identified here) or the
  parent's formation (the squadron's, for a craft) has that same target as its attack target. In M2 only the bombers'
  torpedo hardpoints (`HP_BOMBER_02`, `_03`) turn both opportunity flags off.
- **WWP-09** (debug build, EWW-01) A hardpoint holds fire at its ordered target when that target is
  its parent's special-ability attack target and the target's type sets
  `Should_Attacker_Hold_Fire_For_Special_Ability` (not in M2).
- **WWP-10** (debug build, EWW-01) With an ordered target, the hardpoint attempts it (WWP-13 to
  WWP-35) at its aimed hardpoint, if any. The first success after a failure sends the parent (or,
  for a squadron leader, its squadron) a "hardpoint target in range" signal.
- **WWP-11** (debug build, EWW-01) A successful ordered attempt ends the service. A failed one goes
  on to opportunity fire only with `Allow_Opportunity_Fire_When_Targeting`; without an ordered
  target, only with `Allow_Opportunity_Fire_When_Idle`. Both default to yes (DG-35).
- **WWP-12** (debug build, EWW-01; space-targeting R-02 to R-14) Opportunity fire: a retained
  opportunity target is attempted first (counted as failed without an attempt when the parent is in
  a nebula and fogged to that target's owner); success ends the service, failure drops it. A scan
  runs when a target was just dropped, or when more than `trunc(logical fps × 0.5)` frames have
  passed since the last one; the scan's inside is space-targeting's. A new find is attempted at
  once; success keeps it and sends the "opportunity target acquired" signal (to the squadron when
  the parent leads one), failure drops it.

### The firing attempt (a hardpoint at one target)

- **WWP-13** (debug build, EWW-03) The attempt fails when the target is dead, when it carries one
  particular behaviour (not identified here; no M2 unit is affected in the traces), or when it is
  stealthed while the parent's type cannot target stealth units.
- **WWP-14** (debug build, EWW-03) While the parent charges a blast ability, nothing fires; once it
  is charged only a special-weapon hardpoint fires, with the ability's projectile and damage
  multiplier (not in M2).
- **WWP-15** (debug build, EWW-03) The attempt fails when the target's category is in the
  hardpoint's `Fire_Category_Restrictions`, when the target is fogged to the parent's owner, or when
  the two are in different hero clashes.
- **WWP-16** (debug build, EWW-03) The aimed hardpoint is the given one; else the target's nearest
  hardpoint that is not destroyed and is targetable, nearest to the parent's height-adjusted
  position (WWP-72), the first on a tie; else none.
- **WWP-17** (debug build, EWW-03) A turret hardpoint (`Is_Turret`) fails unless its barrel already
  points within `Fire_Cone_Width` of the target (space-weapon-fire W-07). Ordinary object turrets
  also gate a pointing call on UA-05 mechanical readiness: reject desired yaw/pitch outside the
  authored extents, advance each by the authored turn speed (shortest wrapped yaw), and release
  firing only after both angles arrive. Turret/barrel bone posing follows UA-05. Space
  targeting invokes pointing on each admitted target-in-range turn independently of the
  weapon recharge/pulse countdown. The firing attempt reads the resulting readiness; the
  separate turret service does not advance a tracked target. Target clear starts the
  strict authored idle hold, then per-call return toward rest; reacquisition cancels it.
  FS-33, FC-60/61 and FD-51 supply this cadence. The named
  `Turret_Rotate_To_Rest_Delay_Sec` and `Turret_XY_Only` are unauthored in stock.
  Both are recognized by the object-definition engine-tag catalog on main (legacy EAWR-2321),
  now carried into Skirmish as `docs/tag-coverage/engine-tags.json` (ET-T053).
  The XY-only absent-tag default is false.
  Its absent-tag default is two seconds; equality still holds before the first later
  admitted return step. (FC-61; debug build EFSB-06)
  `ordinary_object_turret_runtime_boundaries` seam retains UFS-29's runtime normalization
  and indirect lifecycle producers.

- **WWP-18** (debug build, EWW-03) The projectile is the hardpoint's `Fire_Projectile_Type`, replaced
  by a unit ability's override for this target and aimed hardpoint: the Y-wing's ion shot (legacy EAWR-561).
  A craft whose squadron runs the ion-shot or lucky-shot ability and that has no override for this
  shot does not fire at all.
- **WWP-19** (debug build, EWW-03, EWW-06) The aim point:
  - with an aimed hardpoint, its current world position;
  - without one, the target's height-adjusted position, then:
    - a squadron's team container stands for a **random craft of the squadron** (one synchronized
      draw each attempt), since a container has no hardpoints of its own;
    - the aim-point search of space-targeting R-10 on that unit supplies the point; no acceptable
      point fails the attempt.
- **WWP-20** (debug build, EWW-03) The weapon midpoint is the midpoint of the fire bones
  (space-weapon-fire W-07). An attempt fails when the planar (XY) distance from it to the aim point
  exceeds the weapon's range plus the **target's soft radius** (WWP-21), or is less than
  `Fire_Min_Range_Distance`. The range is `Fire_Range_Distance` times one plus the parent's fire
  range modifiers (abilities), times the garrisoned multiplier while the parent is in limbo.
- **WWP-21** (debug build, EWW-05) A unit without model geometry has zero type soft radius,
  before either radius override. Otherwise its soft radius is its type's `Custom_Soft_Footprint_Radius`
  when positive, else its `Space_Obstacle_Radius` when positive, else the larger planar half extent
  of its model's box; times `Scale_Factor`. Only a specialized formation-team locomotor uses its
  hard radius instead (the length of its two planar hard extents). Ordinary space squadron
  containers use the type radius even while their movement formation is active (A-07, WSQ-45,
  UFS-35). The M2 stations author 300 (Rebel) and 250 (Empire); the ships take their model's half
  length. The specialized formation team's ideal box includes
  the origin and each surviving member's base slot widened by its hard extents; its radius is
  the length of the two resulting half extents, independent of scattered current positions
  (debug build, EUS-18). The ground-only `MaxJiggleDistance` padding has no authored space-squadron
  consumer in the stock data.
- **WWP-22** (debug build, EWW-03) The muzzle: with `Randomize_Between_Fire_Bones`, a uniform point
  in the box between the two fire bones (three synchronized draws); else `Fire_Bone_A` when there
  is no second bone; else a synchronized 50 % draw between the two (space-weapon-fire W-06).
- **WWP-23** (debug build, EWW-03; space-weapon-fire W-10) The aim point is led from the muzzle at
  the projectile's `Max_Speed`.
- **WWP-24** (debug build, EWW-03; W-07, W-11, W-12) A led point of zero length, or one the fixed
  hardpoint cannot point at, fails the attempt.
- **WWP-25** (debug build, EWW-03; space-damage DG-24) The led point scatters with the planar
  distance of WWP-20 (before the lead).
- **WWP-26** (debug build, EWW-03) The projectile is created at the muzzle facing the scattered point.
  Its damage is the hardpoint's `Projectile_Damage` when positive, else the projectile's; times the
  blast ability's multiplier while one is charged. Its damage type is the hardpoint's `Damage_Type`,
  else the projectile's.
- **WWP-27** (debug build, EWW-03) Its travel limit is the weapon's range of WWP-20 (the projectile's
  `Projectile_Max_Flight_Distance` when the range is zero), plus the target's soft radius, plus the
  squadron container's soft radius when the target is a squadron craft (unless the parent's type is
  melee), plus the height between the scattered point and the muzzle. The melee test compares
  the parent type's `Targeting_Max_Attack_Distance` to `Melee_Cutoff_Range`, inclusive (debug build).
- **WWP-28** (debug build, EWW-03) `Projectile_Appearance_Delay_Frames` above zero hides the
  projectile and holds its flight until the delay ends (no M2 hardpoint sets it).
- **WWP-29** (debug build, EWW-03; space-damage MS-02, MS-03) A `MISSILE` projectile holds the
  target and aimed hardpoint and keeps its scatter (scattered minus led point) in the target's
  frame. Any other projectile records its start, facing, speed and fire-at point.
- **WWP-30** (debug build, EWW-03) The parent's "weapon fired" hook runs (abilities: the ion shot's
  one shot per craft, ion weapons, energy drain and stun (legacy EAWR-561)).
- **WWP-31** (debug build, EWW-03; space-weapon-fire W-06, space-abilities AB-21) The burst: one
  pulse is spent. After the last pulse the recharge is a synchronized draw of whole hundredths
  between `trunc(100 × Fire_Min_Recharge_Seconds)` and `trunc(100 × Fire_Max_Recharge_Seconds)`,
  times the logical fps, over 100, truncated.
- **WWP-32** (debug build, EWW-03) Then the weapon delay factor (each active unit ability's
  per-hardpoint weapon delay multiplier, times the parent's weapon delay mode multiplier) shortens
  the recharge when below one, rounded; the fire rate `r` (the parent's fire rate mode multiplier
  times one plus its fire rate modifiers, such as the ion stun's) lengthens it when `0 < r < 1`
  (recharge over `r`, rounded); and the burst refills to `Fire_Pulse_Count × max(r, 0)`, rounded.
- **WWP-33** (debug build, EWW-03) Between pulses the recharge is `trunc(Fire_Pulse_Delay_Seconds ×
  fps)` times the weapon delay factor, rounded; with `r ≤ 0` recharge and burst both become zero,
  else the recharge is divided by `r`, rounded.
  Stock Y-wing torpedoes use `HP_BOMBER_03` with `MuzzleA_01`; TIE bomber torpedoes use
  `HP_BOMBER_02` with `MuzzleB_00`. Both author two pulses from their single muzzle,
  a 0.15-second pulse delay and fixed 8-second recharge.
  At 30 logical frames/s with ordinary modifiers, truncation gives a **four-frame gap**
  (about 0.133 seconds), then 240 frames from the second shot to the next volley.
  The debug-build fire and service paths were rechecked for the bomber volley timing
  investigation; there is no torpedo-specific simultaneous-pulse branch. The installed-data
  headless regression in `tests/units/unit_tables_foc_weapons_tests.cpp` checks both bombers,
  both volleys and equal hashes on 1/2/4/8 workers. This timing check does not settle
  visual overlap or spacing between different squadron members.
- **WWP-34** (debug build, EWW-03) The hardpoint remembers the frame it fired and the object it
  fired at.
- **WWP-35** (debug build, EWW-02, EWW-03) No line-of-fire or obstruction test: fog, category,
  range and pointing decide (space-targeting R-15).

### A unit's own weapon (every frame)

- **WWP-40** (debug build, EWW-07) The muzzle flashes count down (presentation).
- **WWP-41** (debug build, EWW-07) The recharge counts down by the frames since the last service
  (not while a forced fire holds it); while above zero the service stops. Unlike a hardpoint's, it
  counts down while the unit is stunned (WWP-43 comes after).
- **WWP-42** (debug build, EWW-07; retail dogfight outcome comparison dogfight scatter and burst timing W-06a) Once a burst has begun, each later pulse is
  spent when the recharge runs out, before any gate (the burst clock); the burst's first shot waits
  for a successful fire.
- **WWP-43** (debug build, EWW-08) The weapon is enabled only when its behaviour is serviced, unit
  weapons are on globally, the unit is not stunned, not deploying or undeploying, its deploy and
  attack-mode flags allow fire, its weapons-enabled mode flag is on, it is not a craft idling in
  formation (WWP-06), and its **targeting state is "okay to attack"** with a target. The ship-level
  targeting service sets that state (walk 2 and walk 1: "moving into attack range", "aiming" and
  "okay to attack" are its values); the weapon itself tests **no range**.
- **WWP-44** (debug build, EWW-07) A target that is no longer an enemy (unless it carries the
  behaviour of WWP-13) is dropped by the unit's targeting.
- **WWP-45** (debug build, EWW-07) The projectile is the type's current `Projectile_Types` entry,
  replaced by a unit ability's override for this target and its aimed hardpoint.
- **WWP-46** (debug build, EWW-07; space-damage EN-05) A unit whose energy pool cannot pay
  `Projectile_Energy_Per_Shot` does not fire; the cost is paid after the shot.
- **WWP-47** (debug build, EWW-07, EWW-09) The muzzle is the next of the model's muzzle bones in turn
  (the X-wing's four wingtip `MuzzleA_00` to `_03`, round robin); with several projectile types, the
  bone of the current type's index. The index advances when the weapon takes the bone, before the launch can be rejected
  (WWP-53's cone or the lead), so a rejected shot still uses its muzzle up.
- **WWP-48** (debug build, EWW-09) The shot's travel limit is the type's `Targeting_Max_Attack_Distance`
  times one plus the fire range modifiers; its damage is the type's own `Projectile_Damage` when
  positive, else the projectile's; its damage type is the type's `Damage_Type` unless that is the
  default, else the projectile's. This is the unit's own `Projectile_Types` weapon only:
  the unit tag does not replace the damage type of its hardpoint weapons (DG-12). The M2
  ships do not author the unit tag; their hardpoints supply their own damage types.
- **WWP-49** (debug build, EWW-09; conditional parent reference rechecked in the debug build) A
  squadron's team container as target stands for its craft nearest the actual firing muzzle.
  Only when the shooter's parent exists and has ordinary targeting does its centre replace the
  muzzle reference; squadron membership and team target metadata alone do not qualify.
  That craft is fired at with no aimed hardpoint (space-fighters FO-04). A target hardpoint that is
  gone or destroyed is dropped.
- **WWP-50** (debug build, EWW-09; the targeting service, G-W3) The aim point: the hardpoint attached to
  the unit's attack target when it has one (the one its order named, else the one the ship-level
  targeting attached when it acquired the target: the nearest standing targetable hardpoint for a
  targeting set without priorities, re-picked when it is destroyed); else one of the target type's
  `Target_Bones`, picked by a synchronized draw, at its animated position; else the target's
  height-adjusted position (WWP-72). The weapon itself makes no nearest-hardpoint choice and runs no
  range test in the aim choice.
- **WWP-51** (debug build, EWW-09; space-weapon-fire W-10) The aim point is led from the muzzle at
  the projectile's `Max_Speed`. The target's current, previous and before-previous matrix
  translations define its motion. The chords from the oldest point are normalized; stationary
  lengths within 0.001 or a direction dot product within 0.001 of one use linear lead. Otherwise
  fit their circle and predict around it at the latest displacement length divided by radius.
  Degenerate geometry, a radius above 1000 units or failed numerical convergence falls back to
  linear lead. The meeting-time residual is spatial distance from the muzzle to the predicted
  centre minus projectile distance. A secant solve starts at 0.1 and 1000 seconds, permits
  fewer than 15 iterations, and stops when successive times differ by less than 0.001 seconds;
  equal residuals fail. The selected aim's offset from the current target centre is added after
  predicting the centre. The remake uses deterministic Q24 geometry and frame units; absent
  history keeps linear lead. The captured muzzle origin and member selection remain separate
  upstream inputs, so circular-lead parity alone does not establish ordinary battle parity.
- **WWP-52** (debug build, EWW-09; space-damage DG-24) Scatter: a draw within the type's
  `Targeting_Fire_Inaccuracy_Fixed_Radius` when it sets one; else a radius of the spatial distance
  times its `Targeting_Fire_Inaccuracy` row for the target (doubled in space) over its
  `Targeting_Max_Attack_Distance`. No M2 unit authors either tag, so no M2
  unit weapon scatters; the fighters' `Fire_Inaccuracy_Distance` rows are hardpoint data and are
  not read for it. A laser flies at the scattered point; a missile keeps the scatter as its offset.
- **WWP-53** (debug build, EWW-09; space-weapon-fire W-09) `Fires_Forward` yes: the shot leaves along
  the unit's facing, with no cone test. Otherwise the fired-at point must lie within
  `Turret_Rotate_Extent_Degrees` in yaw and `Turret_Elevate_Extent_Degrees` in pitch (whole, not
  halved) of the unit's facing at the muzzle; a point of zero length fails. The flag defaults
  to no and the extents independently default to 360 degrees yaw and 180 degrees pitch
  (debug build, OW-01 to OW-03 in space-weapon-fire). Authored extents apply even when
  `Fires_Forward` is absent, resolving G-W4; StarViper therefore uses its authored 45/45 cone.
- **WWP-54** (debug build, EWW-09) The projectile is created at the muzzle; its muzzle delay ends at
  the current frame plus the bone's appearance delay (zero in M2), so it moves from the next frame.
- **WWP-55** (debug build, EWW-07) After the shot: the pulse is spent (a burst's first shot, WWP-42),
  the energy paid, the fire sound and attack animation played.

### Projectile flight (every frame)

- **WWP-60** (debug build, EWW-10) A projectile whose muzzle delay has not ended does not move (when
  it ends, a hidden projectile shows).
- **WWP-61** (debug build, EWW-10) Its speed gains `Projectile_Acceleration_Per_Frame` each frame,
  never below zero (no M2 projectile sets it).
- **WWP-62** (debug build, EWW-10) By `Projectile_Category`: a `MISSILE` that holds a target not
  under sensor jamming flies the missile path (WWP-64); any other missile and every laser flies the
  direct path (WWP-63). Bombs, grenades, rockets and sabres have paths of their own (no M2 weapon).
- **WWP-63** (debug build, EWW-11; space-damage DG-22) Direct path: a missile shield near the
  projectile may turn it (no M2 unit has one); then it moves `speed` along its facing.
- **WWP-64** (debug build, EWW-10; space-damage MS-03 to MS-05) Missile path: it steers at its aimed
  hardpoint, or the target's height-adjusted position, plus its offset. A missile that has lost its
  target looks for a new one and finds none (`Projectile_Max_Scan_Range` 0 in M2).
- **WWP-65** (debug build, EWW-13) The step from the current position to the new one is tested for
  ground (land only) and objects (WWP-66); the nearer contact wins, an object on a tie.
- **WWP-66** (debug build, EWW-14) Objects: only the players the projectile's owner counts as
  enemies, visited in the sorted player order; the first player that yields a hit supplies it. In a
  player's collision tree, only units whose box the step meets are tested, in the tree's order; the
  first that is collidable by a living projectile (`Collidable_By_Projectile_Living`; a dead one,
  `_Dead`) and that the step meets wins: its collision meshes (the shield mesh only for a
  shield-damaging projectile, DG-38), then its craft sphere (DG-37). Allies, the owner's own units
  and neutral objects are never hit.
- **WWP-67** (debug build, EWW-15) On a hit the damage goes to the unit hit, with the contact, the
  mesh met and the projectile's aimed hardpoint (walk 2 routes it: DG-01 to DG-39). Then the
  detonation sound, the projectile's blast area damage (`Projectile_Blast_Area_Damage` and `_Range`;
  no M2 projectile), and its detonation effects: stun, ion stun (legacy EAWR-561), and the land-only ones.
- **WWP-68** (debug build, EWW-12; space-damage DG-33) The projectile moves to the new position and
  adds its speed to its travel. It expires when its travel reaches the limit (WWP-27, WWP-48); one
  with no limit expires after `Projectile_Max_Lifetime` seconds. A `DEFAULT`-category projectile
  that explodes at its target radius expires once it passes its fire-at point.
- **WWP-69** (debug build, EWW-12) A hit or an expiry destroys it. Only an expiry without a hit plays
  the lifetime detonation particle and the detonation effects of WWP-67.
- **WWP-70** (debug build, EWW-12) The step that ends the flight is tested for a hit first, so a
  projectile can hit up to one step past its limit (DG-33).

### Shared points

- **WWP-72** (debug build, EWW-16) A unit's height-adjusted position is its position raised by its
  type's `Ranged_Target_Z_Adjust` (M2: the Nebulon-B 35, every other M2 type 0 or unset).

- **WWP-73** (debug build; BK-05, W-07) Weapon geometry uses the current world placement of the
  model's fire bones, including the ship's bank. Setting the synchronized facing also sets the
  displayed facing; the placement applies yaw, pitch and roll before the fixed model turn, and
  that placement feeds the model and its bones. The hardpoint's midpoint and cone axes come from
  those bones. A level heading used for movement planning is not a separate weapon placement.
- **WWP-74** (debug build; WWP-47, WWP-51, WWP-53, WWP-54) Projectile construction takes its
  spawn position from the current world fire bone, and aims from that bone's placement toward
  the led/scattered target. Thus banking moves an off-axis gun's launch point and can change
  the launch ray; it does not force an aimed shot to fly along the gun axis. Categories whose
  launch follows the bone axis also take that axis from the banked placement. `Fires_Forward`
  retains its separate facing rule (WWP-53). A source without a fire bone retains the
  height-adjusted object-position fallback (WWP-72).

<a id="planar-or-3-d-666"></a>

## Planar or 3-D

With the per-unit flight heights the M2 ships sit up to 290 units apart in height. From WWP-20 to WWP-70:

| Test | Metric | Rule |
|---|---|---|
| Hardpoint fire range and minimum range | planar, plus the target's soft radius | WWP-20 |
| Opportunity scan box | 3-D cube, 1.1 × range | R-07 |
| Opportunity aim-point range | 3-D, plus the soft radius (R-11) | R-11 |
| Ship-level scan and attack distance | planar | T-06, A-04 |
| Hardpoint cone | yaw and pitch in the fire bone's frame | W-07 |
| Unit weapon cone | yaw and pitch in the unit's frame | WWP-53 |
| Unit weapon range | none in the weapon (the targeting state gates it) | WWP-43 |
| Hardpoint scatter radius | planar distance | WWP-25 |
| Travel limit | range plus the height between muzzle and aim | WWP-27 |
| Hit test | 3-D (meshes, sphere) | WWP-66 |
| Missile steering | yaw and pitch | WWP-64 |

So FoC keeps a hardpoint's range planar and adds the height to the travel limit, which lets a ship
hit a target on another layer at full range; the cone's pitch test and the 3-D opportunity aim
distance do see the height. The remake computes each of these with the same metric (see the gap
list); only the missing heights themselves (per-unit flight heights) and the soft radius (G-01) change what it fires
at.

## Existing notes against this walk

| Note rule | Verdict |
|---|---|
| space-weapon-fire W-01, W-02 | same (WWP-03, WWP-12); W-02 misses WWP-02's stun, limbo and disabled gates |
| W-03 | differs: the unit weapon's gate is the targeting state and it tests no range (WWP-43, WWP-50) |
| W-04 | same (WWP-05, WWP-08, WWP-11) |
| W-05 | differs for the unit weapon (its aim point is WWP-50, not the nearest hardpoint); for a hardpoint the range adds the soft radius (WWP-20) and a squadron target is a random craft (WWP-19) |
| W-06, W-08 | same (WWP-22, WWP-31) |
| W-07, W-10, W-11, W-12 | same (WWP-23, WWP-24) |
| W-09 | same for the cone and the omitted-tag defaults (G-W4 resolved); `Fires_Forward` yes fires along the facing (WWP-53), which the code does not do |
| P-04 | type soft-radius computation shared by movement and combat (WWP-21), including craft without a movement layer; `Fire_Min_Range_Distance` is read (no M2 hardpoint authors it) |
| P-06 | same (project choice kept) |
| space-damage DG-21 | same for the unit weapon (WWP-54); for a hardpoint's projectile **unverified** (U-02) |
| DG-22, DG-24 | same (WWP-63, WWP-25, WWP-52) |
| DG-23 | travel adds the target's soft radius and its squadron container's, with the melee exception (WWP-27) |
| DG-25 | **wrong**: the MC80's weapon hardpoints author `Projectile_Damage` (20 on the lasers against the projectile's 15, 40 on the ion cannons against 20), and FoC uses it (WWP-26) |
| DG-30 | same; the first eligible object in sorted hostile-player and persistent projectile ray-collection order is WWP-66; DG-30a to DG-30g specify the tree walk, lifecycle and continuing flight after target detach |
| DG-32 | same, sharpened: only players the owner counts as enemies (WWP-66) |
| DG-33, DG-34, DG-36 to DG-38 | same (WWP-66, WWP-68, WWP-70) |
| MS-01 to MS-06 | same (WWP-62, WWP-64) |
| MS-07 | resolved: the height-adjusted position is the position plus `Ranged_Target_Z_Adjust` (WWP-72) |
| EN-05, EN-06 | same (WWP-46; hardpoints pay nothing) |
| space-targeting R-01 to R-15 | same (WWP-12 hands off to them) |
| space-fighters FT-07 | same (WWP-06) |
| space-fighters FO-04 | same (corrected by random craft selection by ordered hardpoints (legacy EAWR-711)): a hardpoint ordered at a squadron fires at a random craft each attempt (WWP-19); only a unit's own weapon takes the nearest (WWP-49) |
| space-abilities AB-21 | same (WWP-32, WWP-33) |

## Gap list against the remake

Ours: `src/sim/tactical/combat_fire.cpp` and `src/sim/tactical/combat_aim.cpp` (`UnitCombat::service_weapon`, `attempt`, `choose_aim`,
`scatter`, `lead`), `projectiles.cpp` (`launch_projectile`, `step_projectile`),
`src/units/unit_combat.cpp` and `unit_tables.cpp` (the tables).

| Rule | Ours | Verdict |
|---|---|---|
| WWP-01, WWP-03 | `service_weapon` | same |
| WWP-02 | only the destroyed gate (`weapon_up`); stun and disable come with ion energy drain and hardpoint damage (legacy EAWR-561) | differs (G-07) |
| WWP-04 | not loaded | missing (not in M2; G-08) |
| WWP-05 | restriction and a dead target drop; mode flags not loaded | same for M2 |
| WWP-06 | FT-07's `squadron_idle` | same |
| WWP-07 | aim picks the nearest again each attempt | same |
| WWP-08 | torpedoes fire at their direct (squadron) target only | same for M2 |
| WWP-09 | not modelled | missing (not in M2) |
| WWP-10, WWP-11 | `service_weapon`; no signals | same (the signals are an interface) |
| WWP-12 | `service_opportunity` | same |
| WWP-13, WWP-15 | `attempt` (live, fogged, restricted); stealth and hero clash not modelled | same for M2 |
| WWP-14 | not modelled | missing (not in M2) |
| WWP-16 | nearest targetable hardpoint from the shooter's height-adjusted position | **same** for space units and structures |
| WWP-17 | ordinary TURRET objects use authored mechanical aim/readiness and posed muzzle | applied to ordinary space objects; manual hardpoint visual pose remains missing |
| WWP-18 | projectile override comes with targeted ion-shot input and HUD (legacy EAWR-561) | same after the targeted ion-shot input and HUD work |
| WWP-19 | a squadron resolves to a random live craft per hardpoint attempt (`random_member`, a draw keyed by seed, frame, unit and slot), then the R-10 search on it | **same** (G-04 resolved) |
| WWP-20 | planar range plus the target radius | same for M2; minimum range missing (not in M2; G-08) |
| WWP-21 | shared type-footprint conversion (AV-05), including no-layer types and ordinary space squadron containers | same for M2; specialized formation-team geometry has no ordinary space consumer |
| WWP-22 | 50 % draw | same for M2 (no `Randomize_Between_Fire_Bones`; G-08) |
| WWP-23 to WWP-25 | `lead`, `pointable`, `scatter` | same |
| WWP-26 | the projectile's damage only | **differs (G-02)** |
| WWP-27 | range plus target radius, non-melee craft target's container radius, and height | same |
| WWP-28 | not loaded | missing (not in M2; G-08) |
| WWP-29 | `launch_projectile` | same |
| WWP-30 | ion shot with targeted ion-shot input and HUD | same after the targeted ion-shot input and HUD work |
| WWP-31 to WWP-33 | `attempt`, `scaled_weapon_delay`; fire rate with ion energy drain and hardpoint damage | same (after the ion energy drain and hardpoint damage work) |
| WWP-34, WWP-35 | no memory needed; no line of fire | same |
| WWP-41 | countdown | same |
| WWP-42 | dogfight scatter and burst timing | same after the dogfight scatter and burst timing work |
| WWP-43 | planar range to the aim point against `Targeting_Max_Attack_Distance` in `attempt` | **differs (G-05)**, the gate's inside is walk 1/2's |
| WWP-44 | drops only a dead or fogged target | same for M2 (no ownership changes) |
| WWP-45, WWP-46 | `service_weapon` | same |
| WWP-47 | the object weapon's muzzle bones (`Muzzle_Bone_Name`, default `MUZZLEA`, then `_00` and consecutive two-digit suffixes; U-06), next one in turn kept in the weapon state and advanced on every launch attempt, also one the cone or lead rejects (EWW-07); the unit's origin when the model has no `_00` bone | **same** for the first projectile type (the loader reads the first of several types); the bone of the current type's index is not modelled |
| WWP-48 | the type's `Targeting_Max_Attack_Distance` plus the target's soft radius and the height (`launch_projectile`); the projectile's own flight distance only when the type authors none | **same**; the fire range modifiers of walk 7 are not applied to travel (none is active in M2 skirmish) |
| WWP-49 | `resolve_target` from the next actual muzzle for object weapons, substituting the parent position only when the immutable parent has an ordinary combat binding, without an aimed hardpoint | conditional parent-targeting clause implemented; team target metadata alone retains the muzzle reference |
| WWP-50 | the object weapon: the hardpoint attached to its attack target (ordered, else the unit's best one the targeting attached, capital-combat WCC-18, EWW-09 with G-W3), else a keyed draw over `Target_Bones` at their bind positions, else the height-adjusted position; hardpoint weapons keep the nearest hardpoint (WWP-19, G-04) | **same**; bone positions are bind poses until animated attachment poses exist (legacy EAWR-1759); the attachment is chosen per shot instead of kept until destroyed (G-W3) |
| WWP-51, WWP-52 | `lead`; no unit weapon scatter | same |
| WWP-53 | cone as W-09; `Fires_Forward` yes flies at the aim point | same for M2; missing for `Fires_Forward` yes (G-08) |
| WWP-54, WWP-55 | new shots move from the next frame | same |
| WWP-60, WWP-61 | no delay, no acceleration | missing (not in M2; G-08) |
| WWP-62 to WWP-64 | `step_projectile`; no jamming or missile shield | same for M2 |
| WWP-65 | no ground in space | same |
| WWP-66 | hostile-player gate shared with ordinary targeting; living collision permission loaded and applied; a separate persistent owner tree supplies ray contacts in DG-30 order | agrees in query order; DG-30f records deterministic tick commit and equal-centre tie choices |
| WWP-67 | hand-off to `damage.cpp`; no blast area | same for M2 |
| WWP-68 to WWP-70 | `step_projectile` | same; lifetime expiry missing (not in M2; G-08) |
| WWP-72 | loaded for `SpaceUnit` and `SpaceStructure`; applied to launch fallback, nearest-hardpoint reference and homing | **same** for these classes |

Counts, by rule (62 simulation rules; WWP-40 is presentation): **same 47**, **differs 8**
(WWP-02, 26, 43, 47, 48, 50, 72, and the project choice of WWP-66), **missing 7**
(WWP-04, 09, 14, 17, 28, 60, 61; none authored by an M2 object).

### The gaps, by impact on the M2 battle

| Gap | Rules | What | Size |
|---|---|---|---|
| G-01 target soft-radius range adjustment (legacy EAWR-708) | WWP-20, WWP-21, WWP-27 (and R-11, A-07) | Resolved: shared scaled type geometry reaches hardpoint admission, aim selection, attack distance and partitioned projectile launches, including no-layer craft and their container radius. | resolved |
| G-02 MC80 hardpoint damage overrides (legacy EAWR-709) | WWP-26 | A hardpoint's `Projectile_Damage` replaces the projectile's: the MC80's lasers do 20 (not 15) and its ion cannons 40 (not 20). | S |
| G-03 fighter aim and muzzle bones (legacy EAWR-710) | WWP-47, WWP-48, WWP-49, WWP-50 | The unit's own weapon: aim at its attached hardpoint (ordered, else the nearest the targeting attached), else a random target bone, else the height-adjusted centre; fire from the muzzle bones in turn; travel `Targeting_Max_Attack_Distance`. | **Implemented** (legacy EAWR-710) for the unit's own weapon: keyed bone draw and height-adjusted centre when the target has no attached hardpoint; muzzle bones in turn; travel `Targeting_Max_Attack_Distance`. The aim at the nearest hardpoint stays: the debug build's targeting attaches it (G-W3), so the issue's "never the nearest hardpoint" did not hold (S-45 records no direct hull loss). The weapon's cone opens at the muzzle (WWP-53); its planar range test still measures from the unit (G-05). |
| G-04 random craft selection by ordered hardpoints (legacy EAWR-711) | WWP-19 | **Resolved**: a hardpoint ordered at a squadron fires at a random craft each attempt (a keyed draw), not the nearest; FO-04 is corrected. | resolved |
| G-05 fighter targeting-state fire gate (legacy EAWR-712) | WWP-43 | The unit's own weapon fires only in the "okay to attack" targeting state and tests no range; ours tests the planar attack distance. The state's inside (walk 1/2) decides when a fighter may fire; the retail dogfight outcome comparison shot log shows retail fighters never firing beyond 424 units. | M (after walk 1/2) |
| G-06 height-adjusted target positions (legacy EAWR-713) | WWP-16, WWP-19, WWP-64, WWP-72 | **Implemented** for `SpaceUnit` and `SpaceStructure`: launch fallbacks, nearest-hardpoint reference and missile steering use `Ranged_Target_Z_Adjust` (Nebulon-B +35; resource container +30). | S |
| G-07 ion weapons, energy drain and stun (legacy EAWR-561) | WWP-02 | A stunned, disabled or in-limbo parent's hardpoints freeze their recharge (ion energy drain and hardpoint damage (legacy EAWR-561)). | XS in ion weapons, energy drain and stun (legacy EAWR-561) |
| G-08 unused M2 weapon-rule coverage (legacy EAWR-714) | WWP-04, 09, 14, 17, 20 (minimum), 22 (random box), 28, 53 (`Fires_Forward` yes), 60, 61, 68 (lifetime) | Rules FoC reads that no M2 object authors. | M (nice-to-have) |

### XML tags this subsystem reads

Hardpoint: `Type`, `Fire_Projectile_Type`, `Fire_Range_Distance`, `Fire_Min_Range_Distance`,
`Fire_Cone_Width`, `Fire_Cone_Height`, `Fire_Bone_A`, `Fire_Bone_B`, `Randomize_Between_Fire_Bones`,
`Fire_Min_Recharge_Seconds`, `Fire_Max_Recharge_Seconds`, `Fire_Pulse_Count`,
`Fire_Pulse_Delay_Seconds`, `Fire_Inaccuracy_Distance`, `Fire_Category_Restrictions`, `Damage_Type`,
`Projectile_Damage`, `Allow_Opportunity_Fire_When_Idle`, `Allow_Opportunity_Fire_When_Targeting`,
`Requires_Manual_Target_Assignment`, `Is_Turret`, `Projectile_Appearance_Delay_Frames`, the per-mode
fire flags. Unit: `Projectile_Types`, `Projectile_Fire_Pulse_Count`, `Projectile_Fire_Pulse_Delay_Seconds`,
`Projectile_Fire_Recharge_Seconds`, `Projectile_Damage`, `Damage_Type`, `Targeting_Max_Attack_Distance`,
`Targeting_Fire_Inaccuracy`, `Targeting_Fire_Inaccuracy_Fixed_Radius`, `Fires_Forward`,
`Turret_Rotate_Extent_Degrees`, `Turret_Elevate_Extent_Degrees`, `Target_Bones`,
`Ranged_Target_Z_Adjust`, `Custom_Soft_Footprint_Radius`, `Space_Obstacle_Radius`, `Scale_Factor`,
`Collidable_By_Projectile_Living`, `Collision_Box_Modifier`, `Can_Fighter_Fire_When_Idle`.
Projectile: `Projectile_Category`, `Max_Speed`, `Max_Rate_Of_Turn`, `Projectile_Damage`,
`Damage_Type`, `Projectile_Max_Flight_Distance`, `Projectile_Max_Lifetime`,
`Projectile_Max_Scan_Range`, `Projectile_Acceleration_Per_Frame`, `Projectile_Does_Shield_Damage`,
`Projectile_Does_Energy_Damage`, `Projectile_Does_Hitpoint_Damage`, `Projectile_Energy_Per_Shot`,
`Projectile_Blast_Area_Damage`, `Projectile_Blast_Area_Range`, `Projectile_Ion_Stun_*`.

In [statuses.json](../../tag-coverage/statuses.json) (M2 XML tag coverage audit), these rows are `todo` and belong to
this walk: `HardPoint/Projectile_Damage` (G-02), `SpaceUnit/Ranged_Target_Z_Adjust` and
`SpaceStructure/Ranged_Target_Z_Adjust` (G-06), `Projectile/Projectile_Max_Scan_Range` (read: 0 on
every M2 missile, so a lost missile never retargets; MS-05), `SpaceUnit/Collidable_By_Projectile_Living`,
and the same tag on `SpaceStructure`, `StarBase`, `SpaceBuildable` and `SecondaryStructure` (FoC
reads it, WWP-66; every M2 unit sets yes, so loading it changes nothing in M2; G-08). `Projectile/Projectile_Width`
and `_Length` are drawing data (presentation). `HardPoint/Allows_Special_Weapon_Use` and the
`Energy_Beam_*` constants belong to special weapons (not M2).

## Interfaces to other walks

- **Targeting (walk 2, walk 1).** In: the unit's attack target, whether an order made it, its aimed
  hardpoint; the targeting state (WWP-43); the opportunity scan (WWP-12). Out: the signals of
  WWP-10 and WWP-12.
- **Damage routing (walk 2).** Out: the unit hit, the damage, its type, the contact, the mesh met
  and the aimed hardpoint (WWP-67), in the projectile service's order.
- **Abilities (walk 7).** In: the fire range modifiers (WWP-20, WWP-48), the weapon delay and fire
  rate factors (WWP-32, WWP-33), the weapons-enabled mode flag (WWP-43), projectile overrides
  (WWP-18, WWP-45), the blast ability (WWP-14).
- **Movement (walk 4).** In: positions and heights (per-unit flight heights), the soft radius it already computes
  (AV-05).

## Symptoms

- **retail dogfight outcome comparison (TIEs fire 64 shots where retail's fire 163).** The fire itself matches (WWP-41 to WWP-55
  with dogfight scatter and burst timing), except that ours holds a fighter's fire outside its attack distance where FoC
  asks the targeting state (G-05), and ours aimed at the nearest hardpoint exactly where FoC attaches one (G-03). Fire-to-target distance in the retail shot log never exceeds 424 units. The count gap is
  the chase geometry (walk 1) plus G-05.
- **concussion-missile flight comparison (legacy EAWR-660) (missile comparison).** The flight rules match (WWP-62, WWP-64, WWP-66 to WWP-70); the
  steering point misses the Nebulon-B's height adjustment (G-06). The rest of concussion-missile flight comparison (legacy EAWR-660) is presentation.
- **AI bombing-run departure (legacy EAWR-633) (bombers hitting the adjacent station, not the corvette).** FoC's projectiles hit any enemy
  unit in their path (WWP-66); nothing makes a torpedo skip the station. Whether the station lies in
  the path depends on the approach (walk 1) and the heights (per-unit flight heights); G-01 lets FoC's bombers fire
  from further out. No weapon rule differs here.
- **per-unit flight heights (are range and arcs planar?).** See "Planar or 3-D": hardpoint range planar, travel
  compensated for height, cones and the opportunity aim distance 3-D.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-01 | Most recent service-registration first object traversal; each object runs periodic behaviours in attachment order, then its special/script/hardpoint services. Ordinary attachment follows general Behavior then SpaceBehavior XML lists. This is per-object order, not global subsystem phases (WFO-12/15/17/24). | EUS-04 |
| U-04 | The fallback uses the type model returned for the active mode without an instance/faction override, taking the greater absolute X/Y box extent. A positive custom soft radius overrides it; otherwise a positive space-obstacle radius overrides it, then Scale_Factor applies. With no model the result is zero before those overrides. The object normally delegates to this type radius; the team/locomotor special case uses hard coordinate radius. | EUS-18 |
| U-05 | Hostility reads the stored player relationship. The neutral faction establishes neutral relationships, so stock Neutral props and asteroids are not enemies merely because they are objects. Non-neutral pirate factions are distinct and may be hostile. Other targetability/opportunity-fire gates still apply (WNO-05 and WHZ-51). | EUS-28 |
| U-06 | Object weapons search the authored muzzle prefix followed by _00, then consecutive two-digit suffixes starting at zero. The first absent bone ends enumeration; an absent _00 leaves the firing list empty and warns. Next firing index becomes zero once a bone is appended. Related flash/light subobjects are resolved per discovered bone. | EUS-11 |

## Unverified

| ID | What | What would settle it |
|---|---|---|
| U-02 | Whether a hardpoint's projectile moves in the frame it is fired (its muzzle delay is left at the data default). | A retail per-frame projectile position trace of one hardpoint shot (S-15 has shot and hit ticks only). |
| U-03 | What sets the "okay to attack" targeting state (two targeting checks not read here). **Sweep:** Still unverified: Attack wrappers assign targets, but the two virtual okay-to-attack gates are not resolved by those assignments. Their setters and policy remain unverified. | Walk 1/2's read of the targeting service. Retained sweep boundary: EUS-37. |

## Space skirmish special weapons

Walk date: 2026-10-07; remake audited at `5b3fa1d83269fa64ba516fca9ac3d81729d44978`.
Tracking: space skirmish special weapons (legacy EAWR-1917).
WSW rules cover the Rebel planetary ion cannon, Empire planetary hypervelocity gun and
Underworld station plasma cannon. They exclude the Death Star, campaign ownership,
land weapons and the Y-wing's `ION_CANNON_SHOT` ability.

Fresh read-only **debug build** receipts use opaque WSW-E identifiers; their mapping,
queries and original implementation details remain in ignored research output. WSW-D
identifies parsed effective XML or stock Lua data. **Sourced** means the stated branch
was read, or explicitly reuses an existing sourced damage/manual-cannon interface;
it does not claim a retail capture. **Unverified** identifies a remaining boundary.
No new retail capture was made for this walk.

### Active definitions and ownership

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-01 | Standalone space initialization visits playable players, reads each faction's `Standalone_Space_Maps_Special_Weapon_A` and `_B`, creates each nonempty type with that player's ownership and registers it by `Special_Weapon_Index`. This runs from both stock skirmish ownership-initialization routes. Weapon existence is separate from purchasing permission to fire. | **Sourced**, debug build WSW-E01 and its callers; WSW-D01 |
| WSW-02 | Search map objects carrying `DUMMY_SPECIAL_WEAPON_SOURCE`; use the first returned object's position and facing for every created planetary weapon. With no source object, use zero position and zero facing. This is a map source, not the player's station or start marker. Stock source objects include planet backdrops and `Space_Special_Weapon_Source_Marker`; their XML authors `Layer_Z_Adjust` -2500. The creation route copies the resulting object position rather than applying a new fixed altitude. | **Sourced**, debug build WSW-E01; WSW-D01 |
| WSW-03 | Rebel maps to active `Ground_Ion_Cannon` (weapon index 0, target-action index 1); Empire maps to active `Ground_Empire_Hypervelocity_Gun` (weapon index 1, target-action index 0). Both have `SpaceBehavior` `SPECIAL_WEAPON` and `Is_Special_Weapon_In_Space` Yes. The `Space_Mode_Ground_*` definitions are inside XML comments and do not exist in the parsed active definitions. Their old 15-second recharge values must not supply skirmish inputs. | **Sourced**, parsed effective XML WSW-D01 |
| WSW-04 | Underworld's two standalone mappings are empty. Its plasma weapon is the manually targeted `HP_Underworld_L3_Station_Main_Cannon`, present on skirmish station levels 3, 4 and 5. It fires from the station's posed muzzle through ordinary hardpoint service. It is not a third planetary object or the index-2 magnepulse weapon. | **Sourced**, WSW-D01/03; debug build WSW-E10/15/17; WAD-39/40 |
| WSW-05 | Registration is per owner and weapon index. A second registered object of the same type at that index increments the existing weapon's instance count and destroys the duplicate. The ordinary skirmish faction mapping creates one source per authored slot per playable player. The influence of a count greater than one on recharge is outside that stock case and remains unverified. | **Sourced** for registration, debug build WSW-E19; **unverified** for multiple-instance recharge |

### Purchases, enablement and consumption

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-06 | Station levels 3 through 5 offer their faction's use upgrade in `Tactical_Upgrades`. All three upgrades author multiplayer cost 2800, `Build_Limit_Current_Per_Player` 1 and `Is_Skirmish_Tactical_Super_Weapon` true. Rebel and Underworld build time is 40 seconds; Empire is 45 seconds. These are production inputs, subject to the production walk's ordinary modifiers and admission. | **Sourced**, WSW-D02/04; WPR-22/33 |
| WSW-07 | Each use upgrade has a `Space_Automatic` battlefield modifier with `Enable_Special_Weapons` Yes, `Apply_To_All_Allies` No and `Stacking_Category` 0. Multiplayer fire enablement combines the player's special-weapon modifiers and the required-hardpoint flag. A destroyed conflict planet disables it; outside multiplayer the ordinary non-destroyed-planet path is enabled. Readiness alone does not authorize a skirmish shot. | **Sourced**, debug build WSW-E09; WSW-D02 |
| WSW-08 | Rebel and Empire use upgrades author `DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED`. Underworld authors only `DUMMY_UPGRADE`, so its completed plasma upgrade remains an enabling source after fire and its current-player limit continues to block a second purchase. Do not impose consume-on-fire on all three factions by upgrade name or tooltip. | **Sourced**, WSW-D02; debug build WSW-E03/15/16 |
| WSW-09 | The player fire event checks the firing player's enabled flag before dispatching a planetary shot or assigning the station's manual target. It then destroys all objects owned by the weapon owner that carry `DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED`. The cleanup is after dispatch, before projectile impact, and does not inspect the planetary fire helper's success result. A malformed event can therefore consume permission without producing a shot. Ordinary UI admission is a separate earlier gate. | **Sourced**, debug build WSW-E03 |
| WSW-10 | The object Lua route performs the same marker-based cleanup for the supplied player after a successful planetary fire or after attempting a valid manual assignment. The TaskForce Lua route cleans up the selected weapon owner's marked objects only after successful fire. These entry routes have distinct rejection/return contracts; consumption is not an impact or last-pulse callback. | **Sourced**, debug build WSW-E15/16 |
| WSW-11 | Production's current-player limit counts queued and existing objects. Consuming a Rebel/Empire use object releases its current count and removes that object's enabling effect; it does not erase lifetime production history or destroy the persistent planetary source. Re-purchase and the source's own recharge are independent conditions for the next shot. | **Sourced**, WPR-22/33/51 and WSW-01/08/09; debug build WSW-E03 |
| WSW-12 | Match denial belongs to purchase admission for types flagged `Is_Skirmish_Tactical_Super_Weapon`, as WSS-30 specifies. At the audited remake revision, `src/skirmish/inputs.cpp` and `src/skirmish/economy.cpp` already load and apply that policy to human/AI listings and purchases. It does not imply deleting map source objects or disabling unrelated ship abilities. | **Sourced**, WSS-30; current remake consumers |

### Target selection and entry gates

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-13 | A planetary target needs a nonnull source/type and target/type, must be alive and must not be allied to the source owner. Its category mask must intersect `Special_Weapon_Valid_Targets`. The stock mask is Transport, Corvette, Frigate, Capital or Super; Fighter/Bomber are absent. This helper does not require a positive enemy relationship beyond rejecting allies. | **Sourced**, debug build WSW-E02; WSW-D01 |
| WSW-14 | The planetary target validator checks no fog, distance, minimum range, source-to-target firing cone or destroyable-hardpoint requirement. The UI's special-target branch reuses it and does not add a fog/range test there. Shared projectile creation still has its own aim/cone/stealth checks. Whether the screen picker supplies a hidden remembered target is an upstream visibility boundary, not evidence for adding a fog check to this validator. | **Sourced** for the read validators, debug build WSW-E02/08/11; **unverified** for hidden-object screen picking |
| WSW-15 | `Special_Weapon_Index` selects the stored weapon; `Special_Weapon_Target_Action_Index` selects its target action, cursor family and indexed faction HUD speech. The numbers are separate: Rebel uses action 1, Empire action 0. Underworld's manual targeting uses action 0 directly. Action 2 is not the plasma cannon. | **Sourced**, debug build WSW-E04/06/11/12; WSW-D01 |
| WSW-16 | Planetary fire rechecks full readiness, target validity and the launch wrapper before starting recharge/burst state. The player event's earlier readiness check admits at 0.99, but this helper still requires 1.0. Object/TaskForce Lua routes also require full readiness. A new source starts fully ready, with no pending burst or targeting state; purchasing permission is still required. Do not model 99-percent readiness as a successful early shot. | **Sourced**, debug build WSW-E03/04/15/16/22 |
| WSW-17 | The manual station target requires a nonnull enemy, no `Fire_Category_Restrictions` match, pointability, visibility to the station owner and inclusive planar range from its fire midpoint: minimum range through weapon range plus the target's soft radius. Stock plasma excludes Fighter/Bomber/Transport and authors range 20000. This is different from the planetary category-only validator. | **Sourced**, fresh debug build WSW-E17; WSW-D03; WAD-39 |
| WSW-18 | A manual assignment is refused while that gun already retains a target; accepted assignment stores target, requesting player and current logical frame. The selected hardpoint is the first authored hardpoint requiring manual targeting. Turret yaw admission and desired facing transform the target into attachment coordinates, add 90 degrees to yaw, then wrap before applying the authored extent. Desired facing also clamps pitch; gun service applies its existing pitch and two yaw steps before the posed muzzle cone. Successful clearing and the strict greater-than-300-frame weapon-state timeout remain WAD-39 and MC-02/03 interfaces. | **Sourced**, debug build WSW-E17/20/25/26; WAD-39; MC-02/03 |

### Burst, flight and damage

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-19 | Initial planetary fire launches once immediately, then stores `max(0, Projectile_Fire_Pulse_Count - 1)` remaining pulses. Recharge starts at that initial firing frame. Pulse delay and recharge convert seconds to logical frames by nearest whole frame (`seconds * logical FPS + 0.5`, then truncate). Rebel authors three pulses with 0.5-second separation and 30-second recharge; Empire authors ten pulses with 0.1-second separation and 45-second recharge. | **Sourced**, debug build WSW-E04/05; WSW-D01 |
| WSW-20 | Periodic special-weapon service checks the pending pulse deadline before its readiness/enablement notification. If a stored target still resolves, it attempts one pulse per service; success decrements the count and advances the deadline by its interval, while a wrapper failure ends the burst. The final pulse clears the remembered target. Service does not recheck battlefield enablement or target validity for these continuation pulses. Consuming the enabling purchase after initial fire therefore does not cancel the remaining burst. | **Sourced**, debug build WSW-E05/06 |
| WSW-21 | A missing remembered target produces no continuation launch in the read service. Exact retained-counter cleanup after target removal and visible outcomes when a target dies between pulses remain unverified; do not invent retargeting to another enemy. | **Sourced** for no launch, debug build WSW-E06; **unverified** for retained-state cleanup and retail outcome |
| WSW-22 | Planetary fire uses ordinary object projectile construction with the target object and no authored hardpoint/muzzle selector. It starts from the source's height-adjusted object position. The shared aim route chooses a target location, applies interception and authored inaccuracy, and checks projectile-facing validity, including the source's turret extents. Successful construction creates a travelling projectile with source owner/identity. The special launch wrapper ignores the construction return value, plays its fire cue and reports success even when that shared construction rejects a shot. Thus advancing burst/recharge does not prove a projectile exists. | **Sourced**, debug build WSW-E08/21; ordinary WWP-45–50 interfaces |
| WSW-23 | Projectile damage and damage type fall back to the selected projectile type when the firing properties have no positive damage/nondefault type override. Flight allowance similarly falls back to `Projectile_Max_Flight_Distance` when the source supplies no positive attack distance, then adds target soft radius and the absolute launch-to-target height difference. The planetary wrapper supplies no firing bone, so this shared route supplies no positive muzzle appearance delay. | **Sourced**, debug build WSW-E08; WSW-D01/03 |
| WSW-24 | `Proj_Ion_Cannon_Planetary` authors speed 400, default projectile category, damage 15000 and flight allowance 150000; shield and energy damage Yes, hitpoint damage No. Engine disable after drained power is 10 seconds. Its detonation ion-stun inputs are duration 30 seconds, speed reduction 0.999, shot-rate reduction 0.8 and nonstacking duration. **The two timers are independent (Focumentation cross-check, debug build).** The ion stun applies its speed and shot-rate modifiers for the stun duration only (30 s here), and the engine disable is a separate locomotor timer set from the engine-disable duration (10 s here); neither reads the other. After 10 s the engines come back, but the 0.999 speed reduction holds until 30 s, so a stunnable victim barely moves for the full stun. The wiki's "must match" is an authoring convention, not a rule, and retail data does not follow it: the Krayt megaweapon authors 3 s and 3 s, the Krayt bombardment 10 s disable against 7 s stun, the planetary cannon 10 s against 30 s, and the small ship ion cannon only a 20 s disable with no stun at all. Both timers are modelled (EN-08, EN-09, IS-01 to IS-06); no change. Shield, energy and stun routing use the existing damage/ion interfaces, including recipient eligibility and armor; these numbers are not a promise that every recipient loses exactly 15000 shield points. No positive blast-area damage/range is authored here. | **Sourced**, WSW-D03; EN-07–09 and the ion-damage interfaces in [space-damage](../space-damage.md) |
| WSW-25 | `Proj_Hypervelocity_Gun` authors speed 70, default category, damage 300 and flight allowance 50000; shield/energy damage No, hitpoint damage Yes. It requests random hardpoint damage routing. Each of the ten projectiles is routed independently through ordinary collision and damage; do not combine them into one guaranteed hull hit of 3000. No positive blast-area damage/range or ion stun is authored here. | **Sourced**, WSW-D03; WAD-27 and WWP-66/67 |
| WSW-26 | Plasma retains the ordinary one-pulse manual hardpoint: weapon recharge 1 second, appearance delay 15 frames and player cooldown 120 seconds. Its projectile authors speed 70, direct damage 6000, blast damage 1000 and radius 300; shield/energy damage No, hitpoint damage Yes. Flight allowance is supplied by the hardpoint firing path, not blindly limited to the projectile type's authored 1000. Turret service, posed muzzle/cone, delay, collision and area routing are the existing WAD-32/39/40 and MC-06/07 interfaces. | **Sourced**, WSW-D03; WAD-32/39/40; MC-06/07 |
| WSW-27 | Special-weapon service readiness is elapsed frames since initial fire divided by its stored recharge interval, clamped to zero through one; it is one once the ready frame is reached. Underworld's HUD instead uses the local player's manual-fire clock: one with no positive cooldown or once elapsed reaches that cooldown, otherwise elapsed/cooldown. Its gun's ordinary readiness is a separate per-gun recharge/burst measure. | **Sourced**, debug build WSW-E07/10/18; MC-04/05 |

### UI, sound and enemy observations

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-28 | In standalone space, the main tactical button uses the local faction's mapped planetary weapon, or for Underworld the first allied station's manual gun. With no source/station, active setup, or battlefield special weapons disabled, it clears the bound source and disables the button. A planetary source supplies its own readiness; a missing, destroyed or disabled manual gun supplies zero, otherwise the manual player clock supplies readiness. Less than one disables the button. The icon comes from the local faction's slot in `b_special_weapon`'s `Icon_Alternate_Texture_Name`: Rebel slot zero (`i_button_command_bar_ion_cannon.tga`), Empire slot one (`i_button_command_bar_hyper_gun.tga`), Underworld slot two (`i_button_command_bar_station_turret.tga`). All three use the same HUD source while ready, targeting and recharging; weapon `Icon_Name` and use-upgrade `Icon_Name` remain separate build/purchase art. A missing alternate supplies no icon. | **Sourced**, debug build WSW-E10; active weapon/upgrade XML WSW-D01/02 and effective `b_special_weapon` command-bar XML |
| WSW-29 | The button's readiness display receives the positive readiness fraction, or one when readiness is zero; the source parameter and disabled flag are updated separately. Resource names `b_special_weapon` and `b_special_weapon2` exist. The exact visible zero-ready fill, second-button use in stock space, hover text and pixel appearance require captures; no second usable plasma slot follows from these names. | **Sourced** for inputs/resource names, debug build WSW-E10 and string discovery; **unverified** for rendering and second-button use |
| WSW-30 | Planetary targeting activation toggles its own active mode off if already active. Otherwise it refuses activation if another owned special weapon already targets in space, with local negative feedback; success sets its targeting flag. This activation helper does not itself enforce purchase/readiness; the button and firing entry gates own those conditions. | **Sourced**, debug build WSW-E13 |
| WSW-31 | Hovering a valid planetary/manual target produces that weapon's indexed target action; invalid targets produce the invalid special-weapon action. A valid click queues the player fire event and clears the planetary or manual targeting flag. Underworld also plays command-bar attack acknowledgement. The invalid-target click uses the targeted-action feedback route and queues no shot in the read branch. | **Sourced**, debug build WSW-E11/12 |
| WSW-32 | Ending special-weapon targeting clears local planetary targeting flags and, for Underworld in space, the first allied station's manual-targeting flag. It creates no shot and consumes no purchase. Mouse handling and beginning another object-targeted ability call this cancellation route. Exact button/right-click/Escape gesture mapping remains a capture/handler boundary. | **Sourced** for cancellation and callers, debug build WSW-E14; **unverified** for each gesture |
| WSW-33 | Initial planetary fire plays the source type's positional `SFXEvent_Fire`, then selects indexed firing HUD speech from the local faction. A locally owned/allied weapon uses `SFXEvent_HUD_Special_Weapon_Firing`; an enemy uses `SFXEvent_HUD_Enemy_Special_Weapon_Firing`. Missing/out-of-range/null authored entries produce no substituted warning. The source-fire wrapper runs for each continuation pulse; indexed firing speech is selected on initial fire. | **Sourced**, debug build WSW-E04/06/08; BA-56 |
| WSW-34 | Once the stored ready frame is reached, readiness notification waits until battlefield special weapons are enabled. Local/allied weapons prefer the source type's `SFXEvent_Special_Weapon_Ready`, falling back to the indexed local-faction ready entry; enemy weapons use the indexed enemy-ready entry. It then clears the notification deadline and emits the weapon-online signal once. A completed use upgrade's build-complete cue is a separate production event; do not synthesize an additional charging/ready cue without its triggering state. | **Sourced**, debug build WSW-E06; WSW-D02; BA-56/60 |
| WSW-35 | Both planetary projectile types and the plasma projectile have `HIDE_WHEN_FOGGED`; their authored projectile models, hit particles and detonation sounds pass to the usual presentation consumers. An enemy's HUD speech follows relationship and authored entries, not visibility of the invisible firing source. Exact enemy-visible trail timing, fog reveal, flash, audible positional attenuation and charging speech trigger remain unverified for these shots. | **Sourced** for authored inputs and speech selection, WSW-D03 and WSW-E04/06; **unverified** for rendered/audio observation |

### Stock AI and remake integration

| ID | Rule | Evidence / status |
|---|---|---|
| WSW-36 | `PurchaseSpaceUpgradesGeneric` offers all three faction use upgrades among ordinary upgrade alternatives and produces up to two selected upgrades. Its sourced stock goal requires game age greater than 120 seconds and friendly force greater than half enemy force; in skirmish it also requires resource structures, no refinery saving/replacement-station need and fewer than eight unit-space slots available. After production the Lua plan waits for a credits threshold based on tech level, at most 120 seconds. These are generic purchase heuristics, not a guaranteed superweapon-first policy. | **Sourced**, stock Lua/XML WSW-D05/06; ordinary tactical-AI production interface |
| WSW-37 | Rebel `GroundToSpaceDisable` produces/attaches `Ground_Ion_Cannon`, fires once through `TaskForce.Fire_Special_Weapon(type_name, AITarget)` when the target is valid, waits five seconds and exits. Empire `GroundToSpaceDamage` similarly uses `Ground_Empire_Hypervelocity_Gun`, fires, then waits while the target is valid; its weapon-online handler retries through `GameObject.Fire_Special_Weapon(AITarget, PlayerObject)`. Neither script searches the whole battlefield in those calls: the goal-selected target is supplied by the AI host. | **Sourced**, stock Lua WSW-D05; debug build entry routes WSW-E15/16 |
| WSW-38 | The active basic-offensive goal-function file maps `Ground_To_Space_Damage` to `Needs_Ion_Shot` and `Ground_To_Space_Disable` to `Needs_Hypervelocity_Shot`. Preserve the authored mapping rather than swapping it to match the names. Both perceptions score zero on Easy and reject Corvette force; both favor non-starbases by a factor of 20. The ion score also includes shield and engine-online terms. Perception data and target-category admission remain separate gates. | **Sourced**, parsed effective XML WSW-D06 |
| WSW-39 | Space free-store service retains the station, attacks its dangerous enemy, and asks for a reachable `Needs_Hypervelocity_Shot` enemy target with the station reference and distance argument 1500, variance 0.5. If valid, it invokes the station object binding with that target and AI player. Its player service interval is 20 seconds. This station call is shared by all factions; only a station with a manual gun can fire it, which stock Underworld levels 3–5 have. | **Sourced**, stock Lua WSW-D05; FH-21/27; debug build WSW-E15 |
| WSW-40 | Object `Fire_Special_Weapon(target, player)` accepts an object target or an AI-target wrapper resolving to one and a player. A station without a manual gun warns and returns nil. A station with one checks the gun's ordinary readiness, the supplied player's enabling flag and manual target validity, then assigns the target and returns its own object wrapper. The read Lua station branch does not check the HUD's separate 120-second player cooldown. That distinction must not be lost by assuming every stock entry uses HUD admission. | **Sourced**, debug build WSW-E15/17/18 |
| WSW-41 | TaskForce `Fire_Special_Weapon(type_name, target)` resolves a space special-weapon type, checks player enablement/category and searches attached members for that original type with full readiness. Unavailable/disabled/category mismatch returns false; successful activation and fire returns the weapon object wrapper. Missing type or malformed arguments are script errors. This is a distinct binding from the station object call and is needed by both planetary AI plans. | **Sourced**, debug build WSW-E16 |
| WSW-42 | At the audited remake revision the station object binding still answers nil under FH-27, nonstation calls fall through to the missing API and no TaskForce special-fire method is registered. Planetary source creation, enabling purchase effects, consume-on-fire, the special HUD and these Lua consumers are missing. Existing manual targeting, partitioned gun/turret service, player clocks, snapshots and replay command hooks already implement MC-01–08; reuse them. Their current command gate enforces the player cooldown for every issuer, so it is not yet the stock Lua admission of WSW-40. | **Sourced**, current `tactical_ai_bindings.cpp`, `plan_bindings.cpp`, `session_step_commands.cpp`, `combat_aim.cpp`, `combat_fire.cpp`; [manual cannon](../manual-cannon.md) |

### Remaining evidence and tag boundaries

| ID | Boundary | Required evidence / status |
|---|---|---|
| WSW-43 | Retail shots, button/targeting cancellation gestures, hidden target picking, enemy presentation and Underworld repeated fire must be observed before claiming full visual/timing fidelity. Also record rejected/dead targets between click and admission, purchase during recharge, target death during a burst, hardpoint loss and station level replacement. | **Unverified**; coordinator-queued retail/debug captures, one faction per controlled battle plus an enemy view |
| WSW-44 | Multiple players sharing stations, manual HUD-versus-Lua cooldown admission, modifier stacking/required-hardpoint changes and source-marker ordering across several candidates need covering tests or additional evidence for those cases. The unauthored turret-extent defaults are resolved by WSW-45. A fixed one-station/two-player fixture does not establish these cases. | **Unverified** beyond the explicitly sourced gates above; targeted debug traces and installed-game cases |
| WSW-45 | The object-type defaults for unauthored `Turret_Rotate_Extent_Degrees` and `Turret_Elevate_Extent_Degrees` are 360 and 180 degrees. These apply to the active planetary source object firing path; they do not add a range or cone check to the special target validator. | **Sourced**: debug build, type construction and WSW-14/21 |
| WSW-46 | A battlefield unlock applies a true enable modifier identified by the ability and its owning object; removing or terminating that effect removes modifiers from the same source. `Apply_To_All_Allies` selects allied players when true, otherwise the owner alone. An effect accepts a null target or its own object. The required-hardpoint gate in WSW-07 remains independent of the enable list. | **Sourced**: debug build, effect application, removal, termination and target-player selection |
| WSW-47 | AI proposals can include existing free-store objects whose original type resolves to a space special weapon when the plan permits special weapons. The proposal pool requires full source readiness and battlefield enablement. Attachment uses that live object identity; the type is not a new purchased combat ship. Plans that disallow special weapons (HA-01) give that type no instantiation weight as an extra, team-less attachment; a team that lists the type admits it either way. `Collect_All_Free_Units` checks category, duplicate membership, movement lock and reservation; it does not consult purchase enablement or `DenySpecialWeaponAttach`. | **Sourced**: debug build, free-store proposal initialization, proposal enumeration, instantiation admission and collection callbacks |
| WSW-48 | The required-hardpoint permission defaults to true. A hardpoint with `Allows_Special_Weapon_Use` writes its enabled state to every allied player on lifecycle notification; the latest notification wins, independently of other stations. Station replacement writes the transferred disabled state, and completing repair restores permission. This gate restricts new dispatch and ready notification; an active planetary burst continues. | **Sourced**: debug build WSW-E23/24, default flag, hardpoint lifecycle and transferred state |

Runtime inputs for this section are the faction standalone mappings, source behavior and map transform,
`Special_Weapon_Index`, `Special_Weapon_Target_Action_Index`, `Special_Weapon_Valid_Targets`,
`Is_Special_Weapon_In_Space`, production costs/time/limits/queue and the superweapon-policy flag,
`DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED`, battlefield modifier activation/enable/allies/stacking,
projectile type/pulse/delay/recharge, the manual hardpoint inputs in WAD-39/40,
ordinary projectile damage/energy/shield/ion/random-hardpoint/area inputs, and presentation inputs
for icons, positional fire, indexed HUD speech, model/particles and fog hiding.
Parsing any of these without its runtime consumer does not make its registry row applied.

The active ground structures' land model, construction, required-land structures, sabotage,
healing and land reveal/damage-alternate fields remain land/campaign interfaces; they do not
authorize a rendered ground building in standalone space. Their auto-resolve fields belong
to campaign resolution. Index-2 magnepulse, tactical planet destruction and Death Star/land
power-up stages remain outside this space-skirmish section. `Energy_Beam_*` applies to a
separate beam family; none of these three selected projectile definitions uses that family.
The obsolete commented `Space_Mode_Ground_*` records supply no runtime input.

Implementation follows the project's partitioned immutable-input/staged-output phases and
ordered commits. Purchase/consumption and command entry must preserve deterministic player,
source, target and pulse identities across worker counts. These are remake policies,
not claims about the original game's scheduling.
