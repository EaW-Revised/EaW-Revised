# Space BARRAGE target and weapon interface

This implements the simulation interface of [area-damage WAD-38](walks/area-damage.md),
using the existing [ability timers](space-abilities.md) and ordinary weapon service.
Broadside and Marauder expose BARRAGE under RG-03. Their HUD button arms world-point
targeting (AB-09); a click on the battle plane dispatches the existing area command,
including clicks on empty space. Automatic AI point choice and U-07 retail captures
remain separate work.

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| BARR-01 | BARRAGE activation takes a world position, not an enemy object. A fogged point rejects activation before applying the authored height offset; an already active target is retained. Cancellation uses the ordinary ability action. | Debug build; WAD-38; ability-button targeting AB-09 |
| BARR-02 | **No range gate at activation.** Activating BARRAGE checks only that the ability exists and is enabled, that the point is not fogged for the owner, and that the area behaviour is idle. It never compares the click with a range. The source is simply ordered to attack the placed target and closes in under the ordinary attack-order rules. A separate validity check exists in FoC (distance from the ship in the horizontal plane at most the unit type's `Targeting_Max_Attack_Distance`, or the ability's own `Targeting_Max_Attack_Distance` when it authors one above zero; at least `Targeting_Min_Attack_Distance`; the weapon can hit the point; the turret can point at it; the point is not fogged; the ship is not under a move order other than an idle drift). Stock BARRAGE authors no ability-level distance. Who calls that check (cursor validity, AI or neither) was not traced (BARR-U1). | Debug build F2166-E01, F2166-E02; stock XML |
| BARR-03 | **Where the shot's reach comes from.** Each barrage shot is an ordinary weapon shot, so the firing weapon's own range decides whether it may fire: a hardpoint weapon fires when the horizontal distance to the aim point is within its `Fire_Range_Distance` plus the target's soft radius (and beyond `Fire_Min_Range_Distance`). The override projectile's `Projectile_Max_Flight_Distance` bounds the flight of the shot only when the firing hardpoint authors no range of its own; otherwise the hardpoint range (plus target radius and height difference) is the shot's travel limit. In stock data the override authors 3000, equal to the Marauder's `Targeting_Max_Attack_Distance`, so the two readings give the same reach there. | Debug build F2166-E03; stock XML; Focumentation: Unit Abilities (BARRAGE comment) |
| BARR-04 | **`Effective_Radius` is not read.** None of the barrage functions (activation, area behaviour start, stop, service, validity check) reads the ability's `Effective_Radius`, and stock BARRAGE does not author it (its comment says the parameter is no longer used). The blast footprint is the override projectile's blast range, the scatter is `Targeting_Fire_Inaccuracy_Fixed_Radius_Override`, and the decal size is `Area_Effect_Decal_Distance`. Other abilities (area effects, missile shield, sensor jamming) do read `Effective_Radius`; the command bar also reads it at three sites that were not traced for BARRAGE. | Debug build F2166-E04; stock XML; Focumentation: Unit Abilities |

The production command sink emits the area command for activation and the ordinary
ability command for cancellation. The simulation remains responsible for point
visibility and the source's ability readiness. No new range or enemy-object gate is
imposed by the HUD. Hover and click share the point query described by
[CU-12](foc-cursors.md), selecting valid/invalid space-position pointers;
the authored area decal is a separate presentation input.

A point activation uses the existing ability slot. An already active slot accepts
another activation without moving or replacing its target. A new activation requires
a ready ability and a source with combat and locomotor state. The point must be visible
before the authored height offset is applied. Bound fog uses its current reveal cell;
sessions without fog cells query the last published allied sensor ranges in worker
partitions. Hidden points create no object.

Activation creates a real `Dummy_Barrage_Target` under a hostile owner, applies
`Target_Position_Z_Offset`, and gives the source an ordinary attack order on that
object. The retail conflict's attacker/defender owner context is absent from this
session setup; the project fallback chooses the first hostile player in the validated
ascending player list. The target retains its authored model, category and projectile
collision flags, has no damageable hull or locomotor, and does not influence capture.
No claim that its authored model is invisible follows from its marker behaviour.

While the slot is active, its shots at that target select `Projectile_Types_Override`.
The selected shot retains the override projectile's inherited damage, area damage,
dropoff, speed, energy cost and flight inputs independently. Object-weapon energy
admission and spending use the selected override's cost, preserving its zero cost
when the ordinary projectile costs energy. Ordinary target validity, range,
alignment, recharge, damage and collision services still decide whether a shot fires.
Its fixed inaccuracy override replaces range-scaled scatter: independent synchronized
draws offset X, Y and Z within the authored symmetric radius in space. These offsets
are a box distribution, not a spherical radius sample.

For object weapons, `FIRE_RATE_MULTIPLIER` divides the full recharge and pulse gap,
with positive values truncated to whole frames, and scales the refilled pulse count
with truncation. Normal authored pulse, recharge and weapon-delay inputs remain in
the same service. The hardpoint clock's separate rules are unchanged. Stock BARRAGE
authors multiplier 3, fixed inaccuracy 320, height offset -150, duration 10 seconds
and recharge 40 seconds. Its Diamond Boron override inherits blast damage 150,
radius 200 and five dropoff tiers, with speed 12 and authored flight distance 3000.
The targeting decal distance is not the projectile's blast radius.

Cancellation, expiry, source destruction or loss of locomotor ends the relationship.
The target is removed and the source's retained target and shot override are cleared;
already launched projectiles retain the inputs captured when they fired. Existing
ability timer rules determine recharge. Source/proxy validity is prepared from
immutable copied inputs before partitioned unit service, then removed through the
ordinary ordered survivor commit. No serial per-frame sweep is added.

The proxy's runtime source backlink participates in state hashing only when nonzero.
It is not a replay setup field. The point command uses the additive reserved opcode
17 described in [the replay format](../replay-format.md); existing entity-target
ability payloads retain their bytes. Contracts exercise creation, idempotence, fog
rejection, override retention, expiry/cancellation, playback and workers 1/2/4/8.
The [rocket contract](rocket-flight.md) states the remaining flight constructions
and retail presentation work. RG-03 admission covers the complete stock handler and
its authored multiplier, independently of those remaining presentation comparisons.

## Focumentation cross-check (BARR-02 to BARR-04)

Checked against the Focumentation page "Unit Abilities" (BARRAGE comment): the wiki says the reach of barrage mode
comes from the override projectile's maximum flight distance and that `Effective_Radius` is unused. Verdict: both
partly right. `Effective_Radius` is unused, as the wiki says (BARR-04). Activation has no range gate, as this note
said (BARR-02). The override's flight distance matters only when the firing weapon has no range of its own (BARR-03),
and the stock values agree, so no visible difference is expected for Broadside or Marauder.

| ID | Unknown | Effect | What settles it |
|---|---|---|---|
| BARR-U1 | Which UI or AI path calls FoC's barrage validity check (BARR-02), and whether a unit's own weapon (the Marauder's `Projectile_Types`) is range-gated by `Targeting_Max_Attack_Distance` or by the override's flight distance. | Whether the pointer shows invalid beyond range instead of the ship closing in. Stock values coincide at 3000, so only a modded value would differ. | Rig capture: order a barrage from a Marauder (a) 2900 units, (b) 3100 units from the target point, and (c) with a unit between; record whether the cursor refuses, whether the ship approaches, and where the first missile lands. |
