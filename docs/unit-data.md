# M2 unit tables

P2-02 (EAWR-65). `eawr::units`
(`include/eawr/units/unit_tables.hpp`) turns FoC data for the pinned M2 fleet
([m2-skirmish.md](../plan/phase-2/m2-skirmish.md), the "Pinned unit types" table) into
Q24 tables. Combat, movement and squadron tickets (EAWR-70–EAWR-76) and the hardpoint-state hook
(EAWR-136) read these tables; they do not read XML themselves.

## Inputs and closure

- The twelve pinned types in the EAWR-64 table order, then every type they reference in
  first-reference order: squadron craft, SK-23 spawned squadrons, hardpoints, projectiles,
  targeting priority sets. Nothing outside that closure is loaded.
- Game objects and hardpoints come from the P1 catalog (`data::load_catalog`, FoC profile).
  Targeting priority sets (`TargetingPrioritySetFiles.xml` and its includes) and
  `gameconstants.xml` are read with `data::load_document`, the same parser outside the five
  catalog registries. Models are ALO files at `data/art/models/<lower-case name>`, with
  `.alo` added when the name has none.
- `unit_scan --game-root <install> [--report <out.json>] [--notes] [--hardpoints] [--movement] [--all-types] [--combat]` lists
  every loaded unit, its unresolved rows, the notes and the content identity; `--movement`
  adds each unit's movement tags and abilities and the motion table. `--all-types` loads every
  FoC game object that authors `HardPoints` instead of the pinned fleet; `--combat` reports each
  weapon fire frame's distance from orthonormal and whether the combat table validates.

## Value rules

- A single-value tag is the catalog's effective value: the last occurrence in the
  most-derived variant layer that authors it. When one layer authors it more than once,
  the scan adds a note.
- A list tag (`Squadron_Units`, `Squadron_Offsets`, `Starting_Spawned_Units_Tech_0`,
  `Reserve_Spawned_Units_Tech_0`, `Fire_Inaccuracy_Distance`) takes every occurrence, in
  order, from the most-derived layer that authors it. A base layer that also authors it
  is noted, not merged. No pinned type authors a list tag in two layers.
- List values split on commas and whitespace; `CategoryMask` also splits on `|`.
  Decimals use `Fixed::from_decimal`, which accepts one trailing C float suffix (`0.8f`,
  `-3f`, as in `Mod_Multiplier`). Booleans are `yes`/`true`/`1` and `no`/`false`/`0`,
  in any case.
- Ability types are upper-cased; `authored_type` keeps the spelling
  (`power_to_weapons` on the Acclamator). Station `Abilities` sub-objects (income, radar)
  are listed as `inactive_abilities` (SK-31).
- A craft's object weapon (`Projectile_Types`) has one recharge time,
  `Projectile_Fire_Recharge_Seconds`, which is stored as both its minimum and its maximum.
- Spawners: only `Tech_0` lists load (SK-23). `reserves` are read and `reserves_used` is
  always false (SK-36).
- A field that a row requires but the data lacks, or a reference that does not resolve,
  becomes an `Unresolved` row. It never becomes a default value.

Retail parse facts from the FoC debug build ([audit](behaviour/debug-build-audit.md), AU-80 to
AU-83). The value rules above agree with them on every pinned type; the differences below
matter only for data the M2 fleet does not author.

- Tags apply in document order, so the last occurrence of a single-value tag wins. A variant
  (`Variant_Of_Existing_Type`) starts as a full copy of its base and then applies its own
  tags.
- An empty value, or the value `TBD`, changes nothing: the earlier or inherited value stays.
- Booleans read true for `1` or for a value whose first letter is Y or T, in either case, and
  false for anything else. Numbers read the leading number and ignore trailing text.
- List tags differ by tag. `Squadron_Units`, `Starting_Spawned_Units_Tech_0` and
  `Reserve_Spawned_Units_Tech_0` append with every occurrence, also onto a list a variant
  inherits. Each spawned-unit occurrence reads only its first type and count pair.
  `Squadron_Offsets` and `Fire_Inaccuracy_Distance` append in a base type, but in a variant each
  occurrence replaces the list, so only the variant's last occurrence stays.

## Priority sets

EAWR-270 loads the whole `Priority_Set` and gives the R-09 lookup
([space targeting](behaviour/space-targeting.md)) as `units::attack_priority` and
`units::hard_point_priority`. The rules were checked against the FoC debug build.

- `CategoryMask` and `Property_Flags` (split on `|`, commas and whitespace) and the set's
  `Category_Exclusions` and `Property_Exclusions` become bits from the dynamic enum files
  `data/xml/enum/GameObjectCategoryType.xml` and `GameObjectPropertiesType.xml` (`0x`
  hexadecimal or decimal values, names without case). An unknown name is an `Unresolved` row.
- An `Attack_Priorities` name is a category if the category enum knows it, else a property if
  the property enum knows it, else an exact object type. The FoC space sets use only
  categories; the ground sets also use properties (`Turret`, `TacticalStructure`) and types.
- `Hard_Point_Priorities` and `Hard_Point_Exclusions` use FoC's priority-set hardpoint names
  (`Engine`, `Shield_Generator`, `Weapon_Laser`, ...: the `HARD_POINT_` type names without the
  prefix), without case. An unknown name is an `Unresolved` row and is dropped.
- Scoring a candidate type, lower wins. An entry naming the exact type returns its weight at
  once, before any exclusion. Otherwise the score starts at `unlisted_priority` (above every
  weight), a matching category entry lowers it to its weight, and a matching property entry
  replaces it the first time and lowers it after that (FoC's order, kept on purpose). Then a
  `Unit_Exclusions` type, a category exclusion (unless a property entry matched) or a
  property exclusion gives no priority (`nullopt`).
- A hardpoint's priority is its 1-based position in `Hard_Point_Priorities`; a hardpoint not
  listed there but in `Hard_Point_Exclusions` has none; any other ranks `unlisted_priority`.
- Every FoC space set excludes the `NotOpportunityTarget` property and the four
  `Destroyable_Asteroid_*` types. No M2 fleet type has `NotOpportunityTarget`.

The parent-side rules of R-09 belong to the targeting ticket (EAWR-73). `units::combat_table`
resolves each set a type uses against every station, ship and craft of the tables with
`attack_priority`; a parent without a set scores every candidate 1.0. The runtime override and
the parent's attack category restrictions are not loaded.

## Combat table

`units::combat_table` (EAWR-73, [space weapon fire](behaviour/space-weapon-fire.md)) gives every
station, ship and craft a profile keyed by its TED type CRC: its `CategoryMask` bits, its
`Targeting_Max_Attack_Distance` (the last authored value, as every single-value tag), its weapon
hardpoints in `HardPoints` order and then its object weapon, its target bones and the position of
every hardpoint whose attachment bone resolved. Points are bone positions times `Scale_Factor`,
turned by FoC's +90 degree model turn, (x, y, z) to (-y, x, z) (EAWR-74, R-ROT-01). A
weapon's recharge bounds are `trunc(100 x seconds)`, its pulse delay `trunc(30 x seconds)` frames
(the Q24 rounding of the decimal does not lose a frame: 0.2 s is 6 frames, EAWR-74),
its pulse count at least 1, its `Fire_Category_Restrictions` category bits; fire bones fall back
to the attachment bone. The weapon also carries that bone's bind-frame x, y and z axes, each
turned like a point and normalized, which orient its cone (EAWR-361, space-weapon-fire W-07). `Allow_Opportunity_Fire_When_Idle` and `_When_Targeting` default to yes,
as in FoC's hardpoint data (EAWR-74). The object weapon (`Projectile_Types`) ranges by
`Targeting_Max_Attack_Distance`. When the unit authors `Fires_Forward` no, its cone is its
`Turret_Rotate_Extent_Degrees` (yaw) and `Turret_Elevate_Extent_Degrees` (pitch) about the unit's
own facing (EAWR-388, space-weapon-fire W-09); without both it has no cone. `Is_Turret`,
`Turret_XY_Only`, `Fire_Min_Range_Distance` and the soft coordinate radius are not loaded.

Since EAWR-74 ([space damage](behaviour/space-damage.md)) each weapon whose projectile has a
`Max_Speed` carries a shot: the projectile's damage, speed and shield/hitpoint flags, the damage
type (the hardpoint's `Damage_Type`, else the projectile's, else `Damage_Default`), its travel
(a hardpoint's `Fire_Range_Distance`, else the projectile's `Projectile_Max_Flight_Distance`) and
the `Fire_Inaccuracy_Distance` rows as category bits. Since EAWR-361 each weapon hardpoint also
carries its AI combat power: its projectile's `AI_Combat_Power` over the sum of the type's weapon
hardpoints' projectiles', times the type's `AI_Combat_Power` (zero without them), which weighs
the directions a ship turns toward ([space weapon fire A-06](behaviour/space-weapon-fire.md#attack-orders)). Each type carries the box of its model's
collidable meshes (each mesh's bounds placed by its bone's bind frame, then scaled and turned like
the points); a model without collidable meshes gives none.

## Damage rules

`units::durability_table` binds the damage rules (EAWR-74): the shield recharge interval in frames,
the `Depleted_Shield_*` values, the `Diminishing_Firepower` pairs (a list constant: the scan keeps
its text) and the `Damage_To_Armor_Mod` rows of the fleet's types, indexed by
`units::damage_type_index` (lower-case names, sorted); a pair without a row is 1. A type with the
`SHIELDED` behaviour gets `Shield_Points` and `Shield_Refresh_Rate`; every type gets its
`Armor_Type` and `Shield_Armor_Type` indices. Since EAWR-361 it also binds the energy recharge interval
(`EnergyRechargeIntervalInSecs`, 150 frames) and `EnergyToShieldExchangeRate`, a type with the
`POWERED` behaviour gets `Energy_Capacity` and `Energy_Refresh_Rate`, and the combat table gives an
object weapon its projectile's `Projectile_Energy_Per_Shot` ([space damage](behaviour/space-damage.md#energy-pool)).

## Ability table

`units::ability_table(tables, humans)` binds the unit abilities (EAWR-76,
[space abilities](behaviour/space-abilities.md) AB-01 to AB-04): per type with a modelled ability
(`DEFEND`, `TURBO`, `POWER_TO_WEAPONS`, `SPOILER_LOCK`) its `Expiration_Seconds` and
`Recharge_Seconds` truncated to frames, `Supports_Autofire` and the six `Mod_Multiplier` kinds of
AB-20 to AB-24 (1 when not authored), and whether it runs `ObjectScript_PowerToShields` with
`DEFEND` (AB-41). Squadron types are skipped (their craft's data acts, AB-15), and so are
`HUNT` and `ION_CANNON_SHOT` (cut, AB-03). Any other multiplier name fails with
`EAWR-UNITS-0006`. `humans` are the human players of the session. `skirmish::session_content`
builds it with the lobby's human slots.

## Hardpoint positions in Q24

A `BonePoint` is the translation of a bone's bind frame in the owner model's asset space,
in asset units, before the unit's `Scale_Factor`. Since EAWR-361 it also keeps the same frame's
three rotation columns (its x, y and z axes, unscaled and unnormalized), encoded after the
attached-model flag as an optional of three vectors:

1. Each of the bone's twelve stored binary32 transform values converts exactly to Q24 with
   `scene::fixed_from_binary32` (round once, nearest-even). Stored value `4r + c` becomes
   row `r`, column `c` of a `Mat3x4`; column 3 is the translation.
2. The bind frame is `compose(parent bind frame, local)` from the root to the bone, using
   `sim::math::compose` (exact widened sums, one rounding per component).
3. Bone names match ASCII case-insensitively; the first bone in model order wins. A fire
   bone the hull lacks is looked up in the hardpoint's `Model_To_Attach` and placed
   through the attachment bone's frame. The FoC fleet does not need this: every bone it
   names is in the hull model.

Corvette and fighter weapon hardpoints name no `Attachment_Bone`. Their position is the
fire bone. Against a double-precision composition of the same stored values, the worst
translation difference over the fleet's 328 bones is 2.1e-6 asset units.

## Sensor ranges

`units::sensor_range(type)` (EAWR-271, [space visibility](behaviour/space-visibility.md) V-01, V-03)
is the range `skirmish::sensor_table` binds. A type reveals only with `REVEAL` in its
effective `Behavior` or `SpaceBehavior` list (`UnitType::reveal`), and then with its own
`Space_FOW_Reveal_Range`. A squadron reveals through its team container: `team_type` is its
`Create_Team_Type`, `Team` when absent, resolved through the catalog like any object (not
added to the unit list), and `team_reveal_range` is that container's
`Space_FOW_Reveal_Range` when the container has `REVEAL`. Both fields are in the identity.

## Durability table

`units::durability_table(tables)` (EAWR-72) gives the tactical session its durability content
([space hardpoints](behaviour/space-hardpoints.md)). It has one profile per station, ship and
craft with a `Tactical_Health`; squadrons have none. Profiles are keyed by the TED
object-type CRC of the type name (`assets::object_type_crc`), the type IDs the skirmish start
uses, and sorted by it.

- Hull and every destroyable hardpoint's `Health` are multiplied by
  `Object_Max_Health_Multiplier_Space` (one Q24 rounding). Other hardpoints have zero health.
- A hardpoint's role follows its `Type`: the four weapon types are weapons; engine, shield
  generator, fighter bay and enable-special-ability map to their own role; the rest are other.
- `Repair_Amount_Per_Frame` and `Repair_Cost_Per_Frame` pass through with zero when absent.
  `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` defaults to yes when absent; an authored
  boolean overrides it (IS-05, IS-06).
- The rules are `Hull_Vs_Hard_Points_Health_Constraint`, `Engines_Disabled_Speed_Modifier`
  and `Health_Low_Percent_Threshold`.
- It fails when one of those four scalars is missing, a destroyable hardpoint has no `Health`,
  two types share a type ID or `validate_durability` rejects the result.

## Motion table

`units::motion_table(tables)` (EAWR-70) gives the tactical session its movement content
([space movement](behaviour/space-movement.md) MV-01). It has one profile per ship with a
`Max_Speed` and a `Max_Rate_Of_Turn`, keyed like the durability table. Stations do not move;
craft fly with the squadron locomotor (EAWR-75) and have no profile.

- `Max_Speed`, `OverrideAcceleration`, `OverrideDeceleration` and `Max_Rate_Of_Turn` (degrees
  per frame) are multiplied by `Object_Max_Speed_Multiplier_Space` (one Q24 rounding). A
  missing acceleration or deceleration is the multiplied maximum speed.
- `Max_Rate_Of_Roll` (degrees per frame, multiplied the same way) and `Bank_Turn_Angle`
  (degrees) set the banking in turns (EAWR-351, space-movement BK-01). A type without them keeps
  the engine's defaults, 2 and 70.
- The turn-in-place slowdown is `TurnInPlaceSlowdownCorvette`, `-Frigate` or `-Capital` by
  `Space_Layer` (`Capital` and `SuperCapital` take the capital value), 1 for any other layer.
- The rules are 360 / `MaxRotationsSpace` (the arc step in degrees) and
  `XYExpansionDistanceSpace`.
- It fails when one of those six scalars is missing, two types share a type ID or
  `validate_motion` rejects the result.

With the path finder's constants loaded (EAWR-71: `WaitOperatorSpeedCoefficient`,
`WaitOperatorBaseFrameTime`, `WaitOperatorCostCoefficient`, `MinObstacleCostSpace`,
`CurrentPathCostCoefficientSpace`, `OccupationRadiusCoefficientSpace`, the four
`SpacePathFailure*` values, `SpacePathfindMaxExpansions`, `SpacePathingTries`,
`SpaceObjectTrackingInterval`, `SpaceObjectTrackingTreeCount` and, since EAWR-266,
`DestinationSearchRadiusIncrementSpace`) the table also carries the
avoidance rules and one footprint per ship, station and pinned map object with a
`Space_Layer` of `Capital`, `Frigate`, `Corvette`, `StaticObject` or `SuperCapital`
([space movement](behaviour/space-movement.md) AV-05):

- The hard half extents are `Custom_Hard_XExtent` and `Custom_Hard_YExtent` when both are
  positive; otherwise, per axis, the positive custom value or the model's collision half
  extent, times `Scale_Factor`. The collision box is the union of the model's collidable
  meshes' bounds (the ALO mesh flag), each moved into the bind pose by its bone's frame (the
  centre transformed, each half extent the absolute row sums of the frame); a model without a
  collidable mesh has the box ±1.
- The soft radius is `Custom_Soft_Footprint_Radius`, else `Space_Obstacle_Radius`, else the
  larger collision half extent, times `Scale_Factor`.
- A type with `SPACE_OBSTACLE` in `Behavior` or `SpaceBehavior` is an obstacle: tracked from
  spawn as a square of its soft radius.
- The pinned map objects (`pinned_m2_obstacles`: `Skirmish_Merchant_Dock`,
  `N_Gravity_Well_Station`, `Defense_Satellite_Laser_Pad`, `Mineral_Extractor_Pad`,
  `Orbital_Resource_Container`) load only these fields, in `UnitTables::obstacles`.

On the FoC data (half extents X by Y, then the radius): Corellian corvette 17.76 × 41.82
(41.82), Tartan cruiser 23.75 × 49.93 (49.93), Nebulon-B 23.48 × 109.95 (109.95), Acclamator
83.05 × 148.38 (148.38); obstacle radii: the Rebel and Empire star bases 300 and 250,
`Mineral_Extractor_Pad` 206.25, `Defense_Satellite_Laser_Pad` 100, `N_Gravity_Well_Station`
500, `Skirmish_Merchant_Dock` 414.91 (its model box × 1.75). `Orbital_Resource_Container` has
no `Space_Layer` and gets no footprint.

## Content identity

`content_identity(tables)` is SHA-256 over `canonical_encoding(tables)`. The encoding
starts with the length-framed text `eawr-unit-tables-v5` (v4 before EAWR-530, v3 before EAWR-75, v2 before EAWR-71, v1 before EAWR-270). It
then writes every unit (with its EAWR-71 footprint fields after its inactive abilities), the
pinned map obstacles (ID, XML type, `Space_Layer`, `Scale_Factor`, model path, footprint
fields), projectile, priority set, category and property enum value, combat scalar and
damage/armor row in table order, with each struct's fields in declaration order (an enum
class is its uint32 value):

- Texts are a uint32 byte length followed by the bytes.
- Counts and indices are uint32. Q24 values are their int64 raw value. All integers are
  little-endian.
- An optional is a 0 byte when absent, or a 1 byte followed by the value.

Unresolved rows, notes, input files, source layers and host paths are not encoded.
Relocating an install, or editing a tag the tables do not read, leaves the identity
unchanged. Any loaded value, bone position or missing field changes it. M2 replays put
this value in the replay header's `content_identity`. The existing synthetic replay
fixtures keep their own identities, so their hashes do not change.

## FoC fleet result

The FoC profile gives 17 units (the 12 pinned types and 5 craft), 13 projectiles, 5
priority sets, 36 combat scalars and 168 damage/armor rows. There are no unresolved rows.
The identity is `2e2540011dfee39f1e8ef1924b6232f172170a4d51a860c383267832aecb6079` (pinned in `tests/units`; EAWR-340 corrected the hardpoint-death default; EAWR-68 added
`Space_FOW_Reveal_Range`; EAWR-72 added hardpoint repair values and health rules;
EAWR-270 added priority-set exclusions, entry kinds, `Property_Flags` and enum tables;
EAWR-74 added `Diminishing_Firepower`, constant texts, `SHIELDED`, collision bounds and the opportunity-fire defaults;
EAWR-70 added movement constants; EAWR-271 added `REVEAL` and squadron team containers; EAWR-71 added the
path finder constants, footprints and the pinned map obstacles; the EAWR-343 review
started each axis of the collision box from its own bounds; EAWR-361 added the bone points' bind-frame axes; EAWR-266 added
`DestinationSearchRadiusIncrementSpace`; EAWR-361 added the projectiles' `AI_Combat_Power`; EAWR-75 added the fighter-bay axis, `Strafe_Distance`,
`Guard_Chase_Range`, `Idle_Chase_Range` and `Squadron_Formation_Error_Tolerance`; EAWR-452 added `Attack_Move_Response_Range`; EAWR-388 added the
object weapon's turret extents; EAWR-409 added the craft's `Out_Of_Combat_Defense_Adjustment`; EAWR-361 added `POWERED`, the energy pool behaviour; EAWR-518 added the Rebel MC80 `Calamari_Cruiser`, SK-22; EAWR-536 added `Collision_Mesh`, `Collision_Box_Modifier` and the collision meshes, DG-36; EAWR-457 added the craft's `Minimum_Follow_Distance`; EAWR-561 added the projectiles' ion stun, `ION_STUN_EFFECT`, a hardpoint's own `Projectile_Damage`, the team types' abilities and `Projectile_Types_Override`; EAWR-599 added `FormationMinimumSideError` and `FormationMaximumSideError`, the squadron table's lane steer, space-fighters FO-11; EAWR-607 reads the object weapon's scatter rows from `Targeting_Fire_Inaccuracy`, not the unit's `Fire_Inaccuracy_Distance`, space-damage DG-24; EAWR-530 added each type's production data (`Tactical_Buildable_Objects_Multiplayer`, `Tactical_Build_Cost_Multiplayer`, `Tactical_Build_Time_Seconds`, `Tactical_Production_Queue`, `Population_Value`, `Reinforcement_Prevention_Radius` and the stations' income streams and bonuses) and `MP_Default_Credits`, `Tactical_Build_Time_Multiplier` and `Allow_Reinforcement_Percentage_Normalized`).
The durability table has 12 profiles (7 stations and ships, 5 craft). The motion table has 5
ship profiles, 15-degree arcs and a 300-unit expansion distance. The duplicate-tag notes are
pinned in `tests/units`. Retail keeps the last occurrence of each of these single-value tags as well (AU-80, re-read for these tags as IS-01 to IS-04), so none of them changes a value:

- `Corellian_Corvette` and `Tartan_Patrol_Cruiser` author `Targeting_Max_Attack_Distance`
  twice (2000, then 800).
- They also author `Space_FOW_Reveal_Range` twice: the Corellian 1200 then 1000, the Tartan
  1200 twice.
- `HP_Empire_Station_One_01` authors `Fire_Bone_B` three times.
- `Darth_Vader_TIE_Fighter_Container`, the base of the Y-wing squadron's container, authors
  `Space_FOW_Reveal_Range` twice (800, then 1000).

Schema check (EAWR-65 prep note, eaw-schema PR EAWR-8): of the tags the loader reads, the pinned
schema marks only `Fire_Inaccuracy_Distance` on game objects and `Priority_Set`/`@Name` in
priority-set files as unknown. The fleet data uses both. The loader reads raw tags, so no
schema change is adopted here.
