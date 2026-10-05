# Space unit abilities: power modes, their timers and multipliers

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, space tactical battles, the abilities
  the M2 fleet authors (space ability implementation). The claims come from the FoC debug build, read on 2026-09-28
  under the [clean-room rule](../clean-room.md), from the FoC XML and Lua data and from the
  S-15 and S-17 recordings. The private evidence map (IDs AB-R01 to AB-R16) is under the ignored
  `out/research/`.
- Bounded question: which abilities the M2 fleet has, when each may be switched on and off, how
  long it runs and recharges, what it multiplies while it is on, and which states switch the
  Nebulon-B's and the MC80's `DEFEND` on by themselves.
- Source tags: **research** (debug build), **data** (a FoC file), **recording** (S-15, S-17),
  **owner** (owner knowledge), **project** (a remake choice) and **unverified**.

## Interface

- Content: per unit type, its `Unit_Abilities_Data` (the primary and the optional secondary
  `Unit_Ability`: `Type`, `Expiration_Seconds`, `Recharge_Seconds`, `Supports_Autofire` and the
  `Mod_Multiplier` pairs) and its `Lua_Script`. `units::ability_table` builds the session's
  `AbilityTable` from the unit tables; the table also lists the human players (AB-41). Like the
  victory rules' human list it is session content, not replay data.
- Input: ability commands (replay opcode 8, `AbilityPayload`: the ability and one of activate,
  deactivate, autofire on, autofire off) on listed units; the Lua `Activate_Ability` of the
  the tactical AI host issues the same command.
- Retained state per unit with abilities (hashed, `ABIL` block): per ability whether it is on and
  on autofire, the tick it started, the tick its duration ends and the tick its recharge ends;
  the damage taken in the current rate window, the last window's rate, and whether a speed
  change still waits for the move plan.
- Output: the multipliers the weapon, shield, energy and movement services read (AB-20 to AB-24),
  and each snapshot instance's ability status (AB-50).
- Cadence: one partitioned ability phase per tick, before the tracking phase; commands act in
  the command phase of their tick.

## Rules

### The fleet's abilities

| Rule | Behaviour | Source |
|---|---|---|
| AB-01 | A type authors at most a primary and a secondary unit ability. Each names its kind and optionally an expiration time, a recharge time, autofire support and multipliers. A multiplier the ability does not author is 1. | research AB-R01, AB-R04; data |
| AB-02 | The M2 fleet: the Nebulon-B has `DEFEND` (15 s, recharge 60 s; weapon delay 1, shield regen 1, shield regen interval 0.1, energy regen 3, energy regen interval 0.1, speed 0.8; autofire; runs `ObjectScript_PowerToShields`). The MC80 (`Calamari_Cruiser`, SK-22) has the same `DEFEND`, multipliers, autofire and script with a 40 s recharge. The Corellian corvette has `TURBO` (20 s, 50 s; delay 3, shield regen 0, energy regen 1, speed 2). The Tartan has `POWER_TO_WEAPONS` (7 s, 60 s; delay 0.2, shield regen -25, energy regen 1, speed 0.5). The Acclamator has `POWER_TO_WEAPONS` (20 s, 60 s; delay 0.5, shield regen -3, energy regen 1, speed 0.5). The X-wing craft has `SPOILER_LOCK` (no expiration or recharge; delay 3, shield regen 3, energy regen 3, speed 1.3). TIE fighters and interceptors have `HUNT`; the Y-wing squadron has `ION_CANNON_SHOT`. The Tartan has no `TURBO` (the space ability implementation brief says it does; the data does not). | data |
| AB-03 | **Cut: `HUNT`.** `HUNT` is not a power mode: it switches a behaviour that sends the squadron seeking targets on its own, which needs the squadron target search and dogfighting that M2 does not have (space-fighters G-F2 to G-F6). The loader skips it; `Has_Ability` still reports it (AB-44). The Y-wing squadron's `ION_CANNON_SHOT`, cut until ion weapons, energy drain and stun, is modelled since (AB-60 to AB-69). | research AB-R10, AB-R11; data; project |
| AB-04 | Seconds become whole logical frames by truncation: frames = trunc(seconds x 30). `DEFEND`'s 15 s is 450 frames, its 60 s 1800. | research AB-R03 |

### Switching on and off

| Rule | Behaviour | Source |
|---|---|---|
| AB-10 | A unit enters with each ability inactive and ready. Its autofire flags follow the creation preference (AB-45). An ability command never replaces the unit's order: a moving corvette keeps its move when `TURBO` switches on. Automatic activation uses the ability's own trigger (AB-41, AB-68), not the flag alone. | research AB-R01, AB-R02; research AF-R01 |
| AB-11 | Switching an ability on succeeds only when it is ready (AB-13) and off; switching on an ability that is on fails and changes nothing. An ability with an expiration runs for its expiration frames from the switch; when they are up it switches off and its full recharge starts. An ability without an expiration has no timer. | research AB-R02, AB-R03 |
| AB-12 | Switching off an ability that is off does nothing. A timed ability switched off early recharges for the share of its duration it ran: round(elapsed / duration x recharge), halves up, so 150 of `DEFEND`'s 450 frames give 600 frames, and a switch-off in the frame it started gives none. An ability without an expiration switches on and off freely and never recharges (`SPOILER_LOCK`). | research AB-R03 |
| AB-13 | An ability is ready when the unit has it, it is not recharging and its gate holds (AB-14, AB-16). A ready ability stays ready while it is on. This is what the Lua `Is_Ability_Ready` reports, and `Activate_Ability` acts only on a ready ability. | research AB-R02, AB-R13 |
| AB-14 | `DEFEND` may switch on only on a shielded unit whose shield generators are online and whose shield is not in its depletion effect ([space damage](space-damage.md) DG-08). It is also refused while the unit is ion stunned ([space damage](space-damage.md) IS-07, ion weapons, energy drain and stun), and in retail in an ion storm, which M2 does not have. | research AB-R02, IR-04 |
| AB-15 | A squadron container passes a non-team ability command to its live craft, and each craft uses its own type's ability data, not the squadron's: the X-wing craft's `SPOILER_LOCK` multiplies its speed by 1.3, not by the squadron's 2. The remake switches the craft on only when every live craft has the ability ready and off, and off together, so a squadron never runs split (**unverified**). A container has no ability status of its own. | research AB-R09; data; project |
| AB-16 | When a unit's engines go offline (space-hardpoints HD-11), its `TURBO` and `SPOILER_LOCK` end, early (AB-12). The remake also keeps them from switching on while the engines are offline (**unverified**: the least visible choice; the other order would switch the mode on for one frame). | research AB-R14; project |
| AB-17 | Any shield set to zero or below ends an active `DEFEND` (early, AB-12). A shield generator loss drops the shield to zero (DG-17), so it ends `DEFEND` too (HD-12). | research AB-R06, AB-R14 |

### Multipliers while on

| Rule | Behaviour | Source |
|---|---|---|
| AB-20 | A unit's multiplier of one kind is the product of that multiplier over its active abilities (primary and secondary); with none active it is 1. Retail gives a team container the minimum over its members when its own multiplier is 1; no modelled ability is a team ability, so the remake has no such case. | research AB-R04 |
| AB-21 | **Weapon delay.** The gap between two shots of one burst is multiplied by the weapon delay multiplier, rounded half up. The full recharge after a burst takes the multiplier only when it is below 1 (it may shorten the recharge, never lengthen it): `TURBO`'s 3 spreads the corvette's bursts out, `POWER_TO_WEAPONS`' 0.2 cuts the Tartan's recharge to a fifth. Retail also has a per-hardpoint ability delay multiplier (default 1); no M2 hardpoint authors one. | research AB-R05; data |
| AB-22 | **Shield regeneration.** Each shield recharge adds the refresh amount x the shield regen multiplier x the generator fraction, clamped to [0, maximum]; a negative multiplier drains (`POWER_TO_WEAPONS`), 0 stops it (`TURBO`). The recharge interval is (int)(3.0 s x the interval multiplier x 30 + 0.5) frames, at least 1: `DEFEND`'s 0.1 gives 9 frames. | research AB-R06; recording S-15 (+50 every 9 ticks) |
| AB-23 | **Energy regeneration.** The energy pool's recharge amount is multiplied by the energy regen multiplier and its interval by the energy interval multiplier, rounded as AB-22 (`DEFEND`: 5 x 0.1 gives 15 frames). | research AB-R07 |
| AB-24 | **Speed.** Maximum speed, acceleration and deceleration are multiplied by the speed multiplier; minimum speed is not (retail computes it and discards the result). A craft's locomotor reads the same maximum. A move under way plans again from the tick the multiplier changes, so the new speed takes effect with a move's usual two-tick latency: in S-17 `TURBO` at tick 60 changes the corvette's motion from tick 62, and it cruises at 7.44 instead of 3.72. | research AB-R08; recording S-17 |
| AB-25 | No modelled ability changes the diminishing-firepower gate ([space damage](space-damage.md) DG-05): only a `MAXIMUM_FIREPOWER`-style ability turns the shooter's flag off, and none is in the M2 fleet. The flag stays on for every M2 shooter. | research AB-R04; data |
| AB-26 | A multiplier name outside AB-20 to AB-24 fails the ability table load (EAWR-UNITS-0006), so new data cannot slip in unmodelled. The M2 fleet authors none. | project |

### Presentation

| Rule | Behaviour | Source |
|---|---|---|
| AB-30 | `DEFEND` switched on shows the unit's shield shell; it hides again when `DEFEND` ends. | research AB-R02 |
| AB-31 | `TURBO` and `SPOILER_LOCK` hide the unit's engine emitters (`pe`) and show its turbo engine emitters (`pte`) while on, and swap back when off. The switch clears the same code-hidden flag that the model's authored visibility sets, so it also shows authored-hidden proxies (battle-presentation BP-43). The capital-ship locomotor shows, each service, the turbo engines while `TURBO` runs and the engines otherwise, and gives that type the engine brightness (battle-presentation BP-45; the maximum is the type's, so a ship under `TURBO` glows at full). The corvette carries `PTE_Corvetteengines`. The X-wing carries no turbo emitters and its fighter locomotor does not show the engines again, so its engine glow goes out while `SPOILER_LOCK` is on. `SPOILER_LOCK` also plays the craft's `DEPLOY` clip when it switches on and `UNDEPLOY` when it switches off (the X-wing's clips are 30 frames at 30 fps: `DEPLOY` folds the open S-foils of the bind pose shut, `UNDEPLOY` opens them), at the type's `Deployment_Anim_Rate` (default 1.0; the X-wing authors none), from the frame that mirrors the running clip's remaining frames (count minus frame minus 1, at least 0), blended over 1/30 s, holding the last frame (unit-animation UA-06). The remake does not draw the 1/30 s blend. | research AB-R02, AB-R16; data; owner (S-foil) |
| AB-32 | `POWER_TO_WEAPONS` shows the unit's power-to-weapons emitters (`pptw`) while on, by the same code flag as AB-31. The Tartan's and the Acclamator's power-to-weapons proxy, `pptw_ptwsa`, is authored hidden in both models; the ability clears that hidden flag (battle-presentation BP-43), so its red flare runs from activation until switch-off or expiration. Hiding it stops emission and lets the existing particles drain (BP-48), using the authored system's lifetime. The viewer follows the snapshot's active state, including an early switch-off, without extending the ability timer. | research AB-R02; data |
| AB-33 (ability audio and engine-boost fade) | **TURBO's end ramps.** When `TURBO` ends (its 20 s, or a switch-off) the corvette's speed falls to the normal maximum at the normal deceleration: 0.06 units per frame squared, from 7.44 to 3.72 in about 62 frames (2 s), because the move is planned again from the current speed (AB-24) and MV-13 flies straight until the unit reaches its maximum. The rig recording agrees (2026-09-29, retail image, `-StagingProbe turbo`, speed sampled every 0.4 s after the switch-off: about 7 (6.0-7.8), 5.4-6.1, 4.8-5.5, 4.8-5.5, 3.6-4.7, then 3.3-4.3 around the normal 3.72, with the recorder's own 0.1 s sampling noise); no sim change. The engine boost that goes with it: battle-presentation BP-65. | recording (turbo staging probe, speeds); research AB-R08 |

### Autofire and the Nebulon-B script

| Rule | Behaviour | Source |
|---|---|---|
| AB-40 | Autofire is a per-ability flag. A command sets or clears it only for an ability with `Supports_Autofire`; for any other it is rejected. The flag itself switches nothing on (AB-10). | research AB-R01, AB-R13; data |
| AB-41 | **The `DEFEND` stand-in.** The Nebulon-B's and the MC80's object script (`ObjectScript_PowerToShields`) runs once a second. For an owner that is not human, or for a human owner while `DEFEND` is on autofire, it switches `DEFEND` on when the unit's rate of damage taken exceeds 20 and `DEFEND` is ready. The remake runs this rule natively for every unit whose type runs that script and has `DEFEND`, at each close of the rate window (AB-42). A human owner's unit without autofire never switches `DEFEND` on by itself. | data (the script); research AB-R13, AB-R15 |
| AB-42 | The rate of damage taken is serviced every 30 frames: the hull and shield the unit lost since the last service, times 30 / 30, per second. The remake closes the window when the tick is a multiple of 30 (**unverified** phase; see AB-U1). | research AB-R12 |
| AB-43 | When `DEFEND` switches on, the unit's next shield and energy recharges come due in the next frame, and run at `DEFEND`'s intervals from there. | research AB-R02; recording S-15 |
| AB-44 | The Lua object calls: `Has_Ability(name)` is true when the unit (or a squadron container's craft) authors the ability, cut ones included; `Is_Ability_Ready`, `Is_Ability_Active` and `Is_Ability_Autofire` read AB-13, AB-11 and AB-40 (a container: all its craft), and are false for a cut ability; `Activate_Ability(name, on)` issues the ability command when the ability is ready. | research AB-R13 |
| AB-45 | **Creation preference.** A fresh player profile defaults to enabling ability autofire. When a local player's unit is created, that enabled preference arms each authored ability with `Supports_Autofire`, including the secondary ability; it activates none. A disabled preference leaves those flags off. The preference also applies to units created during the battle. The remake binds the enabled owners in `AbilityTable.autofire_defaults` as session content; the unit-table binder enables its human owners by default and offers a disabled preference. Each human slot supplies its own profile in a multiplayer session. Toggling an existing unit does not change the creation preference. | research AF-R01 (creation), AF-R02 (fresh profile); project (session binding) |
| AB-46 | **Trigger ownership.** `DEFEND` on the Nebulon-B and MC80 follows AB-41 for human and non-human owners: a damage rate above 20 per second and the shield/readiness gates, with no separate enemy-distance or hull-percentage trigger in the object script. The AI's Lua plan library may separately request `DEFEND` below 80% shield (walk WAB-33). `TURBO`, `POWER_TO_WEAPONS` and `SPOILER_LOCK` do not author autofire support in this fleet and are never armed by AB-45; their automatic AI uses belong to Lua plans. Targeted ion autofire uses the attack target (AB-68); the debug build notifies targeted abilities from attack commands, and nested targeted autofire defaults enabled for an AI owner without an override. Generic AI flags do not need to start enabled for the non-human shield script to run. | data (ability XML and object/AI scripts); research AF-R03 (nested default), AF-R04 (attack notification); WAB-33 |

<a id="ion_cannon_shot-561"></a>

### ION_CANNON_SHOT

The Y-wing squadron's targeted ion shot: each Y-wing fires one ion bolt at a chosen enemy, which
knocks shields down and stuns ([space damage](space-damage.md) IS-01 to IS-07).

| Rule | Behaviour | Source |
|---|---|---|
| AB-60 | `ION_CANNON_SHOT` is a team ability: the squadron's team container holds it, with the data of the squadron's `Create_Team_Type` (`Y_Wing_Squadron_Container`: `Recharge_Seconds` 20.0 (600 frames), `Supports_Autofire`, `Projectile_Types_Override` `Proj_Ion_Cannon_Medium_Laser_Blue`, and a targeted attack ability of `User_Input` style on all unit categories). In the session the container is the squadron company. Each craft (`Y-Wing`) also authors the ability; its own slot only says whether that craft still has its shot due. | research IR-11; data |
| AB-61 | It switches on only with a target: a unit, and optionally one of its hardpoints (the ability command carries both). Switching it off ends it (AB-65); autofire is set as AB-40. | research IR-11 |
| AB-62 | It switches on when the container's ability is off and not recharging, the squadron has a live craft with the ability and no craft still has a shot due, and the target is a live unit of another team that can be hit (FoC requires a model: a squadron is aimed at through a craft, never its team container). FoC also refuses a target in hyperspace (as space-damage IS-02; none in an M2 battle) and one in another hero duel (none in M2). An invalid hardpoint is dropped (the whole unit is aimed at). | research IR-11 |
| AB-63 | Switched on, the container and every live craft of the squadron are on and the target is stored; the squadron takes an attack order on the target (space-fighters FO-01). While the ability is on, the squadron is ordered onto the target again whenever its attack target differs. | research IR-11 |
| AB-64 | Each frame, the ability stays on while at least one craft still has its shot due and the target is a live enemy; otherwise it ends (AB-65). FoC also ends it when the container or the target is in a nebula (**not modelled**: M2 units carry no nebula state). | research IR-11 |
| AB-65 | Ending switches the container and every craft off and forgets the target; the container recharges (600 frames) only when at least one craft fired, so a shot cancelled before any craft fired costs nothing. | research IR-11 |
| AB-66 | While the ability is on, each craft's hardpoints fire only the override projectile, only at the target (a craft of a targeted squadron counts), at its hardpoint if one was aimed at; with the hardpoint's `Damage_Type`, own damage if it authors one and `Fire_Range_Distance` (space-damage DG-12, DG-25, DG-23; no Y-wing hardpoint authors a damage) and its own recharge (a Y-wing's torpedo hardpoint may still be recharging). A craft that has already fired, or a shot at anything else, stays silent until the ability ends. | research IR-11, IR-09, IR-12 |
| AB-67 | A craft that fires the override at the target has fired it: its own slot switches off. One ion bolt per Y-wing. | research IR-11 |
| AB-68 | **Autofire.** With autofire on, a ready ion shot locks onto the squadron's attack target when that is a valid target (AB-62). A player the engine plays (not human) always has autofire (AB-40), so the Rebel AI's Y-wings use it; a human player's only when they switch it on. FoC queues the ability when the unit attacks; when exactly it is asked is not traced (**unverified**): the remake asks every frame while the squadron attacks and the ability is ready. | research IR-11 (autofire); project |
| AB-69 | FoC's Lua `Activate_Ability` takes a target object for a targeted ability and issues it on the unit: called on a Y-wing squadron with a Tartan, it fires the ion shot at the Tartan (retail recording, ion weapons, energy drain and stun). No M2 script calls it for `ION_CANNON_SHOT`; only a story mission queries it. So the remake reports such a call instead of issuing it (AB-U8). `Is_Ability_Ready` and the other queries read the container's slot. | research IR-13; retail recording; data |

<a id="the-command-bar-api-454"></a>

### The command bar API

| Rule | Behaviour | Source |
|---|---|---|
| AB-50 | Each snapshot instance carries one status per ability: its kind; whether it is on, ready (AB-13) and on autofire; whether it supports autofire; and its timer: while a timed ability is on, the frames left of its duration and the duration; while it recharges, the frames left and the recharge; otherwise both zero. A command bar switches abilities with ability commands on the selected units (a squadron container stands for its craft, AB-15) and reads the result from the next snapshot; a rejected command reports `ability_unavailable`. The live view implements ability buttons and recharge dials's `AbilityState` and `AbilityCommands` this way (foc-ability-buttons.md, "Interface to the simulation"). | project |

## Cases

| Case | Input | Expected |
|---|---|---|
| ABC-01 | Corvette moving since tick 30; `TURBO` at tick 60 | Positions unchanged through tick 61; from 62 it speeds up to twice its cruise; `TURBO` ends at 660 and is ready again at 2160 (AB-11, AB-24) |
| ABC-02 | `DEFEND` switched on at 100 and off at 250 | Ready again at 850: 150 / 450 x 1800 = 600 (AB-12) |
| ABC-03 | `SPOILER_LOCK` switched on and off three times in one second | Each switch acts; never recharging (AB-12) |
| ABC-04 | A 400 hit on a non-human Nebulon-B at tick 500 | The window closing at 510 reads 400 per second and switches `DEFEND` on; the shield gains its refresh at 511 and then every 9 ticks; `DEFEND` ends at 960 (AB-41 to AB-43) |
| ABC-05 | ABC-04 with a human owner using the fresh profile default, then with an autofire-off command | The default arms and fires `DEFEND` without an activation command; the explicit off prevents activation (AB-41, AB-45) |
| ABC-06 | On the corvette: `DEFEND`, then `TURBO` twice, off after 2 frames, on again; autofire on its `TURBO` and on the frigate's `DEFEND` | `DEFEND` (not authored), the second `TURBO`, the recharging `TURBO` (2 / 600 x 1500 = 5 frames) and the `TURBO` autofire are rejected with `ability_unavailable`; the switch-off and the `DEFEND` autofire are accepted (AB-11 to AB-13, AB-40) |

`tactical_ability_contracts` runs these with 1, 2 and 4 workers and replays them; the S-17
scenario trace checks ABC-01 against the recording and the S-15 duel trace runs with the stand-in.

## Unknowns

| ID | Unknown | Effect |
|---|---|---|
| AB-U1 | The phase of retail's damage-rate service. Each object's service runs 30 frames after its own start, not on a shared clock. | A switch-on can land up to 29 frames apart from retail's. In the S-15 recording the frigate takes about 8 damage every 90 frames until the 400-point hit at tick 500 and switches `DEFEND` on at 529. Since retail time-to-kill comparison the remake's frigate also takes about 8 damage a hit from the Tartan's lasers (space-damage DG-24, DG-36; before it took about 21 a second and switched `DEFEND` on at tick 60), so its stand-in switches `DEFEND` on at tick 511, after the same hit. The duel's end is unaffected (DEFEND leaves the weapons as they are). |
| AB-U2 | Where retail's replanned move ends. | S-17's retail corvette stops 55.8 units short of its target after `TURBO`; the remake's replanned move arrives. |
| AB-U3 | Whether a squadron container needs all its craft ready (AB-15). | A split squadron, possible only after craft losses mid-recharge, switches on in retail and not in the remake. |
| AB-U4 | Whether lost engines block `TURBO` from switching on or only end it (AB-16). | At most a one-frame flicker in retail. |
| AB-U5 | The order within a frame of the shield hit that ends `DEFEND` and the move plan. | The remake plans the slower speed from the next tick (one tick late after a depletion). |
| AB-U6 | `DEFEND` blocking hangar launches (space-hardpoints HD-13). | No M2 type has both a hangar and `DEFEND`; not modelled. |
| AB-U7 | The exact frame FoC asks an autofire ion shot to lock on (AB-68). The matched S-48 archive contains six ion contacts, so the earlier claim that quick-loaded non-human recordings never fired ion shots is withdrawn. The debug build notifies nested targeted abilities from an attack command (AF-R04), but the complete retry cadence remains unverified. | The remake may lock on earlier after a recharge than FoC; the Rebel AI's Y-wings use the shot on autofire (AB-68). |
| AB-U8 | A Lua `Activate_Ability` with a target (AB-69). | No M2 script issues one; the remake reports it. |
