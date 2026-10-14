# Focumentation cross-check: combat, abilities, projectiles

Source: the Focumentation modding wiki (focumentation.fandom.com, CC BY-SA; local copy under
`out/research/focumentation/pages/`, URLs in `out/research/focumentation/MANIFEST.json`). Pages read: Unit Abilities,
Ability Modifiers, Ability modifier types, Unit ability types, Ability activation types,
Abilities, Special Projectiles, Projectile Categories, Projectile Combat Mods, Behaviours, and
the tables of XML Tags (lines 1-193). Everything below is a paraphrase; the wiki is cited by page
title. The wiki was treated as untrusted data.

## Summary

- The wiki is mostly annotated XML examples. Where it gives mechanics, our docs nearly always
  already say the same thing, usually in more detail (traced in the debug build). 30 claims agree.
- 11 conflicts. Most are on our side: the wiki's modifier list shows that our own documents
  disagree with each other and with the loader. `space-abilities.md` AB-20 to AB-26 and the
  `docs/unit-data.md` "Ability table" section still describe a six-name modifier vocabulary
  that fails the load. In fact the heroes walk, LURE and BARRAGE depend on `TAKE_DAMAGE`,
  `CAUSE_DAMAGE` and `FIRE_RATE`, and the loader accepts ten names and skips anything else.
  The black-market gate is also recorded three different ways. The real external conflicts are
  BARRAGE's range source and how the results screen groups units (Text_ID or type).
- 22 new items. The ones that matter for M2 are these: the undocumented modifier and flag
  names; ability-mode hardpoint gates (`Fire_When_In_*_Mode`); the missing roster kinds
  (MISSILE_SHIELD, INTERDICT, CLUSTER_BOMB, FOW_REVEAL_PING), where the wiki gives the data
  shape; and unit-card tooltip rules (multiplayer description override, up to three good and
  bad icons, comma line breaks). Many others are for land or galactic play.
- Many of the wiki's example values (tractor beam, laser defense, power to weapons, self
  destruct) differ from ours. They come from other units: the vanilla Star Destroyer, the HAV
  Juggernaut and the MZ8 tank. They are not conflicts with our FoC readings.

## CONFLICTS (most important first)

| # | Wiki claim (page) | Our docs / registry | Why it matters | Next step | Verdict |
|---|---|---|---|---|---|
| C1 | There are many `Mod_Multiplier` names. One page says nine (cause damage, weapon delay, shield regen and interval, energy regen and interval, speed, take damage, fire rate). Another lists fifteen, adding scatter radius, target speed, target take damage, a weapons-enabled flag, a can-be-squashed flag and the diminishing-firepower flag (Ability Modifiers; Ability modifier types). | `space-abilities.md` AB-20 to AB-24 model six kinds. AB-26 says any other name **fails the ability table load** (EAWR-UNITS-0006). `docs/unit-data.md` "Ability table" says the same and still calls HUNT and ION_CANNON_SHOT cut, which AB-03 contradicts. But WHE-22, WLU-08 (LURE take damage 0.7) and `barrage.md` (FIRE_RATE) rely on extra names. `src/units/unit_abilities.cpp` accepts ten names, including SCATTER_RADIUS, FIRE_RATE, CAUSE_DAMAGE and TAKE_DAMAGE, and quietly **skips** an ability with an unknown name rather than failing. WHE-22's "differs" column (loader rejects cause damage) is also out of date. | M2 rule text no longer matches the loader. Unmodelled data is dropped silently, not refused. | Rewrite AB-20/AB-26 and the unit-data.md "Ability table" section to list the ten accepted names, the fail-or-skip policy and the consumer rule for each name. Add a SCATTER_RADIUS rule (no doc covers it). Refresh WHE-22's status. | both partly; AB-26..29, unit-data; (legacy EAWR-2194); (legacy EAWR-2165) |
| C2 | Many space abilities (FULL_SALVO, LEECH_SHIELDS, BUZZ_DROIDS, SWAP_WEAPONS, SELF_DESTRUCT on MZ8) author `Must_Be_Bought_On_Black_Market`, which implies the ability is gated behind a purchase (Unit Abilities). | WLD-01 and WST-01 (abilities walk) say the flag does **not** gate in a multiplayer tactical game, skirmish included (WHE-38). WHE-37 (heroes walk) still calls the skirmish purchase gate **U-04, open**. The registry has `Unit_Ability/Must_Be_Bought_On_Black_Market` as `todo` (t760). | Three different statuses for one rule. In M3, galactic conquest is where the gate does apply. | Make WHE-37 agree with WST-01. Set the registry row to the settled status. Add a GC rule (M3: the gate applies through the black market) to gc-economy-production. | ours right (skirmish; gates GC-launched battles, M3); WHE-37, WLD-01, WGEP-145; (legacy EAWR-2165, EAWR-2164) |
| C3 | BARRAGE's own `Effective_Radius` is no longer used. The reach and range of barrage mode come from the override projectile's `Projectile_Max_Flight_Distance` (Unit Abilities, BARRAGE comment). | `barrage.md` says the ordinary target-validity and **range** services decide whether a shot fires, and the HUD adds no range gate. WAD-38 only records the override's flight distance (3000) as an input. | Broadside and Marauder are in the RG-03 roster (M2). The click range and fire range of a barrage could differ from retail. | Verify in the debug build what limits barrage activation and firing range. Rig capture: order a barrage just inside and just outside 3000 units and outside the hardpoint range. | both partly; BARR-02..04; capture-needed BARR-U1; (legacy EAWR-2166) |
| C4 | `Text_ID` decides how units are grouped on the battle summary screens (XML Tags, unit descriptions). | WBF-45 (battle-flow walk): results count destroyed objects **by type** for the local owner against enemies. | Variants that share a Text_ID would merge in retail but stay separate in ours. Affects the M2 results screen (legacy EAWR-616). | Verify in the debug build (results aggregation key). Rig capture with two variant types that share a Text_ID. | ours right; WBF-45a; (legacy EAWR-2166) |
| C5 | Modifiers work for abilities that have a duration, for example DEFEND (Ability modifier types). | AB-02 and AB-12: SPOILER_LOCK has **no** expiration and still applies its multipliers while on (speed 1.3 and so on, recording/owner evidence). | The wiki is too narrow. Ours is right. Low. | No action. Note the wiki error only if the claim gets cited. | not in scope; (legacy EAWR-2166) |
| C6 | LURE authors `Is_Pulsing` Yes alongside its pulse frequency and maximum count, which suggests the flag switches pulsing on (Unit Abilities, LURE). | Registry: `Unit_Ability/Is_Pulsing` on SpaceUnit is **foc-ignores**. WLU-03 pulses from the frequency and count alone. | Low: it matters only to mods that set the flag to No. | Check that the foc-ignores note says the pulse cadence does not read the flag (link WLU-03). | ours right (registry foc-ignores kept); WLU-03; (legacy EAWR-2164) |
| C7 | The ion planetary cannon's stun duration "must equal" the projectile's engine-disable duration (Special Projectiles, comment). | WSW-24 (weapons walk) reads engine disable 10 s against a stun of 30 s for the same projectile. The registry says the stock small-ion engine disable is 20 s. | It is a data-authoring note, not a mechanic. But it raises the question of which timer limits the unit's speed after 10 s. | Check in the data and the debug build. A 0.999 speed cut probably makes the difference moot. Record it under WSW-24. | both partly; WSW-24; (legacy EAWR-2166) |
| C8 | `Ability_Name` is used inside `Enable_Ability` blocks (XML Tags). | WRB-05 shows this **applied** (upgrade `Enable_Ability` enables redirect handlers), but the registry row `Abilities/Enable_Ability/Ability_Name` (UpgradeObject) is still `todo` (t760). | Our registry is inconsistent. | Update the registry status, citing WRB-05. | wiki right (registry stale, partial (legacy EAWR-760)); (legacy EAWR-2164) |
| C9 | `Ability_Cooldown_Time_In_Secs` is a general ability tag taking 0 to 999,999 (XML Tags, Abilities table). | The registry finds it only under hero clash (`Clash_Actions/Special_Ability_Action`, todo t650). | The wiki probably files it in the wrong place. Low. | None. Do not treat it as a generic ability timer. | not in scope; (legacy EAWR-2166) |
| C10 | The page calls the tag `<Behaviour>` (British spelling) and lists `Selectable` in mixed case and `DUMMY_STARSHIP DUMMY_CANT_PATHFIND` as one entry (Behaviours). | The real tag is `Behavior` (registry. The wiki's own Special Projectiles page uses it too). Our docs nowhere say whether behaviour values match regardless of case. | A mod that copies the wiki spelling would load nothing. A low risk for mod-loading accuracy. | Confirm in the registry or loader that an unknown `Behaviour` tag is reported and not aliased. Verify in the debug build whether behaviour names are case-insensitive. | not in scope; (legacy EAWR-2166) |
| C11 | Unit ability types gives the "possible" kinds (63 names) (Unit ability types). | Our docs model or list FoC kinds that are not on it: HUNT, LURE, BARRAGE, SELF_DESTRUCT, BUZZ_DROIDS, CLUSTER_BOMB, CORRUPT_SYSTEMS (abilities walk, foc-ability-buttons AB-10). | The wiki list is EaW-era and incomplete. Low. | Do not use it as a source of kind names. | ours right (wiki list incomplete, 75 kinds); AB-70; (legacy EAWR-2166) |

Note, not a conflict: the wiki's example numbers for TRACTOR_BEAM (0.75 target slow, speed 0.8,
range 10-1000), LASER_DEFENSE (28 s, 0.25 s look, radius 350, distance flown 40),
POWER_TO_WEAPONS (cause damage 3, shield regen -2) and SELF_DESTRUCT (2 s fuse) come from
different units than ours. The heroes walk table, WLD-01 to WLD-04, AB-02 and WSD-03 already
hold the values we use.

## NEW (grouped by the doc that should own it)

### space-abilities.md / walks/abilities.md

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N1 | More modifier and flag names: `SCATTER_RADIUS_MULTIPLIER`, `TARGET_SPEED_MULTIPLIER`, `TARGET_TAKE_DAMAGE_MULTIPLIER`, `WEAPONS_ENABLED_FLAG`, `CAN_BE_SQUASHED` (Ability modifier types). Our docs mention none of them. The loader reads SCATTER_RADIUS without a rule. | M2 (scatter: loader already reads it), later (others) | Verify in the debug build: the full name table and what each one changes. Check the FoC data for any space user. Add the rules with C1. | wiki right; AB-26; (legacy EAWR-2194); (legacy EAWR-2165) |
| N2 | MISSILE_SHIELD shape: locks movement (`Disable_Movement`), has a radius and a timer and recharge (Unit Abilities). INTERDICT also locks movement (Unit Abilities). Census M150 and M156 are `missing`. Registry `Unit_Ability/Disable_Movement` for SpaceUnit is `todo`. | M2 (Interdictor in the roster) | Add an INTERDICT and MISSILE_SHIELD section to the abilities walk. Trace in the debug build (WLD-09 already knows the missile shield's category set). | confirmed; WIN-01..04, WMS-01..04; (legacy EAWR-2190); captures WIN-U1, WMS-U1; (legacy EAWR-2166) |
| N3 | CLUSTER_BOMB: a nested handler releases a fixed number of bombs of a named projectile type with a detonation delay. The unit ability has a radius and autofire (Unit Abilities). M141 is `missing`. The registry's nested tags are `todo` (t760). | M2 (MC30 frigate) | Trace the handler in the debug build. Add a walk section. | confirmed; WCB-01..03; (legacy EAWR-2191); (legacy EAWR-2166) |
| N4 | FOW_REVEAL_PING works by spawning a projectile with the REVEAL behaviour and its own reveal range (Unit Abilities ability list; Special Projectiles, land reveal-ping blast). For the TIE Scout, census M146 is `missing` and the registry Spawned_Object_Type for SpaceUnit is `todo`. | M2 (TIE Scout) | Trace in the debug build. Owner: sensors-ui walk (reveal), with an abilities walk section for the switch. | confirmed; WFP-01..02; (legacy EAWR-2192); capture WFP-U1; (legacy EAWR-2166) |
| N5 | `Effective_Radius` sentinels: 0 means the whole map, -1 means unlimited (Unit Abilities, scanner and jet pack comments). | later (land examples). Our SpaceUnit Effective_Radius row is `partial` | Verify whether the sentinels are generic or per handler when adding the next space user. | not checked |
| N6 | `Active_By_Default` on REPLENISH_WINGMEN (Unit Abilities). The registry has it as `todo` (t760). Its meaning is not documented. | M2 heroes (TIE_Prototype) / M3 | Verify in the debug build what it switches on (autofire, or active on creation). Owner: heroes walk next to WHE-63. | not checked |
| N7 | Other kinds the wiki names: AFTERBURNER, SPREAD_OUT, DISTRACT (Unit ability types). SPREAD_OUT has a hotkey in foc-ability-buttons AB-10 but no behaviour doc. | later, unless a space type authors them | Search the FoC data for space users. If any exist, add them to the unit census. | not checked |
| N8 | Order of automatic abilities: an earlier-declared automatic ability can starve a later one, so authors must order them (Unit Abilities, Force Lightning comment). WHE-10 covers declared-order activation for nested handlers. The abilities walk does not say the same for unit-ability autofire. | later (land), M2 low | Add one sentence to WAB-36 if the special-attack pass also stops at the first success. | not checked |

### space-hardpoints.md / walks/weapons.md

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N9 | Hardpoints can be limited to an ability mode with `Fire_When_In_Rocket_Attack_Mode` / `Fire_When_In_Normal_Attack_Mode` (Unit Abilities, ROCKET_ATTACK). The registry also has `Fire_When_In_Power_To_Weapons_Mode`. All three are `todo` (t650) and no doc mentions them. | M2 if any space hardpoint authors the power-to-weapons gate. Otherwise land | Search the FoC space hardpoints for these tags. Trace the gate in the debug build. Add an HD rule. | wiki right; W-13; (legacy EAWR-2166) |

### walks/heroes.md (space heroes, G1) and GC walks

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N10 | The command fleet bonus includes ability-recharge and fire-range bonus fractions, for example Ackbar's 0.85 and 0.15 (Unit Abilities, FLEET BONUS; XML Tags). WHE-16 and WHE-55 store these stats but say their consumers are outside the current scope. | M2 heroes / M3 | Trace both consumer formulas in the debug build. | not checked |
| N11 | Find Weakness: a space-automatic passive that makes allied units deal double damage to the affected unit (Unit Abilities). Registry `todo` (t760). | M3 (GC hero) | Add to heroes walk. Verify in the debug build. | deferred M3/M4; (legacy EAWR-2166) |
| N12 | SUPER_LASER: a nested beam with damage per frame, a fire frame, a firing cone and turn settings, limited to Capital and Frigate. Planet destruction is a separate galactic ability (Unit Abilities, SUPER LASER and DESTROY PLANET). The registry `Super_Laser_Ability/*` and `Planet_Destruction_Ability/*` rows are `todo`. | M3 | Add a Death Star section (tactical beam) to the abilities walk and a planet-destruction rule to the GC walks. | deferred M3/M4; (legacy EAWR-2166) |
| N13 | Black market items carry `Ability_Names` and an ability-recharge bonus fraction (XML Tags). | M3 | Owner: gc-economy-production (next to the black-market row). | deferred M3/M4; (legacy EAWR-2166) |

### foc-unit-cards.md (presentation)

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N14 | Encyclopedia tooltip: several comma-separated description keys are shown with line breaks between them. `MP_Encyclopedia_Text` replaces the description in multiplayer. Good-against and vulnerable-to each show up to three object icons. `Encyclopedia_Unit_Class` holds the class line (XML Tags). Today C-4 draws only the name. The registry has these as `presentation-later`. | M2 (skirmish counts as multiplayer?) | Rig capture: a skirmish tooltip for a unit that authors MP text. Record whether the MP text is used in skirmish. | wiki right; P-1..5; (legacy EAWR-2193); (legacy EAWR-2166) |

### projectiles / land-combat / area-damage walks

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N15 | Grenade range: maximum range = speed squared / grenade gravity. A lob-slower flag allows shorter shots at lower speed. A lob gravity modifier exists but its use is unclear (XML Tags, Projectiles). Registry `todo` (t650). | later (land) | Verify the formula in the debug build when grenades are scoped. | deferred M3/M4; (legacy EAWR-2166) |
| N16 | Area stun projectiles can name explicit victim types on top of a category mask (Special Projectiles, stun blast). Registry `Projectile_Stun_Victims_Unit_Types` is `todo` with a **space** carrier (`Proj_Krayt_Bombardment_Ion`). IS-08 says only STUNNABLE units are reached. | M2 low (Krayt) | Verify whether the explicit types bypass the STUNNABLE gate. Note it under IS-08. | not checked |
| N17 | Projectile combat mods: a timed stat change on the projectile's victims, with type, duration, signed strength, effect and category mask (-0.25 means 25% lower) (Projectile Combat Mods; Special Projectiles, carbonite missile). The registry has them as `land-or-galactic`. | later (land) | When land combat opens, map these onto the WHE-20/WHE-55 ledger (same 11 stats). Settle the sign for DEFENSE. | not checked |
| N18 | Absorb: healing = projectile damage x absorb fraction + flat amount. Missiles and grenades cannot be absorbed (Unit Abilities, ABSORB). | later (land hero) | Add to the land hero walk when that walk is written. | deferred M3/M4; (legacy EAWR-2166) |
| N19 | A remote bomb needs both its place and its detonate ability. Chain lightning: maximum victims including the first target, and a hop radius (Unit Abilities). | later (land) | Add to land walks when that work starts. | deferred M3/M4; (legacy EAWR-2166) |
| N20 | Automatic land abilities take `Activation_Chance` pairs (number of valid targets, then the chance) (Unit Abilities, sticky bomb and whirlwind). | later (land) | Add to the land hero walk. | deferred M3/M4; (legacy EAWR-2166) |

### xml-registry / frame-order / mod-loading

| # | Wiki claim (page) | Scope | Next step | Verdict |
|---|---|---|---|---|
| N21 | SpaceBehavior values that no doc mentions: `TURRET`, `TACTICAL_SELL`, `TACTICAL_SUPER_WEAPON`, `AMBIENT_SFX`, `MARKER`, `DUMMY_TOOLTIP` (Behaviours). | M2 if FoC space objects author them | Search the FoC data for space users. Give each a registry note (consumed, ignored, or later). | not checked |
| N22 | Hunt and self destruct each need their behaviour in the unit's behaviour list. Capturing a vehicle needs `VEHICLE_THIEF` (Unit Abilities). WSD-01 and WAB-50 cover the first two. Vehicle thief is not covered. | later (land) | None for M2. | deferred M3/M4; (legacy EAWR-2166) |

## AGREES (compact)

| Wiki claim (page) | Ours |
|---|---|
| Abilities are a fixed set of engine kinds. Mods reuse stock kinds and attach custom effects through object scripts that watch the ability state (Unit Abilities) | WLD-01/WST-01 (kinds bound by name), AB-26 (unmodelled kinds are left out), AB-44 (Lua ability queries) |
| A squadron member's ability must also be in the squadron, and some abilities need a container (Unit Abilities) | AB-15, AB-60 (team abilities live on the `Create_Team_Type` container), WLU-01. Registry: squadron Mod_Multiplier is foc-ignores |
| You can rename the nested handler, but `GUI_Activated_Ability_Name` must match (Unit Abilities) | WLD-01, WST-01 |
| Modifiers act on the unit that uses the ability. Negative shield regen drains shields (Ability Modifiers; Ability modifier types) | AB-20, AB-22 |
| DEFEND, TURBO and SPOILER_LOCK example values (Unit Abilities) | Identical to AB-02 (Nebulon-B, corvette, X-wing) |
| TIE Defender ion shot: 25 s recharge, projectile override (Unit Abilities) | Abilities walk ION_CANNON_SHOT intro, AB-60 |
| HUNT needs its behaviour and has no timers (Unit Abilities) | WAB-50 |
| SELF_DESTRUCT needs its behaviour (Unit Abilities) | WSD-01 (also needs the countdown behaviour) |
| LURE parameters (radius 350, 16 s / 45 s, 0.5 s pulses up to 40, take damage 0.7) (Unit Abilities) | WLU-01 to WLU-08 |
| BUZZ_DROIDS parameters (reach 400, chase 300, damage radius 175, 23 and 3 damage per second, 60 s) (Unit Abilities) | WAB-70 to WAB-74 |
| LEECH_SHIELDS parameters (10-2000, 25 s, 100 per second, damage multiplier 3, 55 s) (Unit Abilities) | WLS-01 to WLS-03 |
| FULL_SALVO 7 s / 45 s (Unit Abilities) | WAB-80 |
| LASER_DEFENSE shoots down MISSILE, ROCKET and MPTL_ROCKET (Unit Abilities) | WLD-08 |
| Sensor jamming: user input, radius 900 (Unit Abilities) | WHE-31, WHE-32 |
| Concentrate fire: radius 6000, +50% damage to the target (Unit Abilities) | WHE-24, WHE-25 |
| Energy weapon: 50 damage per frame, range 800, 5 s / 120 s (Unit Abilities) | WHE-26, heroes walk table |
| Invulnerability = take damage 0 (Unit Abilities) | WHE-52, heroes walk table |
| Harmonic bomb: 1.5 s fuse, 20 s recharge (Unit Abilities) | WHE-29, WHE-61 |
| Weaken enemy: damage taken x (1 + increase), damage dealt x (1 - reduction), radius from the projectile (Special Projectiles; Unit Abilities) | WHE-28, WHE-55, WHE-62 |
| Maximum firepower turns off the diminishing-firepower flag. A weapon delay of 0 fires continuously (Unit Abilities) | AB-25, DG-05, AB-21. Registry Mod_Flag land-or-galactic |
| Reflect: the redirect roll comes first, then the block roll, otherwise damage is taken (Unit Abilities) | WRB-09 |
| A passive buff does not apply to its own unit (Unit Abilities, passives) | WHE-53 (self flag off by default) |
| The station income stream (30 per 10 s) and the supply dock (+20) depend on the hardpoint. The comm array radar is controlled by its hardpoint (Unit Abilities) | PU-02, PU-04, WBP-25, WSL-09, HD-14 |
| Projectile categories (MISSILE, BOMB, GRENADE, ROCKET, MPTL_ROCKET, STATIC_BOMB, LASER, LIGHT_SABER, CLUSTER_BOMB, default) (Projectile Categories) | WPJ-09, WPJ-18, WPJ-34, WRB-06, MS-01 |
| Ion stun: speed and fire-rate reductions as fractions (0.8 leaves 20% fire rate), a stacking flag, duration (Special Projectiles) | IS-01, IS-04 to IS-06, WSW-24 |
| The eleven projectile combat-mod stat types (Projectile Combat Mods) | Same set and order as the WHE-20 combat-modifier stats. Registry land-or-galactic |
| `Projectile_Texture_Slot` picks a colour cell by column and row (XML Tags) | BP-03, BP-06 (atlas cell) |
| AE delay seconds and blast dropoff tiers on bombs (Special Projectiles) | Registry applied (WAD blast service) |
| GameConstants `Activated_*_Ability_Names` route GC action slots (XML Tags) | WGCH-01, gc-corruption-heroes tag table |
| Tractor beam: targets transports, corvettes and frigates plus named exceptions, never Buzz_Droids (Unit Abilities) | Heroes walk table (Admonitor, Accuser), WHE-57, WHE-60 |
