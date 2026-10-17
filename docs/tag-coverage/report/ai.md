# ai tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 35 | 35 |
| partial | 31 | 31 |
| todo | 7 | 7 |
| presentation-later | 0 | 2 |
| foc-ignores | 21 | 21 |
| deferred | 8 | 8 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 8 |
| multiplayer | 0 | 0 |
| **Total** | **102** | **112** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Difficulty_Adjustments/Easy | todo | application not recorded | — | 735 | — | only the Normal difficulty adjustments are read (walk 6 G-07) |
| Difficulty_Adjustments/Hard | todo | application not recorded | — | 735 | — | only the Normal difficulty adjustments are read (walk 6 G-07) |
| Difficulty_Adjustments/Normal | applied | whole class (src/script/foc/ai_service.cpp) | AB-24, BP-17 | — | src/script/foc/ai_service.cpp | basis: reviewed |
| GalacticFreeStoreScript | land-or-galactic | application not recorded | — | — | — | WGA-10 |
| GoalProposalFunctionSets | applied | whole class (src/script/foc/ai_data.cpp) | none recorded | — | src/script/foc/ai_data.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| GoalSetExtensionSize | applied | whole class (src/script/foc/ai_goals.cpp) | GS-30 | — | src/script/foc/ai_goals.cpp | basis: reviewed |
| SpaceFreeStoreScript | applied | whole class (src/script/foc/tactical_ai.cpp) | AI-16 | — | src/script/foc/tactical_ai.cpp | basis: reviewed |
| Templates/Galactic | partial | Selected galactic lifecycle (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition | WGA-02, WGA-09, WGA-102 | 1120 | src/script/foc/galactic_ai_templates.cpp | Preserves authored mode-vector and active-list order; primary installed live-provider proof remains open.; basis: reviewed |

## AITemplates

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Budget/Always | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Defensive | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Hero | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Information | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Infrastructure | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Interventions | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/Offensive | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Budget/StoryArc | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-10, WGA-104, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Priority | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Tactical_Budget_Allowance/Interventions | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-105, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Trigger | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-102, WGA-103, WGA-109 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_Off/Goals/Category | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_Off/Goals/Goal_Type | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_Off/Plans/Name | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open. Deferred under &#35;2585 with the GC AI owner: verify named-plan switch semantics and add support for galactic/campaign AI and mod-authored space templates. SandboxHuman lists intervention templates for Space and Land too; no whole-tag mode exclusion or FoC ignore is claimed.; TCFA-15 stock scope survey (docs/behaviour/tag-coverage-fighters-ai.md): all three skirmish faction AI players select Test_Space without Plans/Name; named switches occur in galactic defaults and campaign intervention templates. The current loader has no named-plan consumer.; basis: reviewed |
| */Turn_On/Goals/Category | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_On/Goals/Goal_Type | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_On/Plans/Goal_Category | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open.; basis: reviewed |
| */Turn_On/Plans/Name | partial | Galactic template policy (src/script/foc/galactic_ai_templates.cpp); missing: Galactic live opponent composition, Land, Space | WGA-106, WGA-107, WGA-108 | 1120 | src/script/foc/galactic_ai_templates.cpp | Selected galactic lifecycle materializes prioritized policy and partial reversal; live funding, goal, plan and freestore provider composition remains open. Deferred under &#35;2585 with the GC AI owner: verify named-plan switch semantics and add support for galactic/campaign AI and mod-authored space templates. SandboxHuman lists intervention templates for Space and Land too; no whole-tag mode exclusion or FoC ignore is claimed.; TCFA-15 stock scope survey (docs/behaviour/tag-coverage-fighters-ai.md): all three skirmish faction AI players select Test_Space without Plans/Name; named switches occur in galactic defaults and campaign intervention templates. The current loader has no named-plan consumer.; basis: reviewed |

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
| Lua_Script | partial | Container Darth_Vader_TIE_Fighter_Container, Container Hutt_VWing_Squadron_Container, Container TIE_Defender_Squadron_Container, Container Y_Wing_Squadron_Container (src/script/foc/tactical_ai.cpp); missing: other object scripts | SOS-19, SOS-40, SOS-42, SOS-44 | 2714 | src/script/foc/tactical_ai.cpp | Vader script attachments use the real live member count and authored leader ability gates; other container scripts remain outside the admitted slice.; basis: reviewed |

## Difficulty_Adjustment

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_AI_Contrast_Multiplier | applied | whole class (src/script/foc/ai_selection.cpp) | AI-07, FH-30, PL-25, PL-30, WTA-15 | — | src/script/foc/ai_selection.cpp | basis: auto |
| Space_AI_Goal_Cycle_Sleep_Duration | applied | whole class (src/script/foc/ai_goals.cpp) | G-07, GS-05, WTA-03 | — | src/script/foc/ai_goals.cpp | basis: auto |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Basic_AI | applied | whole class (src/skirmish/ai.cpp) | AI-11, WSS-35 | — | src/skirmish/ai.cpp | basis: reviewed |
| Bribed_Color | presentation-later | application not recorded | — | — | — | Rendering multiplies bribed-object colour by faction tint; absent presentation support is not an AI decision gap.; TCFA-10; basis: reviewed |
| Easily_Bribed | todo | application not recorded | — | 1142 | — | read into the galactic data tables (src/data/galactic, docs/galactic-data.md); applied by the ticket; basis: reviewed |
| Space_Skirmish_AI_Default_Forces | applied | whole class (src/skirmish/inputs.cpp) | none recorded | — | src/skirmish/inputs.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## FunctionSet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Goal | applied | whole class (src/script/foc/ai_data.cpp) | AI-06, AI-50, GS-01, GS-12, PL-01 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_BuildTaskReservationSeconds | land-or-galactic | application not recorded | — | — | — | Planet-build reservation timer; not a space tactical producer reservation.; TCFA-08; basis: reviewed |
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
| High_Threat_Reachability_Tolerance | partial | immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: campaign execution-path provider, land hint-zone provider | FT-09 | 2494 | src/script/foc/plan_bindings.cpp | Loaded and applied to immutable planet/land execution-path queries; campaign/land providers remain to be integrated. Space candidates have no reachability service (FT-12).; TCFA-08; basis: reviewed |
| Low_Threat_Reachability_Tolerance | partial | immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: campaign execution-path provider, land hint-zone provider | FT-09 | 2494 | src/script/foc/plan_bindings.cpp | Loaded and applied to immutable planet/land execution-path queries; campaign/land providers remain to be integrated. Space candidates have no reachability service (FT-12).; TCFA-08; basis: reviewed |
| MaxCombatAccuracyAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatDamageAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatSensorRangeAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaximumPoliticalControl | applied | whole class (src/campaign/ai_perception_content.cpp) | WGA-475 | — | src/campaign/ai_perception_content.cpp | basis: reviewed |
| MaxInfluenceTransitionAlignmentBonus | land-or-galactic | application not recorded | — | — | — | Planet income or influence/control alignment modifier (WGEP-07, WGM-72); no space skirmish consumer.; TCFA-09; basis: reviewed |
| MaxInfluenceTransitionAlignmentPenalty | land-or-galactic | application not recorded | — | — | — | Planet income or influence/control alignment modifier (WGEP-07, WGM-72); no space skirmish consumer.; TCFA-09; basis: reviewed |
| Medium_Threat_Reachability_Tolerance | partial | immutable planet and land execution queries (src/script/foc/plan_bindings.cpp); missing: campaign execution-path provider, land hint-zone provider | FT-09 | 2494 | src/script/foc/plan_bindings.cpp | Loaded and applied to immutable planet/land execution-path queries; campaign/land providers remain to be integrated. Space candidates have no reachability service (FT-12).; TCFA-08; basis: reviewed |
| ShowUnitAIPlanAttachment | presentation-later | application not recorded | — | — | — | Diagnostic unit tooltip adds attached AI plan/movement text; not a simulation setting.; TCFA-13; basis: reviewed |
| ThreatExpansionDistance | deferred | application not recorded | — | 652 | — | Verified threat-aware space path expansion distance; remake lacks this pathfinding consumer.; TCFA-12; basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, Projectile, ScriptMarker, SecondaryStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | partial | UniqueUnit IG-2000 (src/script/foc/ai_taskforces.cpp); SpaceUnit Interdictor_Cruiser, UniqueUnit Millennium_Falcon, UniqueUnit Slave_I, UniqueUnit Sundered_Heart (src/script/foc/tactical_ai.cpp); SpaceUnit MC30_Frigate (src/sim/tactical/session_abilities.cpp); missing: other object scripts | WHE-67, WHE-68; SOS-11, SOS-15, SOS-16, SOS-17, SOS-40, SOS-42, SOS-44; FC-57, FC-58, FD-47, FS-29 | 652 | src/script/foc/ai_taskforces.cpp; src/script/foc/tactical_ai.cpp; src/sim/tactical/session_abilities.cpp | Falcon, its inherited space variants, Slave I, Sundered Heart and the Interdictor's missile shield attach their isolated object scripts; native IG-2000 and MC30 consumers remain separate from the bounded attachment slice.; basis: reviewed |

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
| Abilities/Tactical_Bribe_Ability/Bribed_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/Bribed_Effect_Small | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 652 | — | — |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Credit_Power_Up, Tech_Power_Up) |

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
| Has_Space_Evaluator | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
