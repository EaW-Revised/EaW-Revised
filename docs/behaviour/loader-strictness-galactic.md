# Loader numeric strictness: galactic, economy, AI and story tags

Scope: how the remake loaders treat numeric XML values compared with the retail parser (legacy EAWR-4994). Mods use sentinels (`-1`, `0`, fractions) that the retail parser accepts; a loader that rejects them breaks a retail-compatible mod. Land and space-skirmish tags are classified in their own notes.

## Retail parse rules

| ID | Parser type (registry `value_type`) | Retail behaviour |
|---|---|---|
| LSA-P1 | signed integer | Leading whitespace, optional sign, leading digits; a fraction truncates toward zero; text parses to 0; 32-bit, no range or sign check. |
| LSA-P2 | unsigned integer | As LSA-P1, reinterpreted unsigned: a negative wraps to a large value. |
| LSA-P3 | float | Decimal float parse; no range or sign check. |
| LSA-P4 | unit float | As LSA-P3, then a warning and a clamp to 0..1. |
| LSA-P5 | all | An empty value is skipped and the field keeps its constructor default; an unknown tag is a warning. |

Status column: `a` matches retail, `b` the remake rejects a value retail accepts (to fix), `c` the remake accepts a value retail clamps or reinterprets (truncate, unsigned wrap), `d` unknown (needs a retail capture or a walk).

## Galactic-family rules

| Rule | Tags | Remake rejects | Type | Status | Test |
|---|---|---|---|---|---|
| LSA-01 | Max_Space_Base, Special_Structures_Land/Space, Additional_Population_Capacity, Population_Value, Planet_Capture_Bonus_Reward, Zoomed_Terrain_Index (planets) | fractional value -> bad_value; refused at campaign load when the planet is bound (live_setup.cpp:530) | signed/unsigned integer | b | pending |
| LSA-02 | Displayed_Tech_Level_Adjustment, Minimum_Visible_Base_Level, Base_Population_Capacity (factions) | fractional -> bad_value / refuse | signed integer | b | pending |
| LSA-03 | Sort_Order, Min/Max_Custom_Tech_Level, AI/Human_Victory_Cycle_Limit, *_Victory_Credit_Target (campaigns) | fractional -> bad_value issue (non-fatal row) | signed/unsigned integer | b | pending |
| LSA-04 | Build_Advances_Tech_Level, Squadron_Capacity, Tech_Level, Required_Ground/Star_Base_Level, Base_Level, Additional_Population_Capacity, Build_Limit_*_Per_Player (objects) | fractional -> bad_value; fatal for BUILD_*/TECH_LEVEL/*_BASE_LEVEL etc. (live_setup.cpp:643) | signed/unsigned integer | b | pending |
| LSA-05 | Starting_Spawned_Units_Tech_N count | non-whole count or outside int32 -> bad_value, row dropped | string and integer pair | b | pending |
| LSA-06 | Build_Max_Instances_Per_Planet | fractional -> refuse "invalid per-planet production limit" | signed integer | b | pending |
| LSA-07 | Squadron_Capacity | negative or > uint32 -> fail (campaign load) | unsigned integer | b | pending |
| LSA-08 | Build_Advances_Tech_Level | negative or > int32 max -> fail | unsigned integer | b | pending |
| LSA-09 | Tech_Level, Required_Ground/Star_Base_Level, Base_Level | negative tech or required/base level outside 0..5 -> "invalid offer" refuses the production setup (retail parses any integer; tech compares signed, so a negative tech is always available; required levels compare unsigned, so a negative or > 5 requirement is never met and the unit is not buildable) | signed integer (tech), unsigned integer (required levels) | b | pending |
| LSA-10 | Hyperspace_Speed | Signed speed retained; nonpositive speed cannot depart (consumer safety clamp; exact negative-speed retail motion unverified) | float | a | galactic_type_contracts; galactic_movement_contracts |
| LSA-11 | Hyperspace_Speed_Factor (faction) | Signed faction factor retained; nonpositive factor cannot depart (consumer safety clamp) | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-12 | Corruption_Hyperspace_Bonus | Signed bonus retained; hop selection compares it with the ordinary multiplier, so a negative bonus supplies no boost | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-13 | Object_Max_Speed_Multiplier_Galactic | Zero and signed mode factors retained; nonpositive factors cannot depart (consumer safety clamp) | float | a | galactic_data_contracts; galactic_movement_contracts |
| LSA-14 | MaximumFleetMovementDistance | Signed range retained; a negative movement range supplies no implicit graph links; explicit routes remain; observer proximity compares its magnitude squared (WGUI-04) | float | a | galactic_data_contracts; galactic_campaign_setup_contracts |
| LSA-15 | Build_Cost_Credits | Signed atoi value retained, including -1; WGEP-18 makes a price below one unavailable at purchase admission; squadron packing retains the signed cost | signed integer | a | galactic_type_contracts; galactic_production_contracts |
| LSA-16 | Build_Time_Seconds | atoi then uint32 reinterpretation; fractions truncate, -1 becomes 4294967295 seconds; frame conversion covers the full source range | unsigned integer | a | galactic_type_contracts; galactic_production_contracts |
| LSA-17 | Production_Speed_Factor, Production_Speed_Mod_Base_Vs_Tech_0..4, Galactic_Build_Time_Multiplier (difficulty) | Signed factors retained; WGEP-18 makes negative effective duration unavailable; omitted difficulty multiplier still requires separate constructor evidence | float | a | galactic_retail_production_numbers_stock |
| LSA-18 | Corruption_Galactic_Production_Price/Time_Multipliers | Signed float tokens retained; WGEP-18 makes negative effective price/time unavailable; no sign rejection at campaign load | float sequence | a | galactic_retail_production_numbers_stock |
| LSA-19 | Political_Control (objects) | anything but 0 or 1 -> fail; retail takes any integer, sums it per planet and clamps to -1..1 | signed integer | b | pending |
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
| LSA-36 | Raid_Force_Max_Heros, Raid_Force_Max_Limited_Objects | negative, fractional or > uint32 -> "invalid raid caps" | signed integer | b | pending |
| LSA-37 | Space_Retreat_Allowed_Countdown_Seconds | negative or overflow -> fail | float | b | pending |
| LSA-38 | AutoResolveDisplayTime, Auto_Resolve_Tactical_Multiplier | negative -> fail; malformed text -> fail "malformed auto-resolve constant" | float | b | pending |
| LSA-39 | Space_Retreat_Attrition_Factor | none | unit float | c | pending |
| LSA-40 | Battle_Pending_Timeout_Seconds (multiplayer) | negative or non-numeric -> fail | signed integer | b | pending |
| LSA-41 | Default_Hero_Respawn_Time | none (any sign, CGS-12) | float | a | pending |
| LSA-42 | Any numeric tag read through leading_number / Record::number | text with no leading number, or magnitude outside Q24 -> bad_value (fatal when the tag is in the live_setup input lists) | all numeric | b | pending |
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
