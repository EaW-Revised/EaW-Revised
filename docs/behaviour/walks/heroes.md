# Space heroes and unique units (walk 11)

This walk records the space-skirmish hero differences: identity, carried heroes, passive command bonuses,
hero ability handlers, hero UI, death and replacement limits. It adds no game code. The audit baseline is
the walk branch's integration snapshot `22b8276b3d9b`, before the purchasing PRs EAWR-556/#574 and reinforcement work EAWR-919.
Their proposed support is not counted as support already present in this snapshot.

Sources are the **debug build**, keyed by opaque EHR evidence IDs, and the effective FoC XML, in its
`gameobjectfiles.xml` load order with variants resolved. The evidence map and raw lookups remain in ignored
`out/research/heroes/`. No retail capture was made for this walk. XML-only rows describe authored data;
they do not establish an untraced handler's runtime behaviour. Unknowns are explicit below.

## Scope and update entry points

The concrete objects are five `HeroCompany` purchases, five directly purchased `UniqueUnit` ships and one
`Squadron` purchase; their ships, craft, team containers and carried `HeroUnit` objects are also in scope.
A `UniqueUnit` is a data class, not a promise of hero identity or a one-per-player limit.

There is no single hero tick. Creation/owner events activate suitable special abilities and notify them
about new objects. The ordinary unit ability countdown is the interface in [walk 7](abilities.md).
The object's special-ability service visits declared abilities in data order and invokes due handlers.
Combat modifier expiry and each stat getter then feed targeting, motion, weapons and damage. Destruction
removes source effects and handles contained objects. The command-bar update paints hero buttons; click
events feed ordinary selection or orders. The tables follow this local evaluation order. Relative ordering
between different objects, commands, modifier expiry and special services in one global frame is **U-01**;
the local order below must not be interpreted as a proven global phase schedule.

## Skirmish roster from XML

All fifteen `Skirmish_{Rebel,Empire,Underworld}_Star_Base_[1..5]` menus were examined via
`Tactical_Buildable_Objects_Multiplayer`. These are the eleven hero/unique entries in those menus; campaign
heroes absent from them are not added to the skirmish roster. Levels show every menu containing the entry.
Credits/time/population come from `Tactical_Build_Cost_Multiplayer`, `Tactical_Build_Time_Seconds` and
`Population_Value`. Runtime mapping of company to space objects still requires U-02.

| Faction / XML purchase type | Class / source XML | Levels | Credits / seconds / population | Space object data |
|---|---|---|---|---|
| Rebel `Han_Solo_Team_Space_MP` | HeroCompany, `herocompanies.xml` | 2–5 | 1800 / 33 / 1 | `Company_Units` = Han_Solo, Chewbacca; `Company_Transport_Unit` = Millennium_Falcon |
| Rebel `Sundered_Heart` | UniqueUnit, `uniqueunits.xml` | 3–5 | 3000 / 30 / 2 | Named; Corvette, SpaceHero, AntiFighter, AntiBomber |
| Rebel `Rogue_Squadron_Space` | Squadron, `units_hero_rebel_rogue_squadron.xml` | 4–5 | 2500 / 25 / 1 | Six authored craft; `Create_Team_Type` = Rogue_Squadron_Space_Container |
| Rebel `Home_One` | UniqueUnit, `uniqueunits.xml` | 5 | 5300 / 38 / 4 | Named; Capital, SpaceHero, AntiFrigate |
| Empire `Boba_Fett_Team_Space_MP` | HeroCompany, `herocompanies.xml` | 2–5 | 1700 / 28 / 1 | `Company_Units` = Boba_Fett; transport Slave_I |
| Empire `Darth_Team_Space_MP` | HeroCompany, `herocompanies.xml` | 3–5 | 2500 / 38 / 1 | `Company_Units` = Darth_Vader; company transport Shuttle_Tyderium, but member `Unique_Space_Unit` = TIE_Prototype |
| Empire `Admonitor_Star_Destroyer` | UniqueUnit, `units_hero_empire_thrawn.xml` | 4–5 | 6200 / 40 / 4 | Named; Capital, SpaceHero, AntiFrigate |
| Empire `Accuser_Star_Destroyer` | UniqueUnit, `uniqueunits.xml` | 5 | 5800 / 40 / 4 | Named; Capital, SpaceHero, AntiFrigate |
| Underworld `Bossk_Team_Space_MP` | HeroCompany, `units_hero_underworld_bossk.xml` | 2–5 | 1200 / 14 / 1 | `Company_Units` = Bossk; transport HoundsTooth |
| Underworld `IG88_Team_Space_MP` | HeroCompany, `units_hero_underworld_ig88.xml` | 3–5 | 1700 / 23 / 1 | `Company_Units` = IG-88; transport IG-2000 |
| Underworld `The_Peacebringer` | UniqueUnit, `units_hero_underworld_tyber_zann.xml` | 5 | 5800 / 20 / 4 | Named; Capital, AntiFrigate; **no SpaceHero category** |

All five companies are named and have `LandHero | SpaceHero`; this does not transfer their flags to ships.
Millennium_Falcon, HoundsTooth and IG-2000 are named and have `Fighter | SpaceHero`. Slave_I has
`Fighter | SpaceHero` without an authored named flag. TIE_Prototype's named flag is commented out; it has
`Fighter | SpaceHero | AntiFighter | AntiBomber`, `Create_Team` = Yes,
`Create_Team_Type` = Darth_Vader_TIE_Fighter_Squadron and `Redirect_Damage_To_Teammates` = Yes.
That squadron's repeated `Squadron_Units` append TIE_Prototype and **six** Escort_TIE_Fighter craft.
Rogue's repeated entries append Wedge_XWing_Rogue, Rogue_2_XWing, Rogue_4_XWing, Rogue_7_XWing,
Rogue_10_XWing and Rogue_11_XWing. Wedge and the team container are named; the other craft are not.
Wedge's own categories omit SpaceHero, while the container has it. `Respawn_Whole_Team_When_Killed` on
Wedge is Yes, but the skirmish gate in WHE-38 prevents campaign respawning.

### Authored ability and bonus parameters

The generic switching/countdown/multiplier rules remain AB-01–AB-25 / WAB-01–WAB-16; the handler rules
below supplement them. Numbers here are XML, not universal engine constants. Omitted timers mean
unauthored, not an assumed zero-duration effect. All named nested handlers use `Activation_Style` =
User_Input unless the passive bonus table says Space_Automatic.

| Space object | `Unit_Abilities_Data/Unit_Ability` and nested `Abilities` parameters |
|---|---|
| Millennium_Falcon | INVULNERABILITY: expire 6 s, recharge 60 s, `TAKE_DAMAGE_MULTIPLIER` 0, `Supports_Autofire` True |
| Sundered_Heart | WEAKEN_ENEMY: recharge 60 s, `Effective_Radius` 1000, `Area_Effect_Decal_Distance` 300, `Spawned_Object_Type` Antilles_Weaken_Enemy_Effect. TURBO: expire 20 s/recharge 60 s; weapon delay 3, shield regen 0, energy regen 1, speed 2 |
| Rogue craft / container | SPOILER_LOCK: weapon delay 3, shield/energy regen 1, speed 2. POWER_TO_WEAPONS: weapon delay 0.5, speed 2, `CAUSE_DAMAGE_MULTIPLIER` 2.5, alternate name/description/icon for Strike. Wedge and container author expire 10 s/recharge 60 s; nonleader craft author **8 s/50 s**, so the container's timers must not replace craft timers |
| Home_One | CONCENTRATE_FIRE: expire 15 s/recharge 20 s, `Effective_Radius` 6000, `GUI_Activated_Ability_Name` Akbar_Home_One_Ability. Nested target damage increase 0.5, speed decrease 0, stacking 0; categories Fighter, Bomber, Transport, Corvette, Frigate, Capital. DEFEND: recharge 20 s, weapon delay 3, shield regen 1, shield interval 0.1, energy interval 0.1, energy regen 5, speed 0.8, autofire True; duplicate `Expiration_Seconds` 20 then 15 (**U-03**) |
| Slave_I | HARMONIC_BOMB: recharge 20 s, `Bomb_Countdown_Seconds` 1.5, spawn Proj_Harmonic_Bomb_Slave_I, autofire True; detonation implementation belongs to weapons |
| TIE_Prototype | REPLENISH_WINGMEN: recharge 60 s, `Active_By_Default` Yes, autofire True; ability SFX/particle authored |
| Admonitor_Star_Destroyer | POWER_TO_WEAPONS/Assault: expire 7 s/recharge 50 s, weapon delay **0.3**, speed 2, cause damage 2. TRACTOR_BEAM: recharge 25 s, owner speed 0.6; nested min/max 0/800, target speed decrease 0.9, stacking 2; categories Transport, Corvette, Frigate; exact types Millennium_Falcon, Houndstooth, IG-2000; excluded Buzz_Droids |
| Accuser_Star_Destroyer | ENERGY_WEAPON: expire 5 s/recharge 120 s, owner speed 0.8, autofire True; nested min/max 0/800, `Damage_Per_Frame` 50; categories Capital, Corvette, Frigate, Fighter, Bomber, Transport. TRACTOR_BEAM: recharge 20 s, owner speed 0.6, autofire True; nested min/max 5/1100, target speed decrease 0.98, stacking 2; categories Transport, Corvette, Frigate; exact Millennium_Falcon, Sundered_Heart; excluded Buzz_Droids |
| HoundsTooth | SENSOR_JAMMING: expire 30 s/recharge 75 s, radius 900; nested HoundsTooth_Sensor_Jamming, owner particle effect |
| IG-2000 | CORRUPT_SYSTEMS: recharge 60 s, decal radius 400; nested min/max 20/350, duration 12 s, shield drain 60/s, inaccuracy radius 500, owner/target particles |
| The_Peacebringer | BLAST: recharge 90 s, nested charge 5 s and damage multiplier 5, charging/charged effects. STEALTH: expire 180 s/recharge 10 s; both ability records require `Must_Be_Bought_On_Black_Market` Yes; transition 0.3 s, colour 255,255,255,255; skirmish availability is **U-04** |

| `Combat_Bonus_Ability` source | Mode / targets / stacking | Nonzero fractional bonuses |
|---|---|---|
| Home_One / Akbar_Combat_Bonus | Space_Automatic; Fighter, Bomber, Transport, Corvette, Frigate, Capital; category 0 | Health 0.25 |
| Admonitor_Star_Destroyer / Admonitor_Combat_Bonus | Space_Automatic; Capital, Corvette, Frigate, Fighter; category 0 | Health 0.20; damage, shield, defense, movement speed each 0.10 |
| Accuser_Star_Destroyer / Piet_Combat_Bonus_General | Space_Automatic; Capital, Corvette, Frigate, Fighter; category 0 | Health 0.25 |
| Accuser_Star_Destroyer / Piet_Combat_Bonus_Star_Destroyer | Space_Automatic; exact `Star_Destroyer`; category 1 | Health 0.25 |
| The_Peacebringer / Tyber_Zann_Combat_Bonus | Space_Automatic; Capital, Corvette, Frigate, Fighter; category 0 | Health 0.20, movement speed 0.10 |

Zero authored bonus fields contribute nothing. `Unit_Strength_Category` is adjacent metadata, not a radius
or replacement for `Applicable_Unit_Categories`. Home_One also authors `Reduce_Production_Price_Ability`
(Capital, reduction 0.25, stacking 0) **without Activation_Style**: its default mode and skirmish application
are U-05, not a confirmed skirmish discount. Thrawn's System_Spy and Remove_Corruption and the companies'
ground combat abilities are outside this walk; a carried hero's ground ability is not its ship's space ability.

## Rule list and comparisons

Each row compares both previous notes and **our audited code**. “same” for an interface means that interface
already works for represented types, not that the absent hero roster is playable. Missing rules are grouped
into implementation gaps below. There are **48 rules: 6 same, 1 differs, 41 missing** in our snapshot.

Code locators used in the last column:

- **types**: [`UnitType`, `UnitKind`](../../../include/eawr/units/unit_tables.hpp),
  [`load_unit_tables`](../../../src/units/unit_tables.cpp); no company/rider/hero metadata or special-handler records.
- **ability loader**: [`ability_table`](../../../src/units/unit_abilities.cpp); unknown kinds skipped, unsupported
  modifiers on recognised kinds rejected. **timers**: [`activate_ability`, `expire_abilities`, `ability_multiplier`](../../../src/sim/tactical/abilities.cpp).
- **session**: [`TacticalSession`](../../../src/sim/tactical/session.cpp); no hero bonus/containment/special-handler state.
- **damage**: [`damage.cpp`](../../../src/sim/tactical/damage.cpp); generic damage, no hero handler/source ledger.
- **UI**: [`selection.hpp`](../../../include/eawr/presentation/ui/selection.hpp),
  [`unit_cards.hpp`](../../../include/eawr/presentation/ui/unit_cards.hpp),
  [`battle_input.cpp`](../../../apps/viewer/src/battle_input.cpp); ordinary selection/cards, no hero trays or rider identity.

### Admission, identity and containment

| ID | Rule and branches | Source | Previous notes | Our code |
|---|---|---|---|---|
| WHE-01 | Hero identity is `Is_Named_Hero OR Is_Generic_Hero`. Neither a UniqueUnit class nor SpaceHero category supplies either flag. | debug build EHR-01; roster XML | missing there: walk 8 excludes hero identity | missing: types; G1 |
| WHE-02 | Named identity, company identity, visible ship and team container can differ; preserve all authored identities and flags rather than copying company flags onto a transport or every craft. Concrete exceptions are in the roster. This row establishes XML relationships only; automatic expansion is U-02. | XML roster / member definitions | missing there: WR-48 covers campaign identity, not these purchases | missing: types; G1 |
| WHE-03 | Target priority reads exact types/categories, including SpaceHero, through R-09. It does not assign a universal named-hero weight. XML SpaceHero weights include Bomber 0.5, Corvette 4, Frigate 3.5, Capital 6, SpaceArtillery 2.5; Fighter has no such entry. Other matching categories can give the smaller priority. | `spaceunittargetingpriorities.xml`; R-09 | same: space-targeting R-09 | same: `src/units/unit_priority.cpp#attack_priority`; new roster still G1 |
| WHE-04 | All eleven menu entries are explicitly listed above; absent campaign heroes have no purchase entitlement from being named. Station level/menu admission remains WPR-31–WPR-33. | station-menu XML | same interface: production walk; missing there: concrete hero list | missing: types/session have no purchasing in this snapshot; G1 / existing EAWR-724 |
| WHE-05 | Space hero companies read company/member/transport relationships as data. Darth's company transport and member unique ship disagree: do not silently choose Shuttle_Tyderium or assume the member mapping wins without U-02. Current registry land-only company classifications cannot describe these space menus. | XML `Company_Units`, `Company_Transport_Unit`, `Unique_Space_Unit`; registry | missing there: reinforcements WR-30–WR-31 do not settle hero conversion | missing: types; scope correction G2, conversion G1 |
| WHE-06 | Adding a carried object appends it to the carrier and sets its parent; an already-parented rider is invalid. Rider setup also visits every team member before the team object. | debug build EHR-21, EHR-32 | missing there: walk 10 only deployment interfaces | missing: session containment; G1 |
| WHE-07 | Rider setup hides its model, disables collision, removes movement coordination/tracking, deselects it and puts it in limbo. In the non-preserving setup branch, independent targeting/weapon behaviours and weapon hardpoints are disabled. The preserved-combat argument is a separate branch, not an unconditional disable. | debug build EHR-22, EHR-32 | missing there: selection notes cover visible objects | missing: session / UI; G1 |
| WHE-08 | `Attach_To_Flagship_During_Space_Battle`, `Unique_Space_Unit` and `Display_Contained_Hero_Grab_Bars` are distinct authored controls. Darth and Tyber author attachment No; Slave_I, HoundsTooth and IG-2000 request contained grab bars. Carrier choice, attachment trigger, ejection and transfer are U-02; rider setup alone does not establish them. | XML hero/ship definitions; EHR-21–EHR-23 establish containment only | missing there: cards L-7 mentions flagship cards, not attachment policy | missing: types / session / UI; G1 / G11 |

### Special service and passive bonuses

| ID | Rule and branches | Source | Previous notes | Our code |
|---|---|---|---|---|
| WHE-09 | Special service exits if owner/type absent, abilities all canceled, owner a death clone, or map editor. Otherwise visit declared abilities in order; require interval >0, enabled and due frame <=current. Invoke once and schedule current+interval; missed services do not catch up with multiple invocations. Base service interval is code 0/no periodic call; the five traced active handlers use code 1 logical frame. | debug build EHR-09, EHR-46–EHR-50, EHR-56 | missing there: WAB generic countdown is a separate service | missing: session special-handler state; G3 |
| WHE-10 | Activation by style checks declared order, matching activation style, ready, appropriate mode, enabled, appropriate target, availability; only then activate. Caller may stop at first success or activate all. The activation context is cleared after Apply; `Causes_Despawn` acts only after success. | debug build EHR-17, EHR-38 | same interface: WAB activation gates; missing there: nested handler manager | missing: ability loader / session; G3 |
| WHE-11 | New-object notifications first require mode/enabled applicability. A canceled ability records a deferred target only if otherwise appropriate/available; an active combat-bonus handler attempts application to the new object. Existing-source start enumeration is not proven by this notification path (U-06). | debug build EHR-10, EHR-18 | same interface: WPR-51 newly produced bonus targets; missing there: hero-source lifecycle | missing: session; G4 |
| WHE-12 | Owner deletion terminates each enabled mode-applicable handler and cancels service. Termination visits tracked targets: missing IDs are removed, live targets receive RemoveEffect; owner-destroy termination is skipped for a handler whose successful activation causes despawn. | debug build EHR-11, EHR-39 | missing there: generic ability death is not target-ledger cleanup | missing: session; G3 / G4 |
| WHE-13 | Bonus requires source and target, type-filter eligibility and target combat data. Without `Specific_Faction`, require an ally of the source owner; with it, require exact target faction instead. Exclude the target if its type is the source's unique ground/space container type. This does not exclude the source object merely by ID. | debug build EHR-02 | same: WPR-51 allied filter; missing there: faction override/container exclusion | missing: session; G4 |
| WHE-14 | Explicit `Applicable_Unit_Types` match succeeds immediately. Otherwise require `Applicable_Unit_Categories` overlap, then reject `Excluded_Unit_Categories` or `Excluded_Unit_Types` matches. An explicit allowed type bypasses those exclusions. Matching Star_Destroyer does not automatically match its named variants. | debug build EHR-16, EHR-53 | missing there: hero/special-ability filter; R-09 is a different targeting filter | missing: types / session; G3 / G4 |
| WHE-15 | Command-bonus eligibility/application contains **no radius or distance test**. The handler inherits code service interval 0 and is event-driven; it is not a periodic nearby-unit aura scan. The concrete bonuses are Space_Automatic. Initial whole-map enumeration and limbo-owner propagation remain U-06. | debug build EHR-02, EHR-03, EHR-05, EHR-56; XML | same: WPR-51 no-radius upgrade bonuses; missing there: hero qualification | missing: session; G4 |
| WHE-16 | Add each nonzero bonus to its stat ledger, source-identified and keyed by `Stacking_Category`, with no time expiry (code sentinel -1). Stats are health, damage, energy pool, shield, movement speed, ability recharge, defense, fire range and tactical build time. Absent values default 0 and stack category defaults 0. Track affected targets for removal. | debug build EHR-03, EHR-33 | same interface: WPR-51; missing there: complete hero ledger | missing: session / damage / types; G4 |
| WHE-17 | The health getter keeps the largest contribution per stacking category, adds categories, then multiplies base space health and difficulty factor by 1+total. The five passive XML bonuses share category 0; Piet's exact Star_Destroyer bonus in category 1 adds to the winning category-0 health bonus (0.25+0.25=0.50). No compounding of the two percentages. Other stat consumers/negative debuff choice are U-07. | debug build EHR-12, EHR-44; XML | same: WPR-51 largest-per-category; missing there: concrete hero overlap | missing: session health modifier aggregation; G4 |
| WHE-18 | When an applied health bonus changes maximum health, add newMax-oldMax to current hull health; scale hardpoint health by newMax/oldMax if oldMax >0. Energy changes only for a powered target, shield changes only for a shielded target; current energy/shield likewise gain the maximum delta. No arbitrary full heal or percentage-of-missing-health heal. | debug build EHR-03 | missing there: WPR-51 does not spell out current-stat adjustments | missing: session / durability state; G4 |
| WHE-19 | Remove only this source/category. Recompute maximums and clamp current hull/energy/shield if above the new maximum; cap hardpoints as needed. Losing a health bonus does not subtract the maximum delta from hull health. Remove other affected stat contributions and untrack target. | debug build EHR-04 | missing there: WPR-51 omits removal asymmetry | missing: session; G4 |
| WHE-20 | Combat modifier expiry visits defense, damage, health, shield, energy, movement speed, fire rate, reveal range, fire range, ability recharge and tactical build time in that local order. Permanent command modifiers survive until source removal. Damage consumes shooter damage/target defense via DG-26; other subsystem consumers own their formulas. | debug build EHR-45; DG-26 | same interface: WCC-44/DG-26; missing there: hero source/expiry ledgers | missing: session / damage hero modifiers; G4 |

### Generic countdown handoff and hero handlers

| ID | Rule and branches | Source | Previous notes | Our code |
|---|---|---|---|---|
| WHE-21 | Unit ability slots retain authored order, truncation to 30 logical frames/s, expiry/recharge and early-switch-off rules. Hero identity alone adds no generic countdown branch. Handler-specific starts below supplement the interface; they must not be forced into one generic recharge policy. | WAB-01–WAB-16 / AB-04–AB-12; hero XML | same: abilities walk | same: timers / ability loader for modelled kinds |
| WHE-22 | Hero XML introduces active `CAUSE_DAMAGE_MULTIPLIER` (Assault 2, Strike 2.5) and `TAKE_DAMAGE_MULTIPLIER` (Falcon invulnerability 0). They are distinct from command-bonus fractions. The damage interface must consume the active damage modes; invulnerability is not arrival protection. Generic damage order remains DG-01/DG-02. Exact bypasses for scripted/nonweapon damage are U-08. | XML; DG-02; WAB scope excludes hero modes | same: DG-02 expressly leaves these outside M2; missing there: hero activation coverage | **differs**: ability loader rejects cause-damage on recognised POWER_TO_WEAPONS; skips unrecognised INVULNERABILITY; damage lacks modes; G5 |
| WHE-23 | For non-team abilities, squadron commands act through each craft's own ability data (AB-15), not squadron multipliers/timers. Rogue's leader and escorts author different Strike durations/recharges. Replenishment and special team abilities are separate handlers, not extensions of this generic toggle. | AB-15; Rogue XML | same: AB-15 / WAB-03 | same: ability loader craft profiles; unsupported cause-damage is WHE-22 |
| WHE-24 | Concentrate Fire locks the chosen target, clears the previous recruited list and searches around **target position**, radius `Effective_Radius` 6000. It selects targeting units of the **same owner**, not every ally; skips source and movement-locked entries, promotes team members to their parent team when applicable, cancels recruited movement and batches attacks on target/selected hardpoint. Recruitment is at activation, not a continuous aura scan. Start expiry after recruitment. | debug build EHR-24; XML | missing there: generic attack orders are only the receiving interface | missing: session targeted handler; G6 |
| WHE-25 | Concentrate Fire places negative `Target_Damage_Increase_Percent` in target defense modifiers (-0.5), and negative target speed decrease (0), stack category 0; it does not boost every recruited unit's weapon data. DG-26 turns -0.5 defense into a 1.5 damage factor before other modifiers. Every code-1 service removes/unlocks the effect if CONCENTRATE_FIRE is no longer on. | debug build EHR-19, EHR-25; DG-26; XML | same damage interface: WCC-44; missing there: handler | missing: session / damage ledger; G6 |
| WHE-26 | Energy Weapon services its locked target each logical frame (code interval 1). If active/enabled, target not in nebula, in min/max attack range, not expired and selected hardpoint still present/alive, send `Damage_Per_Frame` 50 as miscellaneous damage via the ordinary damage interface, using selected hardpoint collision information if any; maintain beam/audio. Failure unlocks target. 50 is per frame, not per second (1500 nominal/s before damage processing). Initial eligibility/lock and exact boundary comparisons are U-09. | debug build EHR-20, EHR-46; XML | same interface: damage walk; missing there: direct hero beam | missing: ability loader / session; G7 |
| WHE-27 | Tractor service interval is code 1. Active/enabled, non-nebula, in-range target with combat data is marked held and keeps beam/audio; failure removes the effect. XML supplies target speed decrease 0.9 or 0.98, stack 2, and owner speed multiplier 0.6. Do not merge the two ships' ranges/recharges. Initial speed-mod application, cancellation/stack selection and boundary comparisons are U-09/U-07. | debug build EHR-26, EHR-48; XML | missing there: ship movement/ability walks are consumers | missing: ability loader / session / motion input; G7 |
| WHE-28 | Weaken Enemy's spawned effect has its own impact radius, independent of the owner's drop reach. With `Projectile_Weaken_Enemy_On_Detonation` Yes and positive `Projectile_Weaken_Enemy_Radius` 300, collidable enemy combat targets matching `Projectile_Weaken_Enemy_Targets_Category_Mask` All receive negative defense adjustment from `Projectile_Weaken_Enemy_Take_Damage_Increase_Percent` (0), and negative damage adjustment from `Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent` (0.2), for `Projectile_Weaken_Enemy_Duration_Seconds` 15 s; `Projectile_Weaken_Enemy_Spawn_Effect` supplies the status particle. Stack category is **code 0**, source is the projectile. Flight and collision remain weapons interfaces. | debug build EHR-30; effect XML | same interface: WWP detonation; missing there: weaken status | missing: session / projectile status application; G8 |
| WHE-29 | Slave_I authors harmonic projectile type, 1.5 s bomb countdown, 20 s recharge and autofire. This is not ordinary fighter laser data. Bomb creation, arming/cancel and AI autofire conditions are unverified U-10; area damage/collision belongs to WWP-67. This rule is an XML contract only. | XML | missing there: hero bomb command; same weapons interface | missing: ability loader / bomb command; G8 |
| WHE-30 | Replenish Wingmen reads `Create_Team_Type` and its ordered `Squadron_Units`. With no parent team, creates escorts around leader and constructs a team. With an existing team, checks expected escort positions **1..N-1**, retains matching living members and creates missing members/formation entries, then starts recharge. It replaces escorts while the leader exists; it does not respawn the defeated hero. Damage redirection semantics are U-08. | debug build EHR-31; squadron/leader XML | missing there: squadron walk covers ordinary composition, not this handler | missing: ability loader / session; G9 |
| WHE-31 | Sensor-jamming service interval is code 1. Combat_Automatic style refreshes its counter to code 10; HoundsTooth uses User_Input instead. With counter >0 and owner outside nebula, stamp eligible recipients, update radius effect if no owner particle, decrement; reaching zero terminates. Nebula terminates and clears counter. Initial user-input counter from expiry is U-10. | debug build EHR-37, EHR-49; XML | missing there: walk 8 scopes this ability out | missing: session; G10 |
| WHE-32 | Jamming uses unit ability `Effective_Radius` (900), with fallback `Passive_Missile_Shield_Radius`. Search nonprojectile locomotor objects which are **not enemies**, alive, not death clones/deletion pending and have combat data. Include 3D distance <=radius and stamp current frame. Neutral/nonenemy qualification differs from an allies-only command bonus. Sensors/projectile/AI consumers of the stamp remain interfaces. | debug build EHR-27; XML | missing there: WSU sensors; AI response stub already EAWR-785 | missing: session jamming stamp; G10; reuse EAWR-785 for AI consumer |
| WHE-33 | Corrupt Systems validates min/max range to an area marker, records its point, destroys the marker, starts recharge, stops owner's movement and applies the area effect. Radius is `Area_Effect_Decal_Distance` 400 around that point. Select enemy collidable locomotor objects, excluding projectiles and source. Create/re-enable recipient effect, overwrite drain 60/s and inaccuracy 500, and reset duration to max(1,trunc(12×30))=360 frames; repeat applications refresh/overwrite instead of adding another effect. | debug build EHR-28, EHR-36; XML | missing there: generic target commands do not represent area marker handlers | missing: session; G12 |
| WHE-34 | Corrupt recipient service with time remaining drains shields by `Shield_Drain_Per_Second/logicalFPS`, clamped at zero, then subtracts its service interval. At expiry remove effect/behaviour; visit primary then secondary ability and start full recharge only if already recharged and authored recharge >0. Inaccuracy consumer and ability suppression during the effect are U-11; do not infer them solely from stored radius. | debug build EHR-43; XML | missing there: damage/ability walks exclude corrupt systems | missing: session / shield state / ability reset; G12 |
| WHE-35 | Blast activation converts `Charge_Up_Seconds` 5 to 150 frames, starts recharge immediately (90 s), shows charge effect/mesh and disables opportunity fire. Each code-1 service decrements positive charge. | debug build EHR-42, EHR-29, EHR-50; XML | same interface: WWP-14 charging prevents weapon fire; missing there: initiation/timer | missing: session / ability handler; G13 |
| WHE-36 | When blast charge reaches zero, replace charging with charged effect; set remaining charged-shot count to negative number of undestroyed SPECIAL weapon hardpoints. Zero eligible hardpoints cleans presentation immediately. The weapon consumer uses `Damage_Multiplier` 5 for charged fire, then owns consumption/reset under WWP-14/WWP-26; exact shot cleanup sequence is U-10. | debug build EHR-29; XML; WWP-14/WWP-26 | same: weapons walk handoff; missing there: hero charge state | missing: session / weapon blast state; G13 |
| WHE-37 | Peacebringer STEALTH requires black-market purchase in both generic and special records. Do not enable it from Is_Named_Hero, button presence or 180 s expiry alone. Visibility transition/reveal interaction belongs to walk 8; purchase gate in skirmish is U-04. XML contract only. | XML | missing there: WAB scope; same sensors interface | missing: ability loader / availability input; G13, gated on U-04 |

### Death, rebuilding and limits

| ID | Rule and branches | Source | Previous notes | Our code |
|---|---|---|---|---|
| WHE-38 | Automatic hero respawn immediately refuses multiplayer-tactical/skirmish rules, including a solo skirmish with no parent campaign. The campaign `Default_Hero_Respawn_Time` 360 s and named/whole-team/location branches are downstream of that refusal. Therefore skirmish has no automatic respawn timer, reappearance location or free-revival quota. | debug build EHR-06, EHR-13, EHR-15; `gameconstants.xml` | missing there: no prior hero-death rule | same: session death has no automatic reinsertion; keep this when G1 adds heroes |
| WHE-39 | Build limits use all four WPR-33 counters (current per player/all allies, lifetime per player/all allies). Current counts include owned objects plus production; lifetime counts are ever-built. Negative authored limit means unlimited; zero forbids. All roster purchases author current per player 1 except Rogue, which authors **current for all allies 1**. Their lifetime fields are -1: allies-wide for the five companies, Home_One, Admonitor and Accuser; per player for Rogue, Sundered_Heart and Peacebringer. | XML; production walk WPR-33 | same: WPR-33; missing there: concrete hero fields | missing: no economy in snapshot; existing EAWR-724, G14 |
| WHE-40 | Hero death does not spend a separate hero respawn entitlement. Replacement is a new paid purchase, governed by normal current/lifetime counter release and queue/prerequisite rules, not the campaign respawn delay. With these -1 lifetime fields, no finite lifetime ban follows from their death; current ownership/queued state still matters. | WHE-38; XML; WPR-33 | same: WPR-33 limits, WPR-23 destruction economics | missing: economy / logical purchase identity for carried heroes; G14 / G1 |
| WHE-41 | Carrier's contained-object destruction routine reports hero defeat/attacker attribution, destroys team members if applicable, then the contained object; clears contained list. Upgrade behaviour has a relocation branch, not ordinary hero revival. Whether a particular carrier death invokes this path or an ejection path is U-02. Source bonus cleanup is WHE-12/WHE-19. | debug build EHR-23 | missing there: generic ship death is only the interface | missing: session rider lifecycle; G1 / G4 |

### Hero presentation and input

| ID | Rule and branches | Source | Previous notes | Our code |
|---|---|---|---|---|
| WHE-42 | World hover bars accept a selectable object **or hero identity** or the existing special-type exceptions. Ordinary space team-container and parented-craft suppression still applies. Hero does not universally override all bar suppression. | WSU-50 (debug-build audit); EHR-01 supplies identity | same: WSU-50 | same: generic bar interface in UI; missing identity admission is WHE-01 |
| WHE-43 | World health/shield bar values retain WSU-52–WSU-53. Hero identity adds no separate shield maximum or display-health formula. Command-bonus changes must reach the existing health/max-health consumers. This does not describe tray bars. | WSU-52–WSU-53; EHR-03/EHR-04 | same: sensors/UI walk | same: generic UI bars; bonus data path remains G4 |
| WHE-44 | Hero icon update receives hero/represented-object pairs, partitions ally versus enemy trays and hides with the command bar. The traced layout has code capacity 11 per side, with a shell-layout branch clamping to 6. Collection order, fog filtering and duplicate membership are U-12. | debug build EHR-07 | missing there: walk 8 excludes hero icons | missing: UI hero collection/trays; G11 |
| WHE-45 | Icon uses `Icon_Name` from display/true type. Button stores hero identity and represented object separately. Health is hero display health, replaced by parent-container health fraction when parent exists. This path has no shield bar. Display-type fallback and contained-grab-bar collection gate are U-12. | debug build EHR-07; XML | same icon interface: cards L-7; missing there: dual identities/health | missing: UI; G11 |
| WHE-46 | Hero highlight effect requires a noncanceled special-ability manager and an automatic combat ability, or automatic space ability in space mode. It is not a test that the hero has a radius aura or only Combat_Bonus_Ability. | debug build EHR-07 | missing there: cards/WSU hero presentation | missing: UI special-ability highlight; G11 |
| WHE-47 | Allied hero-button left click uses the ordinary selection/guard path. A team without a represented object uses its first member; a represented limbo rider can resolve through parent/grandparent to a suitable moving carrier. Normal double click selects same type on screen (Ctrl: class) and centers camera; carrier substitution suppresses ordinary type-double-click selection and uses camera centering. | debug build EHR-34, EHR-41, EHR-52 | same selection interface: foc-battle-selection; missing there: hero/rider routing | missing: UI; G11 |
| WHE-48 | Enemy hero-button right click issues an attack through ordinary target/attack handling. It does not select an enemy hero as an owned unit. Ally/enemy ownership branching applies after represented-object/carrier resolution. | debug build EHR-35, EHR-52 | same orders interface: selection notes; missing there: enemy tray entry | missing: UI; G11 |

## Interfaces and scope boundaries

- **Production / reinforcements:** consume logical purchase type, ownership, menu, limits and population; produce
  deployed ship/team plus optional carried identity. WR-30/WR-31 own normal placement, facing, free-space search
  (code 2500), member ordering and arrival state. They do not prove the hero-company conversion U-02.
  EHR-54 is a landing-transport path and is not used as evidence for that space conversion.
- **Generic abilities:** countdown/ready/autofire/events remain walk 7. Nested manager activation, target ledgers
  and explicit recharge starts are additional state; aliases/alternate UI strings never define new timer rules.
- **Targeting / motion:** WHE-14 special eligibility is not R-09 priority. Targeted handlers temporarily supply
  special target/ability context to the normal attack-range query and restore it afterward (EHR-55); do not
  permanently replace the ordinary attack target. Motion consumes owner/target speed changes and tractor state.
- **Weapons / damage:** keep collision, armor, shields, hit routing, ordinary projectile blast and charged-shot
  consumption in walks 2/3. Feed damage-mode multipliers and source-ledger damage/defense bonuses into their
  existing interfaces. Direct beam per-frame damage is a handler input, not a fake laser projectile.
- **Sensors / AI:** jamming timestamps, corrupt accuracy state and stealth supply inputs to those consumers;
  EAWR-785 already owns the AI response stubs. No hero-neutralization campaign/ground duel rule is imported into
  space because XML happens to author `Can_Be_Neutralized_By_Major_Heroes`/`Minor_Heroes`.
- **Campaign:** revival/leader defeat, fleet ranking/automatic flagship choice, System_Spy, corruption removal,
  ground abilities and black-market economy internals remain outside M2. Shared containment and a space ability's
  availability gate are in scope, but their untraced campaign-to-space entry points remain unknown.

Additional comparisons with [ability-button notes](../foc-ability-buttons.md), whose AB IDs are distinct
from the simulation's AB IDs: AB-01–AB-08 are **same interfaces** for grouping, state, icon fallback,
dials and marks in `src/presentation/ui/ability_buttons.cpp`; hero state must be supplied by the missing
handlers. AB-04's `Alternate_Icon_Name` override has a UI callback, but the hero data path/aliases still
need G11. AB-09/AB-11 targeted enemy-unit clicks are **same interfaces** in `battle_input.cpp`; an area
marker for WHE-33 is additional missing work in G12. AB-10's default map is **same** in
`ability_hotkey`; the previous note's claim that hero keys are unimplemented **differs from the current
input code**, which includes hero bindings and punctuation dispatch. Live handler availability remains
missing; a hotkey table entry does not make its ability playable. These existing interface comparisons
are not additional hero-rule verdicts in the count above.

## Unverified behaviour and the observation that would settle it

| ID | Unknown | Required evidence / capture |
|---|---|---|
| U-01 | Global within-frame order among command activation, creation, special service, modifier expiry, damage and destruction | Debug-build logical-frame trace with a bonus source killed and a beam/area status applied on the same frame; inspect through the Lua debugger harness, not scraped logs |
| U-02 | Space HeroCompany conversion, member unique-ship precedence, carrier selection/transfer/ejection, and whether each ship death invokes contained destruction | Purchase/deploy each of five companies in a staged space skirmish; inspect object identities/parents before and after carrier death; add a campaign flagship case for attachment controls. No automatic fleet-ranking rule is yet established |
| U-03 | Home_One DEFEND duplicate scalar: whether later 15 s wins over earlier 20 s in this exact parser path | Inspect parser assignment or record activation until first expiry, with pause/step and no early switch-off |
| U-04 | Peacebringer black-market stealth availability in space skirmish | Underworld skirmish purchase at level 5; inspect enable gate with/without any available black-market state; capture button and successful/failed toggle |
| U-05 | Default activation style and tactical relevance of Home_One's production discount | Inspect default handler mode or compare capital purchase cost before/after Home_One; fixed station/upgrades, no other price source |
| U-06 | Initial bonus recipient enumeration, self-qualification, owner-change requalification and bonuses from limbo riders/carriers | Stage source before/after existing ally, neutral, enemy and new spawn; vary range, parent/limbo and owner; observe actual maximum/current stats and source removal |
| U-07 | Per-stat selection/caps for negative modifiers and overlapping target debuffs | Inspect each consuming getter; capture two tractor/weaken sources in same/different stacking categories, preserving source IDs and expiry times |
| U-08 | Redirect_Damage_To_Teammates amount/member choice/dead-leader cases; Falcon damage-mode treatment of direct/scripted and area damage | Stage TIE leader with six/one/no escorts and apply one known hit; stage Falcon mode with projectile/beam/area/direct damage separately, recording recipient health |
| U-09 | Initial energy/tractor target gates, selected hardpoint loss, exact min/max range boundaries, cancel/recharge order | Frame-step beam lock just inside/on/outside XML limits, enter nebula, kill chosen hardpoint, cancel/retarget; inspect slot/target state |
| U-10 | Harmonic bomb creation/arming/autofire; user-input jammer initial counter; blast charged-shot consumption/cleanup | Short lit clips and logical-frame observations for each activation; fixed target positions; step bomb arming, jammer duration and every SPECIAL charged shot |
| U-11 | Corrupt inaccuracy consumer and ability suppression while effect active | Compare target's shot scatter and readiness/activation before/during/after a 360-frame effect; inspect accuracy/ability state through debugger harness |
| U-12 | Tray collection order, enemy fog/death filtering, duplicate hero/carrier entries, exact shell layout, display-type fallback and contained-bar gate | Lit HUD captures with >6 heroes, allied and enemy owners, fog on/off (fog behaviour is the subject), riders and squadron; click each entry and observe selection/camera/attack |

These captures are follow-up evidence, not completed fidelity claims. Do not claim an automatic respawn
experiment is required to establish WHE-38: the skirmish early return is already traced. New harness work
which needs Lua state waits for/uses EAWR-622's debugger API rather than introducing a log-scraping probe.

## Implementation gaps and duplicate check

Tracking: **EAWR-933**. Sizes estimate coherent implementation units (S: narrow metadata; M:
one handler/lifecycle; L: several consumers and deterministic state). No scenario IDs or replay opcodes
are claimed here. Simulation implementations must use copied partition inputs, disjoint staging and ordered
commit; verify replay state for 1/2/4/8 workers and count work deterministically for recipient scans.

| Gap | Size | Rules / exact work | Ticket |
|---|---|---|---|
| G1 | L | WHE-01/02/04–08/41: hero flags, logical purchase versus deployed identity, company/member/unique-ship conversion, rider parent/limbo/destruction; verify U-02 before choosing deployment/flagship policy | EAWR-934 |
| G2 | S | WHE-05: correct space HeroCompany registry scopes for identity, company relationships and build limits; keep ground ability fields land-only; no tag becomes applied until code applies it | EAWR-935 |
| G3 | M | WHE-09/10/12/14: typed nested special-ability activation, filters, due-frame service and target cleanup; shared base for handlers | EAWR-936 |
| G4 | L | WHE-11–20/41: hero command-bonus source ledgers, no-radius filters, health stacking/adjustments/removal and consumer integration; reuse EAWR-540 for station-upgrade bonus support | EAWR-937 |
| G5 | M | WHE-22: cause/take damage modes and Falcon invulnerability; fix loader rejection, preserve generic timers and unrelated arrival protection; settle U-08 bypasses | EAWR-938 |
| G6 | M | WHE-24/25: Home_One target-centered same-owner recruitment, batch orders and target defense modifier removal | EAWR-939 |
| G7 | L | WHE-26/27: energy direct-damage service and tractor target state/speed, target/hardpoint/nebula termination; retain per-ship XML differences, settle U-09/U-07 | EAWR-940 |
| G8 | M | WHE-28/29: weaken projectile status and harmonic bomb activation inputs; weapons walk owns ordinary projectile detonation; settle U-10 command before implementing bomb policy | EAWR-941 |
| G9 | M | WHE-30: Vader's ordered missing-escort replenishment and recharge; redirect damage only after U-08 is settled | EAWR-942 |
| G10 | M | WHE-31/32: jamming service, nonenemy 3D recipient stamps and nebula termination; reuse EAWR-785 for AI response consumers | EAWR-943 |
| G11 | L | WHE-08/44–48: hero trays, paired identities/health, automatic-ability highlight and selection/camera/enemy-attack routing; connect authored ability aliases/icons to the existing button interface; obtain U-12 before choosing collection/fog policy | EAWR-944 |
| G12 | M | WHE-33/34: IG-2000 area marker, refreshable corrupt recipient state, shield drain and expiry recharge; settle U-11 for remaining consumers | EAWR-945 |
| G13 | M | WHE-35–37: Peacebringer charge/shot state and availability inputs; weapon walk owns charged-shot consumption; stealth policy depends on U-04 | EAWR-946 |
| G14 | M | WHE-39/40: logical-type current/lifetime limits, including Rogue's allies-wide current cap and paid replacement; reuse existing production limit issue rather than duplicate | EAWR-724 |

The all-state issue search found no existing hero deployment, hero command-bonus, concentrate, beam,
replenishment, tray or corrupt-handler ticket covering these contracts. EAWR-760 tracks generic walk-7 ability
gaps and explicitly excludes hero abilities; EAWR-650/#653/#654 are broad tag-coverage containers. EAWR-916 covers
arrival invulnerability, not Falcon's active mode. EAWR-724, EAWR-540 and EAWR-785 are linked at their existing ownership
boundaries. Unknown-only policies are not filed as proven bugs. G2 corrects a registry scope defect;
G5 is the measured behaviour/loader mismatch. The remaining implementation tickets describe missing work.

## XML coverage audit

The appendix is an exact path/class status snapshot for hero controls, build limits, unit ability data and
the eight relevant nested handler families from `docs/tag-coverage/statuses.json`. It includes surrounding
generic ability fields: pulse/spawn controls belong to walk 7/weapons, presentation aliases/effects to UI,
and ground/company combat effects retain their scoped status. Their inclusion does not assert every roster
entry authors or reads every field. Core hero relationships and ability values actually used here are
enumerated in the roster/rules. `Icon_Name`, `CategoryMask`, ordinary health/shield/weapon/motion fields
continue through their existing scoped applications; loading a new hero class must reassess those rows.

Every nested Combat_Bonus, Concentrate_Fire, Energy_Weapon, Tractor_Beam, Sensor_Jamming, Corrupt_Systems,
Blast and Stealth path listed below is still todo. There are no deferred rows in this selected snapshot.
No statuses are flipped in this docs-only PR. G2 specifically fixes the incorrect blanket land scope for
space company identity/relationships/limits; it does not reclassify the company's authored ground abilities.

| XML path | Object classes | Registry status / ticket |
|---|---|---|
| `Abilities/Blast_Ability/@Name` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/Activation_Style` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/Charge_Up_Seconds` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/Charged_Effect` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/Charging_Effect` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/Damage_Multiplier` | UniqueUnit | todo EAWR-760 |
| `Abilities/Blast_Ability/SFXEvent_Activate` | UniqueUnit | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/@Name` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Ability_Recharge_Bonus_Percentage` | UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Activation_Style` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Applicable_Unit_Types` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Fire_Range_Bonus_Percentage` | UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Stacking_Category` | HeroUnit, Squadron, UniqueUnit, UpgradeObject | todo EAWR-760 |
| `Abilities/Combat_Bonus_Ability/Unit_Strength_Category` | HeroUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/@Name` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Activation_Style` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Categories` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Types` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Stacking_Category` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Target_Damage_Increase_Percent` | UniqueUnit | todo EAWR-760 |
| `Abilities/Concentrate_Fire_Attack_Ability/Target_Speed_Decrease_Percent` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/@Name` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Activation_Max_Range` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Activation_Min_Range` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Activation_Style` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Duration_In_Seconds` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Inaccuracy_Radius` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Owner_Particle_Effect` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/SFXEvent_Activate` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Shield_Drain_Per_Second` | UniqueUnit | todo EAWR-760 |
| `Abilities/Corrupt_Systems_Ability/Target_Particle_Effect` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/@Name` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Activation_Max_Range` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Activation_Min_Range` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Activation_Style` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Categories` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Types` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Damage_Per_Frame` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Bone_Name` | UniqueUnit | todo EAWR-760 |
| `Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Effect` | UniqueUnit | todo EAWR-760 |
| `Abilities/Sensor_Jamming_Ability/@Name` | SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Sensor_Jamming_Ability/Activation_Style` | SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Sensor_Jamming_Ability/Must_Be_Bought_On_Black_Market` | SpaceUnit, Squadron | todo EAWR-760 |
| `Abilities/Sensor_Jamming_Ability/Owner_Particle_Effect` | SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/@Name` | HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/Activation_Style` | HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/Must_Be_Bought_On_Black_Market` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/SFXEvent_Activate` | SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/Stealth_Color` | HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Stealth_Ability/Stealth_Transition_Time` | HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/@Name` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Style` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Stacking_Category` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Attach_To_Flagship_During_Space_Battle` | GenericHeroUnit, HeroUnit, UniqueUnit | todo EAWR-653 |
| `Build_Limit_Current_For_All_Allies` | Container, Squadron, UniqueUnit, UpgradeObject | todo EAWR-654 |
| `Build_Limit_Current_Per_Player` | HeroCompany | land-or-galactic |
| `Build_Limit_Current_Per_Player` | SpaceUnit, UniqueUnit, UpgradeObject | todo EAWR-654 |
| `Build_Limit_Lifetime_For_All_Allies` | HeroCompany | land-or-galactic |
| `Build_Limit_Lifetime_For_All_Allies` | UniqueUnit | todo EAWR-654 |
| `Build_Limit_Lifetime_Per_Player` | Container, SpaceUnit, Squadron, UniqueUnit | todo EAWR-654 |
| `Build_Limit_Lifetime_Per_Player` | HeroCompany | land-or-galactic |
| `Company_Transport_Unit` | HeroCompany | land-or-galactic |
| `Company_Units` | HeroCompany | land-or-galactic |
| `Create_Team` | GenericHeroUnit, HeroUnit, SpaceUnit, UniqueUnit | todo EAWR-650 |
| `Create_Team_Type` | HeroCompany | land-or-galactic |
| `Create_Team_Type` | Squadron, UniqueUnit | todo EAWR-650 |
| `Display_Contained_Hero_Grab_Bars` | UniqueUnit | todo EAWR-653 |
| `Is_Generic_Hero` | GenericHeroUnit | todo EAWR-650 |
| `Is_Generic_Hero` | HeroCompany | land-or-galactic |
| `Is_Named_Hero` | Container, HeroUnit, Squadron, UniqueUnit | todo EAWR-653 |
| `Is_Named_Hero` | HeroCompany | land-or-galactic |
| `Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent` | Projectile | todo EAWR-650 |
| `Projectile_Weaken_Enemy_Duration_Seconds` | Projectile | todo EAWR-650 |
| `Projectile_Weaken_Enemy_On_Detonation` | Projectile | todo EAWR-650 |
| `Projectile_Weaken_Enemy_Radius` | Projectile | todo EAWR-650 |
| `Projectile_Weaken_Enemy_Spawn_Effect` | Projectile | todo EAWR-651 |
| `Projectile_Weaken_Enemy_Take_Damage_Increase_Percent` | Projectile | todo EAWR-650 |
| `Projectile_Weaken_Enemy_Targets_Category_Mask` | Projectile | todo EAWR-650 |
| `Redirect_Damage_To_Teammates` | UniqueUnit | todo EAWR-650 |
| `Respawn_Whole_Team_When_Killed` | HeroUnit, UniqueUnit | todo EAWR-650 |
| `Unique_Space_Unit` | HeroUnit | todo EAWR-650 |
| `Unit_Abilities_Data/Unit_Ability/Active_By_Default` | UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text` | Container, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Alternate_Icon_Name` | Container, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text` | Container, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance` | Container, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Bomb_Countdown_Seconds` | UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Damage_Percent_When_Activated` | Container | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Disable_Movement` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Effective_Radius` | Container, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Effective_Radius` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Expiration_Seconds` | Container, Squadron | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Expiration_Seconds` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Friendly_Ability` | Container, HeroUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Friendly_Ability` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name` | Container, GenericHeroUnit, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Is_Pulsing` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects` | UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Max_Number_Of_Pulses` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Mod_Flag` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Mod_Flag` | UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Mod_Multiplier` | Container | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Mod_Multiplier` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Must_Be_Bought_On_Black_Market` | Container, HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone` | UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Particle_Effect` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Particle_Effect` | SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Projectile_Types_Override` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Pulse_Frequency_Secs` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Recharge_Seconds` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Recharge_Seconds` | Squadron | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop` | HeroUnit, SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability` | Container, GenericHeroUnit, HeroUnit, SpaceUnit, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type` | HeroUnit, SpaceUnit, Squadron, UniqueUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Stop_When_Activated` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Supports_Autofire` | HeroCompany | land-or-galactic |
| `Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Target_Types` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override` | SpaceUnit | todo EAWR-760 |
| `Unit_Abilities_Data/Unit_Ability/Type` | HeroCompany | land-or-galactic |
