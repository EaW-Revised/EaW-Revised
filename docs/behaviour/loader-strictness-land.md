# Loader numeric strictness: land tags

Scope: how the land unit-table loader and the land content adapter read numeric XML values compared with the
retail parser (legacy EAWR-4994). A mod value the retail game accepts never fails a land start. The galactic note
([loader-strictness-galactic.md](loader-strictness-galactic.md)) holds the shared parser-type rules LSA-P1..P5;
the rules below apply them to the land path and give each accepted value its land meaning.

## Parse rules

| ID | Rule | Evidence |
|---|---|---|
| LSL-P1 | A land float tag reads like the C runtime's decimal float parse: the leading number counts and trailing text is ignored (`1.5f` reads 1.5, `1,5` reads 1); a value with no leading number reads 0; a magnitude beyond the fixed range saturates at the largest fixed value of its sign (retail stores an infinity). Nothing is range or sign checked. A value read other than as written leaves a loader note, never an unresolved row. Known divergence: the C runtime also reads hexadecimal floats (`0x10` is 16) and the `nan` word; this parser reads both as 0 (no stock or swept mod value uses them). | debug build parser walk (LSA-P3); `data::retail::float_value` |
| LSL-P2 | A land signed integer tag (`Max_Squad_Size`) reads like the C runtime's integer parse: optional sign, the leading digits, a fraction truncates toward zero (`2.9` reads 2, `-0.5` reads 0), no digits reads 0, beyond 32 bits it saturates. | debug build parser walk (LSA-P1); `data::retail::integer_value` |
| LSL-01 | An empty or `TBD` value is no value: the earlier, inherited or default value stays (AU-80/81). This holds for every land tag type, booleans and member lists included. Remake's empty `Scale_Factor` and `Tactical_Health` therefore read as absent. A value of only whitespace is treated the same; the retail parser would read it as 0 unless the XML reader trims it first, which is unverified (the AU-80 assumption). | debug build (LSA-P5) |

## Land meanings

| ID | Tags | Rule | Evidence |
|---|---|---|---|
| LSL-02 | `Max_Speed`, `Max_Rate_Of_Turn`, `Scale_Factor` (company members) | An absent value is the object type default: maximum speed 0.25, rate of turn 2.3, scale 1.0. | debug build object type defaults |
| LSL-03 | `Max_Speed`, `Max_Rate_Of_Turn` (members), team `Max_Speed` | Zero binds: a zero speed is an immobile member (the movement service already skips it), a zero turn rate never turns. A negative value applies as zero. | zero: authored stationary objects (Felucia plants, model props); negative: unverified |
| LSL-04 | `Custom_Soft_Footprint_Radius`, `Custom_Hard_XExtent`, `Custom_Hard_YExtent` (members) | A zero value is the type default (zero), which means unauthored: the model's collision half extents apply, per axis for the hard extents, the larger one for the radius. A negative value takes the same fallback; the loaded data keeps it as parsed. | zero: debug build object type defaults plus WLPA-06/14 and AV-05; negative: unverified (RaW `Siege_Tank` -0.4) |
| LSL-05 | `Company_Units` | An empty or `TBD` member list leaves the list at its empty default, the same state as an absent tag, so the company takes LC-01's direct-object branch and attempts its own object. A company object without a locomotor then meets LC-84's formation-admission refusal, which names the company; the start never sees an unresolved member list. | debug build: the type default is an empty member list, LC-01 (no member list = the direct-object branch), LC-84; LSA-P5 |
| LSL-06 | `UnitCollisionClass` | An unknown name fails only that key: the class keeps its None default and the loader notes the name. | debug build enum conversion; type default None |
| LSL-07 | `Max_Squad_Size` | A size of 1 or more groups as authored (a fraction truncated, LSL-P2). Zero or a negative size is noted and the company groups as with no authored size (one squad holds every member). | non-positive grouping: unverified |
| LSL-08 | `Obstacle_Width`, `Obstacle_Height` | A negative authored extent is kept as parsed; like zero-sized geometry it rasterizes no cells and the start continues. | unverified for negative |
| LSL-09 | `OverrideAcceleration`, `OverrideDeceleration` | A negative override takes the absent-override rule (the maximum speed, MV-01). | unverified |
| LSL-10 | `Scale_Factor` (members and statics) | A zero or negative scale sizes the footprint and the static geometry by its magnitude, zero for zero; the authored scale stays in the data. | unverified |
| LSL-11 | `FormationSpacing` (members and companies) | A negative spacing adds no spacing; the authored value stays in the company data. | unverified |
| LSL-12 | `Space_Obstacle_Offset` (land bodies) | Each part is an LSL-P1 float; a missing part keeps its zero default and extra parts are ignored. | debug build vector parse |
| LSL-13 | every converted land physical value: member speed, rates, turn rate, footprints and spacing; static extents and offsets | A product or authored value beyond 2^24 world units (per second, per second squared or turns per second) saturates at that limit instead of refusing the company; retail stores an infinity there. A static obstacle wider than the map still meets the remake's raster work guard (four terrain planes of samples), which names the object. | unverified (far beyond any stock value or map) |

Stock land values all read exactly as written: the installed land test counts every stock company's
retail-parse notes and expects none. A stock company or static that bound before takes none of the changed
branches (those values were refusals), so stock starts and their land identity are unchanged. Booleans already
follow AU-82 and never fail.

## Open (not applied here)

- The type defaults also give `Tactical_Health` 100 and `Max_Squad_Size` 10, but an absent value keeps today's
  meaning (no durability row; one unsized squad) because changing it moves stock land identities.
- An absent static `Scale_Factor` keeps its named `unverified-static-scale` gate for the same reason, although the
  type default is 1.0.
- The type default of both acceleration overrides is zero and MV-01 reads a missing override as the maximum speed,
  which suggests an authored zero also means the maximum speed. Authored zero still applies as zero.
- The combat, durability and targeting rows of a land body still use the strict shared reader and belong to
  the space-skirmish strictness family: `Tactical_Health`, `Shield_Points`, `Shield_Refresh_Rate`,
  `Targeting_Max_Attack_Distance`, `Targeting_Min_Attack_Distance`, `Targeting_Stickiness_Time_Threshold`,
  `Idle_Chase_Range`, `Guard_Chase_Range`, `Attack_Move_Response_Range`, `Ranged_Target_Z_Adjust`,
  `Death_Fade_Time`, `Death_Persistence_Duration`, the object weapon rows (`Projectile_Fire_Recharge_Seconds`,
  `Projectile_Fire_Pulse_Delay_Seconds`, `Projectile_Fire_Pulse_Count`, `Projectile_Damage`) and the shared
  projectile counters (`Projectile_Blast_Area_Dropoff_Tiers`, `Projectile_Blast_Area_Max_Victims`). A bad value
  there leaves an unresolved row that skips the type's combat row (`land_combat_field`); it never refuses the
  land start, but the value is not bound either.
