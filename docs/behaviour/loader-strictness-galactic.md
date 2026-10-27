# Loader numeric strictness: galactic, economy, AI and story tags

Scope: how the remake loaders treat numeric XML values compared with the retail parser. Mods use sentinels (`-1`, `0`, fractions) that the retail parser accepts; a loader that rejects them breaks a retail-compatible mod. Land and space-skirmish tags are classified in their own notes.

## Retail parse rules

| ID | Parser type (registry `value_type`) | Retail behaviour |
|---|---|---|
| LSA-P1 | signed integer | Leading whitespace, optional sign, leading digits; a fraction truncates toward zero; text parses to 0; 32-bit with saturation at INT32_MIN/INT32_MAX, no sign check. |
| LSA-P2 | unsigned integer | As LSA-P1, reinterpreted unsigned: a negative wraps to a large value. |
| LSA-P3 | float | Decimal float parse; no range or sign check. |
| LSA-P4 | unit float | As LSA-P3, then a warning and a clamp to 0..1. |
| LSA-P5 | all | An empty value is skipped and the field keeps its constructor default; an unknown tag is a warning. |

Status column: `a` matches retail, `b` the remake rejects a value retail accepts (to fix), `c` the remake accepts a value retail clamps or reinterprets (truncate, unsigned wrap), `d` unknown (needs a retail capture or a walk).

## Galactic-family rules

| Rule | Tags | Remake rejects | Type | Status | Test |
|---|---|---|---|---|---|
| LSA-01 | Max_Space_Base, Special_Structures_Land/Space, Additional_Population_Capacity, Population_Value, Planet_Capture_Bonus_Reward, Zoomed_Terrain_Index (planets) | Leading decimal integer, text zero, int32 saturation; fractions truncate | signed/unsigned integer | a | galactic_data_contracts |
| LSA-02 | Displayed_Tech_Level_Adjustment, Minimum_Visible_Base_Level, Base_Population_Capacity (factions) | Leading decimal integer, text zero, int32 saturation; fractions truncate | signed integer | a | galactic_data_contracts |
| LSA-03 | Sort_Order, Min/Max_Custom_Tech_Level, AI/Human_Victory_Cycle_Limit, *_Victory_Credit_Target (campaigns) | Leading decimal integer, text zero, int32 saturation; fractions truncate | signed/unsigned integer | a | galactic_data_contracts |
| LSA-04 | Build_Advances_Tech_Level, Squadron_Capacity, Tech_Level, Required_Ground/Star_Base_Level, Base_Level, Additional_Population_Capacity, Build_Limit_*_Per_Player (objects) | Integer fields use atoi; fractions truncate, text zero, overflow saturates | signed/unsigned integer | a | galactic_type_contracts |
| LSA-05 | Starting_Spawned_Units_Tech_N count | Spawned counts use signed atoi and int32 saturation | string and integer pair | a | galactic_type_contracts |
| LSA-06 | Build_Max_Instances_Per_Planet | Per-planet limit uses signed atoi; omitted limit stays -1 and every negative disables the cap | signed integer | a | galactic_production_contracts |
| LSA-07 | Squadron_Capacity | Saturated signed atoi result reinterpreted as uint32; negative values load | unsigned integer | a | galactic_type_contracts |
| LSA-08 | Build_Advances_Tech_Level | Saturated signed atoi result reinterpreted as uint32; huge target loads and ordinary tech eligibility applies | unsigned integer | a | galactic_type_contracts |
| LSA-09 | Tech_Level, Required_Ground/Star_Base_Level, Base_Level | Signed int32 tech/base/required levels survive offer setup, replay and checkpoint. Availability compares tech directly and required levels as uint32; unusual values emit a B note instead of refusing campaign load. The initial-base representation still supports levels 1..5: an unsupported starting-base placement is omitted with a C note while its authored offer stays intact. | signed integer (tech), unsigned comparison (required levels) | a (offer/admission); C project degradation for unsupported initial bases | galactic_production_contracts; galactic_campaign_setup_contracts |
| LSA-10 | Hyperspace_Speed | Signed speed retained; nonpositive speed cannot depart (consumer safety clamp; exact negative-speed retail motion unverified) | float | a | galactic_type_contracts; galactic_movement_contracts |
| LSA-11 | Hyperspace_Speed_Factor (faction) | Signed faction factor retained; nonpositive factor cannot depart (consumer safety clamp) | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-12 | Corruption_Hyperspace_Bonus | Signed bonus retained; hop selection compares it with the ordinary multiplier, so a negative bonus supplies no boost | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-13 | Object_Max_Speed_Multiplier_Galactic | Zero and signed mode factors retained; nonpositive factors cannot depart (consumer safety clamp) | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-14 | MaximumFleetMovementDistance | Signed range retained; a negative movement range supplies no implicit graph links; explicit routes remain; observer proximity compares its magnitude squared (WGUI-04) | float | a | galactic_data_contracts; galactic_campaign_setup_contracts |
| LSA-15 | Build_Cost_Credits | Signed atoi value retained, including -1; WGEP-18 makes a price below one unavailable at purchase admission; squadron packing retains the signed cost | signed integer | a | galactic_type_contracts; galactic_production_contracts |
| LSA-16 | Build_Time_Seconds | atoi then uint32 reinterpretation; fractions truncate, -1 becomes 4294967295 seconds; frame conversion covers the full source range | unsigned integer | a | galactic_type_contracts; galactic_production_contracts |
| LSA-17 | Production_Speed_Factor, Production_Speed_Mod_Base_Vs_Tech_0..4, Galactic_Build_Time_Multiplier (difficulty) | Signed factors retained; WGEP-18 makes negative effective duration unavailable; omitted difficulty multiplier still requires separate constructor evidence | float | a | galactic_retail_production_numbers_stock |
| LSA-18 | Corruption_Galactic_Production_Price/Time_Multipliers | Signed float tokens retained; WGEP-18 makes negative effective price/time unavailable; no sign rejection at campaign load | float sequence | a | galactic_retail_production_numbers_stock |
| LSA-19 | Political_Control (objects) | retains signed integers, sums per planet and clamps to -1..1 (LSA-19); nonbinary values carry a B load note | signed integer | a | galactic_control_contracts |
| LSA-20 | Political_Control_Change_Time_Seconds | zero or negative -> fail (retail parses any float; the consumer of zero is not yet walked) | float | b | pending |
| LSA-21 | Max_Ground_Forces_On_Planet | <=0, fractional or > object limit -> fail | signed integer | b | pending |
| LSA-22 | Stacking_Category (Political_Transition_Bonus) | fractional (bad_value) or outside int32 -> fail | signed integer | b | pending |
| LSA-23 | Time_Reduction_Percentage (Political_Transition_Bonus) | none: outside [0,1) becomes zero (WGM-197) | float | a | pending |
| LSA-24 | Stacking_Category (reduce price/time abilities) | negative, > uint32 or fractional -> fail | signed integer | b | pending |
| LSA-25 | Sabotage_Cost_Credits, Neutralization_Cost, Hack_Super_Weapon_Cost, Sabotage_Cost_Increase_Factor | negative -> refuse | signed integer / float | b | pending |
| LSA-26 | Planet_Destroyed_Credit_Value | negative -> refuse | signed integer | b | pending |
| LSA-27 | Duration_Of_Credit_Halt | negative -> fail; zero or missing registers a zero-length halt (retail parses any float; with Can_Halt_Credit_Production on, a duration of 0 or less is an error message and becomes 10 seconds) | float | b | pending |
| LSA-28 | Min/Max_Respawn_Time_Per_Tech_Level | not 0 or 5 entries, min>max, negative min -> refuse (retail parses a whitespace/comma list of floats with no check; ability validation then reports a count other than 5, a negative entry, a zero mixed with non-zero entries or min > max and clears both lists, so the respawn time is 0; never a load failure) | float sequence | b | pending |
| LSA-29 | Evaluation_Interval_In_Secs (Planet_Income_Gambling_Ability) | > 32767/30 s -> fail (retail parses any float; a negative is an error message and becomes 0, no evaluation; no upper bound, the interval is seconds x logical FPS rounded to an int) | float | b | pending |
| LSA-30 | Evade_Detection_Chance (Galactic_Stealth_Ability) | none: retail clamps to [0,1] with a warning | unit float | c | pending |
| LSA-31 | Population_Value | negative or > 2^40 -> census fail; fractional via LSA-01 | signed integer | b | pending |
| LSA-32 | Credit_Cap_Per_Planet | negative -> invalid fiscal setup | float | b | pending |
| LSA-33 | Fiscal_Cycle_Time_In_Secs | zero, negative or > 2^31 s -> fail (retail parses any float; the consumer of zero is not yet walked) | float | b | pending |
| LSA-34 | Planet_Credit_Value, Planet_Capture_Bonus_Reward, Additional_Population_Capacity | none for sign (signed accepted) | signed integer | a | pending |
| LSA-35 | Black-market item Tech_Level, Base_Cost_Credits, Tutorial_* | fractional tech, out-of-int range -> fail | signed integer | b | pending |
| LSA-36 | Raid_Force_Max_Heros, Raid_Force_Max_Limited_Objects | Signed atoi caps load into the existing encoded fields; admission comparison signedness remains unverified and unchanged | signed integer | d | galactic_raid_contracts |
| LSA-37 | Space_Retreat_Allowed_Countdown_Seconds | Float text zero; nonpositive delay already elapsed, positive frame overflow saturates | float | a | galactic_tactical_return |
| LSA-38 | AutoResolveDisplayTime, Auto_Resolve_Tactical_Multiplier | Signed float values retained; text reads zero | float | a | galactic_auto_resolve_round |
| LSA-39 | Space_Retreat_Attrition_Factor | Unit float clamped to [0,1] | unit float | a | galactic_tactical_return |
| LSA-40 | Battle_Pending_Timeout_Seconds (multiplayer) | Signed atoi seconds; nonpositive timeout already elapsed, unreachable positive delays bounded by campaign lifetime | signed integer | a | galactic_data_contracts |
| LSA-41 | Default_Hero_Respawn_Time | none (any sign, CGS-12) | float | a | pending |
| LSA-42 | Numeric tags read by the shared galactic record/object number helpers and direct campaign number helper | Shared retail numeric readers accept text as zero and saturate at representable limits | all numeric | a | galactic_data_contracts; galactic_type_contracts |
| LSA-43 | Trailing text after a number | none (issue row only) | all numeric | a | pending |
| LSA-44 | Political_Control_Gain, HS_Speed_Factor, Credit_Gain_Factor, Planet_Reveal_Delay_Time | none | signed integer/float | a | pending |
| LSA-44a | MaximumPoliticalControl | none, but a negative or fractional value is kept as authored and zero disables the AI ratio (retail parses an integer, truncating a fraction, and all three consumers read it unsigned: a negative wraps to 4294967295; zero divides by zero in the AI ratio) | unsigned integer | c | pending |
| LSA-45 | Scale_Factor (planet) | zero/negative -> fail model bounds (retail parses any float) | float | b | pending |
| LSA-46 | GoalSetExtensionSize (AI player XML) | anything not a strict base-10 integer (sign +, spaces, fraction) -> load fail | unregistered (AI XML) | b | pending |
| LSA-47 | Tactical_Budget_Allowance entries (AI) | non-numeric -> load fail (retail parses each entry as a float with no check, text -> 0; only an unknown goal category fails the template, which the remake accepts) | float (AI XML) | b | pending |
| LSA-48 | Story reward level / credit amount | negative or non-whole level, non-whole amount -> reward declined (retail: CREDITS parses a float and adds fractional credits, text -> 0; SET_TECH_LEVEL parses an integer, truncating a fraction, then clamps to 0..maximum) | Lua/story data | b | pending |
| LSA-49 | AITerrainEffectiveness.xml cells | negative or non-numeric -> fail (retail parses a float, text -> 0; a negative is an AI log error and becomes 0; an unknown environment or movement class is skipped with a log line) | float | b | pending |

Each `b` row gets a boundary test (-1, 0, maximum, fractional, empty) in the family PR that fixes it; `pending` is replaced by the test name then.

## Signed movement and production consumers

LSA-10..18 use the shared retail numeric readers. Integer values saturate at signed 32-bit limits before any unsigned reinterpretation. Empty and `TBD` values leave inherited values intact. Text with no numeric prefix reads zero. These rules come from the debug-build parser audit (LSA-P1..P5).

Authored movement values keep their signs through campaign loading and checkpointing. At departure, nonpositive speed inputs are clamped to zero independently and departure is declined; this prevents two negative factors from manufacturing forward motion. This is an explicit safety policy while exact negative-speed retail motion is unverified. A negative corruption speed bonus cannot exceed the ordinary hop multiplier. Negative movement distance excludes implicit graph edges; explicit routes survive. WGUI-04's separate observer query compares against range squared: a negative range has the same proximity radius as its magnitude.

Production prices and effective durations keep their signs through preparation, raw income bases and checkpointing. WGEP-18 declines purchases below one credit or with negative effective duration. WGEP-30 rounds signed prices with half ties away from zero. These are admission decisions, not campaign-load errors. `Build_Time_Seconds` is an unsigned integer: `-1` becomes 4294967295 seconds, not a negative time. Frame conversion splits whole and fractional seconds before multiplication, so the full source range fits without overflowing Q24 intermediate arithmetic.

The fixed-point representation saturates float overflow and overflowing price/time products at the signed representable endpoint; retail uses floating-point infinity. This representation policy accepts the campaign and cannot make a negative result purchasable. The default for an omitted difficulty build-time multiplier remains unverified and keeps its existing missing-input diagnostic. The raw-production-basis seam still cannot reconstruct an unscaled nonzero value after an authored zero corruption factor changes to a different factor; equal initial factors require no inversion and load normally.

## Political and running arithmetic degradation

Political inputs with unknown retail consumers load with a class-C note. An absent,
nonpositive or unrepresentable transition duration uses one second; an absent surface
limit uses one slot, and authored limits truncate to an integer and clamp to the
implementation object budget with a minimum of one (or the already placed roster size
when larger, preserving those placements). Unqualified automatic activation
or missing category/multiplier skips that ability. These are project safety defaults,
not claims about retail consumers (LSA-20..22); WGM-197's sourced multiplier fallback
remains unchanged.

During control service, a missing live type contributes nothing and an unbound ability
source is skipped. Fixed-point sums and products saturate at their failing consumers;
an unrepresentable political service rate moves at most one level per admitted frame.
Fiscal owner sums and account arithmetic saturate before the ordinary zero/cap clamp.
Only previously failing inputs take these paths. Partitioned work stays partitioned;
notes merge after the join in stable planet order, and fiscal reduction stays in stable
planet/player order. Their effective values depend only on simulation state.

Runtime notes occur once per code and object per match. New notes stage after the
ordered simulation commit and reach the existing sink when the presentation owner
reads a live frame, without calling the synchronous viewer adapter from a worker. They
are excluded from hashes and checkpoints. A restored match can emit a note again.
The `galactic_control_contracts` and `galactic_fiscal_contracts` cases cover continued
stepping and 1/2/4/8-worker equality. Infrastructure/executor exceptions and malformed
save/replay byte limits retain their failures.
