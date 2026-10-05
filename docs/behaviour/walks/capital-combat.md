# Walk 2: capital-ship combat

## Scope

- **Objects.** Capital ships, frigates and corvettes in tactical space, and the space stations,
  which run the same code. In M2 these are the Nebulon-B, the MC80 (`Calamari_Cruiser`), the
  Corellian corvette, the Tartan, the Acclamator and the two level-1 stations
  ([m2-skirmish.md](../../../plan/phase-2/m2-skirmish.md) SK-20 to SK-24).
- **Per frame.** What such a ship does in one logical frame (1/30 s): its ship-level target, which
  target and which hardpoint its weapons get, the hull cap and the hardpoint drag, and its
  hardpoints' services.
- **Damage.** What a hit does to it: the damage routine from the gates to the hull, shields,
  hardpoints, death and the death sequence.
- **Tags.** The XML tags each of these reads.
- **Boundaries** (the other walks are listed in the tracking issue).
  - Inside a weapon's fire cycle, the projectile's flight, its collision and missiles: walk 3
    (weapons). This walk stops where a weapon hardpoint is handed a target and a hardpoint, and
    starts again where a projectile's hit reaches the damage routine (WCC-21, WCC-40).
  - Squadrons and craft: walk 1. The AI's plans and its choice of priority sets: walk 6.
    Abilities: walk 7.
  - Purchasing, reinforcements and the population cap after a death: walk 5.
  - Presentation (explosions, clones, smoke, sounds) is listed where the damage code triggers it,
    with the presentation note that owns it.

**Sources.** Every rule carries one:
- **debug build**: the FoC debug build, read under the clean-room rule. Evidence IDs WC-nn are
  opaque and their map stays private; older IDs (PD, CF, E72, HO, AU, IS, AB) are those of the
  notes cited.
- **recording**: the retail traces in [tests/fidelity](../../../tests/fidelity/README.md).
- **data**: FoC XML, effective layer.
- **unverified**: not established; see [Unverified](#unverified).

**Existing notes this walk checks.**
- [space-damage](../space-damage.md): DG, EN, MS.
- [space-hardpoints](../space-hardpoints.md): HD, HS, HR.
- [space-weapon-fire](../space-weapon-fire.md): T, A, W.
- [space-targeting](../space-targeting.md): R, CO.
- [space-orders](../space-orders.md): OR, and OR-20 to OR-27 in the open hardpoint-targeted player orders.
- [space-abilities](../space-abilities.md): AB.
- [battle-presentation](../battle-presentation.md): BP. [battle-audio](../battle-audio.md): BA.
- [space-victory](../space-victory.md): VT.
- The "Existing" column says **same**, **differs (how)** or **missing there**.
- The "Ours" column names the code and says **does**, **differs** or **lacks**.

## Entry points in the frame

FoC services every live object once per logical frame, in the object manager's service order
([WFO-12](frame-order.md): most recent service registration first; the remake uses ascending
ID, CO-11). A projectile is an object too. Its hit enters
the target's damage routine during the projectile's own service, so hits interleave with the
ships' services (HS G-H1).

Inside one ship's service FoC runs, in this order (WCC-02):
1. its movement bookkeeping and the engines-disabled check;
2. its combat-modifier timers (ion stun);
3. its delayed-damage queue;
4. its due periodic behaviours, in attachment order ([WFO-15](frame-order.md)); these include
   ship-level targeting (WCC-10 to WCC-19), the object
   weapon, shield and energy recharge (WCC-80, WCC-81), damage tracking (WCC-82) and movement;
5. its abilities;
6. its object Lua script;
7. the hull cap (WCC-30) and the hardpoint drag (WCC-31);
8. each hardpoint in `HardPoints` order: weapon, turret, repair (WCC-03, WCC-20).

The remake runs the tick as phases: movement, targeting and fire, orders, projectiles (every hit),
abilities, the frame's commands, then the unit systems (the hull cap, the drag and the shield
and energy recharge).

## Rules, in the order the game evaluates them

### Frame placement

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-01 | Each serviced object is visited once per frame. A projectile's hit applies during the projectile's service. Ordinary service-list traversal is most recent service registration first (WFO-12). | debug build WC-01; WFO-11/12 | same (HS G-H1, CO-11), with service order now settled | differs: all of a tick's hits apply in one serial projectile phase, in ascending projectile ID, after every unit's targeting and fire (session_step.cpp, projectiles phase; DG-34). |
| WCC-02 | Within a ship's service the order is steps 1 to 8 of [Entry points](#entry-points-in-the-frame). Periodic behaviours preserve attachment order: general `Behavior` then `SpaceBehavior`, authored first to last, with a due/enabled test for each (WFO-15/16). Reverse traversal of their storage does not reverse XML order. The hull cap and the drag run after every behaviour and before any hardpoint weapon of that ship. | debug build WC-01, WC-02; WFO-15/16/23 | same (HS-01); EN-03 is a project order | differs: the hull cap and the drag (unit-systems phase) run after the tick's fire and hits, so a ship's weapons fire before its own cap and drag of that frame; the behaviour order is EN-03's project order. |
| WCC-03 | Each hardpoint's service, in `HardPoints` order: nothing while the parent is being removed or the battle's setup phase runs. A weapon hardpoint whose weapon is enabled runs its weapon service (WCC-20). A turret runs its turret service. A hardpoint being repaired runs its repair (HR-01). A destroyed weapon hardpoint's weapon is disabled (WCC-61), so it no longer fires. | debug build WC-03 | same (HD-10, HR-01) | does (combat.cpp weapon service, `weapon_up`); turrets are not loaded (P-04); no repair in M2 (HR-07). |

### Ship-level target (the targeting behaviour, step 4)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-10 | A queued special attack is tried first (walk 7 owns it). | debug build WC-19 | missing there (abilities) | lacks: no M2 ability queues one. |
| WCC-11 | A unit that is not on a player order and belongs to a movement formation that has an attack target takes the formation's target and its hardpoint, when that target is suitable (WCC-25). Every weapon hardpoint then gets the same target and hardpoint as its attack target. | debug build WC-19, HO-04 | missing there | differs: an attack order gives each unit its own target (A-01); there are no formation attack targets. For M2's player orders the effect is the same. |
| WCC-12 | With no target: a unit whose opportunity fire is switched off (Lua) does nothing more. Otherwise it scans (T-05). A squadron craft's result goes to its squadron (walk 1). | debug build WC-19 | same (T-02, T-05) | does (combat.cpp `ship_target`, `scan`). |
| WCC-13 | A target is dropped at once when it is in limbo, in a hero clash, dead, being removed, stealthed from a unit that cannot target stealth, or fogged to the unit's owner. A unit with `BARRAGE` active keeps a fogged target. | debug build WC-19 | same (T-01), missing there for stealth and `BARRAGE` (not M2) | does for death and fog (`ship_target`). |
| WCC-14 | A target the unit was not ordered to attack is kept while it is suitable and either its priority is `1.0` or it is sticky (WCC-15). Otherwise the unit scans again. A result replaces the target only when the target is no longer suitable or the result is better (WCC-16). | debug build WC-19 | differs: T-03 lists stickiness but says the remake always rescans | differs: no stickiness (combat.cpp `ship_target`). |
| WCC-15 | **Stickiness.** The target's estimated time to death is (the health of its destroyable hardpoints, each at least 0, or its hull when it has none, plus its shield) over its rate of damage taken (WCC-82). No damage tracking or a zero rate gives "never". For a squadron it is the sum over its craft. A target is sticky when that time is at most the threshold: a runtime override when one is set (Lua), else the unit's type's `Targeting_Stickiness_Time_Threshold` (a squadron craft: its leader's type). Every M2 ship authors 5.0 s. | debug build WC-17, WC-18; data | differs: T-03 names the tag without the formula | lacks: the tag is loaded (`targeting_stickiness_seconds`, unit_tables_profiles.cpp) and never used. The damage rate exists only for DEFEND's script (AB-42). |
| WCC-16 | **Better target.** As T-07, but "damaged" is the displayed health at most `Health_Low_Percent_Threshold` (0.33). The displayed health is the hull fraction, or, on a type that dies with its hardpoints (HD-21) and has destroyable hardpoints, the lesser of the hull fraction and the combined hardpoint fraction (WCC-30's S/T). | debug build WC-14, WC-15, WC-16 | differs: T-07 uses the hull fraction | differs: `damaged` in combat.cpp uses the hull only. |
| WCC-17 | **Hardpoint hand-off.** After the rescan: while the unit has a target (ordered or scanned), and the hardpoint it holds for it is none or destroyed, a unit in a movement formation picks one. It takes the formation's hardpoint for that target when one stands, else the best one (WCC-18). It holds that hardpoint and hands the target with it to **every** weapon hardpoint as their attack target, which each tries before opportunity fire (W-04). A unit outside a formation hands nothing. | debug build WC-19, WC-24, HO-04 | differs: A-02 says a scanned target is never handed to the hardpoints (S-01 to S-03 are craft outside any formation). OR-23 and OR-24 (hardpoint-targeted player orders) cover ordered hardpoints only. | differs: only a player-ordered target is handed, with no hardpoint (combat.cpp weapon service: `attack = state_.direct ? ...`). The formation condition is **unverified** in play (U-1). |
| WCC-18 | **Best hardpoint.** Over the target's destroyable hardpoints that stand, whether targetable or not: each gets the priority of its hardpoint type in the unit's priority set. A type listed in `Hard_Point_Priorities` gets its 1-based place in the list. A type listed in `Hard_Point_Exclusions` is skipped. Any other type ranks after every listed one. The lowest priority wins. A tie goes to the strictly nearer hardpoint, by 3D distance from the attacker's height-adjusted position to the hardpoint's target point, first in `HardPoints` order on an exact tie. An attacker without a priority set takes the nearest standing targetable hardpoint. None of the M2 ships' default sets lists hardpoints. The AI's attack-move sets list `Shield_Generator, Engine, FIGHTER_BAY, WEAPON_LASER, WEAPON_ION_CANNON, TRACTOR_BEAM`, and `Bomber_Hit_And_Run` lists `Shield_Generator` (walk 6 owns when the AI uses them). | debug build WC-20, WC-21, WC-22; data | same as OR-24 (hardpoint-targeted player orders); A-07 and G-W3 note the priority part | differs: `ordered_aim_point` and the A-04 aim take the nearest targetable standing hardpoint. They ignore `Hard_Point_Priorities` and `Hard_Point_Exclusions` (loaded in unit_priority.cpp, unused in the sim) and include targetable hardpoints that are not destroyable. |
| WCC-19 | **Turning toward the target**: A-04 to A-07, aiming at the held hardpoint's target point when there is one. | debug build WC-19, AT-01 to AT-13 | same | does (combat.cpp `face_target`, with WCC-18's difference). |

### Weapons (the interface to walk 3)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-20 | A weapon hardpoint's service tries its attack target first. That target is the one handed by WCC-11, WCC-17 or a player order, with its hardpoint. On a failure it goes on only with `Allow_Opportunity_Fire_When_Targeting`. Without an attack target it fires only with `Allow_Opportunity_Fire_When_Idle` (R-01 to R-15). Walk 3 owns everything inside. | debug build WC-19, HO-06 | same (W-04) | does, with WCC-17's difference. |
| WCC-21 | **A shot's aimed hardpoint.** A weapon given a hardpoint aims at that hardpoint's position and nowhere else, and its projectile carries that hardpoint. Without one it aims at the target's nearest hardpoint that is targetable and stands, measured from the shooter, and carries that one. When there is none it aims at the R-10 point and carries no hardpoint. | debug build WC-23, HO-07 | same (W-05, R-13, OR-25) | does (combat.cpp `attempt`; the shot's `target_hardpoint`). |
| WCC-22 | **A hardpoint's own damage.** A hardpoint's `Projectile_Damage`, when above zero, replaces the projectile's `Projectile_Damage`. Its `Damage_Type` replaces the projectile's damage type (DG-12). In M2 the MC80's hardpoints author it: `HP_Calamari_Cruiser_Weapon_FL/FR` 40 on `Proj_Ship_Ion_Cannon_Large` (20), and `_BL/_BR/_ML/_MR` 20 on `Proj_Ship_Turbolaser_Red` (15). A `BLAST` charge multiplies it (not M2). | debug build WC-23; data | differs: DG-25 says no M2 hardpoint authors one | differs: the hardpoint's `Projectile_Damage` is not read; statuses.json: `HardPoint/Projectile_Damage` todo, combat tag coverage (legacy EAWR-650). The MC80's turbolasers do 15 instead of 20 and its ion cannons 20 instead of 40. |
| WCC-23 | A shot's travel limit is the hardpoint's `Fire_Range_Distance` (else the projectile's `Projectile_Max_Flight_Distance`), plus the target's soft radius, plus its squadron's for a craft target, plus the height between the aim point and the fire bone. | debug build WC-23 | same (DG-23) | does, with soft radii zero (P-04). |

### Hull cap and hardpoint drag (step 7, once per service)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-30 | On a type that dies with its hardpoints (HD-21), with at least one destroyable hardpoint and not dead: the hull fraction is capped at min(1, S/T + `Hull_Vs_Hard_Points_Health_Constraint` (0.2)). S and T are the current and maximum health of the destroyable hardpoints. S/T is 0 once all are destroyed. | debug build WC-01, E72-06, E72-07; recording S-45 (hardpoint hit routing: every hull drop sits on this cap) | same (HS-02) | does (durability.cpp `service_durability`), at the time WCC-02 notes. |
| WCC-31 | Then the drag: with L = min(1, H/M + C), X = S - T·L. When X > 0, each standing destroyable hardpoint loses X·sᵢ/T through the hardpoint damage path (WCC-60). The drag never destroys the last hardpoint. Then each hardpoint is serviced (WCC-03). | debug build WC-02 | same (HS-03, HS-04) | does (`service_durability`). |

### A hit: the damage routine, in order

A projectile's hit, a delayed hit and scripted damage (the Lua `Take_Damage`, HD-30) all enter here.

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-40 | **Gates.** Nothing happens when any of these holds: the type is immune to damage; the unit is in limbo; objects are globally set not to take damage (a debug switch); **a victory is pending**; the unit is flagged immune; its health is already 0; or the damage is 0 or less. After a battle is decided, nothing takes damage during the 210 frames before it ends (VT-11). | debug build WC-04 | differs: DG-02 mentions a pending victory only as a damage scale | lacks: the session keeps applying hits after the deciding destruction until `end_tick` (session_step.cpp projectile commit). |
| WCC-41 | **Scales.** In order: the target's take-damage unit-mode multiplier; the shooter's cause-damage multiplier; base-shield vulnerability; the diminishing-firepower curve (DG-05); the under-construction multiplier; the retreat multiplier. | debug build WC-04, PD-07 | same (DG-01, DG-02, DG-05) | does DG-05; the rest are 1 in M2 (DG-02). |
| WCC-42 | **The aimed hardpoint decides.** When the projectile's original target is this unit and it carries an aimed hardpoint (WCC-21), the damage goes to that hardpoint's `Collision_Mesh`, whatever mesh the projectile met. Otherwise the met mesh decides. On land, a hit with no mesh goes to the hardpoint nearest the shooter. | debug build WC-04, WC-13 | differs: DG-11 routes by the mesh met; DG-39 in hardpoint hit routing (hardpoint-directed damage routing) adds this rule | lacks on the integration branch. Hardpoint hit routing (`aimed_routes`) implements it. |
| WCC-43 | **Lucky shot** (`LUCKY_SHOT`): scales the hit and may move a hull hit to the nearest hardpoint. No M2 unit has it. | debug build WC-04 | missing there (listed by hardpoint hit routing) | lacks; not M2. |
| WCC-44 | **Combat modifier**: (1 + the shooter's damage bonuses) × (1 − the target's defense bonuses). | debug build WC-04, CF-05 | same (DG-26) | does (DG-26 term only). |
| WCC-45 | An active ability of the target may take the whole hit (it becomes 0). | debug build WC-04 | missing there (abilities) | lacks; no M2 ability does (walk 7 to confirm). |
| WCC-46 | **The shield armor multiplier.** For a projectile hit on a unit with a shield, base shield or energy pool, the damage is multiplied by the `Damage_To_Armor_Mod` of the damage type (WCC-22) against `Shield_Armor_Type`. Scripted and delayed damage use the default damage type when a shield is there. | debug build WC-04 | same (DG-06, DG-12, DG-20) | does (damage.cpp `apply_hit`). |
| WCC-47 | **Base shield, then shield.** The shield takes nothing when any of these holds: the type is shielded only while deployed and is not deployed; the projectile has no `Projectile_Does_Shield_Damage`; the unit is in an ion storm (which also ends `DEFEND`); or the depletion effect runs (each hit adds `Depleted_Shield_Damage_Increment`, DG-09). Otherwise it absorbs min(damage, shield). A shield that absorbed anything flashes (`Shield_Flash_Scale`, `Shield_Flash_Duration`). The shield mesh collides only while the shield is above 0 and neither depleted nor in an ion storm (BP-19). | debug build WC-12; BP-19 | same (DG-06, DG-08, DG-09, BP-19, BP-21); DG-38 omits the depletion condition | does the absorption; differs on the mesh: projectiles.cpp lets the `shield` mesh collide whenever the shield is above 0, also while the depletion effect runs and the shield has recharged up to 0.25 (G-4). |
| WCC-48 | **Energy pool.** A projectile with `Projectile_Does_Energy_Damage` drains min(damage left, energy) from the pool. A drain above 0 disables the engines for `Projectile_Disable_Engines_Duration` when the projectile has `Projectile_Disables_Engines_When_Power_Drained`. | debug build WC-04, IR-02, IR-15 to IR-20 | same (EN-07 to EN-09) | implemented: `unit_combat.cpp` carries the flags and duration; `session_step_combat.cpp` passes energy damage to `apply_hit` and starts an engine-disable deadline only on positive drain. MC80 and upgraded-station ions share the drain path without requesting engine disable; B-wing and TIE Defender small-ion shots request it. See the [ion inventory](../space-damage.md#inventory-of-the-starting-fleet-and-production-roster). |
| WCC-49 | What is left is divided by the shield armor multiplier. A multiplier of 0 or less restores the amount from before the shield stage. | debug build WC-04 | same (DG-07) | does. |
| WCC-50 | Then × the `Damage_To_Armor_Mod` against `Armor_Type`. A projectile without `Projectile_Does_Hitpoint_Damage` does 0 from here. | debug build WC-04 | same (DG-10) | does. |
| WCC-51 | **Where it lands.** A mesh name that names a hardpoint's `Collision_Mesh` (ASCII case ignored, first in `HardPoints` order) selects that hardpoint. A **destroyable** hardpoint with health takes it all, and the hull nothing (WCC-60). A destroyable hardpoint that is **destroyed** makes the hit do nothing, to the hull as well. A hardpoint that is not destroyable, or no hardpoint, sends it to the hull (WCC-53). | debug build WC-04, WC-05, WC-13 | same (DG-11, HD-02) | does (session_step.cpp hit commit, durability.cpp `apply_damage`). |
| WCC-52 | A hit that brings a destroyable hardpoint to 0 kills the unit when its type dies with its hardpoints (`Should_Be_Destroyed_When_All_Hardpoints_Destroyed`, default yes) and no destroyable hardpoint stands. The hull goes to 0 and the kill runs (WCC-70). | debug build WC-04, IS-05 | same (HD-21) | does (`apply_damage`). |
| WCC-53 | **Hull.** The hull loses the damage. A unit that cannot be killed (Lua) keeps max(1, its hull before). Crossing 75 % or 50 % of the maximum hull adds the type's `Damaged_Smoke_Asset_Name` (no M2 ship authors one). The damage-state art index is updated. At 0 the unit is killed (WCC-70). | debug build WC-08 | same (HD-20) | does; the smoke and the art index are not M2. |
| WCC-54 | **After the hit.** The hull and shield the hit took go to the unit's damage tracking (WCC-82), and its attacker to the AI's threat lists (walk 6). A unit whose displayed health crosses `Health_Critical_Percent_Threshold` or `Health_Low_Percent_Threshold` plays its type's health warning to its local owner, or the attacker's type's enemy-damaged warning to the attacker's local owner. The minimap shows a damage icon for 3 frames (walk 8). | debug build WC-04, WC-18 | missing there | differs: damage is tracked only for units running `DEFEND`'s script (session_abilities.cpp `track_damage`, AB-42); no health warnings (statuses.json: `GameConstants/Health_Critical_Percent_Threshold` todo). |

### Hardpoint damage and loss

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-60 | A destroyable hardpoint with health loses the damage. Health stops at 0, and damage beyond it is lost. Each change goes to the destruction check. A station losing a hardpoint may drop a base level (not M2, SK-31). | debug build WC-05, WC-06 | same (HD-02, HD-03) | does. |
| WCC-61 | **What losing one switches off**, when it reaches 0: its gameplay effect goes off. The last engine permanently disables the engines and ends `TURBO` and `SPOILER_LOCK`. The last shield generator takes the shield off-line and ends `DEFEND`. A weapon makes the AI recompute its directed firepower. A special-ability hardpoint switches its ability off. A gravity well generator switches off. `Allows_Special_Weapon_Use` clears the allies' special-weapon flag. Fighter bays are read at launch time (HD-13). | debug build WC-06, WC-07 | same (HD-10 to HD-15); gravity well and special weapon missing there (not M2) | does (durability.cpp, damage.cpp `shield_generators_lost`). |
| WCC-62 | **Its art.** The hardpoint's death explosion plays. Its `Death_Breakoff_Prop` is created, owned by the neutral player, unless the health-setting path suppressed it. Its attached model is removed, so its collision meshes stop colliding (DG-38). Its `Damage_Decal` and the proxies under its `Damage_Particles` bone are shown. The engine proxies are hidden when `Engine_Death_Hide_Engine_Particles`. | debug build WC-06 | same (BP-15, BP-30, BP-41, BP-42, DG-38) | does (viewer battle_effects.cpp, debris_props.cpp, unit_emitters). |
| WCC-63 | **Its sound**, to the local owner of a unit that is still alive, and not for a self-destruct (WCC-71): when no weapon hardpoint of any kind stands, `SFXEvent_Hardpoint_All_Weapons_Destroyed`. Otherwise, when none of this type stands, the type's `SFXEvent_Hardpoint_Destroyed` entry for it (the Nebulon-B, Acclamator and MC80 author lines such as `Unit_Lost_Laser...`). | debug build WC-06; data | missing there (BA-17 has only the explosion) | lacks. |

### Death

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-70 | **The kill, in order.** The unit is marked dead and its hull set to 0. It is detached. The final blow goes to the killer's owner (the shooter's player, or a given player), and to the squadron for a craft. The player-elimination check runs. The unit leaves the shield and missile-shield registries. Its type joins the mode's killed-types list. Its hardpoints self-destruct (WCC-71). It is destroyed (WCC-72). Its death clone is made (none for an "eaten" death), with its damage type overridden. One of its `Death_Projectiles`, drawn at random, is fired from its height-adjusted position (no M2 ship authors one). Its `SFXEvent_Unit_Lost` plays to its local owner. | debug build WC-09 | same in outline (HD-20, SP-02 for craft) | differs: the kill leaves the session at once (HD-20) and the viewer plays the clone (BP-14). No unit-lost sound; final blow and killed types are walk 5's and the battle end's. |
| WCC-71 | **Hardpoints at death.** The engine and turbo emitters are hidden. Every destroyable hardpoint that still **stands** is destroyed through the hardpoint damage path with its full health (WCC-60 to WCC-62), without its sound: its explosion, its breakoff prop, its model removed. Every destroyable hardpoint **already destroyed** plays its death explosion again. | debug build WC-10 | missing there (BP-30 says "the damage path always does this") | lacks: a hull kill publishes only `unit_destroyed`, so the viewer throws no breakoff props and plays no hardpoint explosions for a ship that dies with hardpoints standing (durability.cpp `apply_damage`, debris_props.cpp listens to `hardpoint_destroyed`). |
| WCC-72 | **Removal.** The destroy step gives a skirmish reinforcement back and updates the population cap (walk 5). It schedules the death clone. It ejects contents and survivors (no M2 ship has any). It removes the object. | debug build WC-11 | same (HD-20) | does removal; the rest is walk 5's. |

### Over time

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-80 | Shield recharge. | debug build PD-08, PD-26 | same (DG-13 to DG-17, EN-04) | does (damage.cpp `recharge_shields`). |
| WCC-81 | Energy pool recharge and costs. | debug build PD-22 to PD-24 | same (EN-01 to EN-06) | does. |
| WCC-82 | **Damage tracking**, serviced every 30 frames (AB-42). It sums the hull and shield each hit took (WCC-54). At each service the rate becomes that sum × 30 / the interval, per second, and the sum restarts. Each attacker's threat drops by (maximum hull + maximum shield) × `AI_Space_Threat_Decay_Step` and leaves at 0 or less (walk 6). Every M2 ship has it. It feeds WCC-15 and `DEFEND`'s script (AB-41). | debug build WC-18, AB-R12 | same (AB-42) | differs: only units running `DEFEND`'s script track damage (session_abilities.cpp `track_damage`). |

### Suitability (used by WCC-11 and the scans)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WCC-25 | A target is suitable when its type is a valid target and can be hit by a projectile (a squadron is judged by itself), and it is not in limbo, dead, in a transport or in a hero clash. A victory-relevant special structure is suitable only as a star base. | debug build WC-25 | same (T-06, R-08) | does (`ship_suitable`). |

## Gaps against our code

| Gap | Rules | Kind | Impact on the M2 battle |
|---|---|---|---|
| G-1 | WCC-42 | lacks: routing by the aimed hardpoint | High: the hull dies with hardpoints standing (hardpoint-directed damage routing). **hardpoint hit routing** implements it; no new ticket. |
| G-2 | WCC-17 | differs: a scanned target and a hardpoint are not handed to the weapon hardpoints | High: in FoC every weapon of a ship in a formation fires at one hardpoint of its ship-level target. Ours spreads the weapons over opportunity targets, so they focus less and pick other hardpoints. The retail time-to-kill comparison tracks this gap (legacy EAWR-536). |
| G-3 | WCC-22 | differs: a hardpoint's `Projectile_Damage` is ignored | High for the MC80: its turbolasers do 75 % and its ion cannons 50 % of FoC's damage. The retail time-to-kill comparison tracks this gap (legacy EAWR-536). |
| G-4 | WCC-47 with BP-19 | differs: the shield mesh collides while the depletion effect runs (the shield regains up to 0.25) | Medium: such hits absorb nothing and land on the hull through the `shield` mesh (DG-11), another hull leak besides G-1 (hardpoint-directed damage routing). |
| G-5 | WCC-14, WCC-15, WCC-82 | lacks: target stickiness and damage tracking for every unit | Medium: in FoC a ship finishes a target that is about to die instead of switching. The retail time-to-kill comparison tracks this gap (legacy EAWR-536). |
| G-6 | WCC-18 | differs: the best hardpoint | Medium: the hardpoint priority lists of the AI's sets are ignored, and targetable but not destroyable hardpoints are chosen. |
| G-7 | WCC-40 | lacks: no damage while a victory is pending | Low to medium: a ship can die in the 7 s after the battle is decided; in FoC nothing does. |
| G-8 | WCC-71 | lacks: the hardpoints' self-destruct at death | Low (presentation): no breakoff props or hardpoint explosions when a ship dies with hardpoints standing. |
| G-9 | WCC-16 | differs: "damaged" is taken from the hull, not the displayed health | Low: the hull may sit up to 0.2 above the hardpoints' share (WCC-30), so FoC counts a ship that has lost most of its hardpoints as damaged before ours does. |
| G-10 | WCC-48 | resolved: drain and temporary engine disable | EN-07 to EN-09 cover the expanded production roster. Exact movement-replan latency remains unverified in space-damage G-D2. |
| G-11 | WCC-54, WCC-63, WCC-70 | lacks: the hardpoint-lost, unit-lost and health-warning sounds | Low (audio). |
| G-12 | WCC-01, WCC-02 | differs: order within the frame | Low: the cap and the drag lag a ship's fire by a frame; HS G-H1 and EN-03 hold it on the fidelity list. No ticket. |

Tickets, under the tracking issue capital-ship combat rule walk (legacy EAWR-707):

| Gap | Work |
|---|---|
| G-1 | hardpoint-directed damage routing (hardpoint hit routing) |
| G-2 | capital-ship target propagation (legacy EAWR-698) |
| G-3 | hardpoint projectile-damage overrides (legacy EAWR-699) |
| G-4 | depleted-shield collision gating (legacy EAWR-700) |
| G-5 | target stickiness and damage tracking (legacy EAWR-701) |
| G-6 | best-hardpoint priority and distance selection (legacy EAWR-702) |
| G-7 | pending-victory damage gate (legacy EAWR-703) |
| G-8 | dying-ship hardpoint self-destruction (legacy EAWR-704) |
| G-9 | displayed-health target scoring (legacy EAWR-705) |
| G-10 | ion weapons, energy drain and stun (legacy EAWR-561) |
| G-11 | local loss and health-warning sounds (legacy EAWR-706) |

Totals over the 45 rules. **Same: 30**:
- WCC-03, -12, -13, -19, -20, -21, -23, -25;
- WCC-30, -31, -41, -44, -46, -48, -49, -50, -51, -52, -53;
- WCC-60, -61, -62, -72, -80, -81;
- WCC-10, -11, -43 and -45, the same in effect: ours lacks only what no M2 unit uses;
- WCC-70 in outline.

**Differs: 11**: WCC-01, -02, -14, -15, -16, -17, -18, -22, -47, -54, -82.

**Missing in ours: 4**: WCC-40, -42 (in hardpoint hit routing), -63, -71.

### Tags this subsystem reads that statuses.json marks todo or deferred

All are todo in the combat tag report (legacy EAWR-650) unless marked otherwise.

| Tag | Rule | Note |
|---|---|---|
| `HardPoint/Projectile_Damage` | WCC-22 | G-3 |
| `HardPoint/Death_Breakoff_Prop` | WCC-62 | read by the viewer, not by a traced loader |
| `HardPoint/Allows_Special_Weapon_Use` | WCC-61 | not M2 (no superweapons, SK-35) |
| `GameConstants/Health_Critical_Percent_Threshold` | WCC-54 | G-11 |
| `GameConstants/Base_Shield_Vulnerability_Modifier`, `Base_Shield_Delay_Time` | WCC-41, WCC-47 | not M2 (no base shield) |
| `GameConstants/Under_Construction_Damage_Multiplier` | WCC-41 | not M2 (no construction) |
| `GameConstants/Ion_Storm_Shield_Disable_Time` | WCC-47 | not M2 (no ion storms) |
| `GameConstants/First_Strike_Extra_Damage_Percent` | not reached | no path of this walk reads it (walk 3 or 6 to check) |
| `GameConstants/Override_Death_Persistence_Duration` | WCC-72 | death presentation |
| `GameConstants/Damage_Types`, `Armor_Types` | WCC-46, WCC-50 | the pair table is read from `Damage_To_Armor_Mod` (DG-12); the lists themselves are not |
| `SpaceUnit/Remove_Upon_Death`, `SpaceStructure/Remove_Upon_Death`, `SpaceUnit/Death_Leave_Hulk_Behind` | WCC-72 | read by the viewer (unit_clips) |
| `SpaceStructure/Death_Projectiles` | WCC-70 | a prop in the scene; no M2 ship or station |
| `SecondaryStructure/Shield_Points`, `Shield_Refresh_Rate`, `Shield_Armor_Type`, `Armor_Type`, `Energy_Capacity` economy tag coverage (legacy EAWR-654), `Energy_Refresh_Rate`; `SpaceStructure/Shield_*`, `Armor_Type`, `Energy_*` | WCC-46, WCC-80 | variant or parent objects in the scene whose values the loaded types override; to confirm under combat tag coverage (legacy EAWR-650) |
| `SpaceUnit/Damage`, `Squadron/Damage`, `StarBase/Damage`, `Container/Damage` | none | an autoresolve value, not tactical damage (unverified; combat tag coverage (legacy EAWR-650) to confirm) |

## Unverified

| Id | Question | What would settle it |
|---|---|---|
| U-1 | **When is a ship in a movement formation** (WCC-11, WCC-17)? Only the formation code sets it: when units join, merge or are cleaned up. Unknown: whether a ship keeps its formation after its move ends; whether a freshly spawned ship or one moved only by the AI has one; whether a lone ship ordered to move gets one. | Debug build: the formation clean-up's callers and when it runs. Retail capture: a staged Nebulon-B and Acclamator, once with no order and once after a move order, each fighting an enemy frigate. Log each weapon hardpoint's aimed hardpoint per shot. The behaviour recorder already logs hits by mesh; add the shot's target hardpoint. |
| U-2 | The objects' service order within a frame (WCC-01). | Debug build: the object manager's service list, and where new objects join it. |
| U-3 | The behaviour list order of the M2 ship types (WCC-02). It decides whether targeting runs before or after shield and energy recharge within a frame. | Debug build: the behaviour creation from `Behavior` and `SpaceBehavior`, in the order the XML lists them. |
| U-4 | Whether any M2 ability takes a whole hit (WCC-45). | Walk 7. |
| U-5 | What the `Damage` tag on units and squadrons is (tag table). | Debug build: the xref from the tag string. |

## Cases for the tickets

- **G-2**: a Nebulon-B in a formation against an Acclamator. Its four lasers take the same held
  hardpoint (the nearest destroyable standing one) until it is destroyed, then the next.
- **G-3**: an MC80 turbolaser hit on an unshielded target does 20 × the armor multiplier. An ion
  cannon hit on a shield does 40 × the shield multiplier.
- **G-4**: a frigate whose shield is depleted and recharged to 0.25 during the effect. A laser
  crossing its shield mesh passes through and meets the hull or hardpoint mesh behind it.
- **G-5**: a ship whose current target will die within 5 s at the observed rate keeps it when a
  better-priority target enters range. At 6 s it switches.
- **G-7**: after the deciding station dies, a projectile in flight hits a ship. Its hull and
  shield are unchanged.
