# Walk: unit abilities in a space battle

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. This is walk 7 of the
  coordinator's list (the subsystem walk of 2026-09-30). It covers every ability an M2 space unit
  can use, per frame and per event:
  - switching on and off, and the effects while on;
  - duration, expiry and recharge;
  - autofire, for the player and the AI;
  - the targeted ion shot, the behaviour ability `HUNT`, and the interactions with orders,
    stuns, shields and engines.
  It lists every rule in evaluation order and gives the gaps against the remake. It also answers
  the owner's question about MC80 shield-boost duration (legacy EAWR-670).
- Sources:
  - **debug build**: the FoC debug executable with symbols, read under the clean-room rule.
    The evidence IDs (EAB-nn) are opaque and their map stays private.
  - **data**: the FoC XML and Lua.
  - **recording**: the fidelity traces.
  - **unverified**.
- Most of this subsystem was already read, rule by rule, in [space abilities](../space-abilities.md)
  (AB), [space damage](../space-damage.md) (IS, DG) and [space hardpoints](../space-hardpoints.md)
  (HD). This walk re-reads their order in the frame and cites them instead of repeating them.
- Out of scope, recorded as an interface:
  - the command bar, the buttons, hotkeys, the card marks and the world ability icon (walk 8,
    [ability buttons](../foc-ability-buttons.md), ability-button eye check, retail ability-state staging (legacy EAWR-678));
  - the AI plans' choice of when to run a plan (walk 6);
  - the weapon, shield, energy and movement services that read the multipliers (walks 2 to 4);
  - land abilities, hero abilities, and the nebula and ion-storm cancels (no M2 map has
    either).

## Scope

- **Objects.** A unit type authors at most a primary and a secondary unit ability (AB-01). Each
  unit carries per ability:
  - whether it is on;
  - whether it is on autofire;
  - a countdown.
  The countdown runs one way while the ability is on (its expiration) and the other way while
  it recharges.

  A squadron's team container holds the team ability `ION_CANNON_SHOT` (AB-60). A behaviour
  ability (`HUNT`) switches a behaviour of its own on and off.
- **The M2 roster's abilities** (data; AB-02, AB-60):

  | Unit | Ability | Expiration / recharge | Multipliers while on | Autofire | Script |
  |---|---|---|---|---|---|
  | Nebulon-B | `DEFEND` | 15 s / 60 s | weapon delay 1.0, shield regen 1.0, shield interval 0.1, energy regen 3.0, energy interval 0.1, speed 0.8 | yes | `ObjectScript_PowerToShields` |
  | MC80 (`Calamari_Cruiser`) | `DEFEND` | 15 s / 40 s | as the Nebulon-B | yes | `ObjectScript_PowerToShields` |
  | Corellian corvette | `TURBO` | 20 s / 50 s | weapon delay 3.0, shield regen 0, energy regen 1.0, speed 2.0 | no | none |
  | Tartan | `POWER_TO_WEAPONS` | 7 s / 60 s | weapon delay 0.2, shield regen -25, energy regen 1.0, speed 0.5 | no | none |
  | Acclamator | `POWER_TO_WEAPONS` | 20 s / 60 s | weapon delay 0.5, shield regen -3, energy regen 1.0, speed 0.5 | no | none |
  | X-wing (craft; the squadron passes the command on, AB-15) | `SPOILER_LOCK` | none / none | weapon delay 3.0, shield regen 3.0, energy regen 3.0, speed 1.3 | no | none |
  | TIE fighter, TIE interceptor (squadron and craft) | `HUNT` | none / none | none | no | none |
  | Y-wing squadron (team container) | `ION_CANNON_SHOT` | none / 20 s | override projectile `Proj_Ion_Cannon_Medium_Laser_Blue` | yes | none |
  | TIE bomber, the stations | none | | | | |

- **Cadence.** Ability commands act in the frame they are processed:
  - the player's clicks;
  - the Lua `Activate_Ability` of object scripts and AI plans;
  - the engine's own switch-offs (expiry, depletion, stun, engine loss, a new order).

  The following are serviced each frame:
  - the ability countdown (every frame; it counts the frames elapsed since its last service);
  - the ion shot's per-frame check (AB-64).

  The following run on their own intervals:
  - the damage-rate service (30 frames, AB-42);
  - the `DEFEND` object script (once a second, AB-41);
  - `HUNT`'s behaviour (30 frames).
- **Entry points, in words.**
  - The unit's special-ability switch takes the ability, on or off and an optional target. It
    checks the gate, switches the ability, starts or cancels the countdown and applies the
    ability's own effect: power modes, emitters and clips, the ion shot's lock-on, or a
    behaviour.
  - The countdown behaviour services every ability of the unit each frame.
  - The ability signals (ready, finished) go to the unit's AI TaskForce, which turns them into
    plan events.

## Rules, in evaluation order

### Switching on and off (in the frame of the command)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-01 | **Who switches.** An ability command may come from three sources:<ul><li>the player: the command bar, a hotkey or the autofire toggle, walk 8;</li><li>a Lua script: `Activate_Ability(name, on)` or `(name, target)`, AB-44 and AB-69;</li><li>the engine itself: WAB-12, WAB-40, WAB-60 to WAB-63.</li></ul>A squadron container passes a non-team command to its craft (AB-15). A team ability is switched on the team container (AB-60). The engine never switches an ability **on** by itself. Every automatic use comes from a script (WAB-30 to WAB-34) or from the ion shot's autofire (AB-68). | debug build EAB-11; AB-10, AB-15 |
| WAB-02 | **The gate.** An ability switches on only when it is ready and off (AB-11, AB-13), and only while its kind's gate holds:<ul><li>`DEFEND` needs shields online and outside their depletion effect, and no stun (AB-14, IS-07);</li><li>`TURBO` and `SPOILER_LOCK` need engines online (AB-16);</li><li>the ion shot needs a valid target (AB-62).</li></ul>A refused switch changes nothing. | debug build; AB-11 to AB-16, AB-62 |
| WAB-03 | **The expiration countdown.** Switching on an ability whose `Expiration_Seconds` is above 0 starts its countdown in the *expiration* direction. The countdown's length is E = trunc(`Expiration_Seconds` x 30) frames (AB-04) and its count starts at 0. An ability without `Expiration_Seconds` has no countdown and stays on until it is switched off (`SPOILER_LOCK`, `HUNT`, the ion shot). | debug build EAB-02 |
| WAB-04 | **What switching on does**, by the ability's kind:<ul><li>**Power modes** (`DEFEND`, `TURBO`, `POWER_TO_WEAPONS`, `SPOILER_LOCK`): the unit's multipliers change from this frame (WAB-20). A move already under way is planned again (AB-24).</li><li>The shield and energy recharges come due at once (`DEFEND`, AB-43).</li><li>Emitters and clips switch (AB-30 to AB-32).</li><li>**The ion shot** locks onto its target (AB-63).</li><li>**A behaviour ability** (`HUNT`, WAB-50): the unit's attack target is cleared, the team's attack target is cleared, the behaviour's service is enabled and the countdown starts (`HUNT` has none). `HUNT` keeps the move under way; the other behaviour abilities, none of them in M2, also stop it.</li></ul> | debug build EAB-11; AB-20 to AB-24, AB-30 to AB-32, AB-43, AB-63 |
| WAB-05 | **Switching off early.** A player click on an active ability, a script or an engine switch-off (WAB-60 to WAB-63) runs the ability's off path: the multipliers revert, and the emitters and clips swap back. When the expiration countdown is still running, its completion c = 1 - count / E becomes the recharge share: the recharge runs trunc((1 - c) x R + 0.5) = round(count / E x R) frames (AB-12, with R from WAB-11). An ability without a running countdown recharges nothing. A behaviour ability disables its service, stops the unit's move and cancels a running countdown. | debug build EAB-02, EAB-04, EAB-11; AB-12 |

### The ability countdown (every frame, per unit)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-10 | The countdown is serviced every frame. For each ability it takes the frames elapsed since its own last service (1 in steady play, never negative). Because it counts elapsed frames, a skipped service loses no time. The game speed does not change it: a logical frame is 1/30 s of game time at every speed (TR-02). | debug build EAB-01, EAB-12 |
| WAB-11 | **Expiration.** While the ability is on, the count rises by the elapsed frames, up to E. In the service where the count reaches E, the ability **expires**:<ul><li>it switches off (on a team container, through its team leader);</li><li>the local player hears the faction's "ability off" sound (walk 8);</li><li>the recharge starts;</li><li>the ability-finished signal goes to the unit's AI TaskForce (WAB-34).</li></ul>The recharge length is R = trunc(`Recharge_Seconds` x (1 + the unit's ability-recharge modifiers) x 30) frames. The modifiers are the sum of the `Ability_Recharge_Bonus_Percentage` of the combat-bonus abilities that affect the unit. Only the Underworld's land upgrades author one (-0.15), so every M2 unit has R = trunc(`Recharge_Seconds` x 30). A recharge of 0 or less starts no recharge (FoC shows a data error). Every M2 ability recharges after it expires: the ability type table exempts only self-destruct. | debug build EAB-01, EAB-03, EAB-04, EAB-05; data |
| WAB-12 | **Where the timer starts and what it counts.** The expiration countdown starts in the frame the ability switches on and counts logical frames. It counts nothing else: no damage, no shield level, no game speed. `DEFEND` therefore runs E = 450 frames, 15.0 s of game time, on the MC80 and the Nebulon-B alike. The count gains its first frame in the first countdown service after the switch:<ul><li>when activation precedes the countdown service, the count reaches 450, and `DEFEND` ends, 449 frames after the switch;</li><li>otherwise it ends 450 frames after the switch.</li></ul>WFO-02/17 settles the ordinary admitted-frame command-before-countdown branch; object-script/late AI activation must use its actual caller position. Exact remake/original endpoint normalization remains frame-order UFO-02. | debug build EAB-01, EAB-02; WFO-02/17/21/22/31 |
| WAB-13 | **Recharge.** While the ability recharges, the count falls by the elapsed frames, clamped at 0. When it reaches 0 and the ability is enabled, the ability-ready signal goes to the unit's AI TaskForce (WAB-34). The ability is ready from that frame (AB-13). | debug build EAB-01 |
| WAB-14 | **Completion for display.** Each service also computes the timer's completion (1 - count / length) for the command bar's dial (AB-50, walk 8). | debug build EAB-01 |

### Effects while on (read by the other services each frame)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-20 | The multipliers of the unit's active abilities multiply per kind (AB-20). The other services read them:<ul><li>the weapon delay (AB-21);</li><li>shield regeneration and its interval (AB-22);</li><li>energy regeneration and its interval (AB-23);</li><li>speed, acceleration and deceleration (AB-24, AB-33).</li></ul>No M2 ability changes the diminishing-firepower flag (AB-25). The other ability kinds FoC authors are outside the roster and are refused at load (AB-26). No M2 unit has two abilities, so no two power modes combine in M2. | debug build; AB-20 to AB-26 |
| WAB-21 | While the ion shot is on, the Y-wing craft fire only the override projectile, only at the target (AB-66 and AB-67). | debug build; AB-66, AB-67 |

### Autofire and scripts

| Rule | Behaviour | Source |
|---|---|---|
| WAB-30 | Autofire is a per-ability flag that a command sets only on an ability with `Supports_Autofire` (AB-40). The flag switches nothing by itself. The engine reads it only for the ion shot (AB-68). Scripts read it through `Is_Ability_Autofire`. | debug build; AB-40, AB-68 |
| WAB-35 | Newly created local units inherit the player's creation preference, enabled for a fresh profile; only abilities with `Supports_Autofire` are armed. Initial and mid-battle creation use the same rule. This supplies the human branch of WAB-31 without a manual activation command; AI shield use and plan-owned power modes retain their separate triggers. | debug build AF-R01 to AF-R04; AB-45, AB-46 |
| WAB-31 | **The `DEFEND` object script** (`ObjectScript_PowerToShields`, `ServiceRate` 1 s; the Nebulon-B and the MC80) switches `DEFEND` on when all of these hold:<ul><li>the damage rate (WAB-32) is above 20.0;</li><li>`DEFEND` is ready;</li><li>the owner is not human, or it is human and `DEFEND` is on autofire.</li></ul>A human owner's unit without autofire never uses it. The script never switches `DEFEND` off (AB-41). | data; AB-41 |
| WAB-32 | The damage rate is the hull and shield lost over the last 30 frames, per second, serviced every 30 frames per unit (AB-42, AB-U1). | debug build; AB-42 |
| WAB-33 | **The AI plan library**. Every space TaskForce's default handlers use abilities through FoC's own library (`Try_Ability`, `Use_Ability_If_Able`). `Try_Ability` first draws a difficulty chance, 100 % at every difficulty. The uses:<ul><li>**Damage taken.** When a TaskForce unit is damaged, it tries `DEFEND` when its shield is below 0.8 (after `INVULNERABILITY` below 0.2, which no M2 unit has). The missile and laser defences it then tries don't exist in M2.</li><li>**A fighter shot on purpose by a fighter** switches `SPOILER_LOCK` off and attacks back.</li><li>**A unit about to die** (it would die within 20 s, or its hull is low as the library defines) switches `Power_To_Weapons` off before it looks for repair.</li><li>**The end of a diversion** switches `Turbo` and `SPOILER_LOCK` off.</li><li>**Plans** switch `Turbo` on for their moves: `turboattack`, `turboattacklocation`, `areasweep`, `spacescout`. `bombingrun`, `areasweep` and `movetolocationrush` use `SPOILER_LOCK`.</li></ul>No script uses `HUNT`, and no M2 plan uses `ION_CANNON_SHOT`. | data (scripts/library, scripts/ai/spacemode) |
| WAB-34 | **Plan events from the ability signals.** The TaskForce turns a member's ability signals into plan events:<ul><li>`Unit_Ability_Ready` when a recharge counts down to 0 (WAB-13), and when a nebula's ability disable ends (not M2). An ability switched off in its first frame recharges 0 frames and sends none. `turboattack` and `turboattacklocation` switch `Turbo` back on on this event while their move still needs it. The library's handler switches back on a remembered cancelled `SPOILER_LOCK` or `Turbo`.</li><li>`Unit_Ability_Finished` when a timed ability expires (WAB-11) and when the ion shot ends (AB-65). The TaskForce's own `Activate_Ability` also sends it at once for each member whose switch fails, so a plan blocking on the command does not wait. An early switch-off of a power mode sends none. The library's handler does nothing.</li><li>`Unit_Ability_Cancelled` only when a nebula disables the unit's abilities. It never fires in M2, so the library's recovery of a cancelled ability never runs there.</li></ul> | debug build EAB-01, EAB-04, EAB-12, EAB-14; data |

### ION_CANNON_SHOT (targeted, per frame)

The table above describes the starting fleet. The expanded production roster also includes
the TIE Defender squadron and its team container, which author `ION_CANNON_SHOT` with the
same override projectile and a 25-second recharge (the Y-wing's is 20 seconds). See the
[ion inventory](../space-damage.md#inventory-of-the-starting-fleet-and-production-roster)
for their ordinary ion weapons as well. The targeted shot and the ordinary energy-draining
shots have separate projectile flags; the ability's stun does not drain energy.

| Rule | Behaviour | Source |
|---|---|---|
| WAB-40 | The targeted ion shot runs as AB-60 to AB-69 describe: switch-on with a target, lock-on, the per-frame check, one bolt per craft, the recharge only when a craft fired, and autofire. In addition, **a new order ends it**: when the squadron receives any of these, the ion shot switches off on the team container (AB-65):<ul><li>a move (plain or queued);</li><li>an attack order;</li><li>an escort or guard order;</li><li>a Lua `Move_To` or `Attack_Target` on a unit.</li></ul>It recharges only when a craft had already fired (AB-65). The re-order of AB-63 only follows the squadron's attack target while no new order came. | debug build EAB-06; AB-60 to AB-69 |

### HUNT (behaviour ability; the TIE fighter and interceptor squadrons)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-50 | `HUNT` has no expiration, no recharge and no autofire. It is on from the player's switch-on until one of two things ends it:<ul><li>the player switches it off;</li><li>an order ends it (WAB-51).</li></ul>While on, the unit's hunt behaviour is serviced every 30 frames, and every frame while the unit is on fire. | data; debug build EAB-07, EAB-10, EAB-11 |
| WAB-51 | **Orders end it.** Each of these switches an active `HUNT` off:<ul><li>a player's move, plain or queued, including a move with a requested facing;</li><li>an attack order;</li><li>an escort or guard order;</li><li>ordinary Stop, from the command button or its hotkey;</li><li>a Lua `Move_To` or `Attack_Target` on a unit.</li></ul>Stop follows the normal ability-off path (WAB-05), ending the patrol and clearing its attack/chase targets. An order addressed to a squadron member ends Hunt on the whole team; a rejected order leaves it unchanged. `HUNT`'s own moves (WAB-55) pass a flag that keeps it on. Low-level movement stopping is separate from this player order. | debug build EAB-06, EAB-14; project (rejection contract) |
| WAB-52 | **Who acts.** The service acts only on a squadron's team leader, or on a unit outside a squadron; for a team container it takes the leader. It does nothing when the unit's service is disabled. | debug build EAB-07 |
| WAB-53 | **When it acts.** The unit is idle when either holds:<ul><li>its locomotor is in the fighter-idle (or walk-stopped) state;</li><li>it is not moving and plays an idle animation.</li></ul>It is then no longer idle if any of these holds:<ul><li>its formation holds more than one queued destination;</li><li>it has an attack target;</li><li>it is on an uninterruptible move.</li></ul>An idle hunter picks a destination (WAB-54) and moves there (WAB-55). A hunter that has a target fights it through the squadron's own targeting and dogfights ([squadrons](squadrons.md)). | debug build EAB-07 |
| WAB-54 | **The destination.** The area is the map's bounds, or the tactical camera's bounds when there are any. A map without bounds keeps the unit where it is. All draws use the synchronized game random stream, in this order:<ol><li>A float in [0, 1). For a unit that is not force sensitive (every M2 TIE), when the draw is above 1 - 0.5 (0.5, a code constant), it samples up to 99 points.<ul><li>The area is cut into 100 x 100 cells (100, a code constant). The cells are walked along the diagonal, both indices stepping together, and one random point is drawn in each cell, x then y.</li><li>The points fogged for the owner are kept. If any are, one is drawn uniformly and becomes the destination.</li></ul></li><li>Otherwise it looks at up to 128 of the owner's enemy units with a locomotor. It draws uniformly among those fogged for the owner, else among all of them. A force-sensitive hunter would prefer force-sensitive enemies; there are none in M2.</li><li>With no such enemy it draws a point in the area, x then y. When that point is more than 1000 or less than 500 units away, it becomes the unit's position plus the direction to it times a random distance in [500, 1000] (code constants). The result is clamped inside the area, less half the unit's `Space_FOW_Reveal_Range` on every side.</li><li>For a unit that is not force sensitive, the destination then gains a random 30 to 400 units in x and another in y. Its height is the unit's own.</li></ol> | debug build EAB-08, EAB-13 |
| WAB-55 | **The move.** A squadron's leader moves the whole squadron: every member and the container, as one coordinated group move to the position with movement type 1 (the attack-move kind of [space orders](../space-orders.md); squadrons FO-06). The group move does not end `HUNT` (WAB-51). A lone unit moves alone the same way. | debug build EAB-09 |
| WAB-56 | The next service after the move finds the squadron moving, so it waits. On arrival (idle again) it picks a new destination. Enemies met on the way are engaged by the attack-move's own rules (walk 4, WMV-15 to WMV-17). | debug build EAB-07; the movement walk |

### LASER_DEFENSE (the Crusader gunship's point defence)

The Underworld Crusader (build list from station level 2, garrison from level 2) authors `LASER_DEFENSE` in
`Unit_Abilities_Data` and a nested `Laser_Defense_Ability`. It is a user-input special ability: the player switches it on
with its button (hotkey in [ability buttons](../foc-ability-buttons.md)), and for an AI owner the Crusader's own object
script switches it on (WLD-10). While it is on, the Crusader shoots missiles and rockets of other, non-allied owners
out of the air. The debug build was read for the handler's switch-on, per-frame service, target search, shot-down
action and its two Lua type queries (ELD-01 to ELD-09); the XML is the Crusader's data
(`units_space_underworld_crusader_gunship.xml`). Ability kind number 18.

| ID | Rule | Evidence | Ours |
|---|---|---|---|
| WLD-01 | `LASER_DEFENSE` is bound by name to ability kind 18 and its nested handler by `Laser_Defense_Ability` (named by the ability's `GUI_Activated_Ability_Name`). The unit ability authors `Recharge_Seconds` 60, `Supports_Autofire` False and an `Effective_Radius` that this handler never reads; the handler authors the run, reach and look timing (WLD-02 to WLD-04). `Must_Be_Bought_On_Black_Market` Yes does not gate it in a multiplayer tactical game, skirmish included (WHE-38). | data; debug build ELD-01 | **applied**: `abilities.cpp` (`ability_kind`, `special_ability_kind`), `unit_tables_profiles.cpp` (nested read), `unit_abilities.cpp` |
| WLD-02 | Switching on needs no target (a missing target or the Crusader itself). It sets the run counter to trunc(`Defense_Duration_In_Secs` x 30) = 1050 frames, zeroes the look counter and starts the ability's countdown (WLD-06). The time limit of the ordinary slot is that run. | debug build ELD-02; data | **applied**: `unit_abilities.cpp` sets the slot's expiration from the handler; `validate_abilities` refuses a handler that disagrees |
| WLD-03 | The handler services once per logical frame while its run counter is above zero. Each service first removes the beam when the counter is a multiple of `Laser_Beam_Frames` (WLD-12), then looks if the look counter is below 1 and reloads it with trunc(`Recharge_Time_In_Secs` x 30) = 15, else decrements it; the run counter then falls by one and at zero the ability terminates. A look therefore comes at the first service after the switch-on and every 16 services after it (`Recharge_Time_In_Secs` 0.5). Within the frame the service follows the command; the remake's first service is the frame after the switch-on (**unverified**: the exact frame order, WAB-12 class). | debug build ELD-03 | **applied**: `session_abilities.cpp` `service_laser_defence`, called at the head of the projectile phase |
| WLD-04 | A look scans the object manager's projectiles in its order and takes the first whose owner differs from the Crusader's owner and is not that owner's ally, whose type's projectile category is one of `Projectile_Types_Targeted`, whose distance flown is strictly more than `Projectile_Dist_Travelled` (100) and whose squared 3D distance from the Crusader's position (no height adjustment) is at most `Protection_Radius` (500) squared. Neutral owners are not allies, so their missiles are shot down too. The Crusader's own side's shots are never taken. The remake takes the first match in ascending projectile ID. | debug build ELD-04; data | **applied**: `service_laser_defence`, `projectile_defence_category` (a homing shot is a MISSILE, a rocket-path shot a ROCKET; MPTL_ROCKET is a land projectile with no space shot) |
| WLD-05 | A match is shot down at once: the zap sound starts on the Crusader (`Zap_SFXEvent`), the beam is created, the projectile detonates through its ordinary explosion (blast area damage, detonation particle and sound as for any detonation) and the object is destroyed. No direct damage is dealt, and one shot goes per look. Two Crusaders that look in the same frame never take the same shot: the later one reads the list past it. | debug build ELD-05 | **applied**: the zapped shot gets the explicit explosion request (WAD-05), so the projectile phase of the same frame removes it and delivers its blast and detonation event; a conflict re-reads the list in unit order |
| WLD-06 | The ability's countdown (`Recharge_Seconds` 60) starts at the switch-on, not at the end of the run, and runs while the ability is on. A switch-off (the button, the run's end, the owner's death) terminates the handler (WLD-07) and changes nothing about the countdown. The ability can be switched on again when the countdown has run out. | debug build ELD-02, ELD-06 | **applied**: `recharge_at_activation` in `activate_ability`, `deactivate_ability`, `expire_abilities`; the snapshot dial shows the countdown |
| WLD-07 | Terminating removes the beam and zeroes both counters. | debug build ELD-07 | **applied**: terminating is the ordinary switch-off; the handler keeps no state beyond the slot (the look phase derives from the switch-on tick) |
| WLD-08 | The projectile categories of the Crusader's list are `MISSILE`, `ROCKET` and `MPTL_ROCKET`. A handler that lists another category stays out of the ability table until the category is modelled (AB-26). | data; debug build ELD-04 | **applied**: `unit_tables_profiles.cpp` |
| WLD-09 | The Lua type queries are fixed sets: `Is_Affected_By_Laser_Defense` is true for MISSILE, ROCKET and MPTL_ROCKET; `Is_Affected_By_Missile_Shield` is true for ROCKET and MPTL_ROCKET only. A type that is not a projectile answers false. (The older note that the missile shield takes missiles (legacy EAWR-785) is wrong.) | debug build ELD-08 | **applied**: `tactical_ai_bindings.cpp` |
| WLD-10 | AI use has two sources. The plan library's damage response tries `LASER_DEFENSE` after the missile shield when the attacker's current projectile type is affected (WTA-34). The Crusader's object script (`ObjectScript_PointDefense`) updates once a second: for a non-human owner, with the ability ready, it takes the deadly enemy of the Crusader (DT-03) and switches the ability on if any projectile type that enemy fires (its standing hardpoints' types, then its object weapon's) is affected; for a human owner it follows the ability's autofire flag, which a `Supports_Autofire` False ability never sets. | debug build ELD-08, ELD-09; data | **applied**: `Engine::service_point_defence` stands in for the script; `Get_Current_Projectile_Type`, `Get_All_Projectile_Types` and both type queries are bound (WTA-34's stubs are gone); the object script's once-a-second phase is spread over objects by ID (**unverified**) |
| WLD-11 | The button, its hotkey, the icon and the activation and deactivation voice lines come from the ordinary ability presentation (BA-50, BA-52); nothing is special to this ability. | data; BA-52 | **applied**: generic by ability kind |
| WLD-12 | Presentation: the beam is a simple additive line from the model bone nearest the shot (the handler's `Bone_Names`, re-chosen as the Crusader moves) to the shot's position at the zap, `Laser_Beam_Width` wide, `Laser_Beam_Texture`, `Laser_Color` (bytes), drawn for `Laser_Beam_Frames` logical frames; the zap sound is the handler's `Zap_SFXEvent` on the Crusader. The `Lightning_Effect_Name` particle is not drawn (its placement is unread: **unverified**). | debug build ELD-03, ELD-05; data | **applied**: `battle_effects_projectiles.cpp` (beam), `battle_audio_events.cpp` (sound); the lightning effect is `presentation-later` |

Determinism and work: the look runs in a partitioned phase over the units with the ability on, reading the start-of-frame
projectile list, and writes only that unit's pick; the commit is ordered by entity ID. The zap event
(`point_defence_zap`) is presentation and adds no canonical bytes; the shot's removal and its blast are canonical. A
Crusader looks 2 times a second at most, each look one pass over the projectiles in flight.

### STEALTH (the Vengeance frigate's cloak; also the TIE Phantom and the Peacebringer)

The Underworld Vengeance frigate (station level 3 build list) authors `STEALTH` in `Unit_Abilities_Data`
(`Expiration_Seconds` 60, `Recharge_Seconds` 15, on and off voice lines) and a nested `Stealth_Ability`
(`User_Input`, `Stealth_Transition_Time` 0.5, `Stealth_Color` 255,255,255,255). The Empire's TIE Phantom craft and
squadron (80 s, 10 s, transition 0.8) and the Peacebringer (180 s, 10 s, 0.3) author the same pair, so the same rules
serve them. The debug build was read for the nested handler's switch-on, per-frame service, detection scan and end,
the unit-ability switch, the stealth visibility test and every reader of it, the weapon-fired, stun and ion-stun
callers that end it, and the GameConstants default (EST-01 to EST-12). Ability kind number 20.

| ID | Rule | Evidence | Ours |
|---|---|---|---|
| WST-01 | `STEALTH` is bound by name to ability kind 20 and its nested handler by `Stealth_Ability`, named by the ability's `GUI_Activated_Ability_Name`. `Must_Be_Bought_On_Black_Market` Yes does not gate it in a multiplayer tactical game, skirmish included (the Underworld abilities scope note (legacy EAWR-1876); WHE-38), so the Vengeance, the TIE Phantom and the Peacebringer cloak in skirmish. | data; debug build EST-01 | **applied**: `abilities.cpp` (`ability_kind`, `special_ability_kind`), `unit_tables_profiles.cpp`, `unit_abilities.cpp` |
| WST-02 | The ability is a switch. Switching on starts the ordinary expiration countdown (WAB-03) and then applies the named handler (WST-03); a type without the handler, or without a time limit, has nothing to apply. Switching the active ability off cancels the countdown with the share-of-duration recharge (WAB-05) and ends the cloak (WST-05). | debug build EST-02 | **applied**: `unit_abilities.cpp` keeps a cloak without its handler or time limit out of the table (AB-26); `validate_abilities` refuses one |
| WST-03 | **Switching on.** The handler counts trunc(`Expiration_Seconds` x m x 30) frames, where m is 1 plus the owner's battlefield `Stealth_Duration_Multiplier` modifiers (the Underworld cloaking-generator upgrades author 1.25 and 1.5); it sets the fade's time to `Stealth_Transition_Time` and its target to 1, marks the cloak on and its detection frame 0, releases the unit's current target, clears its hardpoints' targets and sets its formation's destination to the end of its current path, or its position when it has none: an attack order ends and a move under way goes on. It hides the `BASEMESH` sub-object and the hardpoints and shows the `STEALTH` sub-object. | debug build EST-03 | **applied**: `stealth_switched_on` (as HUNT's switch-on, WAB-04); the modifiers are **not applied** (m = 1): battlefield modifiers are not modelled (registry `todo`) |
| WST-04 | The handler services every logical frame while its count is above zero: the count falls by one and the cloak terminates when it reaches zero; otherwise it runs the detection scan (WST-08) and keeps the model's shadow volumes off. They come back once the fade has returned to zero. | debug build EST-04 | **applied**: the cloak ends with its slot's time limit (`service_abilities`), the same frames with m = 1; the scan runs in the ability phase |
| WST-05 | **What ends it.** The time limit; a switch-off (WST-02); and any weapon the unit fires: the first shot ends the cloak with a 0.3 s fade, and the ability's own countdown runs on to its end, so the button stays on until then and recharges as after an expiry (or by share after a switch-off). Damage does not end it. | debug build EST-05, EST-06 | **applied**: the weapon-fired commit (`session_step_targets.cpp`) and the switch-off clear the cloak; the slot is left to its countdown |
| WST-06 | A projectile's area ion stun ends an active cloak; an ordinary stun ends it only while it hides the unit. | debug build EST-06 | **applied**: `ion_stun_unit` ends it (the only stun M2 has, IS-01) |
| WST-07 | `Target_Stealth_Units` marks the types that see and shoot cloaked enemies: in space the A-wing, the TIE Scout, the Underworld sensor satellite and IG-2000. | data; debug build EST-07 | **applied**: `CombatProfile::target_stealth` |
| WST-08 | **Detection.** Each service scans the object list in its order for the first object that is alive, not a death clone, not about to be deleted, not a projectile, has a model, and is an enemy of the cloaked unit's owner, whose 3D distance to the cloaked unit is strictly less than its detection distance: its type's `Space_FOW_Reveal_Range` when the type targets stealth units (whether or not it has `REVEAL`), else 0. The first such object sets the detection frame to the current frame and ends the scan. | debug build EST-08 | **applied**: `detect_stealth`, in ascending unit ID, partitioned per cloaked unit; it reads the other units' positions, owners and types, which the phase does not write |
| WST-09 | **Hidden.** A unit is stealth-invisible while its cloak is on and its detection frame plus trunc(`Stealth_Detection_Time` x 30) is before the current frame. `Stealth_Detection_Time` is a GameConstants value the FoC data does not author; the debug build defaults it to 1.0 s, so a detection shows the unit for 30 frames after the detector leaves. The test is the same for every player. | debug build EST-09 | **applied**: `stealth_invisible`, `stealth_detection_frames` (the default, cited; a mod's own value is not read) |
| WST-10 | **The cloaked unit.** While its cloak is on it finds no targets: its hardpoints' opportunity scan (R-05) and its ship-level scan return nothing, the latter before it sets its next scan frame. An explicit attack order still fires, and that shot ends the cloak (WST-05). | debug build EST-10; space-targeting R-05 | **applied**: `scan`, `service_weapon` (`parent_suppressed`) |
| WST-11 | **Its enemies.** A stealth-invisible enemy is not a suitable target for a unit whose type cannot target stealth units: hardpoints and ship-level targeting drop and skip it (WCC-13, R-08), a firing attempt at it fails (WWP-13) and no projectile is created. An attack order on it is refused for such a unit of another owner. | debug build EST-11 | **applied**: `visible_to_me`; the order is refused with `target_hidden` (reject reason 19); a squadron is hidden when every live craft is; a squadron of a type without Target_Stealth_Units neither keeps nor acquires a hidden enemy as its target (an approach in flight ends too) |
| WST-12 | **Presentation of the cloak.** While on, `BASEMESH` and the hardpoints are hidden and the `STEALTH` sub-object (a `MeshShield.fx` surface on the Vengeance) is drawn instead, for every player who sees the unit; shadows are off. The fade blends the unit's tint from white to `Stealth_Color` (white in all FoC data). | debug build EST-03, EST-04; data | **applied**: the viewer swaps the sub-objects (`space_populate`); the tint is not drawn (white is no change). The stock `STEALTH` material names a distortion texture the FoC data does not ship; ours binds one that offsets nothing (what FoC binds for a missing texture: **unverified**) |
| WST-13 | **Hidden from enemies.** A stealth-invisible enemy fades out of the local player's view like a fogged one (WSU-03, WSU-04): no radar icon, no bars or brackets, no tooltip, no targeting reticle, no click. | debug build EST-12; WSU-08 | **applied**: `visible_entities` and the viewer's shared visible list (draw, fade, selection, minimap, economy) leave it out for a hostile player, and so does the attached-sound gate; the owner and allies keep seeing it |
| WST-14 | **Sounds.** The on and off voice lines are the ordinary ability presentation (BA-50). When a scan reveals a unit that was hidden, `SFX_Stealth_Reveal` plays as a 2D event. | debug build EST-08; data | **applied**: `battle_audio_events.cpp` |
| WST-15 | **AI use.** The stock plans, the free store and the event library call `Try_Ability(.., "STEALTH")` (diversion, kiting, retreat, scouting, moves). | data | **partial**: the name reaches the AI ability verb; the verb's bound (legacy EAWR-1891) admits kind 20 |

Determinism and work: the scan runs inside the existing partitioned ability phase, one pass over the live units per
cloaked unit per frame (as the original's object-list scan), writing only that unit's detection frame. The cloak's
state is hashed in the `STLH` block, written only while a cloak is on, so sessions without one hash as before.
Unverified: where in the frame the original's scan runs relative to targeting (here: after the combat phase, so a
switch-on hides the unit from the next frame); the cloak's frame count with battlefield modifiers.

### REDIRECT_BLASTER (the Underworld armour upgrades' deflection)

Three Underworld space carriers author a nested `Redirect_Blaster_Ability` in their `Abilities` list: the Starviper
fighter (levels 1, 2 and 3: chance 0.25, 0.30, 0.30), the Crusader (levels 2 and 3: 0.30) and the Kedalbe battleship
(level 3: 0.30). IG-88 and Urai Fen author it too, on the ground only. Every stock instance has `Redirect_Chance` 0, `Reaction_Arc_In_Degrees` 360,
`Max_Projectile_Redirection_Angle_In_Degrees` 30, `Turn_To_Face_Unblockable_Shots` No, `Initially_Enabled` No and the
shooter filter `Fighter | Bomber | Corvette | Frigate | Capital | Structure`. It is not a `Unit_Ability`: it has no button, no
duration and no recharge. The debug build was read for the handler's reaction, its data checks and the projectile
redirect routine it calls (EBR-01 to EBR-06), and for the damage routine that calls it (EBR-07).

| ID | Rule | Evidence | Ours |
|---|---|---|---|
| WRB-01 | `Redirect_Blaster_Ability` binds by name to a nested handler kind; its `Name` is what an upgrade's `Enable_Ability` refers to. | data; debug build EBR-01 | **applied**: `abilities.cpp` (`special_ability_kind`), `unit_tables_profiles.cpp` (`load_abilities`), for `SpaceUnit` carriers only: the hero and structure objects that author it (Darth_Vader, IG-88, Urai_Fen, the ground barracks and factories) are land objects and stay out of the content |
| WRB-02 | Its activation style is `Take_Damage`: it has no per-frame service and runs inside the damage routine of the unit a projectile hit, once per handler, in authored order, until one reports success. | debug build EBR-07 | **applied**: no service interval; the reaction runs in the hit delivery of the projectile phase |
| WRB-03 | The reaction comes after the combat damage modifier (the cause and take multipliers) and before the armor, shield and hull steps. Creation alone is not success: a moving defender still launches the reflected projectile but reports failure, so direct damage remains and no block sound plays. Stationary success cancels direct damage; the rest of the routine runs as for a zero hit. | debug build EBR-07 | **applied**: `session_step_combat.cpp` (`impacts`), after the cause and take multipliers and before the damage redirect and `apply_hit`; a deflected shot delivers no direct damage and its area blast follows as for other cancelled hits |
| WRB-04 | The data check refuses a chance outside 0 to 1, an arc outside 0 to 360 and an angle outside 0 to 180: it reports it and resets the value (the angle is clamped). Defaults: chances 0, arc 0, angle 180. A handler with neither `Applicable_Unit_Categories` nor `Applicable_Unit_Types` reacts to nothing. | debug build EBR-05, EBR-06 | **applied**: `load_abilities` reports an out-of-range value and clears it to zero (the angle is clamped); `validate_abilities` refuses a profile outside the ranges |
| WRB-05 | A handler reacts only while enabled. `Initially_Enabled` sets the start state (an absent tag is taken as on, **unverified**). An upgrade object's `Enable_Ability` (`Ability_Name`, `Applicable_Unit_Types`, `Affects_All_Allies`) enables the named handler on those types for the upgrade's holder and, with the flag, its allies. The three stock levels enable their own level's handler (a higher level replaces the lower one, `Destroy_Previous_Upgrade_Level`), so one handler is on at a time. | data; debug build EBR-02 | **applied**: the enabled state is derived at hit time from the held upgrade objects in the economy ledger (`redirect_blaster_handlers`); it is not stored. **Unverified**: whether the debug build also switches the handler off when the holder station is lost (ours does, with the held object). The upgrade entries in the Underworld menus become purchasable. |
| WRB-06 | The shooter must match the handler's `Applicable_Unit_Types` and `Applicable_Unit_Categories` (a listed type, else a listed category that is not excluded). The shot's projectile category must not be MISSILE, GRENADE, ROCKET, MPTL_ROCKET, LIGHT_SABER, BOMB or CLUSTER_BOMB. | debug build EBR-03; data | **applied**: `redirect_blaster_handlers` (filter on the shooter's categories), `unit_combat.cpp` (`redirect_blockable` from the projectile's category). A shot whose shooter has died is not deflected. The launch profile is retained independently of the new shooter and its weapon slots. |
| WRB-07 | The reaction arc is the part of the circle the handler reacts to: with margin m = (360 - arc) / 2, a shot is ignored when the defender's facing minus the shot's facing, wrapped to plus or minus 180, lies strictly between -m and m. At arc 360 the margin is zero and every shot is reacted to. If `Turn_To_Face_Unblockable_Shots` is on, the idle unit also turns by that difference (**unverified**; every stock space handler has it off). | debug build EBR-03 | **applied**: `redirect_blaster_reaction`; the turn is **not modelled** (registry: `todo`) |
| WRB-08 | After creating the replacement, the movement-animation branch reports failure and skips sound and animation. Otherwise the handler plays `SFXEvent_Activate` at the unit and starts a block animation when idle or after its last block finishes. | debug build EBR-03; data | **applied**: the hit event carries `hit_outcome_redirected`; the viewer plays the sound and shows no impact. The movement gate uses an active movement path; the idle block animation and blending state are **not modelled**. |
| WRB-09 | Two rolls, in this order, each a uniform draw in [0, 1): first redirection against `Redirect_Chance` times the shot type's redirect modifier, then block against `Block_Chance` times its block modifier. The redirect roll is always drawn, also at chance 0. A roll below its limit is a success only if the projectile redirect routine then succeeds. The modifiers default to 1 (a ground hero-clash projectile is the only stock type that sets them). | debug build EBR-03, EBR-04 | **applied**: `redirect_blaster_reaction`; successive enabled matching handlers consume one progressing roll stream per hit; angle/pitch use a separate progressing stream. Disabled/nonmatching handlers consume no draws. The modifiers are taken as 1. |
| WRB-10 | A redirection creates a fresh projectile of the same type, owned by the defender's player and fired by the defender, from the old shot's position, aimed at the shooter's position extrapolated over the flight time, with the old shot's damage and damage type, the type's speed and range, and a redirected internal damage type that skips the misc gate (WPJ-42). A block creates it the same way but faces it back along the old facing plus 180 degrees, plus a uniform spread of plus or minus the handler's angle, and half the time (a uniform draw below 0.5) a pitch drawn uniformly in [-60, 30] degrees. | debug build EBR-04 | **applied**: `redirect_blaster_reaction` through `redirect_projectile` (WPJ-42); the immutable launch type/weapon/override key survives every reflection and resolves both the simulation profile and viewer appearance; the shooter's previous position is its current one, so a redirection leads a moving shooter by nothing (**unverified** effect: no stock handler redirects). |
| WRB-11 | Deterministic and worker-independent: the draws are keyed by (seed, tick, the old shot's ID, dedicated roll or direction slot, successive draw index), the deflections commit in the flights' ascending order, and the new projectile takes the next ID of the shared counter. | project policy | **applied**: `session_step_combat.cpp`; the test checks hash equality at 1/2/4/8 workers |
| WRB-12 | Presentation: the block sound plays attached to the unit; the deflected shot leaves no impact effect; the fresh projectile draws as any projectile of its type. | data | **applied**: `battle_audio_events.cpp`, `battle_effects_impacts.cpp` |

### Interactions (engine switch-offs)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-60 | A shield set to 0 or below ends an active `DEFEND` early (AB-17). So does losing the last shield generator (HD-12); in M2 only the stations have one, never the MC80 or the Nebulon-B. | debug build; AB-17, HD-12 |
| WAB-61 | An ion stun ends an active `DEFEND` early and keeps it from switching on (IS-07). The Y-wing's ion shot stuns (IS-01). | debug build; IS-07 |
| WAB-62 | Losing the last engine hardpoint ends `TURBO` and `SPOILER_LOCK` early (AB-16, HD-11). | debug build; AB-16 |
| WAB-63 | A new order ends `ION_CANNON_SHOT` (WAB-40) and `HUNT` (WAB-51). It never ends a power mode: a moving corvette keeps `TURBO` (AB-10). | debug build EAB-06; AB-10 |
| WAB-64 | Out of M2: a nebula cancels abilities (`Nebula_Ability_Disable_Time`), and an ion storm blocks `DEFEND`. No M2 map has either. | debug build; AB-14, AB-64 |

### BUZZ_DROIDS: Underworld StarViper squadrons, missing Underworld space abilities (legacy EAWR-1876)

Sources: debug build EAB-70 to EAB-78 (the buzz-droid ability's apply step, the droid object's
behaviour service, its victim scan and chase step, the Lua activation dispatch and the
black-market availability checks), and the StarViper and Buzz_Droids XML.

| Rule | Behaviour | Evidence |
|---|---|---|
| WAB-70 | **Holder, target and reach.** The StarViper squadron's team container holds `BUZZ_DROIDS` (its `StarViper_Team` authors the nested `Buzz_Droids_Ability`, as the Y-wing team holds the ion shot, AB-60); the craft's own copy acts only when a script addresses a single craft, which the M2 AI does not. The ability takes a **point**: the command bar's click, or an area marker the engine places at the Lua argument (WAB-76). It applies only when the holder passes the ordinary attack-range test against that point with the nested `Activation_Min_Range` / `Activation_Max_Range` (5 / 400) in place of its own: planar distance at most the maximum and more than the minimum; a point has no extent. Out of reach it does nothing (the remake refuses with `invalid_position`; whether FoC first moves the holder into reach is **unverified**, WAB-U1). `Must_Be_Bought_On_Black_Market` does not gate it in skirmish: both ability-availability checks answer available in a multiplayer tactical game before they look at purchases (WHE-38's mode predicate). | debug build EAB-70, EAB-77, EAB-78; data |
| WAB-71 | **Apply.** In the activation frame the holder stops moving and turns to face the point (yaw only), the marker is destroyed, the full `Recharge_Seconds` (60) recharge starts and the slot is not left on (an instant ability, like WHE-61). One object of the nested `Object_Type` (`Buzz_Droids`) is created at the point for the holder's owner, unturned. It copies the nested `Chase_Radius` (300), `Damage_Radius` (175), `Enemy_Damage_Per_Second` (23) and `Own_Damage_Per_Second` (3), keeps the point as its creation point, and is armed at the activation frame plus trunc(`Activation_Time_In_Seconds` x 30) = 30 frames. Each unit of a group command creates its own droids. The droid type has no locomotor behaviour (`DUMMY_STARSHIP`), so it never joins the AI free store and takes no orders. | debug build EAB-70; data |
| WAB-72 | **Droid service, from the arming frame on, every frame.** First the droids take `Own_Damage_Per_Second` / 30 miscellaneous damage from themselves through the ordinary damage interface (`Buzz_Droids` has `Tactical_Health` 20: about 200 frames, 6.7 s, of life after arming, less under fire). Then every victim in reach (WAB-73) takes `Enemy_Damage_Per_Second` / 30 miscellaneous damage, aimed at the collision mesh of its hardpoint nearest the droids (the hull without one). Before the arming frame the service does nothing. | debug build EAB-71; data |
| WAB-73 | **Victims.** For each enemy player, the collidable objects within `Chase_Radius` of the **creation point** (not the droids' current position): movers (a locomotor), not projectiles, of a category in the droid type's `Special_Weapon_Valid_Targets` (`Fighter`, `Transport`, `Corvette`, `Frigate`), alive. The aim point is the victim's hardpoint nearest the droids, else its position. A victim whose aim point is strictly closer than `Damage_Radius` is damaged; every such victim takes damage in the same frame. | debug build EAB-72; data |
| WAB-74 | **Chase.** When no victim is in damage reach, the droids move straight towards the nearest candidate's position (by its aim-point distance) by the droid type's maximum speed per frame (`Max_Speed` 3.5 x `Object_Max_Speed_Multiplier_Space` 1.2 = 4.2, MV-01), setting their position directly (no path, no turn). With a victim in reach they hold still. With no candidate they drift nowhere. | debug build EAB-73; data; MV-01 |
| WAB-75 | **Presentation.** The droid object is drawn and revealed as an ordinary unit of its type (`p_b_droids.alo`, `Space_FOW_Reveal_Range` 800, death explosion), and is neither selectable nor a deliberate target (`Is_Valid_Target` False). In the arming frame its owner, if local, hears the nested `Activate_SFX` (`GUI_Buzz_Droids_SFX`) at the droids. The activating squadron's own cues are its unit-ability voice and target lines (BA-52 to BA-54). | debug build EAB-71; data |
| WAB-76 | **Scripts.** Lua `Activate_Ability("BUZZ_DROIDS", x)` reads `x` as a position (an object's position, or a point), places an area marker there and issues the ability event; the stock AI uses it in the free store and `pgevents.lua` with the unit itself, its attacker or its attack target (`Try_Ability(unit, "BUZZ_DROIDS", target)`). | debug build EAB-77; data |

Remake notes (fidelity list): the droids' victims are scanned in parallel per droid and their
damage commits in ascending droid ID, so a second swarm in the same frame does not see the first
swarm's kills in its scan (a kill it would also have damaged is skipped, not replaced); the
holder's yaw turn of WAB-71 is not modelled (only team containers hold the ability, whose facing
is not shown); `Find_Collidable_Objects` is read as the victim's centre within the chase radius;
the nearest-hardpoint pick admits only standing hardpoints (the engine's exclusion arguments are
not traced). WAB-U1 below.

### FULL_SALVO: the Underworld Interceptor IV frigate (legacy EAWR-1876)

Sources: debug build EAB-80 to EAB-83 (the ability's switch-on and switch-off, the hardpoint fire step's delay
factor, the hardpoint data's per-ability multiplier array and its defaults), the Interceptor IV's and its
missile hardpoint's XML, and the stock AI library. Ability kind number 19. The ability authors
`Expiration_Seconds` 7 and `Recharge_Seconds` 45 and no `Mod_Multiplier`, no nested handler and no target.

| Rule | Behaviour | Source |
|---|---|---|
| WAB-80 | **Switch-on and switch-off.** `FULL_SALVO` is an ordinary timed ability (WAB-01 to WAB-14): switching it on starts its 7-second expiration timer (WAB-03, WAB-11), switching it off or expiry cancels the timer, and the 45-second recharge follows the generic rules (WAB-10, WAB-13). Its handler does nothing else: it moves nothing, resets no weapon timer and draws no target. It shows the model's power-to-weapons emitter type while on, the same emitter switch as `POWER_TO_WEAPONS` (AB-32). | data; debug build EAB-80 |
| WAB-81 | **The weapon delay factor, per hardpoint.** A hardpoint's recharge and pulse gap (WWP-32, WWP-33) are scaled by one factor: the hardpoint's own multiplier for each of the unit's first two abilities that is on (1 when the hardpoint authors none for that ability), times the unit-level weapon delay multiplier of its on abilities (AB-21). `FULL_SALVO`'s per-hardpoint multiplier is `Full_Salvo_Weapon_Delay_Multiplier`; every hardpoint starts with 1 for every ability. As WWP-32 and WWP-33 state: the recharge after a burst is scaled only when the factor is below 1 (rounded half up), the gap between pulses is always scaled (rounded half up), so a factor of 0.18 turns a 15-frame gap into 3 frames and a 120 to 150 frame recharge into 22 to 27. The factor is read when the pulse is spent, so a countdown that is already running when `FULL_SALVO` switches on or off finishes at its old length. | debug build EAB-81, EAB-82; WWP-32, WWP-33 |
| WAB-82 | **What it reaches.** Only the Interceptor IV's missile hardpoint (`HP_Interceptor_Frigate_00`: six missiles 0.5 s apart, a 4 to 5 s recharge) authors the multiplier (0.18); its two laser hardpoints keep their ordinary cadence. Object weapons read only the unit-level multipliers (`Mod_Multiplier`), so `FULL_SALVO` does not touch them. The ability changes no damage, range, target choice or movement. | data; debug build EAB-81 |
| WAB-83 | **The AI.** The stock attack handler (`Default_Target_In_Range`) tries `FULL_SALVO` with no target each time a TaskForce member reaches its target, through `Try_Ability` (a difficulty chance, 100 % at every difficulty) and `Use_Ability_If_Able` (WAB-33): it activates when the unit has the ability, it is ready and it is not on. | data (`pgevents.lua`); WAB-33 |

Remake notes (fidelity list): the factor is a Q24 product rounded to nearest (the engine multiplies floats),
so a delay that lands exactly between two frames could differ by one frame (U-01 class). The other
per-ability hardpoint multipliers (`Defend_Mode_`, `Turbo_Mode_`, `Power_To_Weapons_Mode_Weapon_Delay_Multiplier`)
and the `Fire_When_In_*_Mode` flags follow the same array in the debug build but no M2 stock hardpoint
change is wired for them here; they stay **todo** in the tag registry.

### SELF_DESTRUCT: the Underworld Vengeance frigate and Krayt destroyer (legacy EAWR-1876, EAWR-1888)

Sources: debug build EAB-90 to EAB-96 (the behaviour ability's switch-on and switch-off, the behaviour's own per-frame
service and its start and stop effects, the spawned-object routine, the kill routine, the countdown behaviour's
expiry step and the ability type table's recharge flag, the blast object's first projectile service), the XML of both
ships and of both blast objects, the game constants' damage-to-armor rows, the Krayt's object script and the stock AI
library. Ability kind number 21. Both ships author `Expiration_Seconds` 3.5, `Recharge_Seconds` 10,
`Spawned_Object_Type` (`Vengeance_Self_Destruct_Blast`, `Krayt_Self_Destruct_Blast`) and `Stop_When_Activated` No; the
Krayt also authors `Supports_Autofire` True and the object script `ObjectScript_Krayt`. Both skirmish availability
checks pass in a multiplayer tactical game, so `Must_Be_Bought_On_Black_Market` does not gate it there, as the
Underworld abilities scope note found (legacy EAWR-1876).

| Rule | Behaviour | Source |
|---|---|---|
| WSD-01 | **Data.** SELF_DESTRUCT is a behaviour-based ability, like `HUNT` (WAB-50): its switch is the behaviour ability's switch and its effect is a behaviour of the unit. The behaviour refuses to start without an authored expiration above 0 and without the countdown behaviour; the remake refuses such a type at load. The ability needs its spawned object type; the object is a projectile type with a zero flight distance, a zero direct damage and an area blast (WSD-06). | data; debug build EAB-90, EAB-91 |
| WSD-02 | **Switching on and off.** The generic gates apply (ready and off, WAB-02; a unit whose movement is locked refuses, no M2 source locks movement). Both switches clear the unit's attack target and its team's attack target (WAB-04), but, like `HUNT`'s switch-on, **neither the activation nor the cancel stops the move under way** (every other behaviour switch does): the unit keeps moving and keeps firing at what its weapons choose. Switching on enables the behaviour's service, which starts its start sound (`Selt_Destruct_SFXEvent_Start_Die`, `Unit_ZC_Self_Destruct`), its self-destruct animation (time-aligned to end with the countdown), a red pulsing light for twice the expiration and any `Particle_Effect`, and starts the expiration countdown (WAB-03). `Stop_When_Activated` No: no stop and no movement lock (Yes would stop the move and lock movement; no M2 ship authors it). The behaviour does not start while the unit is on an uninterruptible move. | debug build EAB-90, EAB-91, EAB-92 |
| WSD-03 | **The countdown.** E = trunc(3.5 x 30) = 105 frames (WAB-03, WAB-10). Nothing else shortens or lengthens it: not damage, not a stun, not an order. The activation frame follows the ordinary command-before-countdown order (WAB-12). | debug build EAB-91, EAB-94 |
| WSD-04 | **Expiry ends the unit, not the ability.** The ability type table marks SELF_DESTRUCT as the one ability that does not recharge after expiring, and the countdown behaviour's expiry step, for an ability that does not recharge, only sends the ability-finished signal (WAB-34): it neither switches the ability off nor starts a recharge. The behaviour's own service acts in the first service that sees the countdown complete (a count at or above its length). | debug build EAB-94, EAB-95 |
| WSD-05 | **The detonation, in order.** The service (1) disables itself (the movement unlock, the light and sound stop of the cancel path run), (2) creates the spawned object at the unit's own position and facing for the unit's owner, with the unit as the object's shooter and the object's own damage type, (3) kills the unit with the miscellaneous damage type, the unit itself as the killer, so the final blow and the destruction credit go to the unit's **own owner**. The kill is the ordinary one (WCC-70): the unit leaves the battle at once, its hardpoints self-destruct (WCC-71, not modelled), its death clone plays and its `SFXEvent_Unit_Lost` plays to its local owner. No recharge starts, because the unit is gone (WSD-04). | debug build EAB-92, EAB-93 |
| WSD-06 | **The blast object.** A projectile (`PROJECTILE`, `HIDE_WHEN_FOGGED`), `Projectile_Max_Flight_Distance` 0 (an instant explosion), `Projectile_Damage` 0, area damage `Projectile_Blast_Area_Damage` / `Projectile_Blast_Area_Range` **900 / 500 (Vengeance)** and **1500 / 700 (Krayt)**, no dropoff, `Max_Secs_For_AE_Delayed_Damage` 0 (no delay by distance), damage type `Damage_StarBase_Self_Destruct`, shield damage Yes, hit-point damage Yes, energy damage No. The game constants list that type's damage-to-armor rows (1 for the default, fighter, bomber, transport and capital armors, 1.25 to 1.5 for several frigate and corvette armors). The detonation sound is `SFX_Plex_Missile_Detonation`, its particle `Friggin_Huge_Explosion_Space`. The object explodes at the position it was made, in its first projectile service (WAD-04, WAD-05), the frame after the kill in the remake; the area rules WAD-08 to WAD-29 apply without change: the shooter is gone when it lands, so the range factor reads the unmodified range (WAD-10). | data; game constants; debug build EAB-93, EAB-96 |
| WSD-07 | **Friendly fire and credit.** The blast queries the owner's **enemies only** (WAD-08): the unit's own units, its allies and neutrals are never hurt, and no category filter exists (WAD-30), so enemy fighters, bombers and capital ships inside the radius all take it. Kills it makes belong to the unit's owner (the object carries its owner and its dead shooter, WAD-23). | debug build EAB-93; WAD-08, WAD-23 |
| WSD-08 | **Cancel.** A switch-off before the expiry runs the generic off path (WAB-05): the countdown's completed share becomes the recharge share, round(count / E x R) with R = trunc(`Recharge_Seconds` x 30) = 300 frames, and the ability is refused while it recharges (WAB-13). The service stops, the light changes to a flash for the remaining time and the start sound stops. A scripted or player cancel is the only way back; no order and no AI plan cancels it. | debug build EAB-90, EAB-92 |
| WSD-09 | **Who uses it.** The stock AI library never calls SELF_DESTRUCT (no plan, no freestore and no `pgevents` handler names it), and the Vengeance authors no object script, so the AI never starts it on a Vengeance. The Krayt's object script (`ObjectScript_Krayt`, service rate 1, in space only) acts for its owner: **an AI owner** below half hull, with the threat of the enemy objects near it at least the trigger scaled by the hull's share of one half, calls `Try_Ability` (a difficulty chance, 100 % at every difficulty, then `Use_Ability_If_Able`, WAB-33). The scaling is 1500 x (1 - (0.5 - hull) / 0.5) = 3000 x hull with the hull as a fraction. **A human owner** only while the ability's autofire is on (WAB-30, WAB-35: new units start armed): at 20 % hull or less with more than two enemy parent objects near it, it switches the ability on. The script's constants: proximity range 400, threat trigger 1500, three neighbours. A Lua `Activate_Ability("SELF_DESTRUCT", true)` from any script reaches the same command. | data (`objectscript_krayt.lua`, library); debug build EAB-90 |
| WSD-10 | **The threat sum.** The enemy objects the script counts are those the proximity trigger reports within its range, each enemy once per second, a fighter promoted to its squadron. Each adds its type's combat rating, the type's `AI_Combat_Power` metric (a squadron type's is the sum over its craft, PL-13). The script sums them between its services. | data; debug build EAB-96 |
| WSD-11 | **Presentation.** The command-bar button (`i_sa_self_destruct`, Shift+S, foc-ability-buttons) and the activation voice (`SFXEvent_GUI_Unit_Ability_Activated`) follow the generic ability rules (walk 8). The remake also plays the type's start sound at the unit when the countdown starts and the blast's detonation particle and sound at its position (WAD-07). **Light (debug build):** the countdown starts a linear-pulse light effect on the unit, colour (3, 0, 0), one pulse over twice the expiration; the draw blends the unit's final light scale toward that colour by a triangle (0 at the start, 1 at the expiry, back to 0 at twice the expiry), so a unit that detonates at the expiry shows only the rising half. A cancel (the disable path) changes it to a colour flash of the same colour for expiration x (1 - completed share), returning linearly to the unit's own light, and stops the start sound with a 1.0 argument (taken as a fade-out in seconds, unverified). The remake takes both from the frozen ability status (elapsed = total - remaining frames; the flash's length is the last remaining count). **Animation:** the countdown starts the unit's self-destruct clip, aligned to end with the countdown (start frame = clip frames - expiration x clip rate, rounded). Neither M2 self-destruct ship (Vengeance, Krayt Class Destroyer) has such a clip in the data, and with no clip the start call changes nothing, so no animation is shown for them; a type that does author one is not wired (no M2 consumer; fidelity list). | data; debug build EAB-90, EAB-92; WSD-02, WSD-08 |

Remake notes (fidelity list): the remake services the countdown in the ability phase and the detonation in the
impact phase of the tick the count completes, so a command in tick T ends the unit in tick T + 105 and its blast
lands in T + 106; the order of the two engine services within a frame, and the frame the object's first service
runs in, are not read (U-01 class). The proximity trigger's cadence, its distance metric (planar here, spatial in
FoC if it uses the full position) and its handling of fogged objects are not read: the remake evaluates the script
once per second (the damage-rate window, AB-42) over every enemy within a planar 400 of the unit, fogged or not.
An unverified difference is cheap: the AI Krayt only acts below half hull.

### LEECH_SHIELDS: the Underworld Keldabe battleship

Sources: effective ship and nested-handler data; debug build ELS-01 to ELS-11 (eligibility,
application, service, lock/unlock, damage contribution and presentation); direct shield damage
ELS-12. Ability kind number 23. The ship is available at Underworld station level 5.

| Rule | Behaviour | Source |
|---|---|---|
| WLS-01 | The named user-input handler requires an enemy of an applicable category or exact type, with a model, a shield behaviour and positive effective shield percentage, outside a nebula. The stock filter admits Corvette, Frigate and Capital plus Millennium_Falcon. Shared planar, target-extent-adjusted range tests are WHE-58: nested minimum 10 and maximum 2000 override ordinary targeting. Retargeting removes the previous lock before adding its replacement. Black-market flags do not gate multiplayer tactical availability. | data; debug build ELS-01/02/03; WHE-58 |
| WLS-02 | Each logical service frame directly damages the target's shield by Shield_Damage_Per_Second / 30 (100 / 30 in stock data). This path applies no armor, outgoing/incoming damage modes, diminishing firepower, energy drain or hull overflow. A depleted shield takes no drain and extends its depletion window by the ordinary damage increment; an ion storm absorbs zero. Normal shield regeneration continues independently. The owner receives no shields. Lock installs Damage_Multiplier (3) as a permanent outgoing damage contribution in category 0; WHE-55 selects the category maximum and uses 1 + total, giving nominal fourfold weapon damage. Unlock removes only this source's contribution. | debug build ELS-04/05/06/12; WHE-55 |
| WLS-03 | Application starts trunc(Duration_In_Secs x 30) frames (25 s, 750 frames) and the ordinary 55-second recharge immediately. Service decrements the duration before draining; reaching zero removes the lock without a final drain. A disabled ability, target nebula, failed range gate or empty effective shield releases the lock, beam, particle and endpoint loops. Cancellation retains the activation-time recharge. | data; debug build ELS-02/04/07/08 |
| WLS-04 | Lua Activate_Ability accepts a target object for this targeted ability. No stock skirmish Lua library calls LEECH_SHIELDS. The hosted test freestore uses the supported object-target command to exercise the Keldabe in an AI battle, rather than adding a new stock decision policy. Supports_Autofire exposes the ordinary user preference; automatic target selection remains the shared targeted-ability interface. | data; debug build ELS-13; AB-44 |
| WLS-05 | A textured additive beam runs from Beam_Bone_Name to the target's adjusted centre, using Beam_Color, Beam_Width and Beam_Frames. Four small additive highlights move towards the source. Beam_Effect_Name additionally requests a lightning effect at the same endpoints; Leech_Effect attaches a visual status particle to the target. The ordinary ability voice and targeting cues apply, and SFXEvent_Special_Ability_Loop plays at both endpoints until release. | data; debug build ELS-02/08/09/10 |

Remake boundaries: Q24 rounds the per-frame drain (U-01 class); admission precedes the next
logical service frame, matching the existing beam schedule. The unsupported hero-clash gate
has no space-skirmish subject. The source-owned damage contribution is derived from the
ordinary ability slot, so no replay opcode or state block is added. The lightning overlay and
shared targeted-ability autofire selection are presentation and automatic-policy follow-ups;
the authored textured beam, highlights, target particle and endpoint loops are applied here.

<a id="finding-for-670-the-mc80s-defend-lasts-much-too-short"></a>

## Finding for MC80 shield-boost duration verification (legacy EAWR-670) (the MC80's DEFEND "lasts much too short")

- **FoC's timer is ours.** FoC starts the countdown in the frame `DEFEND` switches on and counts
  logical frames only (WAB-12), so it ends about 450 frames later: 15.0 s of game time on the
  MC80 as on the Nebulon-B. `Recharge_Seconds` (40 against 60) changes only the recharge.
  - Ours: `slot.expires_tick = tick + 450` and the ability phase ends it at that tick
    (`src/sim/tactical/abilities.cpp`, `expire_abilities`). At most one frame apart (U-01).
- **So "too short" comes from an early end, or from the clock of the view.** FoC ends `DEFEND`
  early for:
  - a shield at 0 (WAB-60);
  - an ion stun (WAB-61);
  - a switch-off: a second click on the button toggles it off.

  Ours has all three, and nothing else. Candidates, in order of likelihood:
  1. **The shield runs out sooner in ours.** An MC80 under the Imperial fleet's fire can reach 0
     while `DEFEND` restores 50 every 9 frames (about 167 per second, AB-22). Any excess of
     shield damage in ours against FoC ends `DEFEND` sooner. The open shield-routing tickets:
     depleted-shield collision gating (legacy EAWR-700) (hits during the depletion effect) and hardpoint-directed damage routing (the hull and hardpoint split); walk 2
     covers the shield damage itself.
  2. **Ion stuns.** Only the Rebel Y-wings fire ion shots, so a Rebel MC80 is stunned only in a
     Rebel-versus-Rebel battle; unlikely in the owner's preview.
  3. **The preview's game speed.** A faster game speed runs the same 450 frames in less wall
     time (TR-02). FoC's timer does the same at its fast speed.
  4. **A second click** on the button switches it off early (the button toggles).
- **What settles it.** Record why `DEFEND` ended in the remake's trace, then replay the owner's
  preview situation. The possible reasons are expiry, shield at 0, stun, switch-off and engine
  loss. A retail capture of an untouched MC80 with `DEFEND` on, timed from switch-on to the shell
  going off, would confirm WAB-12. It is optional, because the debug build already shows the
  count.

## The existing rules against this walk

| Existing rule | Verdict |
|---|---|
| AB-01, AB-02 | same (data re-read: the MC80's 40 s, the Tartan without `TURBO`) |
| AB-03 (`HUNT`) | implemented patrol core; the remaining service and bounds limits are recorded below |
| AB-04 | same (WAB-03, WAB-11) |
| AB-10 | same. An order does end `HUNT` and the ion shot (WAB-63); AB-10 is about power modes |
| AB-11 | same. The expiry frame is at most one frame apart (WAB-12, U-01) |
| AB-12 | same (WAB-05: trunc((1 - c) x R + 0.5) = round(elapsed / E x R)) |
| AB-13, AB-14, AB-16, AB-17 | same |
| AB-15 | same (AB-U3 stays open) |
| AB-20 to AB-26 | not re-read, beyond their order in the frame (WAB-20) |
| AB-30 to AB-33 | not re-read (presentation) |
| AB-40 | same |
| AB-41 | same, and missing one path. AI TaskForces also switch `DEFEND` on through the library's damage handler when the shield is below 0.8 (WAB-33); ours runs that library, so the remake does it too |
| AB-42, AB-43, AB-44 | same |
| AB-45, AB-46 | creation defaults and trigger ownership verified in the debug build; WAB-35 |
| AB-60 to AB-67 | same, and missing one rule: **a new order ends the ion shot** (WAB-40, G-2) |
| AB-68, AB-69 | not re-read (AB-U7 and AB-U8 stay open) |
| IS-07, HD-11, HD-12 | same |
| foc-tactical-ai FH-24 | same, and missing the plan events `Unit_Ability_Ready` and `Unit_Ability_Cancelled` (WAB-34, G-3) |

## Gaps against the remake

By rule (WAB-01 to WAB-05, WAB-10 to WAB-14, WAB-20, WAB-21, WAB-30 to WAB-34, WAB-40, WAB-50 to
WAB-56, WAB-60 to WAB-64: 30 rules):

| Rule | Ours | Verdict |
|---|---|---|
| WAB-01, WAB-02 | `session_step.cpp` (the ability command), `abilities.cpp` `activate_ability`, `ability_ready` | same |
| WAB-03, WAB-11 | `abilities.cpp` `activate_ability` (`expires_tick`), `expire_abilities` | same (R without modifiers: none in M2) |
| WAB-04 | `activate_ability`, `session_step.cpp` (speed re-plan, AB-43, the ion lock-on) | same for the power modes and the ion shot; the Hunt switch also clears attacks while keeping current movement |
| WAB-05 | `deactivate_ability` | same |
| WAB-10, WAB-13 | tick arithmetic (`ready_tick`) | same |
| WAB-12 | `expires_tick = tick + 450` | same within one frame (U-01) |
| WAB-14 | snapshot ability status (AB-50) | same |
| WAB-20, WAB-21 | `ability_multiplier`, `scaled_weapon_delay`, `scaled_interval`, the ion fire gate | same |
| WAB-30, WAB-31, WAB-32 | the native stand-in (AB-41), `close_rate_window` | same |
| WAB-35 | `initial_abilities`, `session_abilities.cpp` creation, `unit_abilities.cpp` profile binding | same |
| WAB-33 | FoC's own library through the AI host (`src/script/foc/tactical_ai_bindings.cpp`) | same |
| WAB-34 | no ability plan events (`unsupported_plan_calls`: "no Unit_Ability_Ready plan event") | **missing** (G-3): `_Ready` and `_Finished`; `_Cancelled` does not arise in M2 |
| WAB-40 | `session_step.cpp` ion lock (AB-63 re-orders the squadron onto the target whenever its target differs) | **differs** (G-2): a player's move or attack on another target is overridden back onto the ion target instead of ending the shot |
| WAB-50 to WAB-56 | `abilities.cpp` `hunt_destination`, `session_step_commands.cpp` partitioned hunt service, normal squadron and ship planners | patrol core implemented; accelerated service on fire and camera-bound overrides remain (G-1) |
| WAB-60, WAB-61, WAB-62 | `session_abilities.cpp` `end_depleted_defend`, `ion_stun_unit`, the engine loss | same |
| WAB-63 | `session_step.cpp` (power modes survive orders) | same for power modes; the ion shot differs (G-2), `HUNT` now ends on accepted move, attack, attack-move and guard orders |
| WAB-64 | not modelled (no nebula or ion storm in M2) | same for M2 |

The patrol core now covers WAB-04 and WAB-50 to WAB-56. Its remaining boundaries are listed in G-1;
the ion-shot verdict above describes the original audit and is maintained by its own fix.

| Gap | What | Size |
|---|---|---|
| G-1 | **Hunt service boundaries.** The patrol core switches ordinary ability holders, services only the living team leader, chooses fog/enemy/fallback destinations, uses coordinated attack-moves, and cancels on accepted external orders. Remaining: the simulation has no on-fire state for the one-frame cadence, no camera-bound input for the override, and no queued-order or uninterruptible-move representation beyond the existing formation/arrival gates. Retail minute-long observation remains U-04. | S |
| G-2 | **A new order ends the ion shot** (WAB-40): a move, attack, guard or Lua order on a squadron whose ion shot is on switches it off (recharging only when a craft had already fired, AB-65) instead of being overridden by the AB-63 re-order. Bug. | S |
| G-3 | **Ability plan events** (WAB-34). Send `Unit_Ability_Ready` when a TaskForce member's recharge completes. Send `Unit_Ability_Finished` when a timed ability expires or the ion shot ends, and for each member whose switch fails in a TaskForce `Activate_Ability`. `turboattack`'s `Turbo` re-activation then works. `Unit_Ability_Cancelled` needs no work in M2 (nebulae only). | S |
| MC80 shield-boost duration verification (legacy EAWR-670) | No code gap in the timer (WAB-12). The update on the MC80 shield-boost investigation asks for the end cause in the trace and the replay of the owner's situation (see the MC80 shield-boost finding above). | duration investigation (legacy EAWR-670) |

### XML tags this subsystem reads

| Tag | Where | tag-coverage status |
|---|---|---|
| `Unit_Abilities_Data`, `Unit_Ability`, `Type`, `Expiration_Seconds`, `Recharge_Seconds`, `Supports_Autofire`, `Mod_Multiplier` (the `SpaceUnit` and `Squadron` types) | AB-01, WAB-03, WAB-11, WAB-30 | read; `SpaceUnit/Unit_Abilities_Data/@SubObjectList` and `Squadron/Unit_Abilities_Data/@SubObjectList` listed **todo**: combat tag support (legacy EAWR-650), the element rows are not listed |
| `Container/Unit_Abilities_Data/Unit_Ability/Type`, `Recharge_Seconds`, `Supports_Autofire`, `Projectile_Types_Override`, `Container/Abilities/Ion_Cannon_Shot_Attack_Ability/Activation_Style`, `Applicable_Unit_Categories` | AB-60 (the Y-wing container) | read since the ion weapons, energy drain and stun work (legacy EAWR-561), but still listed as **todo** in the combat tag report (legacy EAWR-650); the report is stale for these rows |
| `Container/Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name`, `Container/Abilities/Ion_Cannon_Shot_Attack_Ability/@Name` | AB-60 (the ability's name link) | **todo**: presentation tag support (legacy EAWR-653) |
| `SpaceUnit/.../SFXEvent_GUI_Unit_Ability_Activated`, `SFXEvent_GUI_Unit_Ability_Deactivated`; `Faction/SFXEvent_GUI_Toggle_Non_Hero_Ability_On`/`_Off` and the `Enemy` pair | WAB-11 (sound on expiry; walk 8) | **todo**: presentation tag support (legacy EAWR-653) |
| `Lua_Script` (`ObjectScript_PowerToShields`) | WAB-31 | `Container/Lua_Script` **todo**: AI tag support (legacy EAWR-652); the ship rows are not listed |
| `Space_FOW_Reveal_Range` | WAB-54 | not listed |
| `Ability_Recharge_Bonus_Percentage` (combat bonus abilities) | WAB-11 | not listed; no M2 space unit carries one |
| `GameConstants/Nebula_Ability_Disable_Time` | WAB-64 | **todo**: combat tag support (legacy EAWR-650); not in M2 |

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-01 | **Settled schedule:** ordinary admitted-frame commands precede countdown (WAB-12); later object-script/AI activation has its own caller position | Previously sourced in this walk |
| U-02 | Which switch-offs the TaskForce reports as `Unit_Ability_Cancelled` | Previously sourced in this walk |

## Unverified, and what would settle it

- **WAB-U1** Whether an out-of-reach `BUZZ_DROIDS` click moves the holder into reach before applying, or is dropped. The remake refuses it. A retail StarViper click beyond 400 units settles it.

| ID | Unknown | Effect | What settles it |
|---|---|---|---|
| U-03 | Why the owner's MC80 lost `DEFEND` early (legacy EAWR-670) | Which of the four candidates it is | Ours: the end cause in the trace, on the owner's preview situation |
| U-04 | `HUNT` in retail: no recording yet | The destination draws (WAB-54) are read, not observed | A retail capture of an idle TIE squadron with `HUNT` on, over a minute: destinations in fog, on enemy ships, or 500 to 1000 units off |

### Hunt patrol implementation boundary

The debug build was re-read for WAB-50 to WAB-56. The non-sensitive diagonal search takes exactly
99 points, with X then Y draws, before choosing one fogged point. The enemy search is capped at
128 locomotor objects; a force-sensitive hunter prefers force-sensitive enemies and skips the
positive offsets. The no-enemy fallback clamps only when its 500-to-1000-unit distance adjustment
ran; the positive offsets follow the clamp. Without map bounds, the base destination is the current
position, after which an ordinary hunter still receives those offsets. These details clarify WAB-54.

The remake uses the existing deterministic keyed random service, preserving the draw order inside
each hunter rather than serializing a shared random stream. It services at activation and every
30 frames from activation while enabled. Decisions run in partitions against immutable staged
inputs; only accepted moves commit in ascending holder order through the existing path planners.
No new replay opcode or state block is used: the ordinary ability slot records activation time.
The current map bounds come from the economy content; camera bounds are still a follow-up.
The current movement model supplies idle, target, formation and arrival gates; it does not supply
an on-fire state. No skirmish AI script activates Hunt (WAB-33); Lua activation uses the ordinary
ability command path. The existing viewer icon, hotkey and ability audio data apply to the newly
supported kind without a separate presentation implementation.

EAB-14 re-reads the ordinary Stop command: it dispatches the selected units to the movement
coordinator, whose space-layer stop submits a coordinated move to their current position with
external-order cancellation enabled. That cancellation switches active Hunt off through the
normal ability handler. Player moves (including moves with a facing) and attacks pass the same
flag; Hunt's own coordinated moves leave it clear. Escort uses the existing order ability-clear
path (EAB-06). The separate flying full-stop action is not the ordinary Stop command examined here.
