# ai tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 41 | 41 |
| partial | 31 | 31 |
| todo | 9 | 9 |
| presentation-later | 0 | 2 |
| foc-ignores | 21 | 21 |
| deferred | 8 | 8 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 0 |
| multiplayer | 0 | 0 |
| **Total** | **110** | **112** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Difficulty_Adjustments/Easy | applied | whole class (src/script/foc/ai_data.cpp) | WSS-36, WTA-46, WTA-47 | — | src/script/foc/ai_data.cpp | The Easy lobby AI row reads this entry; the contrast multiplier and the goal-cycle sleep follow it. Health, shield and damage follow with the combat consumers.; basis: reviewed |
| Difficulty_Adjustments/Hard | applied | whole class (src/script/foc/ai_data.cpp) | WSS-36, WTA-46, WTA-47 | — | src/script/foc/ai_data.cpp | The Hard lobby AI row reads this entry; the contrast multiplier and the goal-cycle sleep follow it. Health, shield and damage follow with the combat consumers.; basis: reviewed |
| Difficulty_Adjustments/Normal | applied | whole class (src/script/foc/ai_service.cpp) | AB-24, BP-17 | — | src/script/foc/ai_service.cpp | basis: reviewed |
| GalacticFreeStoreScript | applied | whole class (src/script/foc/ai_data.cpp); whole class (src/script/foc/galactic_ai.cpp) | WGA-10; WGA-10 | — | src/script/foc/ai_data.cpp; src/script/foc/galactic_ai.cpp | The galactic free-store script name is stored with the player type and the galactic AI manager returns it for the player.; basis: reviewed |
| GoalProposalFunctionSets | applied | whole class (src/script/foc/ai_data.cpp) | none recorded | — | src/script/foc/ai_data.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| GoalSetExtensionSize | applied | whole class (src/script/foc/ai_goals.cpp) | GS-30 | — | src/script/foc/ai_goals.cpp | basis: reviewed |
| SpaceFreeStoreScript | applied | whole class (src/script/foc/tactical_ai.cpp) | AI-16 | — | src/script/foc/tactical_ai.cpp | basis: reviewed |
| Templates/Galactic | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Selected galactic lifecycle (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification | WGA-102, WGA-108; WGA-02, WGA-09, WGA-102 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-02/09/102 initialize the selected authored mode vector and active order, then runtime apply_policy installs engine/funding state and initializes the authored freestore. Complete installed opponent/provider qualification remains; lifecycle-to-live-runtime composition is no longer absent.; basis: reviewed |

## AITemplates

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Budget/Always | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Defensive | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Hero | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Information | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Infrastructure | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Interventions | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Offensive | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/StoryArc | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Priority | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Tactical_Budget_Allowance/Interventions | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-105, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-105/108 retain separate allowance priorities; runtime evaluate/apply_funding_writes apply them to funding. Full installed opponent/provider qualification and Land/Space template consumers remain outside this galactic slice.; basis: reviewed |
| */Trigger | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-102, WGA-103, WGA-109 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102/103/109 evaluate selected-player trigger equations, apply policy and retain activation order; live runtime services carry the result. Full installed opponent/provider qualification and Land/Space trigger lifecycles remain outside this galactic slice.; basis: reviewed |
| */Turn_Off/Goals/Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_Off/Goals/Goal_Type | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_Off/Plans/Name | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 2585 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; TCFA-15 stock scope survey: skirmish faction players select Test_Space without named-plan switches; galactic defaults and campaign intervention templates author them. WGA-107/108 and the cited live policy adapter implement galactic named-plan switching; Land/Space mode coverage remains separate.; basis: reviewed |
| */Turn_On/Goals/Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_On/Goals/Goal_Type | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_On/Plans/Goal_Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; basis: reviewed |
| */Turn_On/Plans/Name | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 2585 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; TCFA-15 stock scope survey: skirmish faction players select Test_Space without named-plan switches; galactic defaults and campaign intervention templates author them. WGA-107/108 and the cited live policy adapter implement galactic named-plan switching; Land/Space mode coverage remains separate.; basis: reviewed |

## Campaign

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Player_Control | applied | whole class (src/campaign/galactic_ai_runtime.cpp) | none recorded | — | src/campaign/galactic_ai_runtime.cpp | no behaviour rule names the tag; the galactic AI runtime selects each faction player type from it; basis: reviewed |

## CIN_SpaceUnit, Container, GenericHeroUnit, GroundBase, GroundCompany, GroundStructure, GroundVehicle, HeroUnit, LandBombingUnit, MOV_Cinematic, Props_Story, SecondaryStructure, SlaveCompany, SpaceUnit, SpecialStructure, Squadron, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autoresolve_Health | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp | Hero-team Container dispatch/application needs qualification; existing Squadron formation and craft-summed AI consumers do not prove this class applies the tag. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Lua_Script | partial | Container Darth_Vader_TIE_Fighter_Container, Container Hutt_VWing_Squadron_Container, Container TIE_Defender_Squadron_Container, Container Y_Wing_Squadron_Container (src/script/foc/tactical_ai.cpp); missing: other object scripts | SOS-19, SOS-40, SOS-42, SOS-44 | 2714 | src/script/foc/tactical_ai.cpp | Family: Tactical AI. Vader script attachments use the real live member count and authored leader ability gates; other container scripts remain outside the admitted slice. Other authored scripts/classes remain outside service_objects attachment admission (SOS-40/42/44); native substitutes do not prove arbitrary script support.; basis: reviewed |

## Difficulty_Adjustment

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_AI_Contrast_Multiplier | applied | whole class (src/script/foc/ai_selection.cpp) | AI-07, FH-30, PL-25, PL-30, WTA-15 | — | src/script/foc/ai_selection.cpp | basis: auto |
| Space_AI_Goal_Cycle_Sleep_Duration | applied | whole class (src/script/foc/ai_goals.cpp) | G-07, GS-05, WTA-03 | — | src/script/foc/ai_goals.cpp | basis: auto |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Basic_AI | applied | whole class (src/campaign/galactic_ai_runtime.cpp); whole class (src/skirmish/ai.cpp) | GCMP-AI-01; AI-11, WSS-35 | — | src/campaign/galactic_ai_runtime.cpp; src/skirmish/ai.cpp | Playable multiplayer GC AI selection is project policy, not retail parity; ambient controller absence is preserved.; basis: reviewed |
| Bribed_Color | presentation-later | application not recorded | — | — | — | Rendering multiplies bribed-object colour by faction tint; absent presentation support is not an AI decision gap.; TCFA-10; basis: reviewed |
| Easily_Bribed | applied | galactic fleet members (src/campaign/fleets.cpp); galactic fleet movement (src/sim/galactic/movement.cpp) | WGM-85; WGM-84, WGM-85 | — | src/campaign/fleets.cpp; src/sim/galactic/movement.cpp | Selects Cheap or Expensive bribe cost for the planets this faction owns on a corruption-benefiting mover's path (WGM-85). Movement admission only: a corruption-benefiting mover pays, before departure, the summed original-type cost of the flattened departing members for each intermediate path planet its owner holds, unless the mover holds that orbit; AI difficulty multiplier and the AI bribe request stay with &#35;3981.; basis: reviewed |
| Space_Skirmish_AI_Default_Forces | applied | whole class (src/skirmish/inputs.cpp) | none recorded | — | src/skirmish/inputs.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## FunctionSet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Goal | applied | whole class (src/script/foc/ai_data.cpp) | AI-06, AI-50, GS-01, GS-12, PL-01 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_BuildTaskReservationSeconds | todo | application not recorded | — | 1965 | — | The planet-build reservation timer is not read by the galactic AI.; basis: reviewed |
| AI_FogCellsPerThreatCell | applied | whole class (src/script/foc/ai_service.cpp) | G-07, PG-01, PG-02, RO-1, TCFA-07, WTA-04 | — | src/script/foc/ai_service.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into fog_cells_per_threat_cell; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceAreaThreatScaleFactor | applied | whole class (src/script/foc/ai_perception.cpp) | G-07, PG-05, TCFA-07 | — | src/script/foc/ai_perception.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into area_threat_scale; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceEvaluatorRegionSize | applied | whole class (src/script/foc/ai_taskforces.cpp) | AI-07, AI-52, DT-02, GS-10, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into region_size; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDecayStep | applied | whole class (src/script/foc/ai_taskforces.cpp) | DT-02, G-07, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_decay_step; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDistanceFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AI_SpaceThreatLookAheadTime | applied | whole class (src/script/foc/ai_perception.cpp) | A-06, G-07, PG-01, PG-02, TCFA-07, WTA-04 | — | src/script/foc/ai_perception.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_look_ahead; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatTurnRateFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AITechLevelProductionTimeWeight | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWar | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWarSpace | deferred | application not recorded | — | 652 | — | Verified space threat visibility input; remake always bypasses AI fog without reading this switch. Stock False matches, configurable True is missing.; TCFA-11; basis: reviewed |
| High_Threat_Reachability_Tolerance | partial | galactic execution-route queries (src/script/foc/galactic_ai_reach.cpp); immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: land hint-zone provider | FT-08, FT-09; FT-09 | 2494 | src/script/foc/galactic_ai_reach.cpp; src/script/foc/plan_bindings.cpp | Family: Tactical AI. FT-08/09 apply this tolerance in target_path_cost. GalacticFindTargetQueries now supplies live execution routes via galactic_ai_reach.cpp&#35;execution_path; the remaining missing provider is land hint-zone reachability. Space deliberately bypasses the service (FT-12).; TCFA-08; basis: reviewed |
| Low_Threat_Reachability_Tolerance | partial | galactic execution-route queries (src/script/foc/galactic_ai_reach.cpp); immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: land hint-zone provider | FT-08, FT-09; FT-09 | 2494 | src/script/foc/galactic_ai_reach.cpp; src/script/foc/plan_bindings.cpp | Family: Tactical AI. FT-08/09 apply this tolerance in target_path_cost. GalacticFindTargetQueries now supplies live execution routes via galactic_ai_reach.cpp&#35;execution_path; the remaining missing provider is land hint-zone reachability. Space deliberately bypasses the service (FT-12).; TCFA-08; basis: reviewed |
| MaxCombatAccuracyAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatDamageAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatSensorRangeAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaximumPoliticalControl | applied | whole class (src/campaign/ai_perception_content.cpp) | WGA-475 | — | src/campaign/ai_perception_content.cpp | basis: reviewed |
| MaxInfluenceTransitionAlignmentBonus | applied | galactic (src/campaign/live_setup.cpp) | CGS-16 | — | src/campaign/live_setup.cpp | Raw target-faction control transition time producer; CGS-16. |
| MaxInfluenceTransitionAlignmentPenalty | applied | galactic (src/campaign/live_setup.cpp) | CGS-16 | — | src/campaign/live_setup.cpp | Raw target-faction control transition time producer; CGS-16. |
| Medium_Threat_Reachability_Tolerance | partial | galactic execution-route queries (src/script/foc/galactic_ai_reach.cpp); immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: land hint-zone provider | FT-08, FT-09; FT-09 | 2494 | src/script/foc/galactic_ai_reach.cpp; src/script/foc/plan_bindings.cpp | Family: Tactical AI. FT-08/09 apply this tolerance in target_path_cost. GalacticFindTargetQueries now supplies live execution routes via galactic_ai_reach.cpp&#35;execution_path; the remaining missing provider is land hint-zone reachability. Space deliberately bypasses the service (FT-12).; TCFA-08; basis: reviewed |
| ShowUnitAIPlanAttachment | presentation-later | application not recorded | — | — | — | Diagnostic unit tooltip adds attached AI plan/movement text; not a simulation setting.; TCFA-13; basis: reviewed |
| ThreatExpansionDistance | deferred | application not recorded | — | 652 | — | Verified threat-aware space path expansion distance; remake lacks this pathfinding consumer.; TCFA-12; basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, Projectile, ScriptMarker, SecondaryStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | partial | UniqueUnit IG-2000 (src/script/foc/ai_taskforces.cpp); SpaceUnit Interdictor_Cruiser, UniqueUnit Millennium_Falcon, UniqueUnit Slave_I, UniqueUnit Sundered_Heart (src/script/foc/tactical_ai.cpp); SpaceUnit MC30_Frigate (src/sim/tactical/session_abilities.cpp); missing: other object scripts | WHE-67, WHE-68; SOS-11, SOS-15, SOS-16, SOS-17, SOS-40, SOS-42, SOS-44; FC-57, FC-58, FD-47, FS-29 | 652 | src/script/foc/ai_taskforces.cpp; src/script/foc/tactical_ai.cpp; src/sim/tactical/session_abilities.cpp | Family: Tactical AI. Falcon, its inherited space variants, Slave I, Sundered Heart and the Interdictor's missile shield attach their isolated object scripts; native IG-2000 and MC30 consumers remain separate from the bounded attachment slice. Other authored scripts/classes remain outside service_objects attachment admission (SOS-40/42/44); native substitutes do not prove arbitrary script support.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); loaded local space battle (src/platform/battle_scoring.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); whole class (src/units/unit_combat.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-46; WBF-45, WGP-178; WGM-106, WGS-91; A-06, G-03, PL-13 | — | src/campaign/auto_resolve_strength.cpp; src/platform/battle_scoring.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp; src/units/unit_combat.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## Goals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */AIGoalApplicationFlags | applied | whole class (src/script/foc/ai_data.cpp) | GS-11 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tactical_Bribe_Ability/Bribed_Effect | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/Bribed_Effect_Small | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 652 | — | Family: Tactical AI. TCFA-10: Marker/SecondaryStructure/SpecialStructure air-target filtering has no verified live consumer in the admitted space AI adapter; retain the gap pending per-class ground/space reader qualification.; basis: reviewed |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded local space battle (src/platform/battle_scoring.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); whole class (src/units/unit_combat.cpp) | WBF-46; WBF-45, WGP-178; WGM-106, WGS-91; A-06, G-03, PL-13 | — | src/platform/battle_scoring.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp; src/units/unit_combat.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); whole class (src/units/unit_combat.cpp) | WBF-45, WGP-178; A-06, G-03, PL-13 | — | src/platform/galactic/campaign_scoring.cpp; src/units/unit_combat.cpp | &#35;850 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Projectile power supplies the relative shares of a ship's weapon hardpoints (A-06, AT-09).; CHECK-842; basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); whole class (src/skirmish/ai.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91; PG-02, PG-06 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp; src/skirmish/ai.cpp | Promoted live pads supply their authored optional power to the existing AI type and perception interfaces; capture plans remain G9. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | whole class (src/script/foc/tactical_ai_world.hpp) | GS-10, WHZ-51 | — | src/script/foc/tactical_ai_world.hpp | Live pad and construction types use the existing goal-target interface; neutral capture points do not match enemy goals, and capture plans remain G9.; basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | ship (src/script/foc/tactical_ai_world.hpp) | GS-10, WHZ-50, WHZ-51 | — | src/script/foc/tactical_ai_world.hpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Air_Vehicle_Turret_Target | deferred | application not recorded | — | 652 | — | Authored on craft also used for land bombing; exact parser/reader and space applicability remain unverified.; TCFA-10; basis: reviewed |

## SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | whole class (src/skirmish/ai.cpp) | G-03, GS-02, GS-10, GS-12, WTA-08 | — | src/skirmish/ai.cpp | basis: auto |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Has_Space_Evaluator | todo | application not recorded | — | 650 | — | Family: Tactical AI. WHE-07/49 retain TransportUnit as metadata for carried heroes/unselected transports; a live transport-space evaluator consumer is not established. Existing ship adapters do not prove this class is applied.; basis: reviewed |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
