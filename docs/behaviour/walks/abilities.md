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
| WAB-51 | **Orders end it.** Each of these switches an active `HUNT` off:<ul><li>a player's move, plain or queued;</li><li>an attack order;</li><li>an escort or guard order;</li><li>a Lua `Move_To` or `Attack_Target` on a unit.</li></ul>`HUNT`'s own moves (WAB-55) pass a flag that keeps it on. | debug build EAB-06 |
| WAB-52 | **Who acts.** The service acts only on a squadron's team leader, or on a unit outside a squadron; for a team container it takes the leader. It does nothing when the unit's service is disabled. | debug build EAB-07 |
| WAB-53 | **When it acts.** The unit is idle when either holds:<ul><li>its locomotor is in the fighter-idle (or walk-stopped) state;</li><li>it is not moving and plays an idle animation.</li></ul>It is then no longer idle if any of these holds:<ul><li>its formation holds more than one queued destination;</li><li>it has an attack target;</li><li>it is on an uninterruptible move.</li></ul>An idle hunter picks a destination (WAB-54) and moves there (WAB-55). A hunter that has a target fights it through the squadron's own targeting and dogfights ([squadrons](squadrons.md)). | debug build EAB-07 |
| WAB-54 | **The destination.** The area is the map's bounds, or the tactical camera's bounds when there are any. A map without bounds keeps the unit where it is. All draws use the synchronized game random stream, in this order:<ol><li>A float in [0, 1). For a unit that is not force sensitive (every M2 TIE), when the draw is above 1 - 0.5 (0.5, a code constant), it samples up to 99 points.<ul><li>The area is cut into 100 x 100 cells (100, a code constant). The cells are walked along the diagonal, both indices stepping together, and one random point is drawn in each cell, x then y.</li><li>The points fogged for the owner are kept. If any are, one is drawn uniformly and becomes the destination.</li></ul></li><li>Otherwise it looks at up to 128 of the owner's enemy units with a locomotor. It draws uniformly among those fogged for the owner, else among all of them. A force-sensitive hunter would prefer force-sensitive enemies; there are none in M2.</li><li>With no such enemy it draws a point in the area, x then y. When that point is more than 1000 or less than 500 units away, it becomes the unit's position plus the direction to it times a random distance in [500, 1000] (code constants). The result is clamped inside the area, less half the unit's `Space_FOW_Reveal_Range` on every side.</li><li>For a unit that is not force sensitive, the destination then gains a random 30 to 400 units in x and another in y. Its height is the unit's own.</li></ol> | debug build EAB-08, EAB-13 |
| WAB-55 | **The move.** A squadron's leader moves the whole squadron: every member and the container, as one coordinated group move to the position with movement type 1 (the attack-move kind of [space orders](../space-orders.md); squadrons FO-06). The group move does not end `HUNT` (WAB-51). A lone unit moves alone the same way. | debug build EAB-09 |
| WAB-56 | The next service after the move finds the squadron moving, so it waits. On arrival (idle again) it picks a new destination. Enemies met on the way are engaged by the attack-move's own rules (walk 4, WMV-15 to WMV-17). | debug build EAB-07; the movement walk |

### Interactions (engine switch-offs)

| Rule | Behaviour | Source |
|---|---|---|
| WAB-60 | A shield set to 0 or below ends an active `DEFEND` early (AB-17). So does losing the last shield generator (HD-12); in M2 only the stations have one, never the MC80 or the Nebulon-B. | debug build; AB-17, HD-12 |
| WAB-61 | An ion stun ends an active `DEFEND` early and keeps it from switching on (IS-07). The Y-wing's ion shot stuns (IS-01). | debug build; IS-07 |
| WAB-62 | Losing the last engine hardpoint ends `TURBO` and `SPOILER_LOCK` early (AB-16, HD-11). | debug build; AB-16 |
| WAB-63 | A new order ends `ION_CANNON_SHOT` (WAB-40) and `HUNT` (WAB-51). It never ends a power mode: a moving corvette keeps `TURBO` (AB-10). | debug build EAB-06; AB-10 |
| WAB-64 | Out of M2: a nebula cancels abilities (`Nebula_Ability_Disable_Time`), and an ion storm blocks `DEFEND`. No M2 map has either. | debug build; AB-14, AB-64 |

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
| AB-03 (`HUNT` cut) | differs from FoC by design. With squadron scans and dogfights in (walk 1), `HUNT` can now be implemented: WAB-50 to WAB-56, G-1 |
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
| WAB-04 | `activate_ability`, `session_step.cpp` (speed re-plan, AB-43, the ion lock-on) | same for the power modes and the ion shot; the behaviour part is missing with `HUNT` (G-1) |
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
| WAB-50 to WAB-56 | the loader skips `HUNT` (AB-03); the Lua host reports it as cut | **missing** (G-1) |
| WAB-60, WAB-61, WAB-62 | `session_abilities.cpp` `end_depleted_defend`, `ion_stun_unit`, the engine loss | same |
| WAB-63 | `session_step.cpp` (power modes survive orders) | same for power modes; the ion shot differs (G-2), `HUNT` is missing (G-1) |
| WAB-64 | not modelled (no nebula or ion storm in M2) | same for M2 |

Counts by rule: **same 22**, **differs 1** (WAB-40), **missing 8** (WAB-34 and the seven `HUNT`
rules WAB-50 to WAB-56). WAB-04 and WAB-63 count as same; their `HUNT` and ion parts are G-1
and G-2.

| Gap | What | Size |
|---|---|---|
| G-1 | **Implement `HUNT`** (WAB-04 behaviour part, WAB-50 to WAB-56, WAB-51 cancel) for the TIE fighter and interceptor squadrons: player switch-on and switch-off, the 30-frame idle check on the leader, the destination draws in FoC's order on the sim RNG, the coordinated attack-move of the squadron, and the order cancel. It un-cuts AB-03 and FH-24's `HUNT`; the command bar button stops being disabled (walk 8). | M |
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

## Unverified, and what would settle it

| ID | Unknown | Effect | What settles it |
|---|---|---|---|
| U-01 | **Settled schedule:** ordinary admitted-frame commands precede countdown (WAB-12); later object-script/AI activation has its own caller position | Command-before-countdown can count the activation frame; normalize clocks before judging the remake endpoint | [WFO-02/17/21/22/31](frame-order.md), debug build; UFO-02's debugger-harness stepping covers endpoint normalization, without a new log-scraping probe |
| U-02 | Which switch-offs the TaskForce reports as `Unit_Ability_Cancelled` | **Settled** (WAB-34): only a nebula disabling abilities, never in M2 | none |
| U-03 | Why the owner's MC80 lost `DEFEND` early (legacy EAWR-670) | Which of the four candidates it is | Ours: the end cause in the trace, on the owner's preview situation |
| U-04 | `HUNT` in retail: no recording yet | The destination draws (WAB-54) are read, not observed | A retail capture of an idle TIE squadron with `HUNT` on, over a minute: destinations in fog, on enemy ships, or 500 to 1000 units off |
