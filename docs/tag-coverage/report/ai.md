# ai tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 66 | 66 |
| partial | 83 | 83 |
| todo | 279 | 279 |
| presentation-later | 0 | 2 |
| foc-ignores | 23 | 23 |
| deferred | 7 | 7 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 0 |
| multiplayer | 0 | 0 |
| **Total** | **458** | **460** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIGoalProposalFunction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Goal | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## AIGoalType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AIGoalApplicationFlags | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

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
| */Budget/High_Priority | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Information | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Infrastructure | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Interventions | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Macro | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/MajorItem | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Map_Control | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Med_Priority | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Micro | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/NoBudget | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Offensive | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/Sandbox_Events | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/StoryArc | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-10, WGA-104, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-10/104/108 policy equations reach the live funding ledger through galactic_ai_runtime.cpp&#35;apply_policy and apply_funding_writes. Full installed opponent/provider qualification and Land/Space template consumers remain outside the reviewed galactic slice.; basis: reviewed |
| */Budget/StoryArcHuman | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Tactical_Targeted | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Tactical_Untargeted | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Budget/Tactics | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Priority | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Tactical_Budget_Allowance/Defensive | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Tactical_Budget_Allowance/Interventions | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-105, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-105/108 retain separate allowance priorities; runtime evaluate/apply_funding_writes apply them to funding. Full installed opponent/provider qualification and Land/Space template consumers remain outside this galactic slice.; basis: reviewed |
| */Tactical_Budget_Allowance/Offensive | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Tactical_Budget_Allowance/StoryArcHuman | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Trigger | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-102, WGA-103, WGA-109 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102/103/109 evaluate selected-player trigger equations, apply policy and retain activation order; live runtime services carry the result. Full installed opponent/provider qualification and Land/Space trigger lifecycles remain outside this galactic slice.; basis: reviewed |
| */Turn_Off/Goals/Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_Off/Goals/Goal_Type | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_Off/Plans/Goal_Category | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| */Turn_Off/Plans/Name | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 2585 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; TCFA-15 stock scope survey: skirmish faction players select Test_Space without named-plan switches; galactic defaults and campaign intervention templates author them. WGA-107/108 and the cited live policy adapter implement galactic named-plan switching; Land/Space mode coverage remains separate.; basis: reviewed |
| */Turn_On/Goals/Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_On/Goals/Goal_Type | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-102..108 apply priority-resolved galactic policy and reversal through live runtime apply_policy to the selected engine/funding owner. Full installed opponent/provider qualification and Land/Space template consumers remain outside this slice.; basis: reviewed |
| */Turn_On/Plans/Goal_Category | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 4532 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; basis: reviewed |
| */Turn_On/Plans/Name | partial | live selected galactic policy adapter (src/campaign/galactic_ai_runtime.cpp); Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: complete installed galactic opponent/provider qualification, Land, Space | WGA-108; WGA-106, WGA-107, WGA-108 | 2585 | src/campaign/galactic_ai_runtime.cpp; src/script/foc/galactic_ai_templates.cpp | Family: Galactic AI. WGA-107/108 resolve category/type/name selectors against existing plan definitions; live apply_policy reaches Engine::apply_galactic_policy. Land/Space consumers and complete installed opponent/provider qualification remain. Named-plan policy is implemented here; the old no-named-plan-consumer claim is stale.; TCFA-15 stock scope survey: skirmish faction players select Test_Space without named-plan switches; galactic defaults and campaign intervention templates author them. WGA-107/108 and the cited live policy adapter implement galactic named-plan switching; Land/Space mode coverage remains separate.; basis: reviewed |
| */Un_Trigger | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Assistance_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Campaign

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Player_Control | applied | whole class (src/campaign/mp_lobby_catalog.cpp) | none recorded | — | src/campaign/mp_lobby_catalog.cpp | no behaviour rule names the tag; ai_controller_choice selects each faction's galactic AI player type from it (single-player rules); basis: reviewed |
| AI_Victory_Faction_Names | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| AI_Victory_Galactic_Control | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| AI_Victory_Leader_Names | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## CIN_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## CIN_SpaceUnit, Container, GenericHeroUnit, GroundBase, GroundCompany, GroundStructure, GroundVehicle, HeroUnit, LandBombingUnit, MOV_Cinematic, Props_Story, SecondaryStructure, SlaveCompany, SpaceUnit, SpecialStructure, Squadron, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autoresolve_Health | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp | Hero-team Container dispatch/application needs qualification; existing Squadron formation and craft-summed AI consumers do not prove this class applies the tag. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Lua_Script | partial | Container Darth_Vader_TIE_Fighter_Container, Container Hutt_VWing_Squadron_Container, Container TIE_Defender_Squadron_Container, Container Y_Wing_Squadron_Container (src/script/foc/tactical_ai.cpp); missing: other object scripts | SOS-19, SOS-40, SOS-42, SOS-44 | 2714 | src/script/foc/tactical_ai.cpp | Family: Tactical AI. Vader script attachments use the real live member count and authored leader ability gates; other container scripts remain outside the admitted slice. Other authored scripts/classes remain outside service_objects attachment admission (SOS-40/42/44); native substitutes do not prove arbitrary script support.; basis: reviewed |

## Corvette

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Debug_Objects

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| DebugUnit/AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Difficulty_Adjustment

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_AI_Contrast_Multiplier | applied | whole class (src/script/foc/ai_selection.cpp); whole class (src/script/foc/ai_selection.cpp) | AI-07, FH-30, PL-25, PL-30, WTA-15; AI-07, FH-30, PL-25, PL-30, WTA-15 | — | src/script/foc/ai_selection.cpp | basis: auto |
| Space_AI_Goal_Cycle_Sleep_Duration | applied | whole class (src/script/foc/ai_goals.cpp) | G-07, GS-05, WTA-03 | — | src/script/foc/ai_goals.cpp | basis: auto |

## Dummy

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## DummyStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Basic_AI | applied | whole class (src/campaign/galactic_ai_runtime.cpp); whole class (src/skirmish/ai.cpp) | GCMP-AI-01; AI-11, WSS-35 | — | src/campaign/galactic_ai_runtime.cpp; src/skirmish/ai.cpp | Playable multiplayer GC AI selection is project policy, not retail parity; ambient controller absence is preserved.; basis: reviewed |
| Bribed_Color | presentation-later | application not recorded | — | — | — | Rendering multiplies bribed-object colour by faction tint; absent presentation support is not an AI decision gap.; TCFA-10; basis: reviewed |
| Easily_Bribed | applied | galactic fleet members (src/campaign/fleets.cpp); galactic fleet movement (src/sim/galactic/movement.cpp) | WGM-85; WGM-84, WGM-85 | — | src/campaign/fleets.cpp; src/sim/galactic/movement.cpp | Selects Cheap or Expensive bribe cost for the planets this faction owns on a corruption-benefiting mover's path (WGM-85). Movement admission only: a corruption-benefiting mover pays, before departure, the summed original-type cost of the flattened departing members for each intermediate path planet its owner holds, unless the mover holds that orbit; AI difficulty multiplier and the AI bribe request stay with &#35;3981.; basis: reviewed |
| Space_Skirmish_AI_Default_Forces | applied | whole class (src/skirmish/inputs.cpp) | none recorded | — | src/skirmish/inputs.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## FlagshipTemplate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |

## FlagshipUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Prerequisites | partial | space skirmish purchase eligibility (include/eawr/sim/tactical/economy.hpp); resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | WPR-33; SG-CLS-01, WPR-33, WPR-79 | 4532 | include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## Freighter

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## FunctionSet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Goal | applied | whole class (src/script/foc/ai_data.cpp) | AI-06, AI-50, GS-01, GS-12, PL-01 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## Galactic_Markup

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Planets/*/Allows_Asuran_Capital_Shipyard | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Planets/*/Allows_G_Capital_Shipyard | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Planets/*/Allows_Syslord_Capital_Shipyard | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Planets/*/Allows_T_Capital_Shipyard | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Planets/*/Allows_W_Capital_Shipyard | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Planets/*/AllowsMining | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_BuildTaskReservationSeconds | todo | application not recorded | — | 1965 | — | The planet-build reservation timer is not read by the galactic AI.; basis: reviewed |
| AI_FogCellsPerThreatCell | applied | whole class (src/script/foc/ai_service.cpp) | G-07, PG-01, PG-02, RO-1, TCFA-07, WTA-04 | — | src/script/foc/ai_service.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into fog_cells_per_threat_cell; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_LandThreatRangeCap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| AI_SpaceAreaThreatScaleFactor | applied | whole class (src/script/foc/ai_perception.cpp) | G-07, PG-05, TCFA-07 | — | src/script/foc/ai_perception.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into area_threat_scale; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceEvaluatorRegionSize | applied | whole class (src/script/foc/ai_taskforces.cpp) | AI-07, AI-52, DT-02, GS-10, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into region_size; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDecayStep | applied | whole class (src/script/foc/ai_taskforces.cpp) | DT-02, G-07, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_decay_step; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDistanceFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AI_SpaceThreatLookAheadTime | applied | whole class (src/script/foc/ai_perception.cpp) | A-06, G-07, PG-01, PG-02, TCFA-07, WTA-04 | — | src/script/foc/ai_perception.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_look_ahead; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatRangeCap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| AI_SpaceThreatTurnRateFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AI_Uses_Galactic_Pop_Cap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| AITechLevelProductionTimeWeight | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWar | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWarSpace | applied | unforced space AI object visibility and threat queries (src/script/foc/ai_perception.cpp) | TCFA-11 | — | src/script/foc/ai_perception.cpp | Authored retail boolean is loaded by ai_data.cpp; stock False bypasses object fog, True uses the observer mask. Forced targeting and real cell-fog age are unchanged; land/galactic family qualification is separate.; TCFA-11; basis: reviewed |
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
| Space_Tactical_Unit_Cap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| ThreatExpansionDistance | deferred | application not recorded | — | 652 | — | Verified threat-aware space path expansion distance; remake lacks this pathfinding consumer.; TCFA-12; basis: reviewed |

## GameObjectType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Corruption_2_Success_Planet_Specific_Tactical_Upgrade | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Corruption_3_Success_Planet_Specific_Tactical_Upgrade | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Generic_Structure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| HQ_Win_Condition_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |

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

## Ground_Structure_Goauld

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Ground_Structure_Neutral

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Ground_Structure_Other

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Land_Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Space_Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Ground_Structure_Tauri

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Ground_Structure_Unused

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| HQ_Win_Condition_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Ground_Structure_Wraith

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## GroundBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Sell_Percentage | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Always_Side_With_Invader | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Percentage | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Attachment_Bone_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## GroundWarCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## GroundWarTemplate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Hero_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tactical_Bribe_Ability/Bribed_Effect | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/Bribed_Effect_Small | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## HomeWorldObjects

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HomeWorldObject/Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| HomeWorldObject/Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Indigenous_Passive

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Always_Side_With_Invader | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 652 | — | Family: Tactical AI. TCFA-10: Marker/SecondaryStructure/SpecialStructure air-target filtering has no verified live consumer in the admitted space AI adapter; retain the gap pending per-class ground/space reader qualification.; basis: reviewed |

## MarkerReinforcement

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## MenuObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## MilitiaMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Lua_Script | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Mission_Object

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded local space battle (src/platform/battle_scoring.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); whole class (src/units/unit_combat.cpp) | WBF-46; WBF-45, WGP-178; WGM-106, WGS-91; A-06, G-03, PL-13 | — | src/platform/battle_scoring.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp; src/units/unit_combat.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Corruption_2_Tactical_Game_Mode | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Corruption_2_Tactical_Map_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Corruption_3_Tactical_Game_Mode | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Corruption_3_Tactical_Map_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Destroyed_Land_Tactical_Map | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); whole class (src/units/unit_combat.cpp) | WBF-45, WGP-178; A-06, G-03, PL-13 | — | src/platform/galactic/campaign_scoring.cpp; src/units/unit_combat.cpp | &#35;850 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Projectile power supplies the relative shares of a ship's weapon hardpoints (A-06, AT-09).; CHECK-842; basis: reviewed |

## Prop_Forest

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Hoth

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Industrial

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Misc

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Plant

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Model_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Prop_Terrain

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Townbuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Tree

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Model_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Prop_Vehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Prop_Wildlife

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_Base

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_BBS

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_Fern

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Model_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_Industrial

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_PortedAotR

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Model_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_Stargate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Tactical_Unit_Cap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Props_Wave

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Space_Tactical_Unit_Cap | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | partial | eligible loaded non-pad, non-under-construction space producers (src/skirmish/economy.cpp); resolved loaded faction-grouped space producer menus (src/units/unit_tables_profiles.cpp); missing: objects outside the loaded space production closure, pad-specific, land and galactic consumers outside this ordinary space producer audit | WBP-33, WPR-33; WBP-06, WPR-33 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | The shared decoder reads the authored multiplayer faction groups, and the space economy adapter creates ordinary producer menus without a type-label gate (WPR-33/WBP-33). A mod label alone neither loads a map-only producer nor admits an unsupported purchase.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); whole class (src/skirmish/ai.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91; PG-02, PG-06 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp; src/skirmish/ai.cpp | Promoted live pads supply their authored optional power to the existing AI type and perception interfaces; capture plans remain G9. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | whole class (src/script/foc/tactical_ai_world.hpp) | GS-10, WHZ-51 | — | src/script/foc/tactical_ai_world.hpp | Live pad and construction types use the existing goal-target interface; neutral capture points do not match enemy goals, and capture plans remain G9.; basis: reviewed |

## SkirmishSpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Prerequisites | partial | space skirmish purchase eligibility (include/eawr/sim/tactical/economy.hpp); resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | WPR-33; SG-CLS-01, WPR-33, WPR-79 | 4532 | include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## SkirmishSquadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Prerequisites | partial | space skirmish purchase eligibility (include/eawr/sim/tactical/economy.hpp); resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | WPR-33; SG-CLS-01, WPR-33, WPR-79 | 4532 | include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | resolved space skirmish purchase menu entries (src/skirmish/economy.cpp); missing: objects outside a loaded space-skirmish production closure | SG-CLS-01, WPR-33, WPR-79 | 4532 | src/skirmish/economy.cpp | Mod element labels resolve from their variant chain or effective tags (SG-CLS-01); the shared menu adapter applies the resolved purchase data without an element-name gate (WPR-79).; basis: reviewed |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SkirmishUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Space_Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Space_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## SpaceFauna

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SpaceHero

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | ship (src/script/foc/tactical_ai_world.hpp) | GS-10, WHZ-50, WHZ-51 | — | src/script/foc/tactical_ai_world.hpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

## SpaceTurret

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Air_Vehicle_Turret_Target | deferred | application not recorded | — | 652 | — | Authored on craft also used for land bombing; exact parser/reader and space applicability remain unverified.; TCFA-10; basis: reviewed |
| Tactical_Buildable_Objects_Multiplayer | partial | eligible loaded non-pad, non-under-construction space producers (src/skirmish/economy.cpp); resolved loaded faction-grouped space producer menus (src/units/unit_tables_profiles.cpp); missing: objects outside the loaded space production closure, pad-specific, land and galactic consumers outside this ordinary space producer audit | WBP-33, WPR-33; WBP-06, WPR-33 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | The shared decoder reads the authored multiplayer faction groups, and the space economy adapter creates ordinary producer menus without a type-label gate (WPR-33/WBP-33). A mod label alone neither loads a map-only producer nor admits an unsupported purchase.; basis: reviewed |
| TacticalBehavior | foc-ignores | application not recorded | — | — | — | TCFA-17: debug-build tag-table and whole-image string lookup found no parser or consumer for this authored name; preserve the existing ignored-tag disposition.; DB-NOTAG |

## SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | whole class (src/skirmish/ai.cpp) | G-03, GS-02, GS-10, GS-12, WTA-08 | — | src/skirmish/ai.cpp | basis: auto |

## SpawnBuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Special_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Attachment_Bone_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Percentage | applied | tactical pad sale refund (src/sim/tactical/session_economy.cpp); loaded TACTICAL_SELL objects (src/units/unit_tables_decode.cpp) | WBP-31; WBP-31 | — | src/sim/tactical/session_economy.cpp; src/units/unit_tables_decode.cpp | WBP-31: the sale percentage of a loaded TACTICAL_SELL object reaches the pad sale table and the sale refund; the synthetic class fixture covers inherited and overridden values.; basis: reviewed |
| Tactically_Built_Child_Object_Persists | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## SpecialStructures

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HomeWorldObject/AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| OrbitalStructure/AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| OrbitalStructure/Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| StagStructure/AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## Sundered_Heart

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |

## SurvivalSpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## Tactical_Capturable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Tactical_Structure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Template

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |
| Corruption_1_Success_Planet_Specific_Tactical_Upgrade | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Corruption_1_Tactical_Game_Mode | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Corruption_1_Tactical_Map_Name | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Describe_Tactical | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Destroyed_Space_Tactical_Map | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| HQ_Win_Condition_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Land_Tactical_Map | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Space_Tactical_Map | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | partial | eligible loaded non-pad, non-under-construction space producers (src/skirmish/economy.cpp); resolved loaded faction-grouped space producer menus (src/units/unit_tables_profiles.cpp); missing: objects outside the loaded space production closure, pad-specific, land and galactic consumers outside this ordinary space producer audit | WBP-33, WPR-33; WBP-06, WPR-33 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | The shared decoder reads the authored multiplayer faction groups, and the space economy adapter creates ordinary producer menus without a type-label gate (WPR-33/WBP-33). A mod label alone neither loads a map-only producer nor admits an unsupported purchase.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |
| Tactical_Sell_Credits | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| TacticalBehavior | foc-ignores | application not recorded | — | — | — | TCFA-17: debug-build tag-table and whole-image string lookup found no parser or consumer for this authored name; preserve the existing ignored-tag disposition.; DB-NOTAG |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Template_Data

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Template/AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
| Template/Tactical_Bribe_Cost | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/Tactical_Build_Prerequisites | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/Tactical_Build_Time_Seconds | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/Tactical_Production_Queue | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/TacticalBehavior | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Template/Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Template_Ground_Structure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Always_Side_With_Defender | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Land_Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Space_Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Template_GroundBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp) | WAR-19, WAR-21, WAR-23, WAR-24; WBF-45, WGP-178; WGM-106, WGS-91 | — | src/campaign/auto_resolve_strength.cpp; src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Has_Space_Evaluator | partial | ordinary live space goal targets (src/script/foc/tactical_ai_world.hpp); new ordinary live space goal targets (src/script/foc/tactical_ai_world.hpp); admitted ordinary live space body evaluator metadata (src/skirmish/ai.cpp); missing: carried or unselected transport deployment into the live target view | GS-10, SG-AI-01; GS-10, SG-AI-01; GS-10, SG-AI-01 | 650 | src/script/foc/tactical_ai_world.hpp; src/skirmish/ai.cpp | Directly admitted transport space bodies use the shared effective flag reader, AI type adapter and initial/new goal-target providers. Carried identities and unselected transport metadata under WHE-07/49 do not establish live deployment; that admission qualification remains separate.; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Prerequisites | partial | purchase card eligibility and visibility (include/eawr/presentation/ui/production.hpp); non-pad tactical purchase eligibility (include/eawr/sim/tactical/economy.hpp); eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-61, WPR-62, WPR-63; WPR-33; WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Build_Time_Seconds | partial | eligible non-pad space tactical purchase menu entries (src/skirmish/economy.cpp); resolved space production entry data (src/units/unit_tables_profiles.cpp); missing: land and galactic consumers outside this audit, objects outside an eligible loaded space tactical purchase menu | WPR-33, WPR-85; WPR-33, WPR-85 | 4532 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | WPR-85 source audit: the existing shared production decoder and non-pad space menu adapter consume this field without an XML element-label gate (SG-CLS-01). Mod labels do not establish eligibility or load map-only objects; those boundaries remain explicit.; basis: reviewed |
| Tactical_Production_Queue | applied | loaded ordinary space purchase options (src/skirmish/economy.cpp) | PU-14, TCFA-16, WPR-30 | — | src/skirmish/economy.cpp | The generic loaded space purchase closure selects the authored queue; inherited and overridden values are tested with an actual purchase on 1/2/4/8 workers. Station object scripts and AI research decisions are separate consumers.; basis: reviewed |

## UnderConstructionShip

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded ordinary space bodies (src/script/foc/ai_engine.cpp) | PG-08, TCFA-16 | — | src/script/foc/ai_engine.cpp | The ordinary space AI type binding carries the inherited or overridden authored metric to object power queries; the synthetic class fixture checks the bound value. This does not qualify custom squadron or team dispatch.; basis: reviewed |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## Unit_Abilities_Data

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Bribe_Ability/Activation_Max_Range | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Bribe_Ability/Activation_Min_Range | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Bribe_Ability/Activation_Style | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Bribe_Ability/Bribed_Effect | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Bribe_Ability/Bribed_Effect_Small | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |
| Tactical_Bribe_Ability/Effect_Z_Offset | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: SK.; basis: auto |

## W_Props_Defunct

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Yoden_GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Yoden_Model_Hero

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Yoden_Model_Infantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Yoden_Moving_Hero

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Yoden_Moving_Infantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: LS.; basis: auto |

## Yoden_Moving_Vehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Yoden_Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |

## Yoden_Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 6215 | — | Coverage backlog; authoring/recognition is not application evidence. Owning lane: GC.; basis: auto |
