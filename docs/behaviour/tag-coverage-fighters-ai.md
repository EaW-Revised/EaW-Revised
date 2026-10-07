# Fighter and AI tag coverage decisions

This audit reconciles the original 25 fighter and 43 AI class/tag pairs with
the current consumers and the debug build. These are registry decisions,
not new simulation behaviour. The source data is the effective FoC XML;
binary lookups and authoring extracts remain in ignored research output.

## Evidence and scope

| Rule | Finding and registry decision | Evidence |
|---|---|---|
| TCFA-01 | Bombing-run targeting blobs, land-bomber selection, bombing cooldowns and their announcements belong to land support. Landing-zone announcements belong to land reinforcement control. Mark these `land-or-galactic`, including faction sound tags; loading a faction in a space battle does not make every faction field a space input. | Effective faction and game-constant XML; land support LC-58/59; M2 space scope. |
| TCFA-02 | `Fighter_Icon_Name` supplies a faction icon in galactic production UI. `Squadron_Icon_Name` is authored as a faction graphic, but this audit does not establish its reader. Both remain `presentation-later`: faction icon presentation is absent here, and the latter's exact consumer needs verification. | Debug build: galactic production-panel icon selection; effective faction XML. |
| TCFA-03 | `Squadron_Capacity` supplies galactic fleet packing, not tactical garrison capacity. `Indigenous_Spawn_Destruction_Reward` supplies the credit reward of a generated indigenous-spawner destruction story event. Mark both `land-or-galactic`. | WGM-24/87; debug build: story-event generation reads the reward constant; effective XML. |
| TCFA-04 | Container formation offsets and tolerance remain `deferred` for hero-team support. The loader changes a Container hinted as a squadron to ship kind, then calls body loading, whereas offsets and tolerance are read in squadron loading. A Squadron consumer is not evidence that a Container consumer exists. Container `AI_Combat_Power` likewise lacks a qualified tactical team consumer; the independent scoring read does not prove tactical AI power application. | WSQ formation rules; `unit_tables_decode.cpp` kind selection and dispatch, `unit_tables_profiles.cpp` squadron/body loaders, `unit_motion.cpp` profile selection; effective hero Container XML. |
| TCFA-05 | `Is_Escort` is read by debug-build combatant classification, including squadron and company combatants. Defer the missing classification with fighter coverage; this flag is not evidence for changing the tactical guard command or inferring an escort movement policy. Its complete mode boundary remains unverified. | Debug build: three combatant classification callers of the type flag; effective unit and squadron XML. |
| TCFA-06 | Squadron `Max_Squad_Size` remains `partial`: results scoring uses it, but team creation is separate. Container `Lua_Script` remains `deferred`: the authored hero-team object script needs general object-script lifecycle support; recognizing one other script by name is not that support. | WBF-46; effective Container XML; current unit loader and object-script dispatch. |
| TCFA-07 | The five AI constants below already reach tactical consumers. Retain `applied`, qualify the rows as reviewed, and name both the loader and consumer. Static tracing establishes those reads; it does not create load-time instrumentation. | `ai_data.cpp` constant reads; `ai_service.cpp`, `ai_perception.cpp`, `ai_taskforces.cpp`; PG-01/02/05, GS-10 and DT-02. |
| TCFA-08 | `AI_BuildTaskReservationSeconds` initializes a planet-build reservation timer. Low/medium/high threat reachability tolerances are read by planet reachability and by the land-only branch of zone reachability. Mark these `land-or-galactic`; do not transplant their thresholds into space target selection. | Debug build: producer reservation and both reachability consumers; effective constants 15 seconds and 1000/3000/5000. |
| TCFA-09 | Faction `Force_Alignment`, credit-income alignment maxima and influence-transition maxima belong to planet alignment/control interfaces. Mark these `land-or-galactic`. The three combat-alignment maxima remain `deferred`: authored values are established, but their parsing and live combat consumers are unverified here. No tactical combat bonus is inferred from their names. | Debug build: type/faction alignment and planet income consumer; WGEP-07, WGM-72; effective XML. |
| TCFA-10 | `Easily_Bribed` selects galactic fleet admission cost. `Bribed_Color` tints a bribed object's rendering, so it is `presentation-later`, not an AI decision input. `Air_Vehicle_Turret_Target` remains `deferred`: it is authored on craft also used for land bombing, but its exact reader and space applicability remain unverified. | WGM-85; debug build: bribed-object colour multiplication; effective XML. |
| TCFA-11 | `AIUsesFogOfWarSpace` changes whether a nonhuman player's threat query bypasses fog. The remake currently bypasses fog for every AI player without reading this setting: retain an explicit `deferred` configuration gap, even though stock XML authors False. | Debug build: mode-specific threat visibility check; SK-45; `ai_perception.cpp#fogged_for`; effective XML. |
| TCFA-12 | `ThreatExpansionDistance` changes forward expansion when space pathfinding considers threat. Defer until threat-aware pathfinding is implemented. The distance/turn-rate threat factors remain `deferred`: the verified threat-zone construction uses range, look-ahead, minimum radius and cap, not these authored factors; their other readers or parser absence remain unverified. | Debug build: space path configuration and threat-zone construction; effective constants 600, 0.5 and 4.0. |
| TCFA-13 | `ShowUnitAIPlanAttachment` adds diagnostic AI plan/movement text to the unit tooltip. Mark it `presentation-later`; it is not a simulation control. | Debug build: tooltip population; stock False in effective XML. |
| TCFA-14 | Preserve existing `foc-ignores` decisions for `Is_Bomber`, `Number_per_Squadron`, `Autoresolve_Health` and `AITechLevelProductionTimeWeight`. Their DB-NOTAG evidence is a whole-image parser/string/template scan, not an inference from this audit's missing consumers. | Registry DB-NOTAG evidence; effective XML contains these authored names. |

## Static AI loader trace

| Class/tag | Loader field in `src/script/foc/ai_data.cpp` | Application target |
|---|---|---|
| GameConstants/AI_FogCellsPerThreatCell | `fog_cells_per_threat_cell` | `src/script/foc/ai_service.cpp#fog_cells_per_threat_cell` |
| GameConstants/AI_SpaceAreaThreatScaleFactor | `area_threat_scale` | `src/script/foc/ai_perception.cpp#area_threat_scale` |
| GameConstants/AI_SpaceEvaluatorRegionSize | `region_size` | `src/script/foc/ai_taskforces.cpp#region_size` |
| GameConstants/AI_SpaceThreatDecayStep | `threat_decay_step` | `src/script/foc/ai_taskforces.cpp#threat_decay_step` |
| GameConstants/AI_SpaceThreatLookAheadTime | `threat_look_ahead` | `src/script/foc/ai_perception.cpp#threat_look_ahead` |
| Faction/Basic_AI | `src/skirmish/inputs.cpp` faction controller name | `src/skirmish/ai.cpp#ai_setup` (WSS-35) |

There are **five** already applied `AI_Space*`/`AI_Fog*` constants in the original
ticket; faction `Basic_AI` and two space-evaluator pairs are also already applied.
The loader additionally reads `DesiredSpaceFOWCellSize`,
`AI_SpaceThreatRangeCap` and `Health_Low_Percent_Threshold`, outside the original
pair list. Player, template, goal, function-set, difficulty and equation files
also use the AI-specific reader. That reader retains no node source locations.
`sim_headless --skirmish m2 --tag-trace-out` stops its recording after start
construction and does not initialize the AI service. Instrumenting the AI
reader alone therefore cannot repair that command's missing AI load trace;
both the reader and the scene entry point need a follow-up. Existing script
consumer rows remain valid application evidence without this runtime trace.

## Boundaries left open

The registry retains tickets and precise reasons for hero Container formations,
team power and scripts, escort classification, authoritative team-size clamping,
configurable AI fog, threat-aware space pathfinding, and the unverified threat
factors, combat-alignment maxima and air-turret flag. A `deferred` status is a
record of work left, not a claim that the tag is unused by FoC or unnecessary
for eventual fidelity. Existing Squadron formation application, faction AI
selection and SecondaryStructure/SpaceBuildable evaluator application remain
unchanged. The rows on other classes outside the original pair list are not
bulk reclassified by this audit.

## Original pair reconciliation

Every original pair is listed below; this is not the larger universe of tags
subsequently assigned to the same coverage tickets. Application targets remain
in the registry. The existing ignored and scoped rows retain their earlier
evidence; only the reviewed decisions above change here.

### Fighters

| Class/tag | Status | Evidence |
|---|---|---|
| `Container/Squadron_Formation_Error_Tolerance` | deferred | TCFA-04 |
| `Container/Squadron_Offsets` | deferred | TCFA-04 |
| `Faction/Bombing_Run_Blob_Size` | land-or-galactic | TCFA-01 |
| `Faction/Bombing_Run_Shadow_Blob_Material_Name` | land-or-galactic | TCFA-01 |
| `Faction/Fighter_Icon_Name` | presentation-later | TCFA-02 |
| `Faction/SFXEvent_Bombing_Run_Ally_Available` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_Bombing_Run_Available` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_Bombing_Run_Begin_Crosstalk` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_Bombing_Run_Cancelled` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_HUD_Landing_Zone_Captured` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_HUD_Landing_Zone_Lost` | land-or-galactic | TCFA-01 |
| `Faction/SFXEvent_HUD_Last_Landing_Zone_Lost` | land-or-galactic | TCFA-01 |
| `Faction/Skirmish_Land_Bomber` | land-or-galactic | SCOPE-LAND |
| `Faction/Squadron_Icon_Name` | presentation-later | TCFA-02 |
| `GameConstants/Indigenous_Spawn_Destruction_Reward` | land-or-galactic | TCFA-03 |
| `GameConstants/Max_Bombing_Run_Interval_Seconds` | land-or-galactic | TCFA-01 |
| `GameConstants/Min_Bombing_Run_Interval_Seconds` | land-or-galactic | TCFA-01 |
| `SpaceUnit/Is_Bomber` | foc-ignores | DB-NOTAG |
| `SpaceUnit/Is_Escort` | deferred | TCFA-05 |
| `SpaceUnit/Land_Bomber_Type` | land-or-galactic | SCOPE-LAND |
| `SpaceUnit/Number_per_Squadron` | foc-ignores | DB-NOTAG |
| `SpaceUnit/Squadron_Capacity` | land-or-galactic | TCFA-03 |
| `Squadron/Is_Bomber` | foc-ignores | DB-NOTAG |
| `Squadron/Is_Escort` | deferred | TCFA-05 |
| `Squadron/Max_Squad_Size` | partial | TCFA-06 |

### AI

| Class/tag | Status | Evidence |
|---|---|---|
| `Container/AI_Combat_Power` | deferred | TCFA-04 |
| `Container/Autoresolve_Health` | foc-ignores | DB-NOTAG |
| `Container/Lua_Script` | deferred | TCFA-06 |
| `Faction/Basic_AI` | applied | WSS-35, AI-11 |
| `Faction/Bribed_Color` | presentation-later | TCFA-10 |
| `Faction/Easily_Bribed` | land-or-galactic | TCFA-10 |
| `Faction/Force_Alignment` | land-or-galactic | TCFA-09 |
| `Faction/Land_Skirmish_AI_Default_Forces` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_BuildTaskReservationSeconds` | land-or-galactic | TCFA-08 |
| `GameConstants/AI_FogCellsPerThreatCell` | applied | TCFA-07 |
| `GameConstants/AI_LandAreaThreatScaleFactor` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_LandEvaluatorRegionSize` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_LandThreatDistanceFactor` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_LandThreatLookAheadTime` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_LandThreatTurnRateFactor` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AI_SpaceAreaThreatScaleFactor` | applied | TCFA-07 |
| `GameConstants/AI_SpaceEvaluatorRegionSize` | applied | TCFA-07 |
| `GameConstants/AI_SpaceThreatDecayStep` | applied | TCFA-07 |
| `GameConstants/AI_SpaceThreatDistanceFactor` | deferred | TCFA-12 |
| `GameConstants/AI_SpaceThreatLookAheadTime` | applied | TCFA-07 |
| `GameConstants/AI_SpaceThreatTurnRateFactor` | deferred | TCFA-12 |
| `GameConstants/AITechLevelProductionTimeWeight` | foc-ignores | DB-NOTAG |
| `GameConstants/AIUsesFogOfWarGalactic` | land-or-galactic | SCOPE-GALACTIC |
| `GameConstants/AIUsesFogOfWarLand` | land-or-galactic | SCOPE-LAND |
| `GameConstants/AIUsesFogOfWarSpace` | deferred | TCFA-11 |
| `GameConstants/High_Threat_Reachability_Tolerance` | land-or-galactic | TCFA-08 |
| `GameConstants/Low_Threat_Reachability_Tolerance` | land-or-galactic | TCFA-08 |
| `GameConstants/MaxCombatAccuracyAlignmentBonus` | deferred | TCFA-09 |
| `GameConstants/MaxCombatDamageAlignmentBonus` | deferred | TCFA-09 |
| `GameConstants/MaxCombatSensorRangeAlignmentBonus` | deferred | TCFA-09 |
| `GameConstants/MaxCreditIncomeAlignmentBonus` | land-or-galactic | TCFA-09 |
| `GameConstants/MaxCreditIncomeAlignmentPenalty` | land-or-galactic | TCFA-09 |
| `GameConstants/MaxInfluenceTransitionAlignmentBonus` | land-or-galactic | TCFA-09 |
| `GameConstants/MaxInfluenceTransitionAlignmentPenalty` | land-or-galactic | TCFA-09 |
| `GameConstants/Medium_Threat_Reachability_Tolerance` | land-or-galactic | TCFA-08 |
| `GameConstants/ShowUnitAIPlanAttachment` | presentation-later | TCFA-13 |
| `GameConstants/ThreatExpansionDistance` | deferred | TCFA-12 |
| `SecondaryStructure/Has_Space_Evaluator` | applied | GS-10, WHZ-51 |
| `SpaceBuildable/Has_Space_Evaluator` | applied | GS-10, WHZ-51 |
| `SpaceUnit/Air_Vehicle_Turret_Target` | deferred | TCFA-10 |
| `SpaceUnit/Autoresolve_Health` | foc-ignores | DB-NOTAG |
| `Squadron/Autoresolve_Health` | foc-ignores | DB-NOTAG |
| `StarBase/Autoresolve_Health` | foc-ignores | DB-NOTAG |
