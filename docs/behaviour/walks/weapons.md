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
  points within `Fire_Cone_Width` of the target (space-weapon-fire W-07; no M2 turret).
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
- **WWP-21** (debug build, EWW-05) A unit's soft radius is its type's `Custom_Soft_Footprint_Radius`
  when positive, else its `Space_Obstacle_Radius` when positive, else the larger planar half extent
  of its model's box; times `Scale_Factor`. A squadron's container in a formation uses its hard
  radius instead (the length of its two planar hard extents). The M2 stations author 300 (Rebel)
  and 250 (Empire); the ships take their model's half length.
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
  melee), plus the height between the scattered point and the muzzle.
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
  bone of the current type's index.
- **WWP-48** (debug build, EWW-09) The shot's travel limit is the type's `Targeting_Max_Attack_Distance`
  times one plus the fire range modifiers; its damage is the type's own `Projectile_Damage` when
  positive, else the projectile's; its damage type is the type's `Damage_Type` unless that is the
  default, else the projectile's. This is the unit's own `Projectile_Types` weapon only:
  the unit tag does not replace the damage type of its hardpoint weapons (DG-12). The M2
  ships do not author the unit tag; their hardpoints supply their own damage types.
- **WWP-49** (debug build, EWW-09) A squadron's team container as target stands for its craft
  nearest the muzzle (or nearest the shooter's own squadron centre when the shooter flies in one);
  that craft is fired at with no aimed hardpoint (space-fighters FO-04). A target hardpoint that is
  gone or destroyed is dropped.
- **WWP-50** (debug build, EWW-09) The aim point: the unit's ordered hardpoint on its attack target
  when it has one; else one of the target type's `Target_Bones`, picked by a synchronized draw, at
  its animated position; else the target's height-adjusted position (WWP-72). There is no
  nearest-hardpoint choice and no range test.
- **WWP-51** (debug build, EWW-09; space-weapon-fire W-10) The aim point is led from the muzzle at
  the projectile's `Max_Speed`.
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
| P-04 | differs: the soft radius is not zero (WWP-21); `Fire_Min_Range_Distance` is read (no M2 hardpoint authors it) |
| P-06 | same (project choice kept) |
| space-damage DG-21 | same for the unit weapon (WWP-54); for a hardpoint's projectile **unverified** (U-02) |
| DG-22, DG-24 | same (WWP-63, WWP-25, WWP-52) |
| DG-23 | differs: the travel adds the target's soft radius and a squadron container's (WWP-27) |
| DG-25 | **wrong**: the MC80's weapon hardpoints author `Projectile_Damage` (20 on the lasers against the projectile's 15, 40 on the ion cannons against 20), and FoC uses it (WWP-26) |
| DG-30 | same; the first eligible object in sorted hostile-player and persistent projectile ray-collection order is WWP-66; DG-30a to DG-30g specify the tree walk, lifecycle and continuing flight after target detach |
| DG-32 | same, sharpened: only players the owner counts as enemies (WWP-66) |
| DG-33, DG-34, DG-36 to DG-38 | same (WWP-66, WWP-68, WWP-70) |
| MS-01 to MS-06 | same (WWP-62, WWP-64) |
| MS-07 | resolved: the height-adjusted position is the position plus `Ranged_Target_Z_Adjust` (WWP-72) |
| EN-05, EN-06 | same (WWP-46; hardpoints pay nothing) |
| space-targeting R-01 to R-15 | same (WWP-12 hands off to them) |
| space-fighters FT-07 | same (WWP-06) |
| space-fighters FO-04 | differs for hardpoints: a hardpoint ordered at a squadron fires at a random craft each attempt (WWP-19); only a unit's own weapon takes the nearest (WWP-49) |
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
| WWP-16 | nearest targetable hardpoint from the shooter's position | same (the shooter's height adjustment differs by at most 35, G-06) |
| WWP-17 | turrets treated as fixed (P-04) | missing (not in M2; G-08) |
| WWP-18 | projectile override comes with targeted ion-shot input and HUD (legacy EAWR-561) | same after the targeted ion-shot input and HUD work |
| WWP-19 | a squadron resolves to its nearest craft (`resolve_target`) | **differs (G-04)** |
| WWP-20 | planar range, soft radius zero, no minimum range | **differs (G-01)**; minimum range missing (not in M2; G-08) |
| WWP-21 | the motion table has it (AV-05); combat does not use it | **differs (G-01)** |
| WWP-22 | 50 % draw | same for M2 (no `Randomize_Between_Fire_Bones`; G-08) |
| WWP-23 to WWP-25 | `lead`, `pointable`, `scatter` | same |
| WWP-26 | the projectile's damage only | **differs (G-02)** |
| WWP-27 | range plus height | **differs (G-01)** |
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
| WWP-47 | the unit's origin | **differs (G-03)** |
| WWP-48 | the projectile's `Projectile_Max_Flight_Distance` (500 for the small lasers) | **differs (G-03)** |
| WWP-49 | `resolve_target` (nearest to the unit's position) | same (nearest to the muzzle, not the centre: G-03) |
| WWP-50 | the nearest targetable hardpoint, else the R-10 search with a range test | **differs (G-03)** |
| WWP-51, WWP-52 | `lead`; no unit weapon scatter | same |
| WWP-53 | cone as W-09; `Fires_Forward` yes flies at the aim point | same for M2; missing for `Fires_Forward` yes (G-08) |
| WWP-54, WWP-55 | new shots move from the next frame | same |
| WWP-60, WWP-61 | no delay, no acceleration | missing (not in M2; G-08) |
| WWP-62 to WWP-64 | `step_projectile`; no jamming or missile shield | same for M2 |
| WWP-65 | no ground in space | same |
| WWP-66 | hostile-player gate shared with ordinary targeting; living collision permission loaded and applied; a separate persistent owner tree supplies ray contacts in DG-30 order | agrees in query order; DG-30f records deterministic tick commit and equal-centre tie choices |
| WWP-67 | hand-off to `damage.cpp`; no blast area | same for M2 |
| WWP-68 to WWP-70 | `step_projectile` | same; lifetime expiry missing (not in M2; G-08) |
| WWP-72 | not loaded | **differs (G-06)** |

Counts, by rule (62 simulation rules; WWP-40 is presentation): **same 43**, **differs 12**
(WWP-02, 19, 20, 21, 26, 27, 43, 47, 48, 50, 72, and the project choice of WWP-66), **missing 7**
(WWP-04, 09, 14, 17, 28, 60, 61; none authored by an M2 object).

### The gaps, by impact on the M2 battle

| Gap | Rules | What | Size |
|---|---|---|---|
| G-01 target soft-radius range adjustment (legacy EAWR-708) | WWP-20, WWP-21, WWP-27 (and R-11, A-07) | Add the target's soft radius to a hardpoint's fire range and travel, to the opportunity aim distance and to the attack distance. FoC's weapons reach the stations 250 to 300 units further and every ship by its half length. | M |
| G-02 MC80 hardpoint damage overrides (legacy EAWR-709) | WWP-26 | A hardpoint's `Projectile_Damage` replaces the projectile's: the MC80's lasers do 20 (not 15) and its ion cannons 40 (not 20). | S |
| G-03 fighter aim and muzzle bones (legacy EAWR-710) | WWP-47, WWP-48, WWP-49, WWP-50 | The unit's own weapon: aim at the ordered hardpoint, else a random target bone, else the height-adjusted centre (never the nearest hardpoint, which with DG-39 also sends the damage to that hardpoint); fire from the muzzle bones in turn; travel `Targeting_Max_Attack_Distance`. | M |
| G-04 random craft selection by ordered hardpoints (legacy EAWR-711) | WWP-19 | A hardpoint ordered at a squadron fires at a random craft each attempt (a synchronized draw), not the nearest; FO-04 is corrected. | S |
| G-05 fighter targeting-state fire gate (legacy EAWR-712) | WWP-43 | The unit's own weapon fires only in the "okay to attack" targeting state and tests no range; ours tests the planar attack distance. The state's inside (walk 1/2) decides when a fighter may fire; the retail dogfight outcome comparison shot log shows retail fighters never firing beyond 424 units. | M (after walk 1/2) |
| G-06 height-adjusted target positions (legacy EAWR-713) | WWP-16, WWP-19, WWP-64, WWP-72 | Load `Ranged_Target_Z_Adjust` and use the height-adjusted position for the aim fallback, the nearest-hardpoint reference and missile steering (the Nebulon-B +35). | S |
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
  asks the targeting state (G-05), and aims at the nearest hardpoint where FoC aims at the centre
  (G-03). Fire-to-target distance in the retail shot log never exceeds 424 units. The count gap is
  the chase geometry (walk 1) plus G-05.
- **concussion-missile flight comparison (legacy EAWR-660) (missile comparison).** The flight rules match (WWP-62, WWP-64, WWP-66 to WWP-70); the
  steering point misses the Nebulon-B's height adjustment (G-06). The rest of concussion-missile flight comparison (legacy EAWR-660) is presentation.
- **AI bombing-run departure (legacy EAWR-633) (bombers hitting the adjacent station, not the corvette).** FoC's projectiles hit any enemy
  unit in their path (WWP-66); nothing makes a torpedo skip the station. Whether the station lies in
  the path depends on the approach (walk 1) and the heights (per-unit flight heights); G-01 lets FoC's bombers fire
  from further out. No weapon rule differs here.
- **per-unit flight heights (are range and arcs planar?).** See "Planar or 3-D": hardpoint range planar, travel
  compensated for height, cones and the opportunity aim distance 3-D.

## Unverified

| ID | What | What would settle it |
|---|---|---|
| U-01 | The order of a unit's services and hardpoints within a frame. | The object manager's service loop (a debug-build read). |
| U-02 | Whether a hardpoint's projectile moves in the frame it is fired (its muzzle delay is left at the data default). | A retail per-frame projectile position trace of one hardpoint shot (S-15 has shot and hit ticks only). |
| U-03 | What sets the "okay to attack" targeting state (two targeting checks not read here). | Walk 1/2's read of the targeting service. |
| U-04 | Which model box gives the soft radius without a custom radius. | A read of the model's bounds call; or compare the stations' 300/250 against a ship's. |
| U-05 | Whether neutral objects (asteroids, props) are enemies of a player and so can be hit. | A read of the player enemy test; M2 has no neutral combatants. |
| U-06 | How the muzzle bones are found by name (the X-wing's `MuzzleA_NN`). | A read of the unit weapon's bone look-up. |
