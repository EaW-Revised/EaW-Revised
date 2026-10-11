# movement tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 132 | 132 |
| partial | 112 | 112 |
| todo | 129 | 129 |
| presentation-later | 0 | 0 |
| foc-ignores | 24 | 24 |
| deferred | 16 | 16 |
| land-or-galactic | 0 | 77 |
| multiplayer | 0 | 0 |
| **Total** | **413** | **490** |

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
| Base_Layer | todo | application not recorded | — | 649 | — | a data loader reads it; where the value goes is not traced |
| Drag_And_Drop | todo | application not recorded | — | 649 | — | a data loader reads it; where the value goes is not traced |
| Drag_Back | todo | application not recorded | — | 649 | — | a data loader reads it; where the value goes is not traced |
| No_Hidden_Collision | todo | application not recorded | — | 649 | — | a data loader reads it; where the value goes is not traced |
| Snap_Drag | todo | application not recorded | — | 649 | — | a data loader reads it; where the value goes is not traced |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); Container (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WSU-15, WSU-19, WSU-21 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| LandBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); missing: other galactic behavior dispatch | WAR-24 | 679 | src/campaign/auto_resolve_strength.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Layer_Z_Adjust | partial | Galactic_Fleet container marker (apps/viewer/src/galactic/galactic_scene.cpp); missing: Other container presentation paths remain outside the galactic fleet marker. | WGP-16 | 650 | apps/viewer/src/galactic/galactic_scene.cpp | The Galactic_Fleet marker adds the authored Layer_Z_Adjust to the model position; other container paths are not claimed.; basis: reviewed |
| Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| Max_Speed | partial | craft (src/units/unit_motion.cpp); missing: ship | WMV-44 | 649 | src/units/unit_motion.cpp | WMV-44: resolved Create_Team_Type maximum supplies the member formation deceleration ratio; ordinary team-container maximum velocity and land consumers remain open under &#35;649. |
| Min_Speed | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| MovementPredictionInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| Override_Acceleration | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Override_Deceleration | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| OverrideAcceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| OverrideDeceleration | partial | craft (src/sim/tactical/fighters.cpp); missing: ship | WMV-44 | 649 | src/sim/tactical/fighters.cpp | WMV-44: space formation upkeep uses the resolved Create_Team_Type container default or mod-authored override, never purchase-template or child data; ordinary team-container and land consumers remain open under &#35;649.; SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Space_Layer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); Container (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-SQ-02; WSU-15, WSU-19, WSU-21 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |

## Container, GenericHeroUnit, HeroUnit, Marker, Projectile, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Mass | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion (+14 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite_Laser, Defense_Satellite_Laser_Small, Defense_Satellite_Missile, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks (+46 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite_Laser, Defense_Satellite_Laser_Small, Defense_Satellite_Missile, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks (+46 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/Chase_Radius | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-73 | 1884 | src/sim/tactical/session_step_combat.cpp | Victim search around the creation point; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Attack_Move_Response_Range | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |
| Guard_Chase_Range | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Container, SpaceUnit, Squadron, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Formation_Priority | todo | application not recorded | — | 649 | — | — |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| FormationOrder | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Container, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Hyperspace_Speed | partial | eligible live galactic leaf members, squadron template packing keys (src/campaign/fleets.cpp); missing: fleet-container authored speeds; live child speeds are used for fleet travel | WGM-118, WGM-151, WGM-26 | 680 | src/campaign/fleets.cpp | basis: reviewed |
| MovementClass | partial | live galactic auto-resolve (src/campaign/auto_resolve_live.cpp); missing: other tactical consumers | WAR-35 | 1965 | src/campaign/auto_resolve_live.cpp | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter.; basis: reviewed |

## Container, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autonomous_Move_Extension_Vs_Attacker | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Random_Rotate | todo | application not recorded | — | 649 | — | — |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Benefits_From_Corruption | partial | galactic movement (src/campaign/fleets.cpp); live corruption reward beneficiary and ownership classification (src/sim/galactic/ability_corruption.cpp); galactic fiscal income (src/sim/galactic/economy_corruption.cpp); missing: other corruption mechanics | WGM-27, WGM-33; WGCH-161, WGCH-165, WGCH-168, WGM-195; WGEP-04, WGEP-13 | 1130 | src/campaign/fleets.cpp; src/sim/galactic/ability_corruption.cpp; src/sim/galactic/economy_corruption.cpp | basis: reviewed |
| Hyperspace_Speed_Factor | applied | galactic departure (src/campaign/fleets.cpp) | WGM-26 | — | src/campaign/fleets.cpp | basis: reviewed |
| Scatters_From_Crushers | todo | application not recorded | — | 649 | — | — |
| Space_Forced_Retreat_Due_To_Superweapon | todo | application not recorded | — | 649 | — | — |
| Space_Retreat_Countdown_Seconds | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WR-44 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Retreat_Flight_Increment | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Space_Retreat_Flight_Move_Increment | todo | application not recorded | — | 649 | — | — |
| Space_Retreat_Off_Map_Dest_Pos | todo | application not recorded | — | 649 | — | — |
| Space_Retreat_Pursue_Max_Speed_Mod_Factor | todo | application not recorded | — | 649 | — | — |
| Space_Retreat_Unit_Increment_Wait_Frames | todo | application not recorded | — | 649 | — | — |
| Space_Retreat_Units_Damaged_Mod_Factor | todo | application not recorded | — | 649 | — | — |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Rotate_For_Space_Targeting | applied | whole class (src/sim/tactical/combat_aim.cpp) | A-05 | — | src/sim/tactical/combat_aim.cpp | basis: reviewed |
| Base_Shield_Speed_Modifier | land-or-galactic | application not recorded | — | — | — | debug build: read only by the object-type speed modifier for a base-shielded object; the only stock base shields are ground structures |
| BetweenFormationSpacing | deferred | application not recorded | — | 626 | — | — |
| CloseEnoughAngleForMoveStart | todo | application not recorded | — | 649 | — | — |
| Corruption_Hyperspace_Bonus | applied | galactic travel (src/campaign/fleets.cpp) | WGM-27 | — | src/campaign/fleets.cpp | basis: reviewed |
| Crouch_Move_Fire_Angle_Cutoff | land-or-galactic | application not recorded | — | — | — | debug build: read only by infantry weapon service (crouched move-and-fire angle) |
| Destination_Collision_Query_Extension | deferred | application not recorded | — | 626 | — | — |
| DestinationSearchRadiusIncrementSpace | applied | whole class (src/units/unit_motion.cpp) | AV-05 | — | src/units/unit_motion.cpp | basis: auto |
| DoubleClickMoveMaxSpeedRatio | todo | application not recorded | — | 649 | — | — |
| DynamicAvoidanceRectangleBound | deferred | application not recorded | — | 626 | — | — |
| DynamicLandComplexityQuota | land-or-galactic | application not recorded | — | — | — | space-movement.md: the land game's per-frame path quota |
| DynamicObstacleOverlapPenalty | deferred | application not recorded | — | 626 | — | — |
| Engines_Disabled_Speed_Modifier | applied | whole class (src/sim/tactical/durability.cpp) | AB-16, FM-09a | — | src/sim/tactical/durability.cpp | basis: auto |
| FinalFacing180Penalty | todo | application not recorded | — | 649 | — | — |
| FinalFormationFacingDeltaCoefficient | deferred | application not recorded | — | 626 | — | — |
| FinalFormationFacingMinimumAngle | deferred | application not recorded | — | 626 | — | — |
| FormationMaximumSideError | partial | space fighter and bomber squadron move lanes (src/sim/tactical/fighters_formation.cpp); missing: land movement formations | FO-10, WSQ-17 | 654 | src/sim/tactical/fighters_formation.cpp | basis: reviewed |
| FormationMinimumSideError | partial | space fighter and bomber squadron move lanes (src/sim/tactical/fighters_formation.cpp); missing: land movement formations | FO-10, WSQ-17 | 649 | src/sim/tactical/fighters_formation.cpp | basis: reviewed |
| FramesPerCollisionCheck | deferred | application not recorded | — | 626 | — | — |
| Hyperspace_Speed_Factor_Empire | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Hyperspace_Speed_Factor_Rebel | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| MatchFacingDeltaSpace | todo | application not recorded | — | 649 | — | — |
| Max_Formation_Area | deferred | application not recorded | — | 626 | — | — |
| Max_Move_Frame_Delay | land-or-galactic | application not recorded | — | — | — | debug build: read only by the formation initialisation and the land team locomotor; the random move delay of team members does not apply in space (docs/behaviour/space-movement.md) |
| MaxObstacleCostSpace | deferred | application not recorded | — | 626 | — | — |
| MaxRotationsSpace | applied | whole class (src/units/unit_motion.cpp) | S-12 | — | src/units/unit_motion.cpp | basis: auto |
| MaxWaypointsPerPath | partial | space queued point movement (src/sim/tactical/session_step_commands.cpp); space queued point movement (src/units/unit_motion.cpp); missing: pre-existing formation reuse and exact arrival phase | HCB-58, HCB-59; HCB-58 | 2400 | src/sim/tactical/session_step_commands.cpp; src/units/unit_motion.cpp | Queue capacity applies to retained point movement; the named completion and formation seams remain tracked.; basis: reviewed |
| MinimumDragDistance | todo | application not recorded | — | 649 | — | — |
| MinimumStoppedVsStoppedOverlapCoefficient | todo | application not recorded | — | 649 | — | — |
| MovementReevaluationFrameCount | applied | whole class (src/units/unit_motion.cpp) | OR-06 | — | src/units/unit_motion.cpp | basis: reviewed |
| MovingVsMovingLookAheadTime | todo | application not recorded | — | 649 | — | — |
| Object_Max_Speed_Multiplier_Galactic | applied | initialized galactic movement (src/campaign/fleets.cpp) | WGM-97 | — | src/campaign/fleets.cpp | basis: reviewed |
| Object_Max_Speed_Multiplier_Space | applied | whole class (src/units/unit_motion.cpp) | BK-01, FM-01 | — | src/units/unit_motion.cpp | basis: auto |
| ObstacleAreaOverlapForMaxSpace | deferred | application not recorded | — | 626 | — | — |
| OccupationRadiusCoefficientSpace | applied | whole class (src/units/unit_motion.cpp) | AV-05, AV-13 | — | src/units/unit_motion.cpp | basis: auto |
| Preferred_Pathfinder_Types | land-or-galactic | application not recorded | — | — | — | debug build: read only by the pathfinder-unit choice for formations; the stock list names ground units only |
| Push_Scroll_Speed_Modifier | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| RepushDistance | todo | application not recorded | — | 649 | — | — |
| Rotate_Formation_Facing_Moves | deferred | application not recorded | — | 626 | — | — |
| Short_Range_Attack_Formation_Coefficient | deferred | application not recorded | — | 626 | — | — |
| Space_Guard_Range | applied | whole class (src/units/unit_motion.cpp) | OR-14 | — | src/units/unit_motion.cpp | basis: reviewed |
| Space_Reinforcement_Collision_Check_Distance | applied | whole class (src/sim/tactical/session_economy.cpp) | WR-25 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Space_Retreat_Allowed_Countdown_Seconds | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WR-43 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Retreat_Attrition_Factor | partial | campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WGT-36 | 1116 | src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services.; basis: reviewed |
| Space_Station_Destruction_Forces_Retreat | applied | whole class (src/campaign/space_rules.cpp); whole class (src/sim/tactical/session_step_finish.cpp) | WGM-51, WGT-29; WGM-51, WGT-29 | — | src/campaign/space_rules.cpp; src/sim/tactical/session_step_finish.cpp | Campaign station-loss condition is independent of the lobby all-unit condition.; basis: reviewed |
| SpaceIdlePathCullCoefficient | deferred | application not recorded | — | 626 | — | — |
| SpaceLocomotorFacingLookaheadAcc | todo | application not recorded | — | 649 | — | — |
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
| Spread_Out_Spacing_Coefficient | land-or-galactic | application not recorded | — | — | — | debug build: read only by the land team locomotor (team member spacing) |
| Strategic_Max_Scroll_Speed | land-or-galactic | application not recorded | — | — | — | debug build: read only by the strategic (galactic map) camera scroll-speed calculation |
| Strategic_Min_Scroll_Speed | land-or-galactic | application not recorded | — | — | — | debug build: read only by the strategic (galactic map) camera scroll-speed calculation |
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
| Attack_Move_Response_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| FormationOrder | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| FormationRaggedness | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel) |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Guard_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| OverrideAcceleration | applied | ship (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| OverrideDeceleration | applied | ship (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| MovementClass | partial | live galactic auto-resolve (src/campaign/auto_resolve_live.cpp); missing: other tactical consumers | WAR-35 | 1965 | src/campaign/auto_resolve_live.cpp |  Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpecialStructure, TechBuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_step_commands.cpp); source-bearing map placements (src/skirmish/start.cpp); missing: other galactic behavior dispatch | WAR-24; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WSW-08, WSW-09, WSW-10, WSW-11; WSW-01 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_commands.cpp; src/skirmish/start.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); missing: other galactic behavior dispatch | WAR-24; none recorded | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## GenericHeroUnit, MiscObject, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, Projectile, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Rate_Of_Turn | applied | direct space projectiles, MISSILE (src/sim/tactical/projectiles.cpp); whole class (src/units/unit_combat.cpp) | WPJ-12, WPJ-17; FM-03, MV-01, MV-20, S-26, S-27 | — | src/sim/tactical/projectiles.cpp; src/units/unit_combat.cpp | basis: auto |
| Max_Speed | applied | whole class (src/units/unit_combat.cpp) | FA-01, FA-04, W-10 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | AV-05, WSQ-45; A-07, OR-04; none recorded | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto |
| Custom_Hard_YExtent | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | AV-05, WSQ-45; A-07, OR-04; none recorded | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto |

## GenericHeroUnit, SecondaryStructure, SpaceBuildable, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Soft_Footprint_Radius | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto |

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
| Turret_Rotation_Offset | partial | one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: nonmanual or other manual burst hardpoints | MC-06, WAD-40 | 1075 | src/sim/tactical/combat_aim.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Elevate_Extent_Degrees | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17; MC-06, WAD-40 | 1172 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; basis: reviewed |
| Turret_Rotate_Extent_Degrees | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17; MC-06, WAD-40 | 1172 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rotate_Speed | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); missing: non-turret objects and unsupported manual burst hardpoints | UA-05, WWP-17; MC-06, WAD-40 | 1075 | src/sim/tactical/combat_aim.cpp | Manual assignment, player clock and mechanical fire frames retain their existing rules; UA-05 ordinary TURRET space objects also apply mechanical readiness and viewer articulation.; basis: reviewed |

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
| Abilities/Berserker_Ability/Berserker_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Retreat_Prevention_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Alternate_Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prisoner (+5 more)) |
| Alternate_Max_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prisoner (+5 more)) |
| Attack_Move_Response_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+22 more)) |
| Autonomous_Move_Extension_Vs_Attacker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, General_Veers (+18 more)) |
| Custom_Hard_XExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Custom_Hard_YExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Custom_Soft_Footprint_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| FormationGrouping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| FormationOrder | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0, Droid_R2D2, Kyle_Katarn, Mara_Jade) |
| FormationRaggedness | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_C3P0, Droid_R2D2, Han_Solo, Jabba_The_Hutt, Kyle_Katarn, Mara_Jade) |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| Guard_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+22 more)) |
| Hover_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prisoner (+5 more)) |
| Max_Lift | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prisoner (+5 more)) |
| Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Max_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Min_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+19 more)) |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| OverrideAcceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| OverrideDeceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| Space_Layer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+22 more)) |
| Uses_Multiple_Locomotors | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prisoner (+5 more)) |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent_Offset | partial | craft, ship, station (src/sim/tactical/session_state.cpp); missing: movement | V-21 | 649 | src/sim/tactical/session_state.cpp | Applied to multisample fog boxes; movement tracking offsets remain pending.; basis: reviewed |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Rotations_Per_Second | todo | application not recorded | — | 649 | — | — |

## Marker, MiscObject, Mobile_Defense_Unit, Projectile, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | applied | whole class (src/scene/idle_tags.cpp) | none recorded | — | src/scene/idle_tags.cpp | basis: auto |

## Marker, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | applied | Projectile (apps/viewer/src/battle_effects_prepare.cpp); whole class (src/scene/scene_build.cpp) | WPJ-38; C-05, D-04, L-05, L-23, L-43 | — | apps/viewer/src/battle_effects_prepare.cpp; src/scene/scene_build.cpp | basis: auto |

## Marker, Projectile, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Dead | todo | application not recorded | — | 649 | — | — |

## Marker, SecondaryStructure, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Apply_Y_Turret_Rotate_To_Axis | todo | application not recorded | — | 649 | — | — |
| Apply_Z_Turret_Rotate_To_Axis | todo | application not recorded | — | 649 | — | — |

## Marker, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. |
| Obstacle_Width | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. |
| Obstacle_X_Offset | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. |
| Obstacle_Y_Offset | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Mining_Facility, Ground_Grand_Arena, Ground_Gravity_Generator, Hutt_Ground_Base_Shield, P_Ground_Base_Shield_Small (+10 more). Mixed ground/space, space carriers on UniqueUnit: Tyber_Zann_Boarding_Shuttle_Prologue, Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Avoidance_Disabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |
| Behavior | applied | whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_step_commands.cpp); source-bearing map placements (src/skirmish/start.cpp) | C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WSW-08, WSW-09, WSW-10, WSW-11; WSW-01 | — | src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_commands.cpp; src/skirmish/start.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers. HCB-12a..12f: Multiplayer_Beacon_Type objects author MULTIPLAYER_BEACON in Behavior. Their agreed setup consumes only the resolved beacon type and Beacon_Lifetime_In_Secs; Affiliation, Behavior and Tactical_Health are not beacon inputs. Other MiscObject consumers retain their application and trace requirements.; basis: reviewed |
| Mass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |
| SpaceBehavior | applied | whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); source-bearing map placements (src/skirmish/start.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp) | BP-31, R-ROT-01, RO-1; WCC-25; WSW-01; WAD-38 | — | src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/skirmish/start.cpp; src/units/unit_tables_decode.cpp | basis: reviewed |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |

## MiscObject, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | applied | whole class (src/skirmish/start.cpp) | LZ-01, LZ-02 | — | src/skirmish/start.cpp | &#35;718: every start object is created at its placement point raised by its type's height; basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Dead | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| DeployedBehavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| FormationGrouping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| FormationOrder | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| FormationRaggedness | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Mass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| MaxSecondaryTurnROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| MinSecondaryTurnROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| OverrideAcceleration | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| OverrideDeceleration | applied | whole class (src/units/unit_motion.cpp) | MV-01 | — | src/units/unit_motion.cpp | basis: auto |
| SecondaryTurnAngle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SecondaryTurnInPlaceROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SecondaryTurnLookaheadDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| UseSecondaryFacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |

## Mobile_Defense_Unit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | applied | whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); held skirmish upgrades with DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED (src/sim/tactical/session_step_commands.cpp); source-bearing map placements (src/skirmish/start.cpp) | C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WSW-08, WSW-09, WSW-10, WSW-11; WSW-01 | — | src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_commands.cpp; src/skirmish/start.cpp | WCC-25: SPECIAL_WEAPON and DUMMY_STAR_BASE admission uses Behavior for loaded unit and structure candidates; projectiles, particles and map markers retain their scene or presentation consumers.; basis: reviewed |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Facing_Adjust | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Avoidance_Disabled | todo | application not recorded | — | 649 | — | — |
| Projectile_Acceleration_Per_Frame | todo | application not recorded | — | 649 | — | — |
| Projectile_Bomb_Fall_Accel_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_TIE_Bomber_Run_Bomb, Skipray_Bombing_Run_Bomb, TIE_Bomber_Bombing_Run_Bomb, Y_Wing_Bombing_Run_Bomb) |
| Projectile_Grenade_Sticks_On_Collision | todo | application not recorded | — | 649 | — | — |
| Projectile_Ion_Stun_Speed_Reduction_Percent | applied | whole class (src/units/unit_combat.cpp) | DG-01, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Space_Layer | todo | application not recorded | — | 649 | — | — |
| SpaceBehavior | todo | application not recorded | — | 650 | — | The M2 projectile loader for Proj_Plasma_Space_Turret_Blast reads projectile payloads without an idle/SpaceBehavior service. Its authored list is unread; unit movement and map-prop consumers remain separate. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Plasma_Space_Turret_Blast, Proj_Ship_Krayt_Megaweapon_Damage, Proj_Ship_Krayt_Megaweapon_Ion, Proj_Ship_Krayt_Special_Megaweapon_Damage, Proj_Underworld_Station_Main_Cannon. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Plasma_Space_Turret_Blast, Proj_Ship_Krayt_Megaweapon_Damage, Proj_Ship_Krayt_Megaweapon_Ion, Proj_Ship_Krayt_Special_Megaweapon_Damage, Proj_Underworld_Station_Main_Cannon. |

## Projectile, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| UnitCollisionClass | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more). Mixed ground/space, space carriers on UniqueUnit: Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. Mixed ground/space, space carriers on UniqueUnit: Underworld_Vehicle_Transport_Cinematic_Landing, Underworld_Vehicle_Transport_Landing_Saleucami_CIN. |

## SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Obstacle_Offset | partial | loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); loaded space footprints (src/units/unit_tables_decode.cpp); missing: dense-fog raw-offset consumer, objects outside loaded tactical closure | WHZ-05; WHZ-01 | 649 | src/sim/tactical/pathfind.cpp; src/units/unit_tables_decode.cpp | Tracking applies raw yaw-rotated XY without scale; the dense-fog geometry consumer remains separate. The footprint reader retains authored offsets; selected-map SpaceProp offsets are outside the pinned M2 table closure and recorded trace.; basis: reviewed; trace: unrecorded |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); source-bearing map placements (src/skirmish/start.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WSW-01; WAD-38 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/skirmish/start.cpp; src/units/unit_tables_decode.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## SFXEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Pitch | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Min_Pitch | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Behavior | applied | whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); SpaceBuildable (src/sim/tactical/session_economy.cpp); SpaceBuildable (src/units/unit_tables_profiles.cpp) | C-05, D-04, L-05, L-23, L-43; WCC-25; WBP-30, WBP-31, WBP-32; WSU-15, WSU-19, WSU-21 | — | src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_economy.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| SpaceBehavior | applied | whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); SpaceBuildable (src/sim/tactical/session_economy.cpp); SpaceBuildable (src/units/unit_tables_profiles.cpp) | BP-31, R-ROT-01, RO-1; WCC-25; WBP-30, WBP-31, WBP-32; WSU-15, WSU-19, WSU-21 | — | src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_economy.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| Turret_Rotate_Speed | partial | space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17 | 1172 | src/sim/tactical/combat_aim.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; basis: reviewed |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | todo | application not recorded | — | 649 | — | — |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | todo | application not recorded | — | 649 | — | — |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | applied | whole class (apps/viewer/src/live_session_prepare.cpp); whole class (src/scene/scene_build.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); source-bearing map placements (src/skirmish/start.cpp) | FW-24; C-05, D-04, L-05, L-23, L-43; WNO-16, WTA-29; WSW-01 | — | apps/viewer/src/live_session_prepare.cpp; src/scene/scene_build.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/start.cpp | basis: reviewed |
| Custom_Soft_Footprint_Radius | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| Space_Obstacle_Radius | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_motion.cpp) | A-07, DG-23, OR-03, R-11, W-05, WWP-21, WWP-27; AV-05 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_motion.cpp | basis: auto; trace: unrecorded |
| SpaceBehavior | applied | whole class (apps/viewer/src/live_session_prepare.cpp); whole class (src/scene/idle_tags.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); source-bearing map placements (src/skirmish/start.cpp) | FW-24; BP-31, R-ROT-01, RO-1; WNO-16, WTA-29; WSW-01 | — | apps/viewer/src/live_session_prepare.cpp; src/scene/idle_tags.cpp; src/sim/tactical/session_step_combat.cpp; src/skirmish/start.cpp | basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Layer | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Space_Obstacle_Radius | todo | application not recorded | — | 649 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); missing: other galactic behavior dispatch | WAR-24 | 679 | src/campaign/auto_resolve_strength.cpp | Skirmish_Merchant_Dock contributes inherited SpaceStructure metadata to the M2 corpus, but no read of this class's authored SpaceBehavior is traced. Applied consumers on the effective live object classes are unchanged. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/Turn_Rate | todo | application not recorded | — | 760 | — | — |
| Avoid_Enemy_Exclusion_Range | todo | application not recorded | — | 649 | — | — |
| Behavior | partial | session-owned map placements (apps/viewer/src/live_session_prepare.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); craft, ship (src/sim/tactical/session_step_systems.cpp); craft, ship (src/units/unit_motion.cpp); SpaceUnit (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | FW-24; WAR-24; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WSU-15, WSU-19, WSU-21 | 679 | apps/viewer/src/live_session_prepare.cpp; src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Space_Layer | applied | ship (src/units/unit_motion.cpp) | AV-01, AV-21, MV-01 | — | src/units/unit_motion.cpp | &#35;853 reassessment: craft do not author this tag, so the check has no craft baseline node. The debug build keeps their default of no layer (AV-21); motion_table excludes craft from ship profiles and footprints, preserving that default. Authored ship layers remain applied (AV-01, MV-01); no missing craft layer implementation was found.; AV-21; basis: reviewed |
| SpaceBehavior | partial | session-owned map placements (apps/viewer/src/live_session_prepare.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); craft, ship (src/sim/tactical/session_step_systems.cpp); craft, ship (src/units/unit_motion.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); SpaceUnit (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | FW-24; WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-SQ-02; WSU-15, WSU-19, WSU-21 | 679 | apps/viewer/src/live_session_prepare.cpp; src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Squadron_Capacity | applied | direct non-container galactic members (src/campaign/fleets.cpp) | WGM-24, WGM-87 | — | src/campaign/fleets.cpp | basis: reviewed |
| Strafe_Distance | applied | craft (src/units/unit_motion.cpp) | FA-01 | — | src/units/unit_motion.cpp | basis: auto |
| TSW_Post_Destruction_Wait_Frames | todo | application not recorded | — | 649 | — | — |
| Unit_Abilities_Data/Unit_Ability/Stop_When_Activated | todo | application not recorded | — | 760 | — | — |

## SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |

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
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-09, WHE-10; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Bank_Turn_Angle | applied | whole class (src/units/unit_motion.cpp) | BK-01, FM-03 | — | src/units/unit_motion.cpp | basis: auto |
| Begin_Turn_Towards_Distance | todo | application not recorded | — | 649 | — | — |
| Collision_Box_Modifier | applied | whole class (src/units/unit_combat.cpp) | DG-31, DG-37 | — | src/units/unit_combat.cpp | basis: auto |
| Custom_Footprint_Radius | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Speed_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Max_Lift | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | FM-04, FM-06 | — | src/units/unit_motion.cpp | WMV-40; ship: debug build: ordinary layered ship path flight ignores Max_Thrust and Max_Lift; craft flight consumes them (docs/behaviour/walks/movement.md WMV-40). Max_Thrust also has an optional Keep_Moving_In_Battle attack-positioning consumer, retained as an explicit gap.; basis: reviewed |
| Max_Rate_Of_Roll | applied | whole class (src/units/unit_motion.cpp) | BK-01, C-18, C-19, FM-03 | — | src/units/unit_motion.cpp | basis: auto |
| Min_Speed | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | FM-01, FO-11 | — | src/units/unit_motion.cpp | WMV-41; ship: debug build: ordinary layered ship path flight has no Min_Speed floor; the coordinator fetches it but uses minimum-speed ratios only in its no-layer formation merge branch (docs/behaviour/walks/movement.md WMV-41).; basis: reviewed |
| Min_Speed_Fraction_For_Turn | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on UniqueUnit: V-wing_Fighter, V-wing_Fighter_E, V-wing_Fighter_P, V-wing_Fighter_R. Mixed ground/space, space carriers on UniqueUnit: V-wing_Fighter, V-wing_Fighter_E, V-wing_Fighter_P, V-wing_Fighter_R. |
| OverrideAcceleration | applied | ship (src/units/unit_motion.cpp); craft: foc-ignores | MV-01, WMV-42 | — | src/units/unit_motion.cpp | WMV-42; craft: debug build: craft flight uses Max_Thrust rather than OverrideAcceleration or OverrideDeceleration; ordinary ship flight consumes the overrides (docs/behaviour/walks/movement.md WMV-42). OverrideDeceleration also bounds no-layer formation correction using member-type data; its formation default is zero with no ordinary-flight fallback.; basis: reviewed |
| OverrideDeceleration | applied | craft (src/sim/tactical/fighters_formation.cpp); ship (src/units/unit_motion.cpp) | WMV-42, WMV-44; MV-01, WMV-42 | — | src/sim/tactical/fighters_formation.cpp; src/units/unit_motion.cpp | WMV-42/44: ordinary craft flight still uses Max_Thrust; no-layer formation upkeep applies the member type deceleration through its own squadron profile, including standalone Millennium_Falcon. Squadron containers never inherit a child craft override here. Absent or zero values cap positive correction at formation speed.; WMV-42; basis: reviewed |
| Space_Full_Stop_Command | todo | application not recorded | — | 649 | — | — |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Proxy_Type | todo | application not recorded | — | 649 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+35 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+35 more). |
| SecondaryOccupationPassability | todo | application not recorded | — | 649 | — | — |
| Space_Layer | applied | whole class (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Garrison_Bunker_Capturable, Garrison_Bunker_Empire, Garrison_Bunker_Rebel, Garrison_Bunker_Template, Garrison_Bunker_Underworld) |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Move_Response_Range | applied | squadron (src/units/unit_motion.cpp) | OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| Autonomous_Move_Extension_Vs_Attacker | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: historical escort threat | WMV-17, WTA-29 | 701 | src/sim/tactical/session_step_combat.cpp | WTA-29 applies the Squadron purchase type extension to accepted self-damage and same-frame escort-damage notifications. Historical escort threats await the general damage-tracking interface. Other object-class loaders retain their existing movement coverage boundary.; basis: reviewed |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); squadron (src/sim/tactical/session_step_systems.cpp); squadron (src/units/unit_motion.cpp); missing: other galactic behavior dispatch | WAR-24; C-05, D-04, L-05, L-23, L-43; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Guard_Chase_Range | applied | squadron (src/units/unit_motion.cpp) | FT-02, OR-11, OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); squadron (src/sim/tactical/session_step_systems.cpp); squadron (src/units/unit_motion.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); station (src/sim/tactical/session_step_systems.cpp); station (src/units/unit_motion.cpp); StarBase (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20, WSL-26; WSU-15, WSU-19, WSU-21 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Space_Layer | applied | station (src/units/unit_motion.cpp) | AV-01, S-12 | — | src/units/unit_motion.cpp | &#35;853 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Stations submit static-layer footprints (AV-01).; CHECK-842; basis: reviewed |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); station (src/sim/tactical/session_step_systems.cpp); station (src/units/unit_motion.cpp); StarBase (src/units/unit_tables_profiles.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20, WSL-26; WSU-15, WSU-19, WSU-21 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_profiles.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

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
| Bank_Turn_Angle | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); missing: other galactic behavior dispatch | WAR-24 | 679 | src/campaign/auto_resolve_strength.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Custom_Hard_XExtent | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Custom_Hard_YExtent | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Layer_Z_Adjust | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Max_Rate_Of_Roll | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Max_Rate_Of_Turn | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). |
| Max_Speed | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). |
| Min_Speed | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Space_Layer | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); missing: other galactic behavior dispatch | WAR-24 | 679 | src/campaign/auto_resolve_strength.cpp | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| TacticalBehavior | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Concentrate_Fire_Attack_Ability/Target_Speed_Decrease_Percent | partial | Home One concentrate fire (src/sim/tactical/session_abilities.cpp); missing: nonzero target speed decreases; overlapping speed debuffs remain unverified | WHE-24, WHE-25 | 939 | src/sim/tactical/session_abilities.cpp | Stock Home One authors zero; nonzero values fail validation rather than inventing an unverified speed policy.; basis: reviewed |
| Apply_Y_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Apply_Z_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Attack_Move_Response_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/scene_build.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); missing: other galactic behavior dispatch | WAR-24; C-05, D-04, L-05, L-23, L-43; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/scene_build.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Close_Enough_Angle_For_Move_Start | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Collidable_By_Projectile_Dead | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| FormationGrouping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Guard_Chase_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 649 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Movement_Directions_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| MovementBoxExpansionFactor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| MovementPredictionInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| SpaceBehavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/scene/idle_tags.cpp); loaded combat candidates with SPECIAL_WEAPON or DUMMY_STAR_BASE (src/sim/tactical/combat_targeting.cpp); loaded UNIT_AI space units and runtime team containers (src/sim/tactical/session_step_combat.cpp); ship (src/sim/tactical/session_step_systems.cpp); ship (src/units/unit_motion.cpp); loaded FIGHTER_LOCOMOTOR craft, including solo heroes (src/units/unit_tables_decode.cpp); missing: other galactic behavior dispatch | WAR-24; BP-31, R-ROT-01, RO-1; WCC-25; WNO-16, WTA-29; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-20; WHE-SQ-02 | 679 | src/campaign/auto_resolve_strength.cpp; src/scene/idle_tags.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_combat.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp; src/units/unit_tables_decode.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Squadron_Capacity | partial | direct non-container galactic members (src/campaign/fleets.cpp); missing: tactical fighter-capacity consumer | WGM-24, WGM-87 | 651 | src/campaign/fleets.cpp | basis: reviewed |
| Stopped_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
