# ai tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 32 | 32 |
| partial | 0 | 0 |
| todo | 26 | 26 |
| presentation-later | 0 | 2 |
| foc-ignores | 21 | 21 |
| deferred | 10 | 10 |
| land-or-galactic | 0 | 13 |
| multiplayer | 0 | 0 |
| **Total** | **89** | **104** |

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
| Difficulty_Adjustments/Normal | applied | whole class (src/script/foc/ai_service.cpp) | AB-24, AU-45, AU-47, BP-17, SK-42 | — | src/script/foc/ai_service.cpp | basis: reviewed |
| GalacticFreeStoreScript | land-or-galactic | application not recorded | — | — | — | WGA-10 |
| GoalProposalFunctionSets | applied | whole class (src/script/foc/ai_data.cpp) | none recorded | — | src/script/foc/ai_data.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| GoalSetExtensionSize | todo | application not recorded | — | 737 | — | — |

## AITemplates

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Budget/Always | todo | application not recorded | — | 737 | — | — |
| */Budget/Defensive | todo | application not recorded | — | 737 | — | FoC reads enum-named budget children; remake application remains to be qualified by mode.; WGA-10 |
| */Budget/Hero | todo | application not recorded | — | 737 | — | — |
| */Budget/Information | todo | application not recorded | — | 737 | — | — |
| */Budget/Infrastructure | todo | application not recorded | — | 737 | — | FoC reads enum-named budget children; galactic application is missing.; WGA-10 |
| */Budget/Interventions | todo | application not recorded | — | 737 | — | — |
| */Budget/Offensive | todo | application not recorded | — | 737 | — | FoC reads enum-named budget children; galactic application is missing.; WGA-10 |
| */Tactical_Budget_Allowance/Interventions | todo | application not recorded | — | 737 | — | — |
| */Turn_Off/Goals/Category | applied | whole class (src/script/foc/ai_goals.cpp) | AI-05, AI-14, FH-20, GS-30, R-09 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */Turn_Off/Goals/Goal_Type | todo | application not recorded | — | 737 | — | — |
| */Turn_On/Goals/Category | applied | whole class (src/script/foc/ai_goals.cpp) | AI-05, AI-14, FH-20, GS-30, R-09 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */Turn_On/Goals/Goal_Type | todo | application not recorded | — | 737 | — | — |
| */Turn_On/Plans/Goal_Category | applied | whole class (src/script/foc/ai_goals.cpp) | none recorded | — | src/script/foc/ai_goals.cpp | basis: auto |

## CIN_SpaceUnit, Container, GenericHeroUnit, GroundBase, GroundCompany, GroundStructure, GroundVehicle, HeroUnit, LandBombingUnit, MOV_Cinematic, Props_Story, SecondaryStructure, SlaveCompany, SpaceUnit, SpecialStructure, Squadron, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autoresolve_Health | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | deferred | application not recorded | — | 652 | — | Hero-team Container dispatch/application needs qualification; existing Squadron formation and craft-summed AI consumers do not prove this class applies the tag.; TCFA-04; basis: reviewed |
| Lua_Script | deferred | application not recorded | — | 652 | — | The authored hero-team object script needs general object-script lifecycle dispatch; special handling of another script does not apply it.; TCFA-06; basis: reviewed |

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
| Easily_Bribed | land-or-galactic | application not recorded | — | — | — | Galactic fleet admission chooses cheap/expensive bribe cost (WGM-85).; TCFA-10; basis: reviewed |
| Force_Alignment | land-or-galactic | application not recorded | — | — | — | Planet alignment, income and influence/control interfaces (WGEP-07, WGM-72).; TCFA-09; basis: reviewed |
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
| AI_SpaceEvaluatorRegionSize | applied | whole class (src/script/foc/ai_taskforces.cpp) | AI-07, AI-52, DT-02, ETA-08, GS-10, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into region_size; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDecayStep | applied | whole class (src/script/foc/ai_taskforces.cpp) | DT-02, G-07, TCFA-07 | — | src/script/foc/ai_taskforces.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_decay_step; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatDistanceFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AI_SpaceThreatLookAheadTime | applied | whole class (src/script/foc/ai_perception.cpp) | A-06, G-07, PG-01, PG-02, TCFA-07, WTA-04 | — | src/script/foc/ai_perception.cpp | Static audit: src/script/foc/ai_data.cpp reads the tag into threat_look_ahead; the cited consumer applies it. The M2 start command does not initialize/trace the AI service.; TCFA-07; basis: reviewed |
| AI_SpaceThreatTurnRateFactor | deferred | application not recorded | — | 652 | — | Verified threat-zone construction does not read these authored factors; other consumers or parser absence remain unverified.; TCFA-12; basis: reviewed |
| AITechLevelProductionTimeWeight | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWar | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| AIUsesFogOfWarSpace | deferred | application not recorded | — | 652 | — | Verified space threat visibility input; remake always bypasses AI fog without reading this switch. Stock False matches, configurable True is missing.; TCFA-11; basis: reviewed |
| High_Threat_Reachability_Tolerance | land-or-galactic | application not recorded | — | — | — | Read by planet reachability and the land-only zone reachability branch; not space target selection.; TCFA-08; basis: reviewed |
| Low_Threat_Reachability_Tolerance | land-or-galactic | application not recorded | — | — | — | Read by planet reachability and the land-only zone reachability branch; not space target selection.; TCFA-08; basis: reviewed |
| MaxCombatAccuracyAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatDamageAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxCombatSensorRangeAlignmentBonus | deferred | application not recorded | — | 652 | — | Authored XML value established; parser and live combat consumer remain unverified, so no tactical alignment bonus is inferred.; TCFA-09; basis: reviewed |
| MaxInfluenceTransitionAlignmentBonus | land-or-galactic | application not recorded | — | — | — | Planet income or influence/control alignment modifier (WGEP-07, WGM-72); no space skirmish consumer.; TCFA-09; basis: reviewed |
| MaxInfluenceTransitionAlignmentPenalty | land-or-galactic | application not recorded | — | — | — | Planet income or influence/control alignment modifier (WGEP-07, WGM-72); no space skirmish consumer.; TCFA-09; basis: reviewed |
| Medium_Threat_Reachability_Tolerance | land-or-galactic | application not recorded | — | — | — | Read by planet reachability and the land-only zone reachability branch; not space target selection.; TCFA-08; basis: reviewed |
| ShowUnitAIPlanAttachment | presentation-later | application not recorded | — | — | — | Diagnostic unit tooltip adds attached AI plan/movement text; not a simulation setting.; TCFA-13; basis: reviewed |
| ThreatExpansionDistance | deferred | application not recorded | — | 652 | — | Verified threat-aware space path expansion distance; remake lacks this pathfinding consumer.; TCFA-12; basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, Projectile, ScriptMarker, SecondaryStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | todo | application not recorded | — | 652 | — | only the PowerToShields object script is compared by name; every other object script is ignored Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Luke_Skywalker (+10 more). Mixed ground/space, space carriers on Marker: DEMO_CONTROLLER, DEMO_CONTROLLER_LAND, DEMO_CONTROLLER_LAND_E, DEMO_CONTROLLER_LAND_R. Mixed ground/space, space carriers on UniqueUnit: Home_One, Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Millenium_Falcon_Backwards (+13 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Luke_Skywalker. Mixed ground/space, space carriers on Marker: DEMO_CONTROLLER, DEMO_CONTROLLER_LAND, DEMO_CONTROLLER_LAND_E, DEMO_CONTROLLER_LAND_R. Mixed ground/space, space carriers on UniqueUnit: Home_One, Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Millenium_Falcon_Backwards (+13 more). |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | loaded local space battle (apps/viewer/src/battle_scoring.cpp); whole class (src/units/unit_combat.cpp) | WBF-46; A-06, AT-07, AT-09, G-03, PL-13 | — | apps/viewer/src/battle_scoring.cpp; src/units/unit_combat.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |

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

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | whole class (src/units/unit_combat.cpp) | A-06, AT-07, AT-09, G-03, PL-13 | — | src/units/unit_combat.cpp | &#35;850 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Projectile power supplies the relative shares of a ship's weapon hardpoints (A-06, AT-09).; CHECK-842; basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | applied | whole class (src/skirmish/ai.cpp) | PG-02, PG-06 | — | src/skirmish/ai.cpp | Promoted live pads supply their authored optional power to the existing AI type and perception interfaces; capture plans remain G9.; basis: reviewed |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | whole class (src/script/foc/ai_perception.cpp) | GS-10, WHZ-51 | — | src/script/foc/ai_perception.cpp | Live pad and construction types use the existing goal-target interface; neutral capture points do not match enemy goals, and capture plans remain G9.; basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Space_Evaluator | applied | ship (src/script/foc/ai_perception.cpp) | GS-10, WHZ-50, WHZ-51 | — | src/script/foc/ai_perception.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

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
| AI_Combat_Power | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Has_Space_Evaluator | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Air_Vehicles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
