# movement tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 100 | 100 |
| partial | 195 | 195 |
| todo | 154 | 154 |
| presentation-later | 0 | 0 |
| foc-ignores | 24 | 24 |
| deferred | 16 | 16 |
| by-design | 2 | 2 |
| land-or-galactic | 0 | 0 |
| multiplayer | 0 | 0 |
| **Total** | **491** | **491** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## CIN_SpaceUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| FormationPriority | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CommandBarComponent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Layer | todo | application not recorded | — | 649 | — | family: hud-radar; read into the catalog; the only consumers are the galactic ground HUD sell button and a unit-card comment, not the battle bar.; basis: reviewed |
| Drag_And_Drop | todo | application not recorded | — | 649 | — | family: hud-radar; parsed into the command-bar catalog (src/data/ui/command_bar.cpp) but no widget reads it.; basis: reviewed |
| No_Hidden_Collision | todo | application not recorded | — | 649 | — | family: hud-radar; parsed into the command-bar catalog (src/data/ui/command_bar.cpp) but no widget reads it.; basis: reviewed |
| Snap_Drag | todo | application not recorded | — | 649 | — | family: hud-radar; parsed into the command-bar catalog (src/data/ui/command_bar.cpp) but no widget reads it.; basis: reviewed |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ship (src/units/unit_motion.cpp); TEAM objects declaring REPLENISH_WINGMEN (src/units/unit_tables_abilities_nested.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); Container (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WLPA-07, WLPA-09; WHZ-20; WHE-70; WLPA-03, WLPA-07, WLPA-08; WSU-15, WSU-19, WSU-21 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/land_start.cpp; src/units/unit_motion.cpp; src/units/unit_tables_abilities_nested.cpp; src/units/unit_tables_land.cpp; src/units/unit_tables_profiles.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| FormationSpacing | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 1450 | include/eawr/skirmish/land_content.hpp | Land member spacing defaults to zero additional spacing; hard geometry remains. Team-type and member spacing are retained separately (LC-79/80).; basis: reviewed |
| LandBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; ML-24; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Layer_Z_Adjust | partial | Galactic_Fleet container marker (apps/viewer/src/galactic/galactic_scene.cpp); missing: Other container presentation paths remain outside the galactic fleet marker. | WGP-16 | 650 | apps/viewer/src/galactic/galactic_scene.cpp | The Galactic_Fleet marker adds the authored Layer_Z_Adjust to the model position; other container paths are not claimed.; basis: reviewed |
| Max_Rate_Of_Turn | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Max_Speed | partial | land company members (include/eawr/skirmish/land_content.hpp); craft (src/units/unit_motion.cpp); missing: ship | LC-06, MV-01; WMV-44 | 649 | include/eawr/skirmish/land_content.hpp; src/units/unit_motion.cpp | WMV-44: resolved Create_Team_Type maximum supplies the member formation deceleration ratio; ordinary team-container maximum velocity and land consumers remain open under &#35;649. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Min_Speed | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| MovementPredictionInterval | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Override_Acceleration | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Override_Deceleration | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| OverrideAcceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| OverrideDeceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); craft (src/sim/tactical/fighters.cpp); missing: ship | MV-01; WMV-44 | 649 | include/eawr/skirmish/land_content.hpp; src/sim/tactical/fighters.cpp | WMV-44: space formation upkeep uses the resolved Create_Team_Type container default or mod-authored override, never purchase-template or child data; ordinary team-container and land consumers remain open under &#35;649. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Space_Layer | todo | application not recorded | — | 3776 | — | parsed into movement profile and used for skirmish footprint (economy.cpp) but no land layer consumer pinned; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); TEAM objects declaring REPLENISH_WINGMEN (src/units/unit_tables_abilities_nested.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); Container (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-70; WHE-SQ-02; WSU-15, WSU-19, WSU-21 | 4532 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_abilities_nested.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| UnitCollisionClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-35 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, Marker, Projectile, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Mass | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| MovementClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-01 | 1450 | include/eawr/skirmish/land_content.hpp | applied only by the galactic auto-resolve (src/campaign/auto_resolve_*); no tactical code reads it, and whether FoC's tactical battle does is unverified (&#35;4002) Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/Chase_Radius | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-73 | 4532 | src/sim/tactical/session_step_combat.cpp | Victim search around the creation point; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Attack_Move_Response_Range | partial | loaded fighter locomotor craft (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Craft (SpaceUnit with FIGHTER_LOCOMOTOR) are read in load_body; ship and container consumers are absent.; basis: reviewed |
| Guard_Chase_Range | partial | loaded fighter locomotor craft (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Craft (SpaceUnit with FIGHTER_LOCOMOTOR) are read in load_body; ship and container consumers are absent.; basis: reviewed |

## Container, SpaceUnit, Squadron, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Formation_Priority | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| FormationOrder | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |

## Container, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Hyperspace_Speed | partial | eligible live galactic leaf members, squadron template packing keys (src/campaign/fleets.cpp); missing: fleet-container authored speeds; live child speeds are used for fleet travel | WGM-118, WGM-151, WGM-26 | 4532 | src/campaign/fleets.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Container, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autonomous_Move_Extension_Vs_Attacker | todo | application not recorded | — | 649 | — | family: movement; read for Squadron purchase types only (load_squadron); craft, solo heroes and ships ignore it.; basis: reviewed |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Random_Rotate | todo | application not recorded | — | 649 | — | family: vfx-models; no loader or consumer.; basis: reviewed |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Benefits_From_Corruption | partial | galactic movement (src/campaign/fleets.cpp); live corruption reward beneficiary and ownership classification (src/sim/galactic/ability_corruption.cpp); galactic fiscal income (src/sim/galactic/economy_corruption.cpp); missing: other corruption mechanics | WGM-27, WGM-33; WGCH-161, WGCH-165, WGCH-168, WGM-195; WGEP-04, WGEP-13 | 1130 | src/campaign/fleets.cpp; src/sim/galactic/ability_corruption.cpp; src/sim/galactic/economy_corruption.cpp | Family: GC corruption + black market. WGM-27/33 apply movement benefits, WGEP-04/13 fiscal benefits and WGCH-161/165/168 reward beneficiary classification; other corruption ability consumers remain outside those paths.; basis: reviewed |
| Hyperspace_Speed_Factor | applied | galactic departure (src/campaign/fleets.cpp) | WGM-26 | — | src/campaign/fleets.cpp | basis: reviewed |
| Scatters_From_Crushers | todo | application not recorded | — | 649 | — | — |
| Space_Forced_Retreat_Due_To_Superweapon | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Space_Retreat_Countdown_Seconds | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WR-44, WR-86, WR-87 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Retreat_Flight_Increment | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Space_Retreat_Flight_Move_Increment | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Space_Retreat_Off_Map_Dest_Pos | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Space_Retreat_Pursue_Max_Speed_Mod_Factor | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Space_Retreat_Unit_Increment_Wait_Frames | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Space_Retreat_Units_Damaged_Mod_Factor | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Rotate_For_Space_Targeting | applied | whole class (src/sim/tactical/combat_aim.cpp) | A-05 | — | src/sim/tactical/combat_aim.cpp | basis: reviewed |
| Base_Shield_Speed_Modifier | todo | application not recorded | — | 3776 | — | GameConstants singleton; no reader on main; land base shields not implemented; basis: reviewed |
| BetweenFormationSpacing | deferred | application not recorded | — | 626 | — | — |
| CloseEnoughAngleForMoveStart | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Corruption_Hyperspace_Bonus | applied | galactic travel (src/campaign/fleets.cpp) | WGM-27 | — | src/campaign/fleets.cpp | basis: reviewed |
| Crouch_Move_Fire_Angle_Cutoff | todo | application not recorded | — | 3776 | — | Evidence says retail reads it in the weapon service; no reader on the origin/main land path.; basis: reviewed |
| Destination_Collision_Query_Extension | deferred | application not recorded | — | 626 | — | — |
| DestinationSearchRadiusIncrementSpace | applied | whole class (src/units/unit_motion.cpp) | AV-05 | — | src/units/unit_motion.cpp | basis: auto |
| DoubleClickMoveMaxSpeedRatio | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| DynamicAvoidanceRectangleBound | deferred | application not recorded | — | 626 | — | — |
| DynamicLandComplexityQuota | todo | application not recorded | — | 3776 | — | no carriers and no land reader found on origin/main; basis: reviewed |
| DynamicObstacleOverlapPenalty | deferred | application not recorded | — | 626 | — | — |
| Engines_Disabled_Speed_Modifier | applied | whole class (src/sim/tactical/durability.cpp) | AB-16, FM-09a | — | src/sim/tactical/durability.cpp | basis: auto |
| FinalFacing180Penalty | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| FinalFormationFacingDeltaCoefficient | deferred | application not recorded | — | 626 | — | — |
| FinalFormationFacingMinimumAngle | deferred | application not recorded | — | 626 | — | — |
| FormationMaximumSideError | partial | space fighter and bomber squadron move lanes (src/sim/tactical/fighters_formation.cpp); missing: land movement formations | FO-10, WSQ-17 | 654 | src/sim/tactical/fighters_formation.cpp | basis: reviewed |
| FormationMinimumSideError | partial | space fighter and bomber squadron move lanes (src/sim/tactical/fighters_formation.cpp); missing: land movement formations | FO-10, WSQ-17 | 649 | src/sim/tactical/fighters_formation.cpp | basis: reviewed |
| FramesPerCollisionCheck | deferred | application not recorded | — | 626 | — | — |
| Hyperspace_Speed_Factor_Empire | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Hyperspace_Speed_Factor_Rebel | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| MatchFacingDeltaSpace | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Max_Formation_Area | deferred | application not recorded | — | 626 | — | — |
| Max_Move_Frame_Delay | todo | application not recorded | — | 3776 | — | debug build constant; no land reader found; basis: reviewed |
| MaxObstacleCostSpace | deferred | application not recorded | — | 626 | — | — |
| MaxRotationsSpace | applied | whole class (src/units/unit_motion.cpp) | S-12 | — | src/units/unit_motion.cpp | basis: auto |
| MaxWaypointsPerPath | partial | space queued point movement (src/sim/tactical/session_cmd_orders.cpp); space queued point movement (src/units/unit_motion.cpp); missing: pre-existing formation reuse and exact arrival phase | HCB-58, HCB-59; HCB-58 | 4532 | src/sim/tactical/session_cmd_orders.cpp; src/units/unit_motion.cpp | Queue capacity applies to retained point movement; the value reads as a C integer and compares as unsigned, so -1 is unlimited (HCB-58, &#35;4992); the named completion and formation seams remain tracked.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| MinimumDragDistance | todo | application not recorded | — | 649 | — | family: hud-radar; only MinimumDragSelectDistance is modelled (a fixed 100 px constant in selection.hpp); this constant is not read.; basis: reviewed |
| MinimumStoppedVsStoppedOverlapCoefficient | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| MovementReevaluationFrameCount | applied | whole class (src/units/unit_motion.cpp) | OR-06 | — | src/units/unit_motion.cpp | basis: reviewed |
| MovingVsMovingLookAheadTime | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Object_Max_Speed_Multiplier_Galactic | applied | initialized galactic movement (src/campaign/fleets.cpp) | WGM-97 | — | src/campaign/fleets.cpp | basis: reviewed |
| Object_Max_Speed_Multiplier_Space | applied | whole class (src/units/unit_motion.cpp) | BK-01, FM-01 | — | src/units/unit_motion.cpp | basis: auto |
| ObstacleAreaOverlapForMaxSpace | deferred | application not recorded | — | 626 | — | — |
| OccupationRadiusCoefficientSpace | applied | whole class (src/units/unit_motion.cpp) | AV-05, AV-13 | — | src/units/unit_motion.cpp | basis: auto |
| Preferred_Pathfinder_Types | todo | application not recorded | — | 3776 | — | Debug build reads it for formation pathfinder choice; pathfinder list todo (legacy &#35;649); family in input was F12, moved to F5; basis: reviewed |
| Push_Scroll_Speed_Modifier | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| RepushDistance | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Rotate_Formation_Facing_Moves | deferred | application not recorded | — | 626 | — | — |
| Short_Range_Attack_Formation_Coefficient | deferred | application not recorded | — | 626 | — | — |
| Space_Collidable_Grid_Cull_Size | by-design | application not recorded | — | — | — | BY-DESIGN-PATHING |
| Space_Guard_Range | applied | whole class (src/units/unit_motion.cpp) | OR-14 | — | src/units/unit_motion.cpp | basis: reviewed |
| Space_Large_Ship_Grid_Cull_Size | by-design | application not recorded | — | — | — | BY-DESIGN-PATHING |
| Space_Reinforcement_Collision_Check_Distance | applied | whole class (src/sim/tactical/session_economy.cpp) | WR-25 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Space_Retreat_Allowed_Countdown_Seconds | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WR-43 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Retreat_Attrition_Factor | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WGT-36 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Station_Destruction_Forces_Retreat | applied | whole class (src/campaign/space_rules.cpp); whole class (src/sim/tactical/session_step_finish.cpp) | WGM-51, WGT-29; WGM-51, WGT-29 | — | src/campaign/space_rules.cpp; src/sim/tactical/session_step_finish.cpp | Campaign station-loss condition is independent of the lobby all-unit condition.; basis: reviewed |
| SpaceIdlePathCullCoefficient | deferred | application not recorded | — | 626 | — | — |
| SpaceLocomotorFacingLookaheadAcc | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| SpaceObjectTrackingInterval | applied | whole class (src/units/unit_motion.cpp) | none recorded | — | src/units/unit_motion.cpp | basis: auto |
| SpaceObjectTrackingTreeCount | applied | whole class (src/units/unit_motion.cpp) | AV-03 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| SpacePathFailureDistanceCutoffCoefficient | applied | whole class (src/units/unit_motion.cpp) | AV-14 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| SpacePathFailureForwardExpansionIncrement | applied | whole class (src/units/unit_motion.cpp) | none recorded | — | src/units/unit_motion.cpp | basis: auto |
| SpacePathFailureMaxExpansionsCoefficient | applied | whole class (src/units/unit_motion.cpp) | AV-14 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| SpacePathFailureRotationExpansionIncrement | applied | whole class (src/units/unit_motion.cpp) | none recorded | — | src/units/unit_motion.cpp | basis: auto |
| SpacePathfindFrameDelayDelta | deferred | application not recorded | — | 626 | — | — |
| SpacePathfindMaxExpansions | applied | whole class (src/units/unit_motion.cpp) | none recorded | — | src/units/unit_motion.cpp | basis: auto |
| SpacePathingTries | applied | whole class (src/units/unit_motion.cpp) | AV-14 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| SpaceStaticObstacleAvoidanceBonusDistance | deferred | application not recorded | — | 626 | — | — |
| Spread_Out_Spacing_Coefficient | todo | application not recorded | — | 3776 | — | debug build constant; no land reader found; basis: reviewed |
| Strategic_Max_Scroll_Speed | todo | application not recorded | — | 1157 | — | Galactic map camera scroll tuning is not read; the map camera uses its own speeds.; basis: reviewed |
| Strategic_Min_Scroll_Speed | todo | application not recorded | — | 1157 | — | Galactic map camera scroll tuning is not read; the map camera uses its own speeds.; basis: reviewed |
| Tactical_Max_Scroll_Speed | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tactical_Min_Scroll_Speed | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| TacticalMaxScrollSpeed | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| TacticalMinScrollSpeed | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| TurnInPlaceSlowdownCorvette | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the slowdown applies to face-target motion orders (MV-01); structural tables change but the 9000-tick scenario observes no resulting difference.; CHECK-842; basis: reviewed |
| TurnInPlaceSlowdownFrigate | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the slowdown applies to face-target motion orders (MV-01); structural tables change but the 9000-tick scenario observes no resulting difference.; CHECK-842; basis: reviewed |
| UseLinearCollisionChecks | deferred | application not recorded | — | 626 | — | — |
| VehicleFormationRecruitmentDistance | todo | application not recorded | — | 649 | — | — |
| WaitOperatorBaseFrameTime | applied | whole class (src/units/unit_motion.cpp) | S-10 | — | src/units/unit_motion.cpp | basis: auto |
| WaitOperatorSpeedCoefficient | applied | whole class (src/units/unit_motion.cpp) | AV-05, S-10 | — | src/units/unit_motion.cpp | basis: auto |
| XYExpansionDistanceSpace | applied | whole class (src/units/unit_motion.cpp) | PC-03, S-12 | — | src/units/unit_motion.cpp | basis: auto |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Move_Response_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| FormationOrder | todo | application not recorded | — | 3776 | — | no land reader of FormationOrder found on origin/main; basis: reviewed |
| FormationRaggedness | todo | application not recorded | — | 3776 | — | no land reader of FormationRaggedness found on origin/main; basis: reviewed |
| FormationSpacing | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 1450 | include/eawr/skirmish/land_content.hpp | Land member spacing defaults to zero additional spacing; hard geometry remains. Team-type and member spacing are retained separately (LC-79/80).; basis: reviewed |
| Guard_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OverrideAcceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); ship (src/units/unit_motion.cpp); missing: GenericHeroUnit objects outside adapted land companies | MV-01; MV-01 | 1450 | include/eawr/skirmish/land_content.hpp; src/units/unit_motion.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| OverrideDeceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); ship (src/units/unit_motion.cpp); missing: GenericHeroUnit objects outside adapted land companies | MV-01; MV-01 | 1450 | include/eawr/skirmish/land_content.hpp; src/units/unit_motion.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| UnitCollisionClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-35 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpecialStructure, TechBuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_cmd_targeted.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); source-bearing map placements (src/skirmish/start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WSW-08, WSW-09, WSW-10, WSW-11; WNO-16, WTA-29; WLPA-07, WLPA-09; WSW-01; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_cmd_targeted.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/land_start.cpp; src/skirmish/start.cpp; src/units/unit_tables_land.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; none recorded; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |

## GenericHeroUnit, MiscObject, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, Projectile, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Rate_Of_Turn | partial | land company members (include/eawr/skirmish/land_content.hpp); direct space projectiles, MISSILE (src/sim/tactical/projectiles.cpp); whole class (src/units/unit_combat.cpp); missing: GenericHeroUnit objects outside adapted land companies, Mobile_Defense_Unit objects outside adapted land companies, Projectile objects outside adapted land companies, SpaceUnit objects outside adapted land companies, UniqueUnit objects outside adapted land companies | MV-01; WPJ-12, WPJ-17; FM-03, MV-01, MV-20, S-26, S-27 | 1450 | include/eawr/skirmish/land_content.hpp; src/sim/tactical/projectiles.cpp; src/units/unit_combat.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Max_Speed | partial | land company members (include/eawr/skirmish/land_content.hpp); whole class (src/units/unit_combat.cpp); missing: GenericHeroUnit objects outside adapted land companies, Mobile_Defense_Unit objects outside adapted land companies, Projectile objects outside adapted land companies, SpaceUnit objects outside adapted land companies, UniqueUnit objects outside adapted land companies | LC-06, MV-01; FA-01, FA-04, W-10 | 1450 | include/eawr/skirmish/land_content.hpp; src/units/unit_combat.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## GenericHeroUnit, Mobile_Defense_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent | applied | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | LC-79, LC-80; AV-05, WSQ-45; A-07, OR-04; none recorded | — | include/eawr/skirmish/land_content.hpp; src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry.; basis: auto |
| Custom_Hard_YExtent | applied | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | LC-79, LC-80; AV-05, WSQ-45; A-07, OR-04; none recorded | — | include/eawr/skirmish/land_content.hpp; src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry.; basis: auto |

## GenericHeroUnit, SecondaryStructure, SpaceBuildable, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Soft_Footprint_Radius | partial | land company members (include/eawr/skirmish/land_content.hpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp); missing: GenericHeroUnit objects outside adapted land companies, SecondaryStructure objects outside adapted land companies, SpaceBuildable objects outside adapted land companies, SpecialStructure objects outside adapted land companies, StarBase objects outside adapted land companies | WLPA-06, WLPA-14; A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | 1450 | include/eawr/skirmish/land_content.hpp; src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## Goals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Activation_Tracking_Duration | applied | whole class (src/script/foc/ai_learning.hpp) | GS-32, WTA-11 | — | src/script/foc/ai_learning.hpp | basis: auto |
| */Tracking_Duration | applied | whole class (src/script/foc/ai_learning.hpp) | GS-32, WTA-11 | — | src/script/foc/ai_learning.hpp | basis: auto |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Formation_Prority | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundInfantry, HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| WaitsForFormationFormup | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundStructure, Props_Generic, Props_Story, SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Facing_Adjust | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rotation_Offset | partial | ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: land hardpoints | HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; MC-06, WAD-40 | 4532 | src/sim/tactical/combat_aim.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Elevate_Extent_Degrees | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: land hardpoints and remaining non-turret object interfaces | UA-05, WWP-17; HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; MC-06, WAD-40 | 4532 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |
| Turret_Rotate_Extent_Degrees | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: land hardpoints and remaining non-turret object interfaces | UA-05, WWP-17; HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; MC-06, WAD-40 | 4532 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rotate_Speed | partial | ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: land hardpoints and remaining non-turret object interfaces | HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; UA-05, WWP-17; MC-06, WAD-40 | 4532 | src/sim/tactical/combat_aim.cpp | Manual assignment, player clock and mechanical fire frames retain their existing rules; UA-05 ordinary TURRET space objects also apply mechanical readiness and viewer articulation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## Hero_Clash

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Clash_Actions/Move_Action/Action_Distance_From_Target_Curve | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Action_Health_Percentage_Curve | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Action_Object | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Action_Relevance_Type | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Action_Target | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Move_Max_Distance | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Move_Min_Distance | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Move_Speed | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Move_Type | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Move_Use_Jetpack | todo | application not recorded | — | 649 | — | — |
| Clash_Actions/Move_Action/Wait_For_Finish_If_Target_Using_Jetpack | todo | application not recorded | — | 649 | — | — |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Berserker_Ability/Berserker_Speed | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Retreat_Prevention_Ability/Activation_Style | todo | application not recorded | — | 760 | — | family: abilities; no loader or consumer for this ability/tag in a tactical battle.; basis: reviewed |
| Alternate_Max_Rate_Of_Turn | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Alternate_Max_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Attack_Move_Response_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Autonomous_Move_Extension_Vs_Attacker | todo | application not recorded | — | 3776 | — | Read only by space combat step (session_step_combat.cpp) and profile load; land combat step not on main (hero row); basis: reviewed |
| Custom_Hard_XExtent | partial | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 4812 | include/eawr/skirmish/land_content.hpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry.; SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Custom_Hard_YExtent | partial | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 4812 | include/eawr/skirmish/land_content.hpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry.; SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Custom_Soft_Footprint_Radius | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-06, WLPA-14 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| FormationGrouping | todo | application not recorded | — | 3776 | — | no land reader of FormationGrouping found on origin/main; basis: reviewed |
| FormationOrder | todo | application not recorded | — | 3776 | — | no land reader of FormationOrder found on origin/main; basis: reviewed |
| FormationRaggedness | todo | application not recorded | — | 3776 | — | no land reader of FormationRaggedness found on origin/main; basis: reviewed |
| FormationSpacing | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 1450 | include/eawr/skirmish/land_content.hpp | Land member spacing defaults to zero additional spacing; hard geometry remains. Team-type and member spacing are retained separately (LC-79/80).; basis: reviewed |
| Guard_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Hover_Offset | todo | application not recorded | — | 3776 | — | only space_entry_stock reads Hover_Offset; no land reader; basis: reviewed |
| Max_Lift | todo | application not recorded | — | 3776 | — | read only in profile decode (unit_tables_profiles.cpp:379) and space craft code; no land consumer; basis: reviewed |
| Max_Rate_Of_Turn | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Max_Speed | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-06, MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Min_Speed | todo | application not recorded | — | 3776 | — | read in profile decode (unit_tables_profiles.cpp:374); the applied twin is space fighters only; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OverrideAcceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| OverrideDeceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Space_Layer | todo | application not recorded | — | 3776 | — | parsed into movement profile but no land layer consumer pinned; applied twin not confirmed for land; basis: reviewed |
| UnitCollisionClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-35 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Uses_Multiple_Locomotors | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent_Offset | partial | craft, ship, station (src/sim/tactical/session_state.cpp); missing: movement | V-21 | 649 | src/sim/tactical/session_state.cpp | Applied to multisample fog boxes; movement tracking offsets remain pending.; basis: reviewed |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Rotations_Per_Second | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 649 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; basis: reviewed |

## Marker, MiscObject, Mobile_Defense_Unit, Projectile, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | partial | whole class (src/scene/idle_tags.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | none recorded; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/scene/idle_tags.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: auto |

## Marker, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | Projectile (apps/viewer/src/battle_effects_prepare.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WPJ-38; WGT-121; C-05, D-04, L-05, L-23, L-43; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | apps/viewer/src/battle_effects_prepare.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: auto |

## Marker, Projectile, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Dead | todo | application not recorded | — | 649 | — | family: weapons-shields; read only for land bodies (unit_tables_land.cpp).; basis: reviewed |

## Marker, SecondaryStructure, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Apply_Y_Turret_Rotate_To_Axis | todo | application not recorded | — | 649 | — | family: weapons-shields; no loader or consumer.; basis: reviewed |
| Apply_Z_Turret_Rotate_To_Axis | todo | application not recorded | — | 649 | — | family: weapons-shields; no loader or consumer.; basis: reviewed |

## Marker, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| Obstacle_Width | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| Obstacle_X_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| Obstacle_Y_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Avoidance_Disabled | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Behavior | partial | GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_cmd_targeted.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); source-bearing map placements (src/skirmish/start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WSW-08, WSW-09, WSW-10, WSW-11; WNO-16, WTA-29; WLPA-07, WLPA-09; WSW-01; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_cmd_targeted.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/land_start.cpp; src/skirmish/start.cpp; src/units/unit_tables_land.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers. HCB-12a..12f: Multiplayer_Beacon_Type objects author MULTIPLAYER_BEACON in Behavior. Their agreed setup consumes only the resolved beacon type and Beacon_Lifetime_In_Secs; Affiliation, Behavior and Tactical_Health are not beacon inputs. Other MiscObject consumers retain their application and trace requirements. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Mass | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SpaceBehavior | applied | whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); source-bearing map placements (src/skirmish/start.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp) | BP-31, R-ROT-01, RO-1; WCC-25; WSW-01; WAD-38 | — | src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/skirmish/start.cpp; src/units/unit_tables_decode.cpp | basis: reviewed |
| UnitCollisionClass | todo | application not recorded | — | 3776 | — | no land member reader for MiscObject found; basis: reviewed |

## MiscObject, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | applied | whole class (src/battle/start.cpp) | LZ-01, LZ-02 | — | src/battle/start.cpp | &#35;718: every start object is created at its placement point raised by its type's height; basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Dead | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| DeployedBehavior | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| FormationGrouping | todo | application not recorded | — | 3776 | — | no land reader of FormationGrouping found on origin/main; basis: reviewed |
| FormationOrder | todo | application not recorded | — | 3776 | — | no land reader of FormationOrder found on origin/main; basis: reviewed |
| FormationRaggedness | todo | application not recorded | — | 3776 | — | no land reader of FormationRaggedness found on origin/main; basis: reviewed |
| FormationSpacing | applied | whole class (src/sim/tactical/land_placement.cpp) | LP-20, WLPA-24, WLPA-27 | 3776 | src/sim/tactical/land_placement.cpp | FormationSpacing x20 drives landing slot spacing and nearest-open padding (session_land.cpp slot spacing, land_placement_padding); formation-move slot geometry is a separate F5 item; basis: reviewed |
| Mass | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| MaxSecondaryTurnROTCoefficient | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| MinSecondaryTurnROTCoefficient | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OverrideAcceleration | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| OverrideDeceleration | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| SecondaryTurnAngle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SecondaryTurnInPlaceROTCoefficient | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SecondaryTurnLookaheadDistance | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| UnitCollisionClass | partial | land infantry and bike members (repulsion eligibility, stateless moving/moving soft pairs) (include/eawr/sim/tactical/land_movement.hpp); land battle objects (include/eawr/units/land_companies.hpp); missing: land battle consumers (see note) | WLPA-82, WLPA-83; WLPA-73 | 3776 | include/eawr/sim/tactical/land_movement.hpp; include/eawr/units/land_companies.hpp | class read (unit_tables_land.cpp:190) and ordered in land content (land_content.hpp:307-328); the contact selector, pair guard and member steering read it for Infantry/Bike soft pairs (WLPA-82, WLPA-83); push, truncate, giant and virtual-flag rules are not applied yet; basis: reviewed |
| UseSecondaryFacing | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |

## Mobile_Defense_Unit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_cmd_targeted.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); source-bearing map placements (src/skirmish/start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WSW-08, WSW-09, WSW-10, WSW-11; WNO-16, WTA-29; WLPA-07, WLPA-09; WSW-01; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_cmd_targeted.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/land_start.cpp; src/skirmish/start.cpp; src/units/unit_tables_land.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Facing_Adjust | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Avoidance_Disabled | todo | application not recorded | — | 649 | — | family: weapons-shields; no loader or consumer.; basis: reviewed |
| Projectile_Acceleration_Per_Frame | todo | application not recorded | — | 649 | — | family: weapons-shields; no loader or consumer.; basis: reviewed |
| Projectile_Bomb_Fall_Accel_Rate | todo | application not recorded | — | 3776 | — | Falling-bomb flight absent (WPJ-19, G2); basis: reviewed |
| Projectile_Grenade_Sticks_On_Collision | todo | application not recorded | — | 649 | — | family: weapons-shields; no loader or consumer.; basis: reviewed |
| Projectile_Ion_Stun_Speed_Reduction_Percent | applied | whole class (src/units/unit_combat.cpp) | DG-01, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Space_Layer | todo | application not recorded | — | 649 | — | family: weapons-shields; projectiles carry no space layer in the shot profile.; basis: reviewed |
| SpaceBehavior | todo | application not recorded | — | 650 | — | family: weapons-shields; SpaceBehavior is read for object bodies (SPAWN_SQUADRON, REVEAL, POWERED...) and DEBRIS props, not for projectiles.; basis: reviewed |

## Projectile, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| UnitCollisionClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-35 | 1450 | include/eawr/skirmish/land_content.hpp | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more). Mixed ground/space, space carriers on UniqueUnit: Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on UniqueUnit: Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Obstacle_Offset | partial | loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); loaded space footprints (src/units/unit_tables_decode.cpp); missing: dense-fog raw-offset consumer, objects outside loaded tactical closure | WHZ-05; WHZ-01 | 649 | src/sim/tactical/pathfind.cpp; src/units/unit_tables_decode.cpp | Tracking applies raw yaw-rotated XY without scale; the dense-fog geometry consumer remains separate. The footprint reader retains authored offsets; selected-map SpaceProp offsets are outside the pinned M2 table closure and recorded trace.; basis: reviewed; trace: unrecorded |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); source-bearing map placements (src/skirmish/start.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WSW-01; WAD-38 | 4532 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/skirmish/start.cpp; src/units/unit_tables_decode.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SFXEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Pitch | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Min_Pitch | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Behavior | partial | GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); SpaceBuildable (src/sim/tactical/session_economy.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); SpaceBuildable (src/units/unit_tables_profiles.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WBP-30, WBP-31, WBP-32; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08; WSU-15, WSU-19, WSU-21 | 1450 | src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_economy.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp; src/units/unit_tables_profiles.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| SpaceBehavior | applied | whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); SpaceBuildable (src/sim/tactical/session_economy.cpp); SpaceBuildable (src/units/unit_tables_profiles.cpp) | BP-31, R-ROT-01, RO-1; WCC-25; WBP-30, WBP-31, WBP-32; WSU-15, WSU-19, WSU-21 | — | src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_economy.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| Turret_Rotate_Speed | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17 | 4532 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate. WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate. WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | todo | application not recorded | — | 649 | — | family: vfx-models; skydome placements take no Layer_Z_Adjust (scene_build applies it to SpaceProp placements only).; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md. |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | whole class (apps/viewer/src/battle_source_local.cpp); whole class (apps/viewer/src/battle_source_replay.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); source-bearing map placements (src/skirmish/start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | FW-24; FW-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WNO-16, WTA-29; WLPA-07, WLPA-09; WSW-01; WLPA-03, WLPA-07, WLPA-08 | 1450 | apps/viewer/src/battle_source_local.cpp; apps/viewer/src/battle_source_replay.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/land_start.cpp; src/skirmish/start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Custom_Soft_Footprint_Radius | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| Space_Obstacle_Radius | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| SpaceBehavior | applied | whole class (apps/viewer/src/battle_source_local.cpp); whole class (apps/viewer/src/battle_source_replay.cpp); whole class (src/scene/idle_tags.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); source-bearing map placements (src/skirmish/start.cpp) | FW-24; FW-24; BP-31, R-ROT-01, RO-1; WNO-16, WTA-29; WSW-01 | — | apps/viewer/src/battle_source_local.cpp; apps/viewer/src/battle_source_replay.cpp; src/scene/idle_tags.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/start.cpp | basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | Map obstacle types (load_obstacle) take their layer footprint from it; every placed object class shares that path.; basis: reviewed |
| Space_Obstacle_Radius | applied | whole class (src/units/unit_motion.cpp) | AV-05 | — | src/units/unit_motion.cpp | Map obstacle types (load_obstacle) read the radius into their footprint; every placed object class shares that path.; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/presentation/live/live_units.cpp); missing: other galactic behavior dispatch | WAR-24; FW-24 | 4532 | src/campaign/auto_resolve_strength.cpp; src/presentation/live/live_units.cpp | Skirmish_Merchant_Dock contributes inherited SpaceStructure metadata to the M2 corpus, but no read of this class's authored SpaceBehavior is traced. Applied consumers on the effective live object classes are unchanged. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/Turn_Rate | todo | application not recorded | — | 760 | — | family: abilities; no loader or consumer for this ability/tag in a tactical battle.; basis: reviewed |
| Avoid_Enemy_Exclusion_Range | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Behavior | partial | session-owned map placements (apps/viewer/src/battle_source_local.cpp); session-owned map placements (apps/viewer/src/battle_source_replay.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); craft, ship (src/sim/tactical/session_step_systems.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); craft, ship (src/units/unit_motion.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); SpaceUnit (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | FW-24; FW-24; WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WLPA-07, WLPA-09; WHZ-20; WLPA-03, WLPA-07, WLPA-08; WSU-15, WSU-19, WSU-21 | 1450 | apps/viewer/src/battle_source_local.cpp; apps/viewer/src/battle_source_replay.cpp; src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/land_start.cpp; src/units/unit_motion.cpp; src/units/unit_tables_land.cpp; src/units/unit_tables_profiles.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Space_Layer | applied | ship (src/units/unit_motion.cpp) | AV-01, AV-21, MV-01 | — | src/units/unit_motion.cpp | &#35;853 reassessment: craft do not author this tag, so the check has no craft baseline node. The debug build keeps their default of no layer (AV-21); motion_table excludes craft from ship profiles and footprints, preserving that default. Authored ship layers remain applied (AV-01, MV-01); no missing craft layer implementation was found.; AV-21; basis: reviewed |
| SpaceBehavior | partial | session-owned map placements (apps/viewer/src/battle_source_local.cpp); session-owned map placements (apps/viewer/src/battle_source_replay.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); craft, ship (src/sim/tactical/session_step_systems.cpp); craft, ship (src/units/unit_motion.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); SpaceUnit (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | FW-24; FW-24; WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-SQ-02; WSU-15, WSU-19, WSU-21 | 4532 | apps/viewer/src/battle_source_local.cpp; apps/viewer/src/battle_source_replay.cpp; src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Squadron_Capacity | applied | direct non-container galactic members (src/campaign/fleets.cpp) | WGM-24, WGM-87 | — | src/campaign/fleets.cpp | basis: reviewed |
| Strafe_Distance | applied | craft (src/units/unit_motion.cpp) | FA-01 | — | src/units/unit_motion.cpp | basis: auto |
| TSW_Post_Destruction_Wait_Frames | todo | application not recorded | — | 649 | — | family: abilities; no loader or consumer for this ability/tag in a tactical battle.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Stop_When_Activated | todo | application not recorded | — | 760 | — | family: abilities; no loader or consumer for this ability/tag in a tactical battle.; basis: reviewed |

## SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | todo | application not recorded | — | 760 | — | family: abilities; parsed into the redirect handler but the flag is never read by the combat step (only content identity).; basis: reviewed |

## SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Elevate_Extent_Degrees | applied | whole class (src/units/unit_combat.cpp) | DG-24, OW-02, OW-03, W-09 | — | src/units/unit_combat.cpp | basis: auto |
| Turret_Rotate_Extent_Degrees | applied | whole class (src/units/unit_combat.cpp) | DG-24, OW-02, OW-03, W-09 | — | src/units/unit_combat.cpp | basis: auto |
| Unit_Abilities_Data/Unit_Ability/Disable_Movement | partial | INTERDICT and MISSILE_SHIELD (src/sim/tactical/abilities.cpp); INTERDICT and MISSILE_SHIELD (src/sim/tactical/session_step_movement.cpp); INTERDICT and MISSILE_SHIELD (src/sim/tactical/session_tracking.cpp); missing: other ability kinds | WIN-02, WMS-02; WIN-02, WMS-02; WIN-02, WMS-02 | 760 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_movement.cpp; src/sim/tactical/session_tracking.cpp | INTERDICT and MISSILE_SHIELD apply the authored deployment movement lock; other ability kinds remain pending.; basis: reviewed |

## SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Hover_Offset | partial | prepared campaign space entry (src/campaign/space_entry.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-19 | 2006 | src/campaign/space_entry.cpp |  Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-09, WHE-10; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Bank_Turn_Angle | applied | whole class (src/units/unit_motion.cpp) | BK-01, FM-03 | — | src/units/unit_motion.cpp | basis: auto |
| Begin_Turn_Towards_Distance | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| Collision_Box_Modifier | applied | whole class (src/units/unit_combat.cpp) | DG-31, DG-37 | — | src/units/unit_combat.cpp | basis: auto |
| Custom_Footprint_Radius | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Speed_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Max_Lift | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | FM-04, FM-06 | — | src/units/unit_motion.cpp | WMV-40; ship: debug build: ordinary layered ship path flight ignores Max_Thrust and Max_Lift; craft flight consumes them (docs/behaviour/walks/movement.md WMV-40). Max_Thrust also has an optional Keep_Moving_In_Battle attack-positioning consumer, retained as an explicit gap.; basis: reviewed |
| Max_Rate_Of_Roll | applied | whole class (src/units/unit_motion.cpp) | BK-01, C-18, C-19, FM-03 | — | src/units/unit_motion.cpp | basis: auto |
| Min_Speed | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | FM-01, FO-11 | — | src/units/unit_motion.cpp | WMV-41; ship: debug build: ordinary layered ship path flight has no Min_Speed floor; the coordinator fetches it but uses minimum-speed ratios only in its no-layer formation merge branch (docs/behaviour/walks/movement.md WMV-41).; basis: reviewed |
| Min_Speed_Fraction_For_Turn | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |
| OverrideAcceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); ship (src/units/unit_motion.cpp); missing: SpaceUnit objects outside adapted land companies, UniqueUnit objects outside adapted land companies; craft: foc-ignores | MV-01; MV-01, WMV-42 | 1450 | include/eawr/skirmish/land_content.hpp; src/units/unit_motion.cpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; craft: debug build: craft flight uses Max_Thrust rather than OverrideAcceleration or OverrideDeceleration; ordinary ship flight consumes the overrides (docs/behaviour/walks/movement.md WMV-42). OverrideDeceleration also bounds no-layer formation correction using member-type data; its formation default is zero with no ordinary-flight fallback.; basis: reviewed |
| OverrideDeceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); craft (src/sim/tactical/fighters_formation.cpp); ship (src/units/unit_motion.cpp); missing: SpaceUnit objects outside adapted land companies, UniqueUnit objects outside adapted land companies | MV-01; WMV-42, WMV-44; MV-01, WMV-42 | 1450 | include/eawr/skirmish/land_content.hpp; src/sim/tactical/fighters_formation.cpp; src/units/unit_motion.cpp | WMV-42/44: ordinary craft flight still uses Max_Thrust; no-layer formation upkeep applies the member type deceleration through its own squadron profile, including standalone Millennium_Falcon. Squadron containers never inherit a child craft override here. Absent or zero values cap positive correction at formation speed. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Space_Full_Stop_Command | todo | application not recorded | — | 649 | — | family: movement; no loader or consumer in the space motion code.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Proxy_Type | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| SecondaryOccupationPassability | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| UnitCollisionClass | partial | land battle objects (include/eawr/units/land_companies.hpp); missing: land battle consumers (see note) | WLPA-73 | 3776 | include/eawr/units/land_companies.hpp | class read and ordered like Mobile_Defense_Unit; bunker-specific use not pinned; basis: reviewed |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Move_Response_Range | applied | squadron (src/units/unit_motion.cpp) | OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| Autonomous_Move_Extension_Vs_Attacker | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: historical escort threat | WMV-17, WTA-29 | 4532 | src/sim/tactical/session_step_combat.cpp | WTA-29 applies the Squadron purchase type extension to accepted self-damage and same-frame escort-damage notifications. Historical escort threats await the general damage-tracking interface. Other object-class loaders retain their existing movement coverage boundary.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); squadron (src/sim/tactical/session_step_systems.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); squadron (src/units/unit_motion.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WLPA-07, WLPA-09; WHZ-20; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/land_start.cpp; src/units/unit_motion.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Guard_Chase_Range | applied | squadron (src/units/unit_motion.cpp) | FT-02, OR-11, OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); squadron (src/sim/tactical/session_step_systems.cpp); squadron (src/units/unit_motion.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20 | 4532 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); station (src/sim/tactical/session_step_systems.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); station (src/units/unit_motion.cpp); station (src/units/unit_motion.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); StarBase (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WLPA-07, WLPA-09; WSL-26; WHZ-20; WLPA-03, WLPA-07, WLPA-08; WSU-15, WSU-19, WSU-21 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/land_start.cpp; src/units/unit_motion.cpp; src/units/unit_tables_land.cpp; src/units/unit_tables_profiles.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Space_Layer | applied | station (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Stations submit static-layer footprints (AV-01).; CHECK-842; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); station (src/sim/tactical/session_step_systems.cpp); station (src/units/unit_motion.cpp); station (src/units/unit_motion.cpp); StarBase (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WSL-26; WHZ-20; WSU-15, WSU-19, WSU-21 | 4532 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## TacticalCamera

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Pitch_Default | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Max | applied | whole class (src/presentation/camera/camera.cpp) | RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Min | applied | whole class (src/presentation/camera/camera.cpp) | R-ROT-01, RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Spline | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-02 | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: reviewed |
| Tactical_Overview_Pitch | applied | whole class (src/presentation/camera/overview.cpp) | V-2 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Pitch2 | applied | whole class (src/presentation/camera/overview.cpp) | V-3 | — | src/presentation/camera/overview.cpp | basis: auto |
| Yaw_Default | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Yaw_Max | applied | whole class (src/presentation/camera/camera.cpp) | RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |
| Yaw_Min | applied | whole class (src/presentation/camera/camera.cpp) | RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |

## Terrain_Effectiveness

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */* | partial | live galactic auto-resolve (src/campaign/auto_resolve_live.cpp); missing: other tactical consumers | WAR-35 | 1965 | src/campaign/auto_resolve_live.cpp |  Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter.; basis: reviewed |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bank_Turn_Angle | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Custom_Hard_XExtent | partial | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 650 | include/eawr/skirmish/land_content.hpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry. |
| Custom_Hard_YExtent | partial | land company member landing hard footprints (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 650 | include/eawr/skirmish/land_content.hpp | Positive authored hard extents or model collision extents are scaled for land landing slots; zero additional formation spacing retains this hard geometry. |
| Layer_Z_Adjust | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Max_Rate_Of_Roll | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Max_Rate_Of_Turn | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Max_Speed | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Min_Speed | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| Space_Layer | todo | application not recorded | — | 650 | — | family: other; transport hulls are not deployed as space bodies in a skirmish (WHE-07/49 keep them as metadata); the generic body loader would apply it.; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/presentation/live/live_units.cpp); missing: other galactic behavior dispatch | WAR-24; FW-24 | 4532 | src/campaign/auto_resolve_strength.cpp; src/presentation/live/live_units.cpp | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| TacticalBehavior | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Concentrate_Fire_Attack_Ability/Target_Speed_Decrease_Percent | applied | concentrate-fire targets (src/sim/tactical/session_abilities.cpp); concentrate-fire activation and manual release (src/sim/tactical/session_cmd_abilities.cpp) | WHE-24, WHE-25; FD-01, WHE-25 | — | src/sim/tactical/session_abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | The negative authored fraction joins the shared source/category speed reduction and is removed on expiry or source deletion; stock Home One authors zero. Nonzero activation and manual release use the existing command replan transaction even for targets without abilities; synthetic expiry/release cases cover 1/2/4/8 workers.; basis: reviewed |
| Apply_Y_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Apply_Z_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Attack_Move_Response_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ship (src/units/unit_motion.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WLPA-07, WLPA-09; WHZ-20; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/land_start.cpp; src/units/unit_motion.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Close_Enough_Angle_For_Move_Start | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Collidable_By_Projectile_Dead | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| FormationGrouping | todo | application not recorded | — | 3776 | — | no land reader of FormationGrouping found on origin/main; basis: reviewed |
| Guard_Chase_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Movement_Directions_Count | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| MovementBoxExpansionFactor | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| MovementPredictionInterval | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-SQ-02 | 4532 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_decode.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Squadron_Capacity | partial | direct non-container galactic members (src/campaign/fleets.cpp); missing: tactical fighter-capacity consumer | WGM-24, WGM-87 | 651 | src/campaign/fleets.cpp | basis: reviewed |
| Stopped_Rate_Of_Turn | todo | application not recorded | — | 3776 | — | read and required as a tag in land content (unit_tables_land.cpp:183 land_content.hpp:298) but no motion consumer pinned; basis: reviewed |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
