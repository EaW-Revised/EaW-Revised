# Space hull and hardpoint damage, loss, death and repair

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space, the M2 fixture
  ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md)). Data claims come from the FoC profile
  of the retail corpus: `hardpoints.xml`, `spaceunitsfrigates.xml`, `spaceunitscorvettes.xml`,
  `starbases.xml` and `gameconstants.xml` (hashes under Sources in the M2 lock), and the FoC
  story scripts for the Lua damage call.
- Bounded question: how a space unit's hull and hardpoints lose health, what losing an
  engine, weapon, shield generator, fighter bay or ability hardpoint switches off, when a unit
  dies, and how hardpoints are repaired. P2-09
  (EAWR-72).
- Source tags: **data** (a tag or value in the FoC files), **research** (the FoC debug build,
  read under the clean-room rule; evidence IDs E72-nn, and AU-nn from the
  [debug-build audit](debug-build-audit.md), are opaque and their map stays private),
  **owner** (an M2 lock rule), **inference** (our conclusion, not established) and **project**
  (a remake decision).
- Out of scope: projectile collision, shield absorption, damage and armor types
  ([space damage](space-damage.md), EAWR-74); weapon
  fire (EAWR-73); movement (EAWR-70); squadron launch timing (EAWR-75); abilities (EAWR-76); presentation
  (EAWR-80, with the EAWR-136 hardpoint-state hook).

## Interface

- Content: the durability table (`sim::tactical::DurabilityTable`). Per unit type: maximum
  hull, `Max_Speed`, `Should_Be_Destroyed_When_All_Hardpoints_Destroyed`, and each hardpoint
  in `HardPoints` order with its role, whether it is destroyable, its maximum health and its
  repair amount and cost per frame. Rules: `Hull_Vs_Hard_Points_Health_Constraint` (C),
  `Engines_Disabled_Speed_Modifier` and `Health_Low_Percent_Threshold`. For M2,
  `units::durability_table` builds it from the EAWR-65 unit tables
  ([unit-data.md](../unit-data.md)); the setup's content identity names it. Like the sensor
  table it is passed to the session and is neither replay data nor state.
- State: for each live unit whose type has a profile, its hull and every hardpoint's health
  in Q24. It is part of the state hash ([replay-format.md](../replay-format.md)).
- Input: the scripted-damage command (replay opcode 4, HD-30). Projectile hits ([space damage](space-damage.md) DG-11) now
  apply the same HD-02 and HD-20 rules inside the tick; a shot aimed at a hardpoint damages that hardpoint wherever it meets the unit ([DG-39](space-damage.md), EAWR-669), so the hull of a unit under fire follows its hardpoints through HS-02.
- Outputs, per completed tick: in the tactical snapshot (encoding v3), each durable instance's
  hull, maximum hull, speed factor and maximum speed, engine, shield and launch flags, and per
  hardpoint its role, state (intact, damaged, destroyed), enabled flag and health; the events
  `hardpoint_destroyed` and `unit_destroyed`.
- Cadence: damage applies in the commands phase, in canonical command order. Then, once per
  tick, the systems phase services every live durable unit (HS rules). Units that died leave
  before the visibility pass. The session never repairs (HR-07).

## Rules

### Health

| Rule | Behaviour | Source |
|---|---|---|
| HD-01 | A unit starts with full hull and full hardpoint health. In space the maximum hull is `Tactical_Health` × `Object_Max_Health_Multiplier_Space` (1.5) and a hardpoint's maximum is its `Health` × 1.5. Retail also applies the AI difficulty health multiplier (1.0 at Normal, SK-42) and the unit's health modifiers; this ticket applies neither. | data; research E72-12; owner SK-42; project (modifiers are EAWR-76) |
| HD-02 | Damage aimed at a destroyable hardpoint that still has health reduces only that hardpoint. The hull is untouched, and damage beyond the hardpoint's remaining health is lost. A hit on a destroyed hardpoint does nothing. | research E72-01, E72-02 |
| HD-03 | Health stops at zero. A destroyable hardpoint at zero is destroyed. | research E72-03 |
| HD-04 | The presentation state of a hardpoint (EAWR-136): destroyed at zero; damaged while alive with health below `Health_Low_Percent_Threshold` (0.33) of its maximum, compared exactly (health × 2^24 < maximum raw × threshold raw); intact otherwise. A hardpoint that is not destroyable is always intact. Retail shows a hardpoint's damage art only once it is destroyed, and tints its target reticle red below 0.33 of its health and yellow below 0.66 (both hard-coded), so "damaged" is a remake presentation state. | data (threshold); research E72-03, E72-13; project |
| HD-05 | Destroyable hardpoints in the M2 roster: `Nebulon_B_Frigate` four lasers and the engines, 390 each; `Acclamator_Assault_Ship` lasers 210, missile and torpedo 240, engines 255, fighter bay 150; each skirmish station all seven of its hardpoints (supply dock, comm array, three weapons, one shield generator at 975 and the fighter bay at 1500). The corvettes' weapon hardpoints are neither targetable nor destroyable. | data |

### What a lost hardpoint switches off

| Rule | Behaviour | Source |
|---|---|---|
| HD-10 | A destroyed weapon hardpoint may no longer fire. The unit's other weapons are unaffected. The snapshot's `enabled` flag is false from that tick. | research E72-10 |
| HD-11 | When the last engine hardpoint of a unit is destroyed, its engines go permanently off-line and its maximum speed is multiplied by `Engines_Disabled_Speed_Modifier` (0.4): `Max_Speed` 2.2 becomes 0.88 on the Nebulon-B and the Acclamator. A type without engine hardpoints keeps its full speed. Engine loss also ends an active `TURBO` or `SPOILER_LOCK` ([space abilities](space-abilities.md) AB-16). | data; research E72-04, E72-05; EAWR-76 for the abilities |
| HD-12 | When the last shield-generator hardpoint is destroyed, the unit's shield goes off-line and an active `DEFEND` ends ([space abilities](space-abilities.md) AB-17). In M2 only the stations have one. A type without shield generators keeps its shield. The shield then drops to zero and never recharges ([DG-17](space-damage.md#shields-over-time)). | data; research E72-04; EAWR-76 for `DEFEND` |
| HD-13 | A spawner launches only from fighter-bay hardpoints that are not destroyed; with none left it launches nothing. Only destroyable bays can drop out, and an active `DEFEND` also blocks launches (not modelled: no M2 type has both, space-abilities AB-U6). The Acclamator's `HP_Acclamator_Fighter_Bay` and each station's bay carry the SK-23 launches. | research E72-11, AU-30, IS-11 (the spawn-from list is the fighter-bay hardpoints); EAWR-75 for the launch, EAWR-76 for `DEFEND` |
| HD-14 | A destroyed special-ability hardpoint (station supply dock, comm array) switches its ability off. SK-31 keeps those abilities off in M2 anyway. | research E72-04; owner SK-31 |
| HD-15 | Retail also recomputes the AI's directed firepower when a weapon hardpoint is lost. | research E72-04; EAWR-79 |

### Death

| Rule | Behaviour | Source |
|---|---|---|
| HD-20 | A unit dies when its hull reaches zero. It leaves the session in that tick: later commands see it as not live, the snapshot no longer lists it, and a `unit_destroyed` event is published. Explosions, death clones and breakoff props are presentation (EAWR-80; breakoff props: [battle presentation](battle-presentation.md) BP-30 to BP-36, EAWR-391). | research E72-01; project (removal at death) |
| HD-21 | A type with `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` dies when a hit destroys its last destroyable hardpoint. FoC defaults the flag to yes; an authored no overrides it. The check runs after a destroyable hardpoint hit brings its health to zero. | research E72-01, IS-05, IS-06; data |
| HD-22 | A type that authors no for HD-21 can lose every destroyable hardpoint and keep its hull. | research E72-01, IS-05 |

### Hull and hardpoint coupling, once per tick

Let M be the maximum hull and H the hull; T the sum of the maximum health of the destroyable
hardpoints (destroyed ones included) and S the sum of their current health; C the constraint 0.2.

| Rule | Behaviour | Source |
|---|---|---|
| HS-01 | After the tick's commands, every live durable unit with at least one destroyable hardpoint runs HS-02, then HS-03 and HS-04. Retail runs them in every service frame of the object, after the object's behaviours and before each of its hardpoints (weapons included) is serviced. | research E72-06, AU-25 |
| HS-02 | HD-21 types only: the hull may not exceed min(1, S/T + C) of its maximum. S/T is zero once every destroyable hardpoint is destroyed. | research E72-06, E72-07 |
| HS-03 | The hardpoints may not exceed the hull: with L = min(1, H/M + C), the excess is X = S − T·L. Nothing happens when X ≤ 0, so a full hull never touches the hardpoints. | research E72-06 |
| HS-04 | Each live destroyable hardpoint i loses X · sᵢ / T, where sᵢ is its health before this service. The loss divides by T, not S, so a damaged set converges on L over several ticks. A hardpoint that reaches zero is destroyed and HD-10 to HD-14 apply; the service never kills a unit. Retail takes T for the loss from the hardpoints' data maxima and S/T from their instance maxima (which add the AI difficulty multiplier and health modifiers); without those, as in M2, the two agree. | research E72-06, AU-26, AU-27 |
| HS-05 | All values are Q24 raw integers. Each capped hull and each loss is one exact rational, rounded once to nearest-even ([fixed-point.md](../fixed-point.md)); `validate_durability` bounds every health and speed to 2^20 units, so every product fits in 192 bits. | project |

### Repair

| Rule | Behaviour | Source |
|---|---|---|
| HR-01 | Retail repair is a player command on one hardpoint (the "repair hardpoint" network event names the object, the hardpoint index and the player). In every frame of the repair, a player whose credits cover `Repair_Cost_Per_Frame` pays it and the hardpoint regains `Repair_Amount_Per_Frame`, capped at its maximum. With several repairing players, each paying player repairs once in the same frame. | research E72-08, E72-09, AU-29; data |
| HR-02 | A destroyed hardpoint cannot be repaired; the repair ends. | research E72-08 |
| HR-03 | A player who cannot pay a frame drops out of the repair. | research E72-08 |
| HR-04 | After a paid frame, a hull fraction h below the combined hardpoint fraction c becomes a hull of H · (1 + c − h). | research E72-08 |
| HR-05 | The repair ends when the hardpoint is back at full health; retail also re-enables it. | research E72-08 |
| HR-06 | In the M2 roster only station hardpoints author repair values: 0.5 health per frame, for 1.3 credits (weapons), 1.5 credits (shield generator, fighter bay), and 1.5 (Rebel) or 1.3 (Empire) for the supply dock and comm array. Ship and craft hardpoints author none and cannot be repaired. | data; research IS-09, IS-10 (retail offers the repair order only on star-base hardpoints below full health and never reads the amount; a zero amount would repair without end) |
| HR-07 | **Selected M2 behaviour: no repair.** The session has no repair command, so destroyed hardpoints stay destroyed and lost health never returns. Until EAWR-530 every M2 player had 0 credits, so a retail repair stopped on its first frame, unpaid; since EAWR-530 players have credits and income ([space purchasing](space-purchasing.md) PU-01 to PU-06), and the missing repair command is fidelity item PU-G11. `sim::tactical::repair_frame` implements HR-01 to HR-05 for it. | owner SK-30, SK-31; project |

### Scripted damage

| Rule | Behaviour | Source |
|---|---|---|
| HD-30 | Damage enters the session through the scripted-damage command, replay opcode 4: the retail Lua `Take_Damage(amount[, hardpoint])`, which FoC story scripts call on named hardpoints. The command names the hardpoint by its `HardPoints` index, or the hull. AI-45 routes script engine calls through the replay command queue. Only the script host issues it. | data (story scripts); AI note AI-45; project |
| HD-31 | Each listed unit is judged in list order: not live gives `unit_not_live`; a type without a profile gives `not_damageable`; an index that is not a destroyable hardpoint gives `hardpoint_invalid`. Ownership is not checked. Otherwise HD-02 and HD-20 apply at once, and the accepted event is followed by the destruction events it caused. Without damage rules the damage is raw; with them the shield absorbs it first, with no armor or damage type ([DG-20](space-damage.md#scripted-damage); S-15 and S-18 show the retail shield taking it). The unit keeps its current order. | project |
| HD-32 | A negative amount is a malformed command. Zero is accepted and changes nothing. | project |

## Cases

C-01 to C-08 are the oracle fixture `tests/replay/fixtures/tactical-durability.*`: a Nebulon-B,
an Acclamator, a Corellian corvette, an Empire station and a squadron with FoC values, and
twelve scripted commands over six ticks. `tactical_durability_contracts` replays it with 1, 2
and 4 workers and scrambled storage, and through a written and parsed replay, against the
oracle's hash, snapshot and event goldens.

| Case | Input | Expected |
|---|---|---|
| C-01 | 400 on the Nebulon-B engines (390) | Engines destroyed, engines off-line, maximum speed 2.2 × 0.4 (raw 14,763,949, about 0.88); the hull stays 5400 (HD-02, HD-11) |
| C-02 | 200, then 100 on laser FL (390) | 190 left: intact; 90 left: damaged (below 128.7), still enabled (HD-04, HD-10) |
| C-03 | 90 more on laser FL | Destroyed and disabled; FR, BL and BR stay enabled (HD-10) |
| C-04 | 150 on the Acclamator fighter bay | Destroyed; the Acclamator can no longer launch; its engines stay on-line (HD-13) |
| C-05 | 975 on the station shield generator | Destroyed; the station's shield is off-line; its own bay can still launch (HD-12) |
| C-06 | 10 on a corvette laser, on the squadron, and on Nebulon-B hardpoint 9 | Rejected: `hardpoint_invalid`, `not_damageable`, `hardpoint_invalid` (HD-05, HD-31) |
| C-07 | 2000 on the Acclamator hull (3000) | Hull 1000. That tick's service takes the live hardpoints from 1575 of 1725 to about 0.62 of their maxima: each laser 210 to about 130.26 (HS-03, HS-04) |
| C-08 | 1000 more on the Acclamator hull | It dies and leaves the snapshot. Next tick an attack on it is `target_not_live` and its owner's stop order `unit_not_live` (HD-20) |
| C-09 | Repair the damaged station shield generator with 0 credits, then with credits | Stops unpaid (HR-03, HR-07); paid, 200 frames of 0.5 restore 100 health and end the repair (HR-01, HR-05) |

## Unknowns

| Gate | Unknown | Effect |
|---|---|---|
| G-H1 | Retail applies hits during the projectile service of a frame. Their order relative to the object's hardpoint service in the same frame was not traced; here all of a tick's damage comes first. Within one object's service, the HS drag runs before that object's own weapons ([audit](debug-build-audit.md)). | A hit and the HS drag of the same tick may be one frame apart from retail. |
| G-H3 | Temporarily disabled hardpoints (ion stun and other disables) are not modelled; no M2 weapon ion-stuns. | EAWR-76 adds them; a disabled weapon would also stop firing. |
| G-H4 | Resolved in part: the repair event and the per-frame service check neither the owner nor the repair amount, and the click that issues the order requires a star-base object and a hardpoint below full health (IS-09, IS-10). Whether an enemy star base can receive the order was not traced (unverified). | None in M2 (HR-07). |
| G-H5 | "Damaged" (HD-04) is a remake presentation state; retail changes a hardpoint's art only when it is destroyed. | EAWR-80 may draw intact art until destruction to match retail. The live battle does: a hardpoint's smoke and fire (its `Damage_Particles` emitters) start only when it is destroyed, and a unit whose engines go off-line (HD-11) stops its engine emitters ([battle presentation](battle-presentation.md#unit-emitters-engines-and-hardpoint-damage-394) BP-41, BP-42). |
| G-H6 | Retail computes in binary32; the remake is exact in Q24 (HS-05). | Health can differ from retail by rounding, typically below 2^-20 of a unit per step. |
