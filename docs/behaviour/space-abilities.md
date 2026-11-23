# Space unit abilities: power modes, their timers and multipliers

## Passive retreat prevention

The debug-build walk WRP-01 to WRP-03 establishes tactical-space admission,
automatic activation and the bearer-player scope of `Retreat_Prevention_Ability`.
The campaign space binding accepts automatic styles and initial named locks, and
tracks ordinary bearers and contained riders. Destruction of a rider removes its source;
a destroyed carrier's hero riders are destroyed with it and never re-homed to another flagship (WRP-03;
only upgrade dummies relocate, WGT-51). Deletion removes the source;
a space retreat's escape service does not itself delete departing objects
(WRP-03/04). Ground and nonautomatic activation, and changes to named locks after
the battle starts, remain separate work.

## Lucky Shot

These space rules come from targeted reads of the debug build. Private evidence and the
address map remain under ignored research output. Authored data selects the behavior for
every type; no ship identity or stock value is a runtime condition.

| Rule | Behaviour | Source |
|---|---|---|
| WLK-01 | `LUCKY_SHOT` resolves `GUI_Activated_Ability_Name` to a `Lucky_Shot_Attack_Ability`. A manual request locks an enemy target and optional hardpoint; an absent named handler fails the request without discarding the owner. Admission uses `Applicable_Unit_Categories` and `Applicable_Unit_Types` through the shared filter, requires a target model, and refuses a target in a nebula. The ordinary availability and hero-clash gates still apply where those states are modelled. | debug build; WHE-14; WHD-07 |
| WLK-02 | The active holder selects the first `Projectile_Types_Override` for shots directed at its locked target, or a member of that target's team, and the selected hardpoint when present. Team craft use the holder's authored override; a solo holder uses its own. Hardpoint weapons suppress ordinary shots while a team lock cannot supply its override. The override retains the ordinary hardpoint damage/range inputs. | debug build; WWP-18 |
| WLK-03 | Lucky Shot is excluded from ordinary active-mode multipliers. The first qualifying positive projectile impact against the locked target/hardpoint consumes the whole holder's shot and applies its authored `CAUSE_DAMAGE_MULTIPLIER`. It does not spend one bonus per craft. Other projectiles already in flight receive ordinary damage after consumption. An untargeted hull impact can route the boosted hit to the nearest hardpoint. The supported multiplier has no stock-derived ceiling; its application uses checked fixed-point multiplication. | debug build |
| WLK-04 | Ending a lock starts the full authored `Recharge_Seconds` only if a qualifying hit spent the shot. Cancellation before that hit is free. Target removal, target nebula and loss of availability end the lock. The remake retains the target and an appended spent flag between impact and its next ability-service phase, then clears the target and starts recharge. Checkpoints preserve that distinction. Exact retail within-frame service placement remains the frame-service-map seam; this work uses the existing partitioned ability boundary. | debug build; project boundary; FC-03 |
| WLK-05 | A `User_Input` handler joins targeted autofire on its current attack target in ordinary range, never a new search. AI owners default to autofire independently of `Supports_Autofire`; the supported flag permits the human toggle. An explicit stored autofire preference overrides the AI default. Named activation also admits an authored `Space_Automatic` handler. Ground activation styles do not execute in this space path. | debug build; WAB-30/36 |
| WLK-06 | Presentation follows the selected projectile's authored appearance and detonation sounds. The existing target-ability response cue uses the runtime holder's `SFXEvent_Target_Ability` (the authored team container for a squadron), then the team-to-member fallback, once on activation. | debug build target-handler boundary; BA-54; project presentation routing |

The stock Red Squadron container's recharge and damage multiplier are content, not constants.
The ordinary non-speed modifiers other than cause and take damage use the representation-derived bound in WAB-65; the cap of 64 remains for cause and take damage; the shot-only exemption applies
only to Lucky Shot's cause-damage input. Speed uses the representation-derived bound in WAB-24.
Projectile override selection reads the firing craft's primary ordinary ability; a secondary
Lucky slot can still boost a qualifying normal hit without selecting that override.
If ion and Lucky locks are both active, the remake's combat-world projection uses
Lucky's target, hardpoint and arming state for the shared weapon lock. This does not
clear either ability slot. This precedence is a project choice; retail precedence
for that combination remains unverified.
Autofire uses the common authored-order candidate pass: an earlier successful targeted
handler defers Lucky Shot until a later pass, rather than activating both in one pass.
Delayed damage retains no source object ([WAD-26](walks/area-damage.md)), so it neither
receives nor consumes the holder's Lucky bonus.
Immunity and ordinary cause/take-damage modes that leave no positive damage also
preserve the unspent lock.
`tests/replay/lucky_shot_tests.cpp` perturbs the handler name, style, category/type filter,
projectile override, damage multiplier, recharge and autofire support, and compares checkpoint
continuation and worker-count hashes. Ordinary stun and hero-clash state remain the shared
unmodelled interfaces described above; this change does not invent separate Lucky Shot states for them.

## Applicability

### Combat-bonus creation self flag

| Rule | Behavior | Evidence |
| --- | --- | --- |
| ABG-05 | An automatic space `Combat_Bonus_Ability` whose `Apply_Special_Ability_To_Self_On_Creation` is true includes its source in its creation notifications. Omitted or false skips that identity. The source still needs the ordinary categorized-recipient, type, faction and unique-container eligibility; the flag does not bypass those gates. The existing partitioned creation path applies the resulting source-owned contributions. This scope covers non-upgrade source bonuses and does not add self application to other handler kinds or land. | Debug-build creation notification recheck; WHE-13/14/53; XML-to-hull and identity perturbations in `tests/units/unit_tables_synthetic_identity_tests.cpp` at 1/2/4/8 workers. |

### Named point-defence admission

| Rule | Behavior | Evidence |
| --- | --- | --- |
| ABG-04 | An ordinary space `LASER_DEFENSE` slot can activate only while its named `Laser_Defense_Ability` handler is enabled. `Initially_Enabled` defaults to true; false refuses activation without starting recharge. Commands, recorded object turns and published readiness use the same handler state. This admission rule does not implement cancellation of an already running defence when a named lock changes. Independently automatic handlers and land remain separate gaps. | Debug-build named activation recheck; `tests/replay/ability_defence_tests.cpp` perturbs absent/true/false at 1/2/4/8 workers on both activation paths. |

### Lure pulse-count range

| Rule | Behavior | Evidence |
| --- | --- | --- |
| ABG-07 | `LURE` compares its pulse counter with the authored signed `Max_Number_Of_Pulses`. Zero or negative permits no pulse; a positive count has no frame-duration ceiling. The parsed table retains the signed integer bits, while runtime binding maps negative values to zero. The remake derives the pulse index from 64-bit elapsed ticks divided by the pulse period, rather than incrementing a stored counter: reaching `INT_MAX` cannot overflow a counter and per-tick work is constant regardless of the count. Pulse-frequency and ability-duration bounds are unchanged. | Debug-build LURE service recheck; WLU-03; XML counts zero, above 108000, `INT_MAX`, -1 and `INT_MIN` through the pulse consumer; full-session 1/2/4/8 coverage with a count above the frame limit. |

### Mod coverage audit

| Rule | Behaviour and current coverage | Source |
|---|---|---|
| ABG-01 | Automatic space command bonuses and healing use the same nested handler loaders for `SpaceUnit`, `UniqueUnit` and `StarBase`; the XML class alone does not select a different consumer. Ordinary command sources compile the shared WHE-14 type/category filter, including exclusions, into their recipient lists and retain their authored percentage contributions for WHE-53/55 reduction. An explicit applicable type bypasses ordinary exclusions. The separate `DUMMY_UPGRADE` recipient compiler is not covered by that command-source claim. Automatic healers retain their name, mode, filter, flat/percentage amount, interval and single-target choice and receive WSH-01..10 service. Land and nonautomatic handlers remain separate coverage, as do the existing healing bounds, ties and lifecycle seams. | debug build: command eligibility, shared type filtering and healing service rechecked; current loader, command-recipient compiler and healing service audit |
| ABG-02 | The common ability light request retains the authored effect type, first and second colours, duration and pulse count, but starts only when the recipient has no active light effect. `Color_Flash` blends the first colour back to the incoming model colour over its duration; its renderer reads neither the second colour nor pulse count. `Linear_Pulse` and `Smooth_Pulse` use the first colour and pulse count; `Two_Way_Pulse` also uses the second colour. Automatic healing applies `Color_Flash` (first colour and duration), `Linear_Pulse` (first colour, duration and pulse count, ABG-12) and `Two_Way_Pulse` (first and second colours, duration and pulse count, ABG-13). `Smooth_Pulse`, other authored types and occupied-light arbitration remain separate gaps; a parsed count is not an applied count. The same automatic healing light path serves `SpaceUnit`, `UniqueUnit` and `StarBase` by shared code; only a corvette (space unit) healer is GPU-tested. | debug build: owner/target light requests and per-type colour application rechecked; WSH-08/10 |
| ABG-03 | A `UniqueUnit` carrying an ordinary `LASER_DEFENSE` slot uses the same named handler binding, authored run/look timing, projectile filtering and beam presentation as a `SpaceUnit`. The XML class does not select a separate handler. The current ordinary space path consumes the listed timing, range, projectile-category, beam texture/width/frame/colour and bone parameters. Land and independently automatic handlers remain outside this scoped claim; the lightning-particle field is not applied by the simple beam renderer. | debug build: named point-defence handler and beam lifecycle; WLD-01..12; current runtime consumer audit |

The registry audit corrects stale TODO rows for these existing consumers; it changes no
battle behaviour. In particular, it does not extend the campaign-only player-ledger adapter
to standalone battlefield modifiers. That handler's tactical player effects, described by
WGT-50 in [the tactical-entry walk](walks/gc-tactical.md), remain a separate space gap.

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
| AB-03 | **Behaviour ability: `HUNT`.** Squadron targeting and dogfighting now support the persistent hunt patrol. The loader exposes it, commands switch it on/off, and the leader issues coordinated attack-moves under WAB-50 to WAB-56 in [the ability walk](walks/abilities.md). The Y-wing team ion shot remains AB-60 to AB-69. | debug build; data; project |
| AB-04 | Seconds become whole logical frames by truncation: frames = trunc(seconds x 30). `DEFEND`'s 15 s is 450 frames, its 60 s 1800. | research AB-R03 |

### Switching on and off

| Rule | Behaviour | Source |
|---|---|---|
| AB-10 | A unit enters with each ability inactive and ready. Its autofire flags follow the creation preference (AB-45). An ability command never replaces the unit's order: a moving corvette keeps its move when `TURBO` switches on. Automatic activation uses the ability's own trigger (AB-41, AB-68), not the flag alone. | research AB-R01, AB-R02; research AF-R01 |
| AB-11 | Switching an ability on succeeds only when it is ready (AB-13) and off; switching on an ability that is on fails and changes nothing. An ability with an expiration runs for its expiration frames from the switch; when they are up it switches off and its full recharge starts. An ability without an expiration has no timer. | research AB-R02, AB-R03 |
| AB-12 | Switching off an ability that is off does nothing. A timed ability switched off early recharges for the share of its duration it ran: round(elapsed / duration x recharge), halves up, so 150 of `DEFEND`'s 450 frames give 600 frames, and a switch-off in the frame it started gives none. An ability without an expiration switches on and off freely and never recharges (`SPOILER_LOCK`). | research AB-R03 |
| AB-13 | An ability is ready when the unit has it, it is not recharging and its gate holds (AB-14, AB-16). A ready ability stays ready while it is on. This is what the Lua `Is_Ability_Ready` reports, and `Activate_Ability` acts only on a ready ability. | research AB-R02, AB-R13 |
| AB-14 | `DEFEND` may switch on only on a shielded unit whose shield generators are online and whose shield is not in its depletion effect ([space damage](space-damage.md) DG-08). It is also refused while the unit is ion stunned ([space damage](space-damage.md) IS-07, ion weapons, energy drain and stun), and in retail in an ion storm, which M2 does not have. | research AB-R02, IR-04 |
| AB-15 | A squadron container passes a non-team ability request to its live craft in member-index order, and each eligible craft uses its own type's ability data. The first failed request stops the dispatch; earlier successes remain in effect, so a team can have a split state. Manual dispatch does not yet enforce the container's declaration and admission gates (legacy EAWR-6531). | research AB-R09; data; debug build FS-52 |
| AB-16 | When a unit's engines go offline (space-hardpoints HD-11), its `TURBO` and `SPOILER_LOCK` end, early (AB-12). The remake also keeps them from switching on while the engines are offline (**unverified**: the least visible choice; the other order would switch the mode on for one frame). The shared hardpoint gate retains this engine-online policy (WHD-05). | research AB-R14; project |
| AB-17 | Any shield set to zero or below ends an active `DEFEND` (early, AB-12). A shield generator loss drops the shield to zero (DG-17), so it ends `DEFEND` too (HD-12). | research AB-R06, AB-R14 |

### Multipliers while on

| Rule | Behaviour | Source |
|---|---|---|
| AB-20 | A unit's multiplier of one kind is the product of that multiplier over its active abilities among **the first two entries of its type's unit-ability list** (primary and secondary, counted in authored order, every authored child taking a position whether or not the remake models it; a type authored twice counts at its first position); an active ability at entry 2 or later contributes nothing, to a multiplier or a flag, and with none active the multiplier is 1 (debug build `Get_Unit_Mode_Multiplier`, `Get_Unit_Mode_Modifier_Flag`). Retail gives a team container the minimum over its members when its own multiplier is 1; no modelled ability is a team ability, so the remake has no such case. | research AB-R04; debug build (list load: one push_back per `Unit_Abilities_Data` child in authored order, no de-duplication; `Get_Unit_Ability(type, index)` reads that vector; walked 2026-11-20) |
| AB-21 | **Weapon delay.** The gap between two shots of one burst is multiplied by the weapon delay multiplier, rounded half up. The full recharge after a burst takes the multiplier only when it is below 1 (it may shorten the recharge, never lengthen it): `TURBO`'s 3 spreads the corvette's bursts out, `POWER_TO_WEAPONS`' 0.2 cuts the Tartan's recharge to a fifth. Each hardpoint also has a per-ability delay multiplier (default 1) that WAB-81 combines with the unit's: only the abilities at list entries 0 and 1 count. | research AB-R05; data |
| AB-22 | **Shield regeneration.** Each shield recharge adds the refresh amount x the shield regen multiplier x the generator fraction, clamped to [0, maximum]; a negative multiplier drains (`POWER_TO_WEAPONS`), 0 stops it (`TURBO`). The recharge interval is (int)(3.0 s x the interval multiplier x 30 + 0.5) frames, at least 1: `DEFEND`'s 0.1 gives 9 frames. An authored interval multiplier of 0 loads: the debug-build getter applies no clamp (0 frames) and the behaviour re-arms at the current frame plus its stored interval (FS-11), so it is serviced on every traversal, which the one-frame floor reproduces (shield and energy alike, AB-23). A negative or above-64 multiplier is still refused by the remake. | research AB-R06; recording S-15 (+50 every 9 ticks) |
| AB-23 | **Energy regeneration.** The energy pool's recharge amount is multiplied by the energy regen multiplier and its interval by the energy interval multiplier, rounded as AB-22 (`DEFEND`: 5 x 0.1 gives 15 frames). | research AB-R07 |
| AB-24 | **Speed.** Maximum speed, acceleration and deceleration are multiplied by the speed multiplier; minimum speed is not (retail computes it and discards the result). A craft's locomotor reads the same maximum. A move under way plans again from the tick the multiplier changes, so the new speed takes effect with a move's usual two-tick latency: in S-17 `TURBO` at tick 60 changes the corvette's motion from tick 62, and it cruises at 7.44 instead of 3.72. | research AB-R08; recording S-17 |
| AB-25 | No modelled ability changes the diminishing-firepower gate ([space damage](space-damage.md) DG-05): only a `MAXIMUM_FIREPOWER`-style ability turns the shooter's flag off, and none is in the M2 fleet. The flag stays on for every M2 shooter. | research AB-R04; data |
| AB-26 | **Modifier vocabulary.** The ability table retains twelve `Mod_Multiplier` names, matched without regard to case: `WEAPON_DELAY_MULTIPLIER`, `SHIELD_REGEN_MULTIPLIER`, `SHIELD_REGEN_INTERVAL_MULTIPLIER`, `ENERGY_REGEN_MULTIPLIER`, `ENERGY_REGEN_INTERVAL_MULTIPLIER` and `SPEED_MULTIPLIER` (AB-21 to AB-24), `FIRE_RATE_MULTIPLIER` (AB-27), `SCATTER_RADIUS_MULTIPLIER` (AB-28), `CAUSE_DAMAGE_MULTIPLIER` and `TAKE_DAMAGE_MULTIPLIER` (AB-29), plus `TARGET_SPEED_MULTIPLIER` and `TARGET_TAKE_DAMAGE_MULTIPLIER`. A multiplier not authored is 1. It also retains the three `Mod_Flag` names: `WEAPONS_ENABLED_FLAG`, `CAN_BE_SQUASHED` and `ALLOW_TARGET_DIMINISHING_FIREPOWER`, preserving authored false values. Unknown names are ignored with a load note, and a supported ability keeps its remaining modifiers (legacy EAWR-2194). **Retail** matches names without regard to case against its table of these twelve multipliers and three flags, stores nothing for an unknown name and still loads the rest of the ability. The remake applies ten multiplier names. The debug build recognizes and stores `TARGET_SPEED_MULTIPLIER` and `TARGET_TAKE_DAMAGE_MULTIPLIER` but never requests either value from its multiplier accessors: they have no gameplay effect. The remake retains them for content identity and reports an informational note that the original ignores them. The three flags retain pending-consumer notes; their victim/land consumers remain unmodelled. Duplicate flags and multipliers use the last authored value as an **inferred host choice**, with retail precedence **unverified** (see Unknowns). Stock FoC XML authors only one of the extra names, `ALLOW_TARGET_DIMINISHING_FIREPOWER` (false), on the land `MAXIMUM_FIREPOWER` ability (AB-25); no stock ability authors the others, and none is in the M2 fleet. **Company-copy exception:** HeroCompany containers skip the ability loader (`src/units/unit_tables_decode.cpp:254`), so the `General_Veers_Team` company's own `Unit_Abilities_Data`, including its multiplier and flag copy, is neither retained nor diagnosed. The walker and its inherited death clones carry the retained flag. Checked against the Focumentation wiki (Ability Modifiers; Ability modifier types), which lists the same names under different counts; the debug build's tables are authoritative. | debug build (F2165-E01, E02, E04; ABT-E01 to ABT-E03); stock XML; project; unverified duplicate precedence |
| AB-27 | **Fire rate.** An object weapon's full recharge and pulse gap are divided by the fire rate multiplier, and the refilled pulse count is scaled by it ([barrage](barrage.md): `BARRAGE` authors 3). | data; project |
| AB-28 | **Scatter radius.** While the shooter's scatter radius multiplier differs from 1, the blast radius of the projectiles it launches is multiplied by it. No stock M2 ability authors it; it is read so that data using it stays modelled. Retail applies the same factor to the blast radius of an area projectile, from the object that fired it. | debug build (F2165-E04); project |
| AB-29 | **Damage modes.** `CAUSE_DAMAGE_MULTIPLIER` multiplies the damage the unit deals, and `TAKE_DAMAGE_MULTIPLIER` the damage it takes, in the damage service ([space damage](space-damage.md) DG-02). A product of 0 makes the unit immune to that damage (the Falcon's invulnerability); `LURE` authors 0.7 take damage (WLU-08). Privileged script damage skips take damage but not cause damage (WHE-51 in [the heroes walk](walks/heroes.md)). | data; project |

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
| AB-42 | The rate of damage taken is serviced every 30 frames: the hull and shield the unit lost since the last service, times 30 / 30, per second. The remake closes the window when the tick is a multiple of 30 (**unverified** phase; see AB-U1). The rate is the unit's damage tracking (capital-combat walk WCC-82), kept for every type with the `DAMAGE_TRACKING` behaviour, the Nebulon-B and the MC80 among them; a type without it has a rate of 0. | research AB-R12; debug build |
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
| AB-63 | Switched on, the container and every live craft of the squadron are on and the target is stored; the squadron takes an attack order on the target (space-fighters FO-01). While the ability is on, the squadron is ordered onto the target again whenever its attack target differs, provided no new order has ended the shot (WAB-40). A move (plain or queued), attack, escort or guard, including Lua `Move_To` and `Attack_Target`, ends the shot before applying that order, including when the receiver is a squadron member: cancellation resolves to its parent container; the ability's own lock-on order does not. | research IR-11; walk abilities WAB-40, debug build EAB-06 |
| AB-64 | Each frame, the ability stays on while at least one craft still has its shot due and the target is a live enemy; otherwise it ends (AB-65). FoC also ends it when the container or the target is in a nebula (**not modelled**: M2 units carry no nebula state). | research IR-11 |
| AB-65 | Ending switches the container and every craft off and forgets the target; the container recharges (600 frames) only when at least one craft fired, so a shot cancelled before any craft fired costs nothing. Ending on a new order uses this same rule (WAB-40); the squadron then follows the new order. | research IR-11; walk abilities WAB-40, debug build EAB-06 |
| AB-66 | While the ability is on, each craft's hardpoints fire only the override projectile, only at the target (a craft of a targeted squadron counts), at its hardpoint if one was aimed at; with the hardpoint's `Damage_Type`, own damage if it authors one and `Fire_Range_Distance` (space-damage DG-12, DG-25, DG-23; no Y-wing hardpoint authors a damage) and its own recharge (a Y-wing's torpedo hardpoint may still be recharging). A craft that has already fired, or a shot at anything else, stays silent until the ability ends. | research IR-11, IR-09, IR-12 |
| AB-67 | A craft that fires the override at the target has fired it: its own slot switches off. One ion bolt per Y-wing. | research IR-11 |
| AB-68 | **Autofire.** With autofire on, a ready ion shot locks onto the squadron's attack target when that is a valid target (AB-62). A player the engine plays (not human) always has autofire (AB-40), so the Rebel AI's Y-wings use it; a human player's only when they switch it on. FoC queues the ability when the unit attacks; when exactly it is asked is not traced (**unverified**): the remake asks every frame while the squadron attacks and the ability is ready. | research IR-11 (autofire); project |
| AB-69 | FoC's Lua `Activate_Ability` takes a target object for a targeted ability and issues it on the unit: called on a Y-wing squadron with a Tartan, it fires the ion shot at the Tartan (retail recording, ion weapons, energy drain and stun). No M2 script calls it for `ION_CANNON_SHOT`; only a story mission queries it. So the remake reports such a call instead of issuing it (AB-U8). `Is_Ability_Ready` and the other queries read the container's slot. | research IR-13; retail recording; data |
| AB-70 | **Authored ion handler admission.** The team ability's `GUI_Activated_Ability_Name` selects its nested `Ion_Cannon_Shot_Attack_Ability` by case-insensitive `Name`; no matching handler means activation fails. The selected handler's explicit included type admits first; otherwise the target must overlap `Applicable_Unit_Categories` and avoid excluded categories and types. Empty/omitted categories admit nothing unless an included type matches. The ion handler defaults to `Special_Attack`, overriding the base unspecified style. An attack-target pass admits that exact style without an autofire flag; a `User_Input` handler joins only while autofire is on (including the AI default). `Space_Automatic`, `Ground_Automatic`, `Global_Automatic` and `Combat_Automatic` handlers do not join this attack-target pass, even with autofire on: mode eligibility alone does not make them attack triggers (debug build ION-E01). Named activation accepts these tactical styles or an automatic style admitted in that battle mode; explicit unsupported styles remain inert. The existing AB-60 to AB-68 firing, cancellation and recharge remain unchanged. Craft firing slots do not require their own nested handler. Stock Y-wing, TIE Defender and Hutt V-wing team handlers author matching names, `User_Input` and `All`; these bindings preserve their visible behavior while making authored variants effective. | debug build ION-E01; authored XML; synthetic XML and deterministic ion admission/checkpoint contracts |

<a id="the-command-bar-api-454"></a>

### The command bar API

| Rule | Behaviour | Source |
|---|---|---|
| AB-50 | Each snapshot instance carries one status per ability: its kind; whether it is on, ready (AB-13) and on autofire; whether it supports autofire; and its timer: while a timed ability is on, the frames left of its duration and the duration; while it recharges, the frames left and the recharge; otherwise both zero. A command bar switches abilities with ability commands on the selected units (a squadron container stands for its craft, AB-15) and reads the result from the next snapshot; a rejected command reports `ability_unavailable`. The live view implements ability buttons and recharge dials's `AbilityState` and `AbilityCommands` this way (foc-ability-buttons.md, "Interface to the simulation"). | project |

<a id="ability-kinds-the-build-knows"></a>

### Ability kinds the build knows (Focumentation cross-check)

| Rule | Behaviour | Source |
|---|---|---|
| AB-70 | **The kind list.** The FoC build recognises 75 authorable ability kinds by name, plus an internal "none" and one internal garrison placeholder that has no XML name. The Focumentation page "Unit ability types" lists 64 of them, a list that stops short of the expansion. Kinds the build has that the wiki list lacks: `HARASS`, `BARRAGE`, `HUNT`, `LURE`, `AVOID_DANGER`, `DEPLOY_TROOPERS`, `SELF_DESTRUCT`, `CLUSTER_BOMB`, `INFECTION`, `BUZZ_DROIDS` and `CORRUPT_SYSTEMS`. Every wiki kind is in the build. The stock data authors `HUNT`, `LURE`, `BARRAGE`, `SELF_DESTRUCT`, `CLUSTER_BOMB` and `BUZZ_DROIDS` on space units (AB-03 and the ability walk cover them), `DEPLOY_TROOPERS` on land units, and `INFECTION` and `CORRUPT_SYSTEMS` on one Underworld hero; no stock object authors `HARASS` or `AVOID_DANGER`. A kind name outside the 75 is not an ability (it maps to none); the loader likewise skips kinds it does not model (unit census M141 to M156). Use the build's list, not the wiki list, as the source of kind names. | debug build F2166-E12; stock XML; Focumentation: Unit ability types |

### Recognized target multipliers without an effect

The complete debug-build multiplier-accessor caller audit (ABT-E01 to ABT-E03)
finds no request for `TARGET_SPEED_MULTIPLIER` or
`TARGET_TAKE_DAMAGE_MULTIPLIER`. The speed consumers request the ordinary speed
multiplier. Damage requests the victim's ordinary take-damage multiplier and the
source's cause-damage multiplier, including Lucky Shot's separate cause-damage
read. The other callers request weapon timing, regeneration, scatter and
lock-setup modifiers. The two target names therefore select no recipients and
create no effect to stack, time, refresh or remove; they change neither shield
nor hull damage and impose no speed limit. A nested handler's
`Target_Speed_Decrease_Percent` remains its independent effect (WHE-60).

A surveyed mod authors the target-speed name alongside a tractor beam; authoring
a recognized name does not give it an effect. The no-effect contract perturbs
each name independently and together on active primary and secondary abilities,
with a locked target, ordinary speed and damage modes, shield and hull damage,
movement, expiry and early release. Every gameplay hash stays equal across the
perturbations and 1/2/4/8 workers. Authored values still distinguish content
identity; the contract does not require different content to have the same
content digest.

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

## Ability geometry during banking

| Rule | Behaviour | Evidence |
|---|---|---|
| ABF-01 | Energy-beam fallback selects standing target hardpoints at their current world bone positions. Buzz Droids likewise choose the nearest standing world hardpoint for damage reach and routing. Both positions include the victim's bank (BK-05, WWP-73). The ordinary energy fallback's targeting-priority policy is unchanged. | debug build: energy lock fallback, ordinary hardpoint selection, Buzz victim selection and world bone placement |
| ABF-02 | Corrupt Systems and space automatic healing first accept inclusive planar centre reach. Outside-centre sphere overlap uses the current oriented hull box, including bank. Collection bounds enclose that same rolled model box; otherwise the broad phase can discard a recipient that the narrow phase would accept. | debug build: both abilities' collidable queries, outside-centre matrix overlap and model world bounds |
| ABF-03 | The beam range's planar facing axis and redirect-blaster yaw gate are roll-invariant. They retain level heading. Movement placement scratch warms a level-heading cache for locomotion; it does not supply collision bounds or hardpoint positions. Healing's footprint-only approximation, when no model box exists, retains the separately named inferred WSH-05 seam. | debug build: targeted range and redirect yaw gate; BK-05; project cache ownership and WSH-05 fallback policy |

`tactical_ability_contracts` distinguishes level and rolled nearest hardpoints for energy beams
and Buzz Droids, includes a hull that enters the area sphere only when rolled, excludes a hull
that overlaps only when level, and heals a rolled box outside centre reach. Each case compares
state and snapshot hashes on 1/2/4/8 workers. Shared victim scans use immutable placement
reads; only each owning partition may update a unit's derived caches.

## Remaining ability unknowns

The [hardpoint walk](walks/abilities.md#hardpoint-dependencies-legacy-2420)
records typed dependencies (WHD-01..04), named owner/type locks (WHD-05) and HUD
admission (WHD-06). Missing or disabled required points refuse activation; a
surviving alternative permits it. Named locks and station comm-array radar remain
unimplemented; the existing supply-dock income predicate is not a general lock.

WHD-07 distinguishes ordinary stun from ion stun. The ordinary-stun lifecycle
refuses tractor, energy weapon, superlaser and lucky shot. M2 models only ion stun,
which does not set ordinary stun; tractor and energy weapon stay available under
ion stun when their other gates hold. DEFEND retains its distinct ion-stun refusal
(AB-14, IS-07). Ordinary-stun availability must be wired when that lifecycle is
implemented, without reusing the ion-stun gate.

| ID | Unknown | Effect |
|---|---|---|
| AB-U1 | The phase of retail's damage-rate service. Each object's service runs 30 frames after its own start, not on a shared clock. | A switch-on can land up to 29 frames apart from retail's. In the S-15 recording the frigate takes about 8 damage every 90 frames until the 400-point hit at tick 500 and switches `DEFEND` on at 529. Since retail time-to-kill comparison the remake's frigate also takes about 8 damage a hit from the Tartan's lasers (space-damage DG-24, DG-36; before it took about 21 a second and switched `DEFEND` on at tick 60), so its stand-in switches `DEFEND` on at tick 511, after the same hit. The duel's end is unaffected (DEFEND leaves the weapons as they are). |
| AB-U2 | Where retail's replanned move ends. | S-17's retail corvette stops 55.8 units short of its target after `TURBO`; the remake's replanned move arrives. |
| AB-U3 | FS-52 settles member order: a non-team request visits eligible members in order and stops at the first failure; it does not precheck the whole team. Manual dispatch does not yet enforce the container's declaration and admission gates (legacy EAWR-6531). | AB-15; FS-52. |
| AB-U4 | Whether lost engines block `TURBO` from switching on or only end it (AB-16). | At most a one-frame flicker in retail. |
| AB-U5 | The order within a frame of the shield hit that ends `DEFEND` and the move plan. | The remake plans the slower speed from the next tick (one tick late after a depletion). |
| AB-U6 | `DEFEND` blocking hangar launches (space-hardpoints HD-13). | No M2 type has both a hangar and `DEFEND`; not modelled. |
| AB-U7 | The exact frame FoC asks an autofire ion shot to lock on (AB-68). The matched S-48 archive contains six ion contacts, so the earlier claim that quick-loaded non-human recordings never fired ion shots is withdrawn. The debug build notifies nested targeted abilities from an attack command (AF-R04), but the complete retry cadence remains unverified. | The remake may lock on earlier after a recharge than FoC; the Rebel AI's Y-wings use the shot on autofire (AB-68). |
| AB-U8 | A Lua `Activate_Ability` with a target (AB-69). | No M2 script issues one; the remake reports it. |

Duplicate `Mod_Flag` and `Mod_Multiplier` precedence (AB-26) is unverified in retail. For abilities read by the loader, the remake retains every authored occurrence for content identity and uses the last value in the ability profile as an inferred host choice. Conflicting duplicate flags or multipliers in mod data may therefore differ from retail; stock data has no duplicate flags.

**APD-01 — point ability preview.** The loader retains the authored distance for
every ability type. Entering point targeting with a positive
`Area_Effect_Decal_Distance` creates a cursor-centred overlay for the local player.
Its square has side twice that distance times the local faction's
`Land_Area_Effect_Range_Overlay_Scale_Factor`; material and tint come from the
faction's `Land_Area_Effect_Range_Overlay_Material_Name` and
`Land_Area_Effect_Range_Overlay_RGBA`. Zero gives no overlay. A squadron uses its
leader's ability data. Right-click ends targeting; the Broadside retail capture
shows the overlay remains behind the menu opened by Escape. The targeting radius
does not alter the ability's effect or reveal range (BARR-04, WFP-01).
Evidence: debug build and Broadside retail capture.

**ABG-06 — healing owner bone candidates.** `Owner_Particle_Bone_Name` is an
ordered list, split on commas and whitespace. Choose the first name present in
the owner's model, using case-insensitive bone lookup. If no candidate exists,
including an omitted or empty list, use `ROOT`. The healing owner's particle and
the source endpoint of its lightning use that same choice. Resolve the choice
once when preparing the loaded type; the model and authored list do not change
between pulses. This covers automatic space healing; land and nonautomatic
healing presentation remain separate consumers. Evidence: debug build.

**ABG-12 — finite linear healing lights.** An automatic space healer may author
`Linear_Pulse` for its owner or target light. Over `Light_Effect_Duration`, each
of the `Light_Effect_Pulse_Count` equal periods rises linearly from the object's
base light to the authored first color at its midpoint and returns to base.
Owner and target have independent color, duration and count. `Color_Flash`
continues to return linearly from its color to base over the whole duration;
it does not consume the pulse count. The existing event-driven healing light
binding selects the renderer's finite pulse, with no new frame traversal.
Evidence: debug build; WSH-08. Second colors belong to `Two_Way_Pulse` (ABG-13). Other
light styles, runtime team presentation beyond ABG-18 and occupied-light arbitration
remain separate coverage gaps.

**ABG-13 — two-color healing pulses.** `Two_Way_Pulse` divides the finite
duration into the authored number of equal periods. A period starts at the
object's base light, reaches the first color at one quarter, returns to base at
half, reaches `Light_Effect_Color2` at three quarters and returns to base at its
end. The blend strength follows the magnitude of one sine cycle; its sign chooses
the first or second color. Owner and target retain independent colors, duration
and count. The effect ends at the duration boundary. Only this style consumes a
second color; Color_Flash and Linear_Pulse ignore it. Evidence: debug build.
Automatic space healing uses the existing material-light pass and opt-in
verification samples. Other styles, runtime-team presentation beyond ABG-18, count bounds and
occupied-light arbitration retain their separate gaps.

**ABG-18 — runtime team healing recipient effects.** A squadron's healing
pulse indexes the nested handlers retained from its `Create_Team_Type`, not the
purchase template's own handlers. Particle and bolt preparation, recipient
material lights and recipient pulse audio select that same runtime handler
list. A missing purchase handler or a different purchase entry at the index
cannot replace the runtime effect. The adapters retain their existing target
bone selection, light styles and audio dispatch (the squadron's own owner light
uses the same list); the effect follows the
already-published heal event and adds no world or frame scan. Evidence: debug
build's runtime-type ability lookup and shared healing presentation (AB-60,
WSH-08). This recipient scope does not establish a visible owner model for a
team, owner-bone attachment to its craft, land/nonautomatic healing, other light
styles, occupied-light arbitration or the nonzero-ROOT fallback. The simulation now services a squadron company's healing (ABG-09, WSH-13), and the
Separate GPU cases (one recipient, several recipients, a recipient killed mid-pulse, none in range) show the
recipient effects, audio and lights from that runtime event.

**ABG-08 — healing target presentation.** For automatic space healing, a
successful recipient pulse presents `Target_Particle_Effect` at the first
available name in the ordered `Target_Particle_Bone_Name` list. Both ordinary
space units and unique units or starbases use the same consumer. These are
presentation effects; they do not change recipient selection or healing.
Evidence: debug build; WSH-08. The current adapter falls back to bone index zero
when no candidate matches; a model whose `ROOT` is elsewhere still needs that
fallback corrected. Land and nonautomatic healing presentation remain separate.

**ABG-09 — runtime team healing scope.** A squadron's `Create_Team_Type`
resolves its runtime container's nested `Force_Healing_Ability` declaration.
The ability table binds those handlers to the session's squadron identity,
including name, activation style, recipient categories/types/exclusions, range,
amount, percentage, interval and single-target choice. The ordinary automatic
healing service consumes them on the container itself: a squadron company has no hull and
serves while it exists (WSH-13). This does not establish support for an arbitrary Container object that
is not reached as a runtime team type. Evidence: debug build and existing team
loading/ability binding. Recipient presentation for those handlers is ABG-18.
Team range-blob data is retained, but its runtime selection path has not been
verified here.

**ABG-10 — unique-unit LURE parameters.** Ordinary LURE binding is shared by
space holders, including UniqueUnit: `Pulse_Frequency_Secs` sets the pulse period
and `Target_Types` supplies exact type identities for the search (WLU-03/04).
This does not extend support to other ability families that author the same
tags. LURE does not read `Is_Pulsing`; other families' use of that flag has not
been established, so its class-wide coverage remains open. Evidence: debug build.

**ABG-11 — space harmonic-bomb countdown.** A SpaceUnit's ordinary
`HARMONIC_BOMB` declaration reads `Bomb_Countdown_Seconds`, binds it to the
spawned-object profile and starts the created bomb's countdown (WHE-61/62).
The current nested reader is specific to HARMONIC_BOMB; it does not read this tag
for WEAKEN_ENEMY or establish support for every other spawned-ability family.
Evidence: debug build and the shared space loader/activation consumer.

**ABG-14 — ordinary station point defence.** A StarBase with an ordinary
`LASER_DEFENSE` slot uses the shared space loader, activation, projectile filter,
radius, duration and recharge consumers (WLD-01..05). Its beam uses the authored
bones, texture, width, colour and frame count (WLD-12). This scope requires the
ordinary slot; it does not establish execution of an independently automatic
nested handler or land point defence. Evidence: debug build and the shared
ability compiler, defence service and beam renderer.

**ABG-15 — custom labels and ordinary countdowns.** `SkirmishSpaceUnit` is an
XML element label, not an additional ability implementation (SG-CLS-01). When
its resolved object is an ordinary space holder, the shared ability compiler
binds `Recharge_Seconds` and `Expiration_Seconds` for supported ability kinds.
Kind-specific timing rules still apply. A runtime team or craft uses its own
ability rows (AB-15/60), and an unloaded scene object has no countdown consumer.
Evidence: debug-build ability getters and the class resolver/ability compiler.

**ABG-16 — automatic runtime team jammers.** The `Create_Team_Type` loader
retains the runtime Container's nested handler name and activation style on the
squadron profile (AB-60). Automatic jammer service uses that handler to register
the source and apply its existing jamming rules (WHE-32, FS-45/46, FC-77,
FD-75). The ordinary team compiler does not publish a `SENSOR_JAMMING` slot, so
this does not establish manual team activation. The debug build's owner
particle is attached through activation, service and termination; current
automatic registration does not publish that persistent presentation lifecycle.
Evidence: debug build and the runtime team loader/compiler and jammer service.

**ABG-17 — unresolved defence lightning placement.** The point-defence loader
retains `Lightning_Effect_Name`, while the current renderer consumes only the
authored beam look (WLD-12). Rechecking debug-build service, beam creation and
update, hit and termination paths did not establish an independent lightning
placement or lifecycle. Keep that consumer unresolved for UniqueUnit and
StarBase; neither parsing nor an ordinary beam demonstrates application of the
lightning field, and these checks do not establish that the original ignores it.

**ABG-19 — healing target ROOT fallback.** Automatic space healing checks
`Target_Particle_Bone_Name` candidates in authored order and uses the first name
present on the recipient model. If the list is omitted, blank or has no matching
candidate, the fallback names `ROOT`; it does not prescribe bone index zero.
The model's ROOT index is resolved once during preparation and supplies the
existing particle attachment and bolt endpoint fallback. The runtime request
still follows the published heal pulse. Evidence: debug build's shared usable
bone finder and WSH-08. The GPU case edits only a temporary model copy: its zero
bone loses the ROOT name, bone two gains it, and an explicit existing candidate
still takes precedence. Models without a ROOT name retain the current adapter's
zero-index fallback; its parity is not established here. Land, nonautomatic and
runtime-team presentation retain their separate adapter scopes.
