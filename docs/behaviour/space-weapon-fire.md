# Space weapon fire, ship-level target choice and attack orders

## Applicability

FoC space tactical combat (target selection and weapon fire), read from the FoC debug build (TargetingBehavior,
TargetingInterface and HardPoint services; private evidence TE-01 to TE-12, WA-01 to WA-13
for weapon arcs and facing, AT-01 to AT-13 for the turn toward an ordered target, weapon arcs, collision, energy and missiles, and
CF-01 to CF-03 for the lead, cone and scatter of a shot, capital-ship fire against fighters; the missile and torpedo arcs
of Acclamator firing-arc verification) and checked against recordings S-01 to S-03 (original-game behaviour recordings part B), S-22 to S-27 (weapon arcs, collision, energy and missiles) and
S-32, S-33 (Acclamator firing-arc verification), fog revealed. It covers a unit's own target (the
"ship-level" target), player attack orders, the weapon fire cycle of hardpoints and object
weapons, and the shots handed to projectiles (projectile, damage and shield resolution). The hardpoint opportunity search itself is
[space targeting](space-targeting.md) R-01 to R-15. Projectile flight, damage and shields are
[space damage](space-damage.md) (projectile, damage and shield resolution). Squadron
behaviour (fighter spawning and simulation), abilities (space ability implementation), movement toward an attack target and the AI (tactical AI host) are not
covered. Rules marked *project* are remake choices; everything else is what FoC does.

## Interface

- Inputs per unit and frame: its type's combat profile (the space-unit data loading unit tables through
  `units::combat_table`: `CategoryMask`, `Targeting_Priority_Set`, `Targeting_Max_Attack_Distance`,
  the `Fire_*` tags of its weapon hardpoints, its `Projectile_Types` object weapon, target bones and
  hardpoint positions scaled by `Scale_Factor`), its position and rotation after the frame's
  movement, its hardpoint health, the other live units, and each player's visibility of every
  unit as the last published snapshot has it ([space visibility](space-visibility.md), the query
  API of space queries and visibility).
- State kept per unit: the ship-level target, whether a player ordered it, the frame of the next
  ship-level scan; per weapon: its opportunity target and last scan frame (R-04), its fire
  countdown in frames and the shots left in its burst.
- Outputs: that state, and combat events in ascending shooter ID then weapon order: an
  opportunity acquisition (R-12) and a shot (shooter, weapon, target, target hardpoint, fire bone
  position and aim point).
- Cadence: every unit with a combat profile is serviced once per logical frame (1/30 s).

## Ship-level target choice

| Rule | Behaviour |
|---|---|
| T-01 | A unit with no `Targeting_Max_Attack_Distance` never chooses a target of its own: its scans (T-05) find nothing. A target that is no longer live, or that is fogged to the unit's owner, is dropped at once, ordered or not, whether or not the unit has an attack distance: FoC's ship-level targeting service has no attack-distance gate, so a unit without one still takes attack orders and still drops them when the target dies, leaves or is fogged. |
| T-02 | With no target the unit scans (T-05) and takes what the scan finds. A target found this way is not an attack order. |
| T-03 | A target taken by a scan is re-examined every frame: when it is no longer suitable (T-06) or its priority is not `1.0`, the unit scans again; a different result replaces it only when the current target is unsuitable or the result is better (T-07). A target whose priority is `1.0` is never rescanned. FoC also keeps a target whose estimated time to death is within `Targeting_Stickiness_Time_Threshold`; the remake has no damage tracking yet and always rescans. |
| T-04 | A player-ordered target (A-01) is never rescanned or replaced by a scan. |
| T-05 | A scan runs only when the frame has reached the unit's next-scan frame, and then sets the next one to the frame plus one second plus a synchronized draw of 0 to half a second (30 + 0..15 frames). It visits the players cyclically from a synchronized random start, skipping the owner and every player of the owner's team. For each visited player it examines that player's units inside a box of half extent `Targeting_Max_Attack_Distance` on every axis around the unit, in collection order, and keeps a per-player best by T-07 starting from none; the last visited player that yields a best supplies the result, and the best priority is carried from player to player. The scan stops early only when the candidate is the current target and its priority is `1.0`. |
| T-06 | A candidate is suitable when it is live, hostile and visible to the unit's owner, at least one of the unit's weapons may fire at its category (not destroyed, category not in `Fire_Category_Restrictions`), the unit's priority set gives it a priority (R-09; a unit without a set scores `1.0`), and it lies within `Targeting_Max_Attack_Distance` of the unit in the XY plane, inclusive, unless it is the current target. |
| T-07 | A candidate is better than the best so far when: it has a priority and the best has none, or there is no best; otherwise not when the best is the current player-ordered target; otherwise, when exactly one of the two is damaged (hull at most `Health_Low_Percent_Threshold` of its maximum), the damaged one; otherwise when its priority is strictly lower; otherwise, when exactly one of the two can be hit by the unit's object weapon, that one; otherwise when it is strictly nearer in the XY plane. A candidate with an equal or worse priority therefore wins when it is nearer: in S-01 the TIE Defender's own target is the nearer X-Wing although the Y-Wing ranks better, because FoC met the Y-Wing first. |

## Attack orders

| Rule | Behaviour |
|---|---|
| A-01 | An accepted attack command (replay opcode 3, the UI command sink of UI input routing and command sink through next-tick replay input) sets each listed unit's ship-level target to the command's target and marks it player-ordered. It acts from the next frame's targeting, like a move (MV-02). |
| A-02 | A player-ordered target is handed to every weapon hardpoint of the unit, which tries it before any opportunity target (W-04). A target taken by a scan is not: in S-01 to S-03 the hardpoint's own attack-target slot stays empty while the unit's object weapon engages its ship-level target. |
| A-03 | *Project:* any other accepted order (stop, move, face, attack-move, guard) ends a player-ordered attack: the target is cleared and the unit scans again (T-02). Retail handling of these replacement orders is not traced. |
| A-04 | **Turning toward an ordered target** (research AT-01 to AT-03, AT-05, AT-06, AT-10; recordings S-26, S-27). Each frame the ship-level targeting checks a unit that has a locomotor and no movement left (no path and no turn in place under way) and whose target lies within `Targeting_Max_Attack_Distance` in the XY plane, measured to its aim point. The aim point is the target's live targetable hardpoint nearest the unit (W-05's choice), else the target's position. The unit's wanted heading is the XY bearing to that point, adjusted by A-06. When the unit's yaw differs from it by more than 10 degrees (wrapped to [-180, 180)), the unit turns in place to face that heading, exactly as a face order does (MV-20, MV-21: the short way, at `Max_Rate_Of_Turn` over the layer's turn-in-place slowdown, ending on the heading, without moving). A heading within 10 degrees is left alone. The distance is the type's last authored `Targeting_Max_Attack_Distance`: the Tartan authors 2000 and then 800, so it turns only toward a target within 800 (S-26/S-27 aim at about 760). The turn starts at the next frame: an order in the commands of tick t reaches targeting at t + 1, and the unit first turns at t + 3 (S-26/S-27: order at 30, first turn at 33). While it turns (or moves) nothing is checked; after the turn it is checked again. |
| A-05 | The ship-level targeting service turns a unit for a player-ordered target (research AT-02, AT-12; recordings S-22 to S-25; AI ship facing before combat: debug build and a retail staging, A-08). FoC also turns toward a scan's target when `Auto_Rotate_For_Space_Targeting` is set, which FoC's `gameconstants.xml` sets to false, and for a type with no hardpoints at all. Nothing else in the turn's gate looks at the unit's owner, so the rule holds for AI-owned units too. An idle Tartan or Nebulon-B keeps its heading with the enemy dead astern and does not fire. |
| A-06 | **Firepower adjustment** (research AT-07 to AT-09). The heading is the bearing plus 0, +90 or -90 degrees. The unit's live, ordinary weapon hardpoints whose projectile does hull damage are summed by direction: a hardpoint counts toward ahead, right or left when the XY direction of its `Fire_Bone_A` x axis lies within half its `Fire_Cone_Width` (capped at 180) of that direction; one hardpoint can count for several. Each adds its AI combat power, its projectile's `AI_Combat_Power` over the sum of all the type's weapon hardpoints' projectiles', times the type's `AI_Combat_Power`. The adjustment is 0 unless the right side's sum exceeds the forward one (+90, the target on the right beam) and the left side's exceeds the larger of those (-90). When the left side ties a winning right side, FoC compares the turn that puts the target on the left beam, |clamp180(90 - d)| with d the bearing minus the unit's yaw wrapped to [-180, 180), with the turn that faces it, |d| (not with the right-beam turn): -90 only when the left-beam turn is smaller, otherwise +90. A target 20 degrees to the left compares 70 with 20 and keeps +90, although the left beam is the nearer one. A type without hardpoints uses 0. FoC recomputes the sums when a weapon hardpoint is destroyed or repaired; the remake sums them whenever it checks. Every M2 ship's forward sum wins (the Tartan's five lasers all reach ahead), so M2 ships turn their bow to the target. |
| A-07 | The attack distance test adds the target's soft radius for a hardpoint aim, or its hard X extent when the absolute cosine of its facing and the incoming bearing exceeds 0.7071, otherwise its hard Y extent (debug build, AT-10). The remake uses the loaded, scaled movement footprint, with collision half extents as a fallback for synthetic profiles. An ordered target out of range is approached ([space orders](space-orders.md) OR-02 to OR-08, attack approach, attack-move and guard; it was G-W2). The separate unit-destination turn is WMV-20. FoC's hardpoint choice for the aim point ranks the target's hardpoints by the attacker's hardpoint-type priorities before distance (G-W3); the remake uses the nearest, which is what S-26/S-27 show. |
| A-08 | **AI-owned ships** (AI ship facing before combat; debug build; three retail stagings on the rig with `staging_probe.lua` `aiturn`: debug build, Easy AI, fog off, the timeline in the AI log). An AI ship turns to bring its weapons to bear only through the AI's orders. An attack of a unit from the AI's Lua (a unit's `Attack_Target`, or a TaskForce's attack of an object through its movement coordinator) is an ordered attack, so A-04 turns the ship when it is in range and at rest. A move to a position turns it along its path. At rest on a unit attack-move or guard destination it also turns toward that destination's centre (WMV-20); this is independent of its combat target. The unit AI's retaliation on damage attacks the attacker as a target that was not ordered, so it never turns the ship ([tactical AI walk](walks/tactical-ai.md), unit-AI damage retaliation). A free-store ship that holds any target counts as having active orders (FH-23), so the free store leaves it alone except to kite or heal it. In the retail staging an AI Acclamator rested with a Nebulon-B 1000 units dead astern, which kept hitting it. It held the frigate as its own target, kept its heading and fired nothing at it. The free store serviced it every 2 s with no order. 5.5 s after the AI resumed, the goal system's burn plan (`burnunits`) collected the free units and attack-moved them at a map structure. The Acclamator started moving at 6.5 s and was more than 10 degrees off its heading at 7.9 s. The remake follows the same rules. In the M2 staging (`foc_ai_turn_668`) the Acclamator keeps its heading until the goal system's first order for it (a `destroyunit` attack-move at tick 529 of a battle that starts cold). An attack order from the AI player turns it at once (WF-10). How soon a plan orders the ship depends on the goal system's state (GS-01, GS-05; the retail staging ran mid-battle on Easy, with a 15 s sleep per goal cycle and the burn goal live), so the two times are not comparable. |

## Weapon service and fire

| Rule | Behaviour |
|---|---|
| W-01 | A weapon's opportunity target that has left the session is cleared in the frame after it leaves, whatever the countdown (S-03: the target is empty from the frame after the removal). |
| W-02 | A destroyed weapon hardpoint does nothing (HD-10). Otherwise its countdown, when nonzero, drops by one each frame; while it is still nonzero afterwards the weapon does nothing more that frame. |
| W-03 | Object weapon: when the countdown allows, it fires at the ship-level target (T-rules), if any, and pays for the shot from the unit's energy pool; a pool short of the cost holds the shot ([EN-05](space-damage.md#energy-pool)). |
| W-04 | Hardpoint: with a player-ordered target (A-02) whose category the weapon may not fire at, the weapon does nothing that frame. Without one and without `Allow_Opportunity_Fire_When_Idle`, it does nothing. Both `Allow_Opportunity_Fire_*` flags default to yes ([DG-35](space-damage.md#projectiles)). With one, it attempts it (W-05); a success ends the frame. After a failed ordered attempt it continues only with `Allow_Opportunity_Fire_When_Targeting`. It then runs the opportunity path R-02 to R-14 with R-01 admitted. |
| W-05 | A firing attempt fails when the target is not live, is fogged to the shooter's owner, or its category is in the weapon's `Fire_Category_Restrictions`. It aims at the target's nearest hardpoint that is targetable and not destroyed (spatial distance from the shooter, first in `HardPoints` order on a tie), else at the first acceptable point of the R-10 search (spatial range, R-11). It fails unless that point lies within the weapon's range plus the target's soft radius of the weapon midpoint in the XY plane, inclusive. The shot then leads the target (W-10), and the weapon must be able to point at the led point (W-07, W-09, W-11). |
| W-06 | A successful attempt fires one shot, from `Fire_Bone_A`, or from `Fire_Bone_B` on a synchronized 50 % draw when the hardpoint has one (drawn after the range test and before the lead, so an attempt that fails W-10 or W-11 has drawn it). It then spends one shot of the burst: while shots remain the countdown becomes `trunc(Fire_Pulse_Delay_Seconds x 30)`; after the last one it becomes a synchronized draw of a whole number of hundredths between `trunc(100 x Fire_Min_Recharge_Seconds)` and `trunc(100 x Fire_Max_Recharge_Seconds)`, times 30, divided by 100 and truncated, and the burst refills to `Fire_Pulse_Count`. The object weapon uses `Projectile_Fire_Recharge_Seconds` for both bounds and its `Projectile_Fire_Pulse_*` tags, and spends its burst by W-06a. |
| W-06a | **The object weapon's burst runs on its own clock** (retail dogfight outcome comparison). Only a burst's first shot waits for a successful attempt: until one succeeds the weapon tries every frame, and that shot spends the first pulse. Each later pulse is spent as soon as the countdown runs out, before the weapon looks for a target, its energy or its cone, whether or not the shot then fires: a craft whose target leaves its cone after the first shot loses the rest of that burst. After the last pulse the recharge is `trunc(Projectile_Fire_Recharge_Seconds x 30)` plus a synchronized draw of 0 to 10 frames. In the S-97 recording an X-wing that fires once and misses its cone three times waits 45 to 54 frames from its last pulse before it tries again; the X-wings fire 86 shots and the TIE fighters 163, where the remake, which kept every pulse until it fired, had the X-wings fire 157 and the TIE fighters 68. `tactical_damage_contracts` checks the clock without game data: three-pulse bursts with the recharge draw at both bounds, and with an energy-costing shot the pairs a begun burst leaves when its third pulse finds the pool short (EN-05). | debug build (retail dogfight outcome comparison: the weapon behaviour's service, pulse counter and recharge reset); recording S-97 (retail dogfight outcome comparison shot log) |
| W-07 | A fixed hardpoint can point at a point when, in `Fire_Bone_A`'s frame placed at the weapon midpoint (the midpoint of its two fire bones), the point's yaw about the bone's z axis, measured from its x axis, is within half `Fire_Cone_Width` and its pitch toward the bone's z axis within half `Fire_Cone_Height`, inclusive. The cone therefore opens along the fire bone's x axis, wherever the model points it: a broadside battery covers its side, and with a cone near 180 degrees also the bow and stern. The unit's rotation enters only through the bone's placement (research WA-01, WA-02, WA-06). FoC's facing takes no arctangent of a point without a planar part: a point on the bone's z axis has yaw 0 and a pitch of 90 degrees, so only a cone height of 180 degrees or more reaches it (Corellian_Gunboat's `HP_Corellian_Gunship_04` and `_08` have a horizontal z axis), and the weapon midpoint itself has yaw and pitch 0 (WA-14). A hardpoint without a fire bone uses its attachment bone, then the unit's own frame. An unauthored cone is zero (FoC's hardpoint default), and FoC applies the same comparison to it: a zero-width or zero-height cone accepts only a point exactly dead ahead on that axis, so in practice such a hardpoint does not fire (the debug build asserts a positive cone here but carries on). FoC authors a cone on every weapon hardpoint but the special-weapon `HP_KEDALBE_SHIELD_LEECH_00`. The object weapon's cone is W-09. A turret (`Is_Turret`) instead tests the point's yaw in its attachment bone's frame, turned by 90 degrees, against `Turret_Rotate_Extent_Degrees`, with no height test, and its barrel must already point within `Fire_Cone_Width` of the target (WA-04, WA-05); the remake does not load turrets (P-04). |
| W-08 | A unit enters the session with each weapon's countdown a synchronized draw between 0 and `trunc(Fire_Max_Recharge_Seconds x 30)` and a full burst (S-01 notes: 26 and 100 frames in two stagings). |
| W-09 | Object weapon cone (research E75-26, E75-27; recording S-28): a unit whose `Fires_Forward` is no, including an unauthored tag, aims its object weapon and drops the shot unless the aim point, taken into the unit's own frame (yaw, pitch and roll) at the weapon's origin, has a yaw within `Turret_Rotate_Extent_Degrees` and a pitch within `Turret_Elevate_Extent_Degrees`, inclusive, each compared whole, not halved. FoC's fighters author 20 and 40 degrees (the TIE Interceptor, TIE Defender, TIE Phantom and StarViper 45 and 45), so a craft fires only while its nose points near the target; in S-28, 79 of the 81 recorded hits on the corvette taken while a craft is within 700 units come while one points within 30 degrees of it. Since capital-ship fire against fighters the cone is tested on the led point (W-10, W-11), as FoC does; since the retail dogfight outcome comparison work on the led point after the object weapon's own scatter (space-damage DG-24), which the debug build adds before its cone test; no M2 object weapon authors one, so the point is the led point. A unit that authors `Fires_Forward` yes fires along its facing. An unauthored `Fires_Forward` defaults to no (debug build, OW-01); absent `Turret_Rotate_Extent_Degrees` and `Turret_Elevate_Extent_Degrees` independently default to 360 and 180 degrees (debug build, OW-02), which admit every yaw and pitch. Authored extents still apply when `Fires_Forward` is absent (debug build, OW-03): StarViper 45/45, Broadside and Marauder variants and Hutt Marauder 30/40, Slave I 50/50, TIE Prototype and Moldy Crow 20/40. Hutt IPV1 and the prologue boarding shuttle have neither extent and retain unrestricted aim. Defense satellites with object weapons also keep their authored pitch limits. G-W4 is resolved. `Turret_XY_Only` (no pitch test) is authored on no space object-weapon unit (FoC data has it once, on a ground indigenous unit) and is not loaded. |
| W-10 | **Leading the target** (research CF-01, CF-02; capital-ship fire against fighters). Every weapon with a projectile aims where its shot meets the target. The target keeps the velocity of its last frame's move (its position now minus its position a frame earlier), and the aim point moves with it. With `r` the aim point minus the shot's origin (the fire bone of W-06), `v` that velocity and `s` the projectile's `Max_Speed` per frame, the time `t` in frames solves `|r + v t| = s t`: the least positive root of `(v.v - s^2) t^2 + 2 (r.v) t + r.r = 0` (when `v.v = s^2`, `t = -r.r / (2 r.v)` if that is not negative). The led point is the aim point plus `v t`. With no such time (the target outruns the shot) the attempt fails. A target that did not move is not led. FoC predicts a turning target along an arc instead (P-06). |
| W-11 | **Cone and scatter on the led point** (research CF-01, CF-03). The weapon's pointing test (W-07, W-09) takes the led point. A hardpoint's inaccuracy (space-damage DG-24) scatters the led point, with the radius taken from the planar distance between the weapon midpoint and the aim point before the lead. A craft approaching head-on is barely led; one crossing the corvette's bow at 5.4 units per frame 700 units out is led by about 150 units, and before the capital-ship fire against fighters work the remake's corvette fired at where such a craft had been. |
| W-12 | **Missiles and torpedoes** (Acclamator firing-arc verification; debug build; recordings S-32, S-33). The projectile a hardpoint fires plays no part in its pointing test: a missile or torpedo hardpoint must point at the led point (W-07) exactly like a laser, before the projectile exists, with the same zero default for an unauthored cone and the same half-angle comparison. Only after launch does a `MISSILE`-category projectile home on its target ([MS-01 to MS-07](space-damage.md#missiles)), so homing never widens the arc. The fire bones are the unit model's own; a hardpoint's `Model_To_Attach` supplies them only when the hardpoint also names a turret bone, and no M2 weapon does. The Acclamator's torpedo launcher (`HP_Acclamator_Weapon_BC`, 130 x 130) and missile launcher (`_FC`, 130 x 90) both point along the bow, so each reaches 65 degrees either side of it and neither fires on the beam or astern; its four lasers cover the beams (the M2 arcs below). What turns the launchers onto a target is the ship: an attack order turns it bow-on (A-04, A-06). |

### Object weapon default evidence

| Evidence | Source | Observation |
|---|---|---|
| OW-01 | debug build | The type's boolean defaults initialize `Fires_Forward` to no before XML is loaded. |
| OW-02 | debug build | The type's numeric defaults initialize yaw and pitch extents to 360 and 180 degrees before XML is loaded. |
| OW-03 | debug build | Object weapon aim uses the resulting flag: yes takes the facing; no takes the led/scattered point into the unit frame and compares whole yaw/pitch extents. Omitted tags have no separate bypass. |

`unit_tables_contracts` checks the flag and independent extent defaults without game data,
then checks the affected stock types with game data. Its StarViper contract holds the facing
fixed, admits targets within the 45-degree yaw and pitch extents, rejects rearward and
out-of-cone targets, and compares state and snapshot hashes at 1/2/4/8 workers.

### M2 weapon arcs

What W-07 and W-12 give the M2 ships and stations, from FoC's `HARDPOINTS.XML` and the
fire bones of the unit models (Acclamator firing-arc verification). "Points" is the XY direction of `Fire_Bone_A`'s x axis
in degrees from the bow, positive to port; "reaches" is that direction plus or minus half the
cone width, measured from the weapon midpoint. No M2 weapon hardpoint is a turret or leaves a
cone tag out. A station's directions are relative to its own facing.

| Unit | Hardpoints | Projectile | Width x height | Points | Reaches |
|---|---|---|---|---|---|
| Acclamator | `FL`, `BL` | lasers | 160 x 90 | 35 | -45 to 115 |
| Acclamator | `FR`, `BR` | lasers | 160 x 90 | -35 | -115 to 45 |
| Acclamator | `FC` | concussion missile | 130 x 90 | 0 | -65 to 65 |
| Acclamator | `BC` | proton torpedo (light) | 130 x 130 | 0 | -65 to 65 |
| Nebulon-B | `FL`, `BL` | lasers | 175 x 160 | 80 | -7.5 to 167.5 |
| Nebulon-B | `FR`, `BR` | lasers | 175 x 160 | -80 | -167.5 to 7.5 |
| Tartan | `00` | laser | 65 x 45 | 0 | -32.5 to 32.5 |
| Tartan | `01` / `04` | lasers | 175 x 175 / 175 x 45 | 85 / 80 | -2.5 to 172.5 / -7.5 to 167.5 |
| Tartan | `02` / `03` | lasers | 175 x 175 / 175 x 45 | -85 / -80 | -172.5 to 2.5 / -167.5 to 7.5 |
| Corellian corvette | `01`, `04` | lasers | 90 x 90 | 0 | -45 to 45 |
| Corellian corvette | `02`, `05` | lasers | 160 x 90 | 0 | -80 to 80 |
| Corellian corvette | `06` / `07` | lasers | 160 x 90 / 180 x 90 | 80 | 0 to 160 / -10 to 170 |
| Corellian corvette | `03`, `08` | lasers | 90 x 90 | -80 | -125 to -35 |
| Rebel station (level 1) | `CCM` | concussion missile | 180 x 180 | -55 | -145 to 35 |
| Rebel station (level 1) | `LC` / `TBL` | lasers | 180 x 180 | -165 / 105 | 105 through 180 to -75 / 15 through 180 to -165 |
| Empire station (level 1) | `00` | concussion missile | 360 x 360 | -90 | every direction |
| Empire station (level 1) | `01` / `02` | lasers | 180 x 180 | -90 / 30 | -180 to 0 / -60 to 120 |

"Fires in any direction" therefore holds only for the Empire station's missile battery (a
360-degree cone) and, taken together, for the station and broadside lasers whose arcs overlap.
The Acclamator carries the only missile and torpedo hardpoints of the M2 ships, and both are
bow weapons.

## Project choices

| Rule | Choice |
|---|---|
| P-01 | Targeting is one partitioned phase, `targeting`, after movement and before the frame's commands ([phase map](../simulation.md#phase-map)). Each unit reads one immutable world view (moved positions and rotations, health, combat state, the last published visibility) and writes only its own state and events; the ordered commit appends events in ascending shooter ID. |
| P-02 | Synchronized draws are keyed, not one stream: a draw depends on the session seed, the frame, the unit, the weapon slot (or the ship-level scan, or the spawn) and how many draws that key made before in the frame (`CombatRandom`). Retail draws from one synchronized stream in service order, so no remake draw reproduces a retail value; timings that follow draws (first shot, recharge) differ from the recordings even where the rules agree. |
| P-03 | Both the ship-level scan and the opportunity search take their candidates in FoC's collection order from per-player trees ([space targeting](space-targeting.md) CO-01 to CO-12, target collection order and fighter approach); before the target collection order and fighter approach work it was ascending stable ID. |
| P-04 | Target-bone and hardpoint aim points turn with the unit; fixed hardpoints are oriented by their fire bone's bind frame (W-07), not its animated pose. The combat table accepts a fire frame only when it is orthonormal: every Q24 dot product of its axes within 2^-12 of 1 (an axis with itself) or 0 (two axes). FoC's 793 weapon fire frames, normalized per axis on load, stay within 22 raw units of that (WA-15). Turrets are not loaded: a turret hardpoint is treated as fixed along its fire bone. No M2 unit has one; FoC's are the Underworld L3 station cannon and the Gargantuan's turrets (research WA-12). The soft radius comes from the loaded movement footprint (AV-05), or collision half extents for synthetic profiles. `Fire_Min_Range_Distance` is not loaded and counts as zero. Since projectile, damage and shield resolution the points themselves (fire bones, target bones, hardpoints) take FoC's +90 degree model turn ([DP-03](space-damage.md#project-choices)), and whole-frame times no longer lose a frame to Q24 rounding (DP-04). |
| P-05 | Nebulae, stealth, hero clashes, abilities, AI-only overrides (R-01's recharge cut-off) are not modelled; squadrons act through craft once fighter spawning and simulation spawns them, and the fighter idle-locomotor gate holds a craft's fire while its squadron has no target ([FT-07](space-fighters.md#targets)). |
| P-06 | *Project:* the lead (W-10) is linear for every target. FoC fits a circle through the target's positions over its last three frames and solves for the meeting time along that arc numerically, falling back to the linear lead when the three points lie on a line, the target stands still, the arc's radius exceeds 1000 units or the solver fails (research CF-02). Craft in a turn are therefore led along their tangent (the fidelity list). |

## Cases

The session cases are in `tests/replay/combat_targeting_tests.cpp`, `combat_aim_tests.cpp` and
`combat_orders_tests.cpp`, with the runner in `combat_tests.cpp` (`tactical_combat_contracts`). The
targeting note's C-01 to C-08 run through the same opportunity service.

- WF-01 (S-01): a TIE Defender-like shooter with a range-700 fixed ion hardpoint sees an X-Wing at
  200 and a Y-Wing at 420. Expected: the first event acquires the Y-Wing and every shot goes to it.
- WF-02 (S-02, S-03): the fighter is acquired; a nearer bomber staged later does not take over;
  after the fighter is removed the target is empty from the next frame and the bomber is acquired
  at the first frame whose countdown reaches zero (W-01, W-02).
- WF-03: shots come in pairs 15 frames apart, and pairs 15 to 105 frames apart (W-06).
- WF-04: an attack order on the fighter makes the hardpoint fire only at it although the bomber
  ranks better, with no acquisition event; a stop order ends it (A-01 to A-03).
- WF-05: a ship-level scan that meets the fighter first takes the better-ranked bomber after it,
  and one that meets the bomber first takes the nearer fighter after it (T-07); before the first
  tree rebuild the scan meets the higher ID first (space-targeting CO-03, CO-04); nothing beyond
  `Targeting_Max_Attack_Distance`.
- WF-06: a weapon restricted from the Bomber category fires only at the fighter; a fogged enemy
  and one behind a fixed hardpoint are never fired at.
- WF-07 (weapon arcs, collision, energy and missiles): with a motion profile turning 0.75 degrees per frame, an idle shooter keeps its
  heading with a bomber on its quarter and holds fire; ordered to attack at tick 0 it plans a turn
  in place from frame 2, first turns at frame 3, ends on the bearing without moving and fires; a
  target within 10 degrees or beyond the attack distance turns nothing; a port broadside with the
  larger AI combat power turns the target onto the left beam, unless its projectile does no hull
  damage; 1, 2 and 4 workers end on the same state (A-04 to A-07).
- WF-08 (capital-ship fire against fighters): a target 700 units ahead and still is not led; closing head-on at 5 units per
  frame against 25-unit shots it is met 116.667 units nearer; crossing at 5 units per frame it is
  led 142.887 units along its path, measured from the muzzle; one that outruns the shot cannot be
  fired at (W-10).
- WF-09 (Acclamator firing-arc verification): a launcher with a 130 x 130 cone along the bow fires at a target 60 degrees off
  the bow and not at 70 degrees, on the beam or astern; a 360-degree battery fires on the beam
  and astern, and a 180-degree one reaches its beam (W-07, W-12).
- WF-10 (AI ship facing before combat, `tests/skirmish/foc_turn_tests.cpp`, `foc_ai_turn_668`, game data): the M2 battle with
  the Empire AI, its Acclamator staged at the midpoint between the stations facing the Empire
  station and the Rebel Nebulon-B 1000 units dead astern. The Acclamator takes the frigate as its
  own target and keeps its heading until the AI's first order for it. An attack order from the
  Empire player at tick 30 makes the frigate its ordered target, and it starts turning within five
  frames. 1, 2, 4 and 8 workers give the same hashes (A-05, A-08).

<a id="acclamator-launcher-arcs-s-32-and-s-33-516"></a>

### Acclamator launcher arcs, S-32 and S-33

The two [fidelity fixtures](../../tests/fidelity/README.md#acclamator-launcher-arcs-516) place an
Acclamator at `(0,-1500,0)`, facing +X, and an invulnerable, non-firing Nebulon-B 800 units off
its port beam (S-32) or 45 degrees off its port bow (S-33). Retail FoC ran 1,200 logical frames
with fog revealed and player AI suspended, as for S-22 to S-27; neither case gives an order.

| Case | Retail yaw and movement | Retail hardpoint shots in 40 s | Remake |
|---|---|---|---|
| S-32 port beam | 0 degrees throughout; no translation | `FL` 57, `BL` 30; `FR`, `BR`, the missile `FC` and the torpedo `BC` 0 | Same heading; the same two fire (54 and 30) |
| S-33 45 degrees off the port bow | 0 degrees throughout; no translation | `FL` 54, `BL` 30, `FC` 9, `BC` 4; `FR`, `BR` 0 | Same heading; the same four fire (54, 30, 9, 4) |

The launchers therefore reach a target off the bow but not one on the beam, and a missile's or
torpedo's homing does not let the hardpoint fire outside its cone (W-12). The counts follow each
side's synchronized draws (P-02); the fixtures' fire windows bound them per hardpoint.

<a id="level-1-station-arcs-s-34-to-s-42-516-follow-up"></a>

### Level-1 station arcs, S-34 to S-42

The owner asked whether a level-1 space station's hardpoints, like the Acclamator's launchers
above, can fire in more directions than the one weapon a hardpoint points at. The nine
[fidelity fixtures](../../tests/fidelity/README.md#level-1-station-arcs-516-follow-up) place
a level-1 station at a single open-space point (more than 2900 units from every one of the
Coruscant map's 58 placement records, so a 2200-range hardpoint's opportunity scan cannot pick
up a map prop as an undeclared target) and one invulnerable, non-firing Nebulon-B frigate 700
units out at one bearing per case; retail FoC ran 1,200 logical frames with fog revealed and
player AI suspended, as for S-32 and S-33. Each case uses a single target, not several: the
station's `Targeting_Priority_Set` is unauthored, so a shared multi-target scenario ties every
candidate at priority 1.0 (R-09) and the opportunity scan (R-08) can stick on whichever one it
meets first, which need not be the one nearest a given hardpoint's bore.

| Case | Bearing | Retail hardpoint shots in 40 s | Remake |
|---|---|---|---|
| S-34 Rebel, CCM/LC overlap | -100 | `CCM` 23, `LC` 54; `TBL` 0 | Same |
| S-35 Rebel, LC only | -155 | `LC` 54; `CCM`, `TBL` 0 | Same |
| S-36 Rebel, CCM only | -55 | `CCM` 23; `LC`, `TBL` 0 | Same |
| S-37 Rebel, TBL only | 70 | `TBL` 54; `CCM`, `LC` 0 | Same |
| S-38 Rebel, LC/TBL overlap | 140 | `LC` 54, `TBL` 54; `CCM` 0 | Same |
| S-39 Empire, missile+01 | -120 | `00` 23, `01` 54; `02` 0 | Same |
| S-40 Empire, missile+01 | -30 | `00` 23, `01` 54; `02` 0 | Same until the target loses a hardpoint; then `02` also fires (19), because the runner lacks `invulnerable` (scenario staging flags, below) |
| S-41 Empire, missile+02 | 60 | `00` 23, `02` 54; `01` 0 | Same |
| S-42 Empire, missile only | 150 | `00` 23; `01`, `02` 0 | Same |

Per hardpoint, in plain words: no station weapon hardpoint fires in more directions than its own
authored cone allows (W-07), same as a ship's. What is different from a single-weapon ship is
that a station carries several hardpoints aimed at different bearings at once, so *together*
they cover more of the circle than any one weapon alone.

- Rebel level-1 station: the concussion missile `CCM` (points -55 degrees, confirmed by direct
  bone decode) and laser cannon `LC` (points -165) each cover half the circle and overlap at
  -100 (S-34); the turbolaser `TBL` (points 105) covers the other side and overlaps `LC` at 140
  (S-38); no bearing tested reached all three or none, so together the three cover the whole
  circle with no gap found.
- Empire level-1 station: the concussion missile `00` is a 360 x 360 battery and fires at every
  bearing tested, same as a ship's omnidirectional hardpoint would. The lasers `01` and `02` are
  each a ship-like 180-degree arc: `01` reaches both -120 and -30 and holds at 60 and 150,
  consistent with a cone centred near -90 (the Rebel `LC`'s mirror). `02`'s fire bone points 30
  degrees to port. It reaches 60, holds at -120 and 150, and holds at -30 as well.
- Why `02` holds at -30: the station's own model carries `02`'s fire bones, the same as the
  debug build resolves them (a non-turret hardpoint's fire bones come from the parent model), so
  the cone frame is not in question. A firing attempt aims at the target's nearest hardpoint
  (W-05). For the Nebulon-B at -30 that is its engine hardpoint, 90.9 degrees off `02`'s bore
  from the weapon midpoint, just outside the 90-degree half-cone (W-07). Retail's target is
  invulnerable and keeps that hardpoint, so `02` never fires.
- The remake's scenario runner does not apply `invulnerable` (scenario staging flags). The station shoots the
  engine hardpoint away (the target's first hull loss is at tick 802), W-05 moves the aim to `BL`
  at -88.3 degrees, just inside, and `02` fires from tick 812. The remake matches FoC for as long
  as the target is intact. The test keeps `02` in S-40 as a strict expected-fail until scenario staging flags is
  fixed.

<a id="rear-arc-and-attack-order-recording-s-22-to-s-27-361"></a>

### Rear arc and attack-order recording, S-22 to S-27

The six [fidelity fixtures](../../tests/fidelity/README.md#rear-arc-and-attack-order-cases-361)
place one shooter at `(0,-1500,0)`, facing +X, and an invulnerable, non-firing target 800 units
astern or on the aft port quarter. Neither ship is held in place. Retail FoC ran for 1,800
logical frames at 30 frames/s on the Coruscant quick-load map, with fog revealed and player AI
suspended; S-26 and S-27 issue the Lua `Attack_Target` command at tick 30. Pose, ship target and
every shooter hardpoint's shot count per frame are in the private traces.

| Case | Retail yaw and movement | Retail hardpoint shots in 60 s | Remake |
|---|---|---|---|
| S-22 Tartan, dead astern, idle | 0 degrees throughout; no translation | 0 on all five | Same (A-05) |
| S-23 Tartan, 166-degree quarter, idle | 0 degrees throughout | `HP_Tartan_Cruiser_01` 92, `_04` 90; other three 0 | Same heading; the same two fire |
| S-24 Nebulon-B, dead astern, idle | 0 degrees throughout | 0 on all four | Same (A-05) |
| S-25 Nebulon-B, 162-degree quarter, idle | 0 degrees throughout | `HP_Nebulon_Weapon_FL` 70, `_BL` 91; other two 0 | Same heading; the same two fire |
| S-26 Tartan, dead astern, ordered | Turns in place from tick 33 at -0.84 degrees/frame to -179.1471 at tick 246 | All five fire; 424 total; first shot tick 41 | Turns from tick 33 at -0.84 degrees/frame to -179.1471 at tick 246; all five fire, 164 shots before it dies at tick 780 (retail 170 by then); first shot tick 42 |
| S-27 Tartan, 166-degree quarter, ordered | Turns in place from tick 33 at +0.84 degrees/frame to 166.0536 at tick 230 | All five fire; 420 total | Turns from tick 33 at +0.84 degrees/frame to 166.0537 at tick 230; all five fire, 351 shots before it dies at tick 1511 (retail 349 by then) |

The final headings are the bearings to the Nebulon-B's `HP_Nebulon_Weapon_FR` bone, the
target hardpoint nearest the Tartan (0.70 x its model offset, 40.50 units ahead of and 11.31
to the right of the frigate's centre): -179.147 and 166.05 degrees (A-04). The Tartan's
`Max_Rate_Of_Turn` 1.4 x 1.2 over the corvette slowdown of 2 gives 0.84 degrees/frame (MV-01,
MV-20). Shot counts repeat across three pinned-seed retail captures but follow retail's
synchronized draws, so they are not recharge oracles (P-02). The fixtures'
[fire windows](../traces.md#fire-windows) turn the which-hardpoints-fire column into
timing-free per-hardpoint shot-count bounds that the comparer checks in every trace. The remake's scenario runner does
not apply the target's `hold_fire` and `invulnerable` staging flags, so its target fires back
and whole-run shot totals are not comparable; which hardpoints fire and when the turn happens
are.

## Unknowns

| Gate | Unknown |
|---|---|
| G-W1 | How the fighter idle-locomotor gate and the recorder's staging frame set the first shot in S-01 to S-03 (tick 100); the remake's first shot follows its own draw. |
| G-W2 | Resolved by attack approach, attack-move and guard: movement toward a player-ordered target that is out of range follows [space orders](space-orders.md) OR-02 to OR-08 (research AT-01, E452-03 to E452-12); not recorded (OR-U4). Turning toward an in-range ordered target is A-04 to A-07; the earlier caller survey (WA-07 to WA-11) missed the targeting service's turn, which S-26/S-27 confirm. Ordered Nebulon-B, moving ships and broadside (A-06 non-zero) turns are not recorded. |
| G-W3 | FoC's hardpoint choice for an attack ranks the target's destroyable, live hardpoints by the attacker's targeting set's `Hard_Point_Priorities` (a listed type ranks by its place in the list, lowest first; an unlisted type ranks after every listed one; a tie goes to the hardpoint nearest the attacker; research AT-11, debug build, retail time-to-kill comparison). Only the `*_Attack_Move` sets and `Bomber_Hit_And_Run` list any, so for every other set all hardpoints tie and FoC aims at the nearest, as the remake does (S-45, tests/fidelity/README.md). The remake does not load the lists, so a unit whose set has one (attack-move, hit-and-run) still aims at the nearest targetable hardpoint as W-05 does. |
| G-W4 | Resolved: `Fires_Forward` defaults to no; absent yaw/pitch extents independently default to 360/180 degrees. Authored extents apply to an unauthored flag (W-09; debug build, OW-01 to OW-03). |
