# presentation tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 631 | 631 |
| partial | 111 | 111 |
| todo | 614 | 614 |
| presentation-later | 0 | 540 |
| foc-ignores | 97 | 97 |
| deferred | 28 | 28 |
| land-or-galactic | 0 | 1053 |
| multiplayer | 0 | 0 |
| **Total** | **1481** | **3074** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Name | applied | whole class (src/script/authoritative/bindings.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/script/authoritative/bindings.cpp | basis: auto |

## AITemplates

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Turn_Off/Plans/Name | applied | whole class (src/script/authoritative/bindings.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/script/authoritative/bindings.cpp | basis: auto |
| */Turn_On/Plans/Name | applied | whole class (src/script/authoritative/bindings.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/script/authoritative/bindings.cpp | basis: auto |

## AmbientMapSounds

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ambient_Map_Sound | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Ambient_Map_Sound/@Name | todo | application not recorded | — | 653 | — | — |
| AmbientMapSound/@Name | todo | application not recorded | — | 653 | — | — |
| AmbientMapSound/Ambient | todo | application not recorded | — | 653 | — | — |

## Audio

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ambient_Map_Sounds | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Delay_Between_Space_Base_Attack_Announcement_Seconds | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | SND-40 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| MSS_3D_Provider_Name | todo | application not recorded | — | 653 | — | — |
| Music_Event_Battle_End_Summary_Screen_Lose | partial | loaded local space battle (apps/viewer/src/battle_audio_events.cpp); missing: objects and modes outside the loaded local space battle | WBF-47 | 653 | apps/viewer/src/battle_audio_events.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |
| Music_Event_Battle_End_Summary_Screen_Win | partial | loaded local space battle (apps/viewer/src/battle_audio_events.cpp); missing: objects and modes outside the loaded local space battle | WBF-47 | 653 | apps/viewer/src/battle_audio_events.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |
| Music_Space_Battle_To_Ambient_Peace_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Command_Bar_Attack | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Command_Bar_Attack_Move | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Command_Bar_Guard | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Command_Bar_Move | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Command_Bar_Stop | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Command_Bar_Waypoint | todo | application not recorded | — | 1503 | — | Waypoint command semantics deferred; mode-entry cue is sourced separately as BA-27. |
| SFXEvent_GUI_Negative_Feedback | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-28 | 1503 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Telekinesis_SFXEvent_Damage | todo | application not recorded | — | 653 | — | — |
| Telekinesis_SFXEvent_Loop | todo | application not recorded | — | 653 | — | — |
| Telekinesis_SFXEvent_Slam | todo | application not recorded | — | 653 | — | — |
| Test_Disable_Weapon_Fire_SFX | todo | application not recorded | — | 653 | — | — |

## Audio, Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Music_Event_Battle_Load_Screen | todo | application not recorded | — | 653 | — | — |

## CIN_GroundInfantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Material_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Blob_Shadow_Scale | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| CanCellStack | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Create_Team | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Deploys | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Ground_Infantry_Turret_Target | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Is_Squashable | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Land_FOW_Reveal_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| LOD_Bias | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Mouse_Collide_Override_Sphere_Radius | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| OccupationStyle | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Type | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Below_Detail_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| LandBehavior | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Movement_Animation_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| No_Reflection_Below_Detail_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| No_Refraction_Below_Detail_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Rotation_Animation_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Cinematic_Object_Only | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Max_Attack_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Recharge_Seconds | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Types | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceProp, CIN_SpaceUnit, Cin_TransportUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Rate_Of_Turn | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundVehicle, Cin_Projectile, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, Cin_GroundVehicle, Cin_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Health | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundProp, CIN_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Text_ID | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, Cin_GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceProp, CIN_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Can_Be_Unlocked_By_Slicer | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Build_Initially_Locked | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Shield_Points | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceProp, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Planets | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Required_Special_Structures | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Required_Timeline | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Fire | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Build_Cost_Credits | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Build_Time_Seconds | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Collidable_By_Projectile_Living | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Damage | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Death_SFXEvent_Start_Die | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Energy_Capacity | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Energy_Refresh_Rate | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Is_Visible_On_Radar | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Mass | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| MovementClass | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Required_Ground_Base_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Required_Star_Base_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Select_Box_Scale | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Select_Box_Z_Adjust | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Attack | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Guard | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Move | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Select | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Shield_Refresh_Rate | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Size_Value | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Victory_Relevant | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_GroundInfantry, GenericHeroUnit, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Sprite | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_GroundInfantry, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Political_Control | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Ranged_Target_Z_Adjust | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Walk_Animation_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceProp, Cin_SpaceUnit, Cin_TransportUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Pulse_Delay_Seconds | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Obstacle_Width | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Obstacle_X_Offset | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Obstacle_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundVehicle, Cin_SpaceProp, Cin_SpaceUnit, Cin_TransportUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Cinematic_Anim_Blend_Seconds | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Cinematic_Anim_Index | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Cinematic_Anim_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Pause_During_Cinematic_Anim | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| User_Bound_Max | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| User_Bound_Min | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundVehicle, Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Pulse_Count | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, CIN_SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Editor_Placed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Clone | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Exclude_From_Distance_Fade | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rotate_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Soft_Footprint_Radius | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Hover_Offset | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Turret_Elevate_Extent_Degrees | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Turret_Rotate_Extent_Degrees | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Turret_Targets_Air_Vehicles | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Turret_Targets_Anything_Else | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Turret_Targets_Ground_Infantry | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundVehicle, CIN_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Max_Thrust | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundVehicle, CIN_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage_Hit_Particles | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundVehicle, Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Min_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Absorbed_By_Shields_Particle | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Damage | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Max_Flight_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Max_Scan_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Rocket_Curve_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Rocket_Curve_Offset | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Projectile_Rocket_Straight_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Idle_Anim_00_Rate_Mod | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| In_Background | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Radar_Icon_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceProp, CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Model_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SpaceBehavior | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceProp, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| OverrideAcceleration | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceProp, Props_Generic, Props_Temperate, SpacePrimarySkydome, SpaceProp, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Sort_Order_Adjust | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HardPoints | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Political_Faction | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Radar_Icon_Scale_Land | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Radar_Icon_Scale_Space | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Move_Opposite | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Shield_Hit_Particles | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Space_Full_Stop_Command | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceUnit, Container, GroundBase, GroundCompany, MOV_Cinematic, Props_Story, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Offset | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_SpaceUnit, MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Bank_Turn_Angle | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Build_Tab_Space_Units | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| CategoryMask | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Death_Explosions | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Formation_Priority | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| GUI_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| GUI_Model_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| GUI_Velocity | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Has_Space_Evaluator | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Hyperspace | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Hyperspace_Speed | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Icon_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Max_Rate_Of_Roll | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Stop | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Ship_Class | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Space_FOW_Reveal_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Space_Layer | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Squadron_Capacity | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Tech_Level | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CIN_SpaceUnit, MOV_Cinematic, Props_Story, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Barrage | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_SpaceUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| xxxSpace_Model_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Inaccuracy_Distance | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Target_Bones | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## CommandBarComponent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Alternate_Font_Name | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Anim_FPS | applied | whole class (src/presentation/godot/ui/ability_buttons_view.cpp) | AB-06, ABE-2, ABE-5 | — | src/presentation/godot/ui/ability_buttons_view.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Animate_Back | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Animate_Upper_Effect | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Bar_Overlay_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Bar_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Blank_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-07, ABE-5, ABE-6 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Blink_Fade | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Can_Animate | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Click_SFX | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Click_Shift | partial | b_reinforcement (src/presentation/godot/ui/production_view.cpp); missing: other command bar components | PU-70 | 650 | src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement pressed state; other components remain out of scope.; basis: reviewed |
| Color | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Cross_Fade | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Disabled_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-02, ABE-2 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Drag_Select | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Flash_Texture_Name | partial | b_reinforcement (src/presentation/godot/ui/production_view.cpp); b_play_pause_t (src/presentation/godot/ui/tactical_hud_build.cpp); missing: other command bar components | PU-71; TM-08 | 653 | src/presentation/godot/ui/production_view.cpp; src/presentation/godot/ui/tactical_hud_build.cpp | Reinforcement notifications and the paused tactical time button; other flashing components remain out of scope.; basis: reviewed |
| Font_Name | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | none recorded | — | src/presentation/godot/ui/tactical_hud_build.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Icon_Alternate_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-06, ABE-2, ABE-5, CARD-1, L-6 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Icon_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, ABE-9 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Icon_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | WU-39 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Loop_Anim | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Lower_Effect_Additive | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Lower_Effect_Offset | partial | st_ability_icon, st_grab_bar (apps/viewer/src/world_ui_prepare.cpp); missing: other command bar components | WU-43, WU-44 | 653 | apps/viewer/src/world_ui_prepare.cpp | World squadron active ability and additional bracket ability lower-effect quads apply the authored offsets; other component lower effects remain in the command bar coverage ticket.; basis: reviewed |
| Lower_Effect_Texture_Name | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Max_Text_Width | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Mega_Texture_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Model_Name | applied | whole class (src/assets/map.cpp); pause_shell (src/presentation/ui/hud_shell.cpp) | none recorded; TM-09 | — | src/assets/map.cpp; src/presentation/ui/hud_shell.cpp | Pause shell model is consumed at its final mounted idle pose; other existing consumers retain their scope.; basis: reviewed |
| Model_Offset_X | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Model_Offset_Y | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Mouse_Over_Offset | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Mouse_Over_SFX | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Mouse_Over_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Offset_Render | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Overlay2_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Overlay_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, ABE-9 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Scale | partial | st_ability_icon (apps/viewer/src/world_ui_prepare.cpp); r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | WU-44, WU-45; PU-72 | 653 | apps/viewer/src/world_ui_prepare.cpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage.; basis: reviewed |
| Scale_Duration | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Selected_Alpha | partial | b_reinforcement (src/presentation/godot/ui/production_view.cpp); missing: other command bar components | PU-70 | 653 | src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement selected overlay; other components remain out of scope.; basis: reviewed |
| Selected_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-07, ABE-5, ABE-6 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Should_Render_At_Drag_Pos | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Swap_Texture | partial | r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | PU-72 | 653 | src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage.; basis: reviewed |
| Text_Color | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | none recorded | — | src/presentation/godot/ui/tactical_hud_build.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Text_Color2 | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Text_Emboss | applied | r_close (src/presentation/godot/ui/production_view.cpp); whole class (src/presentation/ui/hud_shell.cpp) | PU-72; none recorded | — | src/presentation/godot/ui/production_view.cpp; src/presentation/ui/hud_shell.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage.; basis: reviewed |
| Text_Offset | partial | st_control_group, st_grab_bar (apps/viewer/src/world_ui_prepare.cpp); r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | WU-44, WU-45; PU-72 | 653 | apps/viewer/src/world_ui_prepare.cpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage.; basis: reviewed |
| Text_Offset2 | applied | whole class (src/presentation/ui/hud_shell.cpp) | CARD-1, L-8 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Text_Outline | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tooltip_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tutorial_Scene | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced |
| Upper_Effect_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, ABE-9, WU-20 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Hack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Lightning_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Lightning_Max_Targets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Lightning_Source_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Lightning_Target_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Lightning_Targets_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Hack_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Permanent_Weapon_Swap_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Disruptor_Merc_Team) |
| Abilities/Proximity_Mines_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Activate_SFX | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Repair_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Lightning_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Lightning_Max_Targets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Lightning_Source_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Lightning_Target_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Lightning_Targets_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Repair_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| LOD_Bias | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Nightsister_Indig_Team) |
| Name_Adjust | todo | application not recorded | — | 653 | — | — |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Hutt_Soldier_Team, Pirate_Trooper_Team, Rebel_Field_Commander_Team, Rebel_Trooper_Team, Twilek_Slave_Container, Twilek_Team, Underworld_Disruptor_Merc_Team, Underworld_Merc_Team) |
| Show_Name | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |

## Container, Faction, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, MultiplayerStructureMarker, Projectile, ScriptMarker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Text_ID | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | basis: auto |

## Container, Faction, GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Icon_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | CARD-1, CARD-8, L-7, WU-20 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Visible_On_Radar | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06, MME-4, MME-5 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Projectile, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Text | presentation-later | application not recorded | — | — | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on MiscObject: E_Multiplayer_Beacon, R_Multiplayer_Beacon. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+63 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+47 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on MiscObject: E_Multiplayer_Beacon, R_Multiplayer_Beacon. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+63 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+47 more).; SCOPE-MENU |

## Container, GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Select_Box_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-02 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, Projectile, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Unit_Class | presentation-later | application not recorded | — | — | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Defense_Satellite_Laser, Defense_Satellite_Laser_Small, Defense_Satellite_Missile, E_Gravity_Well_Station (+77 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+48 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Defense_Satellite_Laser, Defense_Satellite_Laser_Small, Defense_Satellite_Missile, E_Gravity_Well_Station (+77 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+48 more).; SCOPE-MENU |

## Container, GenericHeroUnit, HeroUnit, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-33, AU-37, BA-20, BA-23, BA-24 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## Container, GenericHeroUnit, Marker, MiscObject, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | applied | whole class (src/units/unit_combat.cpp) | AV-05, E71-15, E71-18, E71-19, R-ROT-01 | — | src/units/unit_combat.cpp | basis: auto |

## Container, GenericHeroUnit, SecondaryStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Row | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small (+67 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small (+67 more). |

## Container, HeroUnit, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Deactivated | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-33, AU-37, BA-20, BA-23, BA-24 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## Container, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Named_Hero | partial | loaded local space battle (apps/viewer/src/live_session_frame.cpp); visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); craft, ship, station (src/presentation/ui/world_ui.cpp); space purchase catalog (src/skirmish/economy.cpp); ship (src/skirmish/economy.cpp); space purchase catalog (src/skirmish/inputs.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects and modes outside the loaded local space battle, objects outside the simulated unit closure | WBF-45; WU-47, WU-48; WSU-50; WSS-29; WHE-01, WHE-49; WSS-29; WSU-50 | 653 | apps/viewer/src/live_session_frame.cpp; apps/viewer/src/world_ui_groups.cpp; src/presentation/ui/world_ui.cpp; src/skirmish/economy.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces. Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate. Resolved inherited space hero policy filters the shared human/AI purchase catalog independently of presentation and results scoring. World identities cover visible standalone space heroes; carried heads remain separate.; basis: reviewed |

## Container, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Model_Name | todo | application not recorded | — | 653 | — | — |

## Container, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Distance | todo | application not recorded | — | 653 | — | — |
| GUI_Velocity | todo | application not recorded | — | 653 | — | — |

## Container, SpaceBuildable, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+8 more). Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Container, SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | partial | ship (apps/viewer/src/battle_audio_events.cpp); craft, ship, squadron, station (apps/viewer/src/battle_audio_events.cpp); missing: release-gated BARRAGE, unimplemented space abilities and land/galactic objects authored with hero/container classes | BA-54; BA-53 | 1505 | apps/viewer/src/battle_audio_events.cpp | M2 targeted confirmation and spawned harmonic/weaken effects attach to the issuer; unsupported actions stay silent.; basis: reviewed |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Buzz_Droids_Ability/Activate_SFX | partial | squadron (apps/viewer/src/battle_audio_prepare.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-75 | 1884 | apps/viewer/src/battle_audio_prepare.cpp | The droid object's owner hears it at the arming frame; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Idle_Chase_Range | todo | application not recorded | — | 653 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Container, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_FOW_Reveal_Range | applied | space hunt fallback destinations (src/sim/tactical/session_step_commands.cpp); whole class (src/skirmish/ai.cpp) | WAB-54; AU-40, AU-49, EAB-08, EAB-13, WAB-54 | — | src/sim/tactical/session_step_commands.cpp; src/skirmish/ai.cpp | basis: auto |

## Container, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Ion_Cannon_Shot_Attack_Ability/@Name | todo | application not recorded | — | 760 | — | — |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Good_Against | presentation-later | application not recorded | — | — | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+45 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+45 more).; SCOPE-MENU |
| Encyclopedia_Vulnerable_To | presentation-later | application not recorded | — | — | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+45 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+45 more).; SCOPE-MENU |
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Alternate_Icon_Name | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | todo | application not recorded | — | 760 | — | — |

## Container, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Dense_FOW_Reveal_Range_Multiplier | applied | craft, ship, squadron, station (src/sim/tactical/fog_cells.cpp) | V-22, WHZ-09 | — | src/sim/tactical/fog_cells.cpp | basis: reviewed |
| Radar_Icon_Scale_Space | applied | whole class (src/presentation/ui/minimap.cpp) | MM-17 | — | src/presentation/ui/minimap.cpp | conditional space radar scaling; types without Radar_Draw_To_Scale keep Radar_Icon_Size |

## Container, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Lucky_Shot_Attack_Ability/@Name | todo | application not recorded | — | 760 | — | — |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fadeout_Time | todo | application not recorded | — | 653 | — | — |
| Slot_Scale | todo | application not recorded | — | 653 | — | — |

## Decal, Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Render_Mode | todo | application not recorded | — | 653 | — | — |

## Decal, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale | todo | application not recorded | — | 653 | — | — |

## Decals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Decal | todo | application not recorded | — | 653 | — | — |
| Decal/@name | todo | application not recorded | — | 653 | — | — |

## DynamicTrack

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| fade_begin_distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| fade_distance_per_second | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| fade_end_distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| min_geometry_lod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| opacity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Render_Mode | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| segment_length | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Texture_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## DynamicTrack, Weather_Scenario

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Alternate_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Alternate_Icon_Prefix | todo | application not recorded | — | 653 | — | — |
| Big_Fleet_Color | todo | application not recorded | — | 653 | — | — |
| Bombardment_Lighting_Color_List | todo | application not recorded | — | 653 | — | — |
| Bombardment_Lighting_Intensity_List | todo | application not recorded | — | 653 | — | — |
| Bombardment_Lighting_Seconds_List | todo | application not recorded | — | 653 | — | — |
| Bombardment_Particle | todo | application not recorded | — | 653 | — | — |
| Bombardment_Shadow_Blob_Material_Name | todo | application not recorded | — | 653 | — | — |
| Can_Win_By_Destroying_Super_Weapon | todo | application not recorded | — | 653 | — | — |
| Carrier_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Color | applied | whole class (apps/viewer/src/land_look.cpp) | A-04, A-05, A-11, P-03, S-12 | — | apps/viewer/src/land_look.cpp | basis: auto |
| Corvette_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Default_Transmission_Message | todo | application not recorded | — | 653 | — | — |
| Defeat_Text | todo | application not recorded | — | 653 | — | — |
| Display_Font_Color | todo | application not recorded | — | 653 | — | — |
| Finale_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Finale_Movie_2 | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Fleet_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Frigate_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Generic_Win_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Helper_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Is_Debug_Switchable_To | todo | application not recorded | — | 653 | — | — |
| Multiplayer_Beacon_Type | todo | application not recorded | — | 653 | — | — |
| Multiplayer_Map_Preview_Icon | todo | application not recorded | — | 653 | — | — |
| Music_Event_List_Ambient | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-27, BA-41 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Music_Event_List_Battle | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-27, BA-41 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Music_Event_Space_Ambient_Super_Weapon | todo | application not recorded | — | 653 | — | — |
| Music_Event_Space_Battle_Super_Weapon | todo | application not recorded | — | 653 | — | — |
| Music_Event_Strategic_Lose | todo | application not recorded | — | 653 | — | — |
| Music_Event_Strategic_Lose_Vs_Faction | todo | application not recorded | — | 653 | — | — |
| Music_Event_Strategic_Win | todo | application not recorded | — | 653 | — | — |
| Music_Event_Strategic_Win_Vs_Faction | todo | application not recorded | — | 653 | — | — |
| Music_Event_Tactical_Lose | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | 1511 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Lose_Vs_Faction | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | 1511 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Space_Battle_Pending | todo | application not recorded | — | 653 | — | — |
| Music_Event_Tactical_Win | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | 1511 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Win_Vs_Faction | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | 1511 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Post_Credits_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Reinforcements_Cancelled_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-16 | 1502 | apps/viewer/src/battle_audio_events.cpp | Failed space placement drops and explicit right-click cancellation request the authored faction event; ordinary overlap admission may refuse playback.; basis: reviewed |
| Reinforcements_Enroute_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-16 | 1502 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; basis: reviewed |
| Reinforcements_Ready_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Reinforcements_Requesting_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Reinforcements_Selection_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-07 | 1502 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; basis: reviewed |
| Reinforcements_Shadow_Blob_Material_Name | todo | application not recorded | — | 653 | — | — |
| Selection_Blob_RGBA | todo | application not recorded | — | 653 | — | read by a loader, but nothing applies the value |
| SFX_Event_Tactical_Space_Battle_Pending | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Arrive_From_Hyperspace | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-62 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Base_Shield_Absorb_Damage | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bombard_Ally_Available | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bombard_Available | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bombard_Cancelled | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bombard_Enemy_Available | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bunker_Vacated | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Enemy_Spotted | partial | standalone space skirmish (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-71 | 653 | apps/viewer/src/battle_audio_events.cpp | Local faction cue; consumed eligibility is not restored on busy/missing/refused playback.; basis: reviewed |
| SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_Off | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | none recorded | — | apps/viewer/src/battle_audio_events.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_On | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | AU-33, AU-34, AU-35, BA-50 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_GUI_Toggle_Non_Hero_Ability_Off | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | none recorded | — | apps/viewer/src/battle_audio_events.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| SFXEvent_GUI_Toggle_Non_Hero_Ability_On | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | AU-33, AU-34, AU-35, BA-50, WAB-11 | — | apps/viewer/src/battle_audio_events.cpp | basis: auto |
| SFXEvent_Hack_Success | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Advisor_Hint | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_HUD_Advisor_Message | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_HUD_Advisor_Urgent | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_HUD_Base_Shield_Offline | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Base_Shield_Online | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Base_Shield_Penetrated | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Build_Pad_Captured | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-37 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_HUD_Build_Pad_Lost | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-37 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_HUD_Enemy_Base_Shield_Offline | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Enemy_Base_Shield_Online | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Enemy_Base_Shield_Penetrated | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Enemy_Special_Weapon_Charging | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Enemy_Special_Weapon_Firing | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Enemy_Special_Weapon_Ready | deferred | application not recorded | — | 1505 | — | BA-56: planetary ion/hypervelocity space upgrades and ground weapons share this readiness route; M2 has no implemented firing/readiness state. |
| SFXEvent_HUD_Gravity_Control_Generator_Off | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Gravity_Control_Generator_On | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Lost_Space_Battle | applied | local faction (apps/viewer/src/battle_audio_prepare.cpp) | WBF-38 | — | apps/viewer/src/battle_audio_prepare.cpp | Local faction space outcome HUD cue, once on first presented outcome; land and story/TSW variants have separate rows.; basis: reviewed |
| SFXEvent_HUD_Lost_Space_Battle_Enemy_TSW_Present | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_05_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_15_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_30_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_60_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Contested | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_05_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_15_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_30_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_60_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Owned_05_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Owned_15_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Owned_30_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Reinforcement_Point_Owned_60_Seconds | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Repairing | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Special_Weapon_Charging | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Special_Weapon_Firing | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Special_Weapon_Ready | deferred | application not recorded | — | 1505 | — | BA-56: planetary ion/hypervelocity space upgrades and ground weapons share this readiness route; M2 has no implemented firing/readiness state. |
| SFXEvent_HUD_Tactical_Victory_Near | todo | application not recorded | — | 653 | — | — |
| SFXEvent_HUD_Won_Space_Battle | applied | local faction (apps/viewer/src/battle_audio_prepare.cpp) | WBF-38 | — | apps/viewer/src/battle_audio_prepare.cpp | Local faction space outcome HUD cue, once on first presented outcome; land and story/TSW variants have separate rows.; basis: reviewed |
| SFXEvent_HUD_Won_Space_Battle_Enemy_TSW_Present | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Mission_Added | todo | application not recorded | — | 653 | — | — |
| SFXEvent_New_Construction_Options_Available | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Player_Taunt | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Space_Base_Under_Attack_Announcement | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-40 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Gain_Enemy_Control | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Tactical_Gain_Friendly_Control | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Tactical_Lose_Enemy_Control | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Tactical_Lose_Friendly_Control | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Tactical_Object_Building_Complete | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Building_Loop | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Building_Started | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Sold | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-31 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Unit_Type_Spotted | partial | standalone space skirmish (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-70 | 653 | apps/viewer/src/battle_audio_events.cpp | Local faction cue; consumed eligibility is not restored on busy/missing/refused playback.; basis: reviewed |
| SFXEvent_Weather_Begin | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Weather_End | todo | application not recorded | — | 653 | — | — |
| Ship_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Space_Advisor_Hints | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Space_Lose_Image | todo | application not recorded | — | 653 | — | — |
| Space_Mode_Selection_Blob_Material_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-01 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |
| Space_Retreat_Begin_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Cancel_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Countdown_Color_RGBA | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Countdown_Text_ID | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Enemy_Begin_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Not_Allowed_Reason_1_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Not_Allowed_Reason_2_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Not_Allowed_Reason_3_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Retreat_Not_Allowed_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Surrender_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Space_Win_Image | todo | application not recorded | — | 653 | — | — |
| SpeechEvent_Super_Weapon_Enemy_Moved_Into_Range | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Into_Range | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_05_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_15_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_30_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_60_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moved_Into_Range | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moving_Into_Range | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moving_Range_05_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moving_Range_15_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moving_Range_30_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Super_Weapon_Moving_Range_60_Seconds | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| SpeechEvent_Tactical_Intro_Space_Attacker | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| SpeechEvent_Tactical_Intro_Space_Attacker_Conditional_And | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| SpeechEvent_Tactical_Intro_Space_Attacker_Conditional_Or | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| SpeechEvent_Tactical_Intro_Space_Defender | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| SpeechEvent_Tactical_Intro_Space_Defender_Conditional_And | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| SpeechEvent_Tactical_Intro_Space_Defender_Conditional_Or | land-or-galactic | application not recorded | — | — | — | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72) |
| Star_Base_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Strategic_Map_Music_Event | todo | application not recorded | — | 653 | — | — |
| Superweapon_Win_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Tactical_Intro_Command_Bar_Movie_Name | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Text_Nickname_ID | todo | application not recorded | — | 653 | — | — |
| Vehicle_Icon_Name | todo | application not recorded | — | 653 | — | — |
| Victory_Text | todo | application not recorded | — | 653 | — | — |

## Faction, GenericHeroUnit, SpaceStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| No_Colorization_Color | partial | capture-point radar (apps/viewer/src/map_mode_hud.cpp); missing: other material colour consumers | WNO-43 | 653 | apps/viewer/src/map_mode_hud.cpp | Nonneutral unassigned community capture radar falls back to nonzero type then faction no-colourisation data; other consumers remain pending.; basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Activated_Hack_Super_Weapon_Ability_Names | todo | application not recorded | — | 653 | — | — |
| Activated_Neutralize_Hero_Ability_Names | todo | application not recorded | — | 653 | — | — |
| Activated_System_Spy_Ability_Names | todo | application not recorded | — | 653 | — | — |
| Advisor_Hint_Duration | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Advisor_Hint_Interval | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Battle_Pending_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | TE-04, TE-09, TM-09 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| Battle_Pending_Message_Font | todo | application not recorded | — | 653 | — | — |
| Battle_Pending_Message_Pos_X | todo | application not recorded | — | 653 | — | — |
| Battle_Pending_Message_Pos_Y | todo | application not recorded | — | 653 | — | — |
| BeaconPlaceDelay | todo | application not recorded | — | 653 | — | — |
| Bink_Player_Caption_Font_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Bink_Player_Caption_Font_Size | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Camera_FX_Manager_Letterbox_Height | todo | application not recorded | — | 653 | — | — |
| Camera_Stop_Left | todo | application not recorded | — | 653 | — | — |
| Camera_Stop_Right | todo | application not recorded | — | 653 | — | — |
| Camera_Z_Position | todo | application not recorded | — | 653 | — | — |
| CB_Flash_Count | todo | application not recorded | — | 653 | — | debug build: pool addition passes this value but the continuous flag bypasses its application (PU-71); other command bar flashes remain pending |
| CB_Flash_Duration | todo | application not recorded | — | 653 | — | — |
| CB_Movie_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| CB_Movie_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Command_Bar_Default_Font_Name | todo | application not recorded | — | 653 | — | — |
| Command_Bar_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the default font size of UI text components |
| Countdowns_Font_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Credits_Bottom_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Font | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Font_Size | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Header_Bottom_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Header_Top_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_1_Height | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_1_Name | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_1_Width | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_1_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_2_Height | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_2_Name | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_2_Width | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_2_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_3_Height | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_3_Name | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_3_Width | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Logo_3_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Margin | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Scroll_Rate | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Spacing | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Credits_Top_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| CrouchIdleWalkBlendTime | todo | application not recorded | — | 653 | — | — |
| CrouchMoveBlendTime | todo | application not recorded | — | 653 | — | — |
| Debug_Hot_Key_Load_Map | todo | application not recorded | — | 653 | — | — |
| Debug_Hot_Key_Load_Map_Script | todo | application not recorded | — | 653 | — | — |
| Demo_Attract_Map_Cycle_Delay_Seconds | todo | application not recorded | — | 653 | — | — |
| Demo_Attract_Maps | todo | application not recorded | — | 653 | — | — |
| Demo_Attract_Start_Timeout_Seconds | todo | application not recorded | — | 653 | — | — |
| Display_Bink_Movie_Frames | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Droid_Date_Color | todo | application not recorded | — | 653 | — | — |
| Droid_Encyclopedia_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Droid_Seperator_Color | todo | application not recorded | — | 653 | — | — |
| Droid_Text_Color | todo | application not recorded | — | 653 | — | — |
| Earthquake_Shake_Magnitude | todo | application not recorded | — | 653 | — | — |
| Earthquake_Shake_Speed | todo | application not recorded | — | 653 | — | — |
| Earthquake_Transition_Time | todo | application not recorded | — | 653 | — | — |
| Encyclopedia_Class_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Cost_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Delay | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Fade_Rate | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Icon_X_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Icon_Y_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Min_Display_Time | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Name_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Encyclopedia_Population_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Enemy_Color | todo | application not recorded | — | 653 | — | — |
| Energy_Beam_Color | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Energy_Beam_Texture | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Energy_Beam_Width | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Event_Message_Default_Font_Name | todo | application not recorded | — | 653 | — | — |
| Evil_Side_Leader_Name | todo | application not recorded | — | 653 | — | — |
| Evil_Side_Name | todo | application not recorded | — | 653 | — | — |
| First_Strike_Particle | todo | application not recorded | — | 653 | — | — |
| Fleet_Movement_Line_Texture_Name | todo | application not recorded | — | 653 | — | — |
| Galactic_Map_Score_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Game_Object_Name_Font_Name | todo | application not recorded | — | 653 | — | — |
| Game_Object_Name_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the 3D game initialisation (object name font) |
| Game_Scoring_Script_Name | partial | loaded local space battle (apps/viewer/src/battle_scoring.cpp); missing: objects and modes outside the loaded local space battle | WBF-46 | 653 | apps/viewer/src/battle_scoring.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |
| GMC_Battle_Fade_Time | todo | application not recorded | — | 653 | — | — |
| GMC_Battle_Zoom_Time | todo | application not recorded | — | 653 | — | — |
| GMC_EdgeHorizontalRotateRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_EdgeVerticalTiltRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialDesiredLookdownPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialDiscRotationAngleDegrees | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialiLookatDistanceFromOrigin | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialLookdownAngleDegrees | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialLookdownPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialPitchAngleDegrees | todo | application not recorded | — | 653 | — | — |
| GMC_InitialPullbackDistance | todo | application not recorded | — | 653 | — | — |
| GMC_LookdownPullbackRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxLookdownAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxRollAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxRotationAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinLookdownAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinRollAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_PullbackFromOriginDistFactor | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_ZoomedPitchAngleDegrees | todo | application not recorded | — | 653 | — | — |
| GMC_ZoomTime | todo | application not recorded | — | 653 | — | — |
| Good_Side_Leader_Name | todo | application not recorded | — | 653 | — | — |
| Good_Side_Name | todo | application not recorded | — | 653 | — | — |
| GripperCombatGridSnapDistance | applied | whole class (apps/viewer/src/world_ui_groups.cpp); whole class (apps/viewer/src/world_ui_prepare.cpp) | WSU-36; WSU-36 | — | apps/viewer/src/world_ui_groups.cpp; apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| GUI_Attack_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Attack_Movement_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Cycle_Color | todo | application not recorded | — | 653 | — | — |
| GUI_Cycle_Speed | todo | application not recorded | — | 653 | — | — |
| GUI_Double_Click_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Flash_Duration | todo | application not recorded | — | 653 | — | — |
| GUI_Flash_Level | presentation-later | application not recorded | — | 828 | — | debug build: read only by the selection flash lighting effect of an object |
| GUI_Guard_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Move_Acknowledge_Scale_Space | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Movement_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Movement_Double_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Rapid_Flash_Duration | todo | application not recorded | — | 653 | — | — |
| GUI_Strategic_Countdown_Timers_Screen_Spacing | todo | application not recorded | — | 653 | — | — |
| GUI_Strategic_Countdown_Timers_Screen_X | todo | application not recorded | — | 653 | — | — |
| GUI_Strategic_Countdown_Timers_Screen_Y | todo | application not recorded | — | 653 | — | — |
| GUI_Tactical_Countdown_Timers_Screen_Spacing | todo | application not recorded | — | 653 | — | — |
| GUI_Tactical_Countdown_Timers_Screen_X | todo | application not recorded | — | 653 | — | — |
| GUI_Tactical_Countdown_Timers_Screen_Y | todo | application not recorded | — | 653 | — | — |
| Hack_Super_Weapon_Particle_Effect | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Enemy_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Enemy_Tracked_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Friendly_Disabled_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Friendly_Disabled_Tracked_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Friendly_Repairing_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Friendly_Texture | todo | application not recorded | — | 653 | — | — |
| HardPoint_Target_Reticle_Friendly_Tracked_Texture | todo | application not recorded | — | 653 | — | — |
| Health_Bar_Scale | todo | application not recorded | — | 653 | — | — |
| Health_Bar_Spacing | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar bracket layout; the viewer draws the spacing from a literal (include/eawr/presentation/ui/world_ui.hpp), not from this tag |
| Hint_Text_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Icons_Per_Column | todo | application not recorded | — | 653 | — | — |
| IdleMovementFrames | todo | application not recorded | — | 653 | — | — |
| IdleWalkBlendTime | todo | application not recorded | — | 653 | — | — |
| In_Game_Cinematics | todo | application not recorded | — | 653 | — | — |
| In_Game_Message_Default_Font_Name | todo | application not recorded | — | 653 | — | — |
| In_Game_Message_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the in-game message window set-up |
| Japanese_Line_Percent | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Japanese_ST_Line_Percent | todo | application not recorded | — | 653 | — | — |
| Land_Tactical_Camera_Locked | land-or-galactic | application not recorded | — | — | — | tactical-camera-input.md: the land mode's camera lock flag (debug build) |
| Laser_Beam_Z_Scale_Factor | applied | whole class (apps/viewer/src/battle_effects_projectiles.cpp) | BP-07 | — | apps/viewer/src/battle_effects_projectiles.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Laser_Kite_Z_Scale_Factor | applied | whole class (apps/viewer/src/battle_effects_projectiles.cpp) | BP-04 | — | apps/viewer/src/battle_effects_projectiles.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Left_Queue_Tint | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar build-queue display; the viewer production view uses a literal default, not this tag |
| Localized_Menu_Overlay | todo | application not recorded | — | 653 | — | — |
| Localized_Splash_Screen | todo | application not recorded | — | 653 | — | — |
| Localized_UK_English_Splash_Screen | todo | application not recorded | — | 653 | — | — |
| Long_Encyclopedia_Delay | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| LoopWaypointLineTextureName | todo | application not recorded | — | 653 | — | — |
| Lose_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | BE-03, BX-04 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| Main_Menu_Demo_Attract_Mode | todo | application not recorded | — | 653 | — | — |
| Main_Menu_Score_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Map_Preview_Image_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the map preview image generator |
| Message_Of_The_Day_URL | todo | application not recorded | — | 653 | — | — |
| Min_Accuracy_For_Icon | todo | application not recorded | — | 653 | — | — |
| Min_Health_Bar_Scale | todo | application not recorded | — | 653 | — | — |
| Min_Sight_Range_For_Icon | todo | application not recorded | — | 653 | — | — |
| MinimumDragSelectDistance | todo | application not recorded | — | 653 | — | — |
| Mouse_Over_Highlight_Scale | todo | application not recorded | — | 653 | — | — |
| MoveBlendTime | todo | application not recorded | — | 653 | — | — |
| Nebula_Effect_Color | partial | space tactical instances (apps/viewer/src/live_session_frame.cpp); missing: land and cinematic rendering | WHZ-71 | 653 | apps/viewer/src/live_session_frame.cpp | basis: reviewed |
| Neutral_UI_Color | todo | application not recorded | — | 653 | — | — |
| Object_Visual_Status_Particle_Attach_Bone_Names | todo | application not recorded | — | 653 | — | — |
| Player_Color | todo | application not recorded | — | 653 | — | — |
| PlayModeSwitchMovies | todo | application not recorded | — | 653 | — | — |
| Radar_Colorize_Multiplayer_Enemy | todo | application not recorded | — | 653 | — | — |
| Radar_Colorize_Selected_Units | applied | whole class (src/presentation/ui/minimap.cpp) | MM-05, MME-3 | — | src/presentation/ui/minimap.cpp | basis: auto |
| Radar_Multiplayer_Enemy_Color | todo | application not recorded | — | 653 | — | — |
| Radar_Selected_Units_Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-05, MME-3 | — | src/presentation/ui/minimap.cpp | basis: auto |
| ReinforcementOverlayBadColor | applied | whole class (src/presentation/ui/production.cpp) | WR-14 | — | src/presentation/ui/production.cpp | basis: reviewed |
| ReinforcementOverlayGoodColor | applied | whole class (src/presentation/ui/production.cpp) | WR-14 | — | src/presentation/ui/production.cpp | basis: reviewed |
| Right_Queue_Tint | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar build-queue display; the viewer production view uses a literal default, not this tag |
| Saliency_Health | todo | application not recorded | — | 653 | — | — |
| Saliency_Power | todo | application not recorded | — | 653 | — | — |
| Saliency_Speed | todo | application not recorded | — | 653 | — | — |
| Saliency_Targets | todo | application not recorded | — | 653 | — | — |
| Saliency_X | todo | application not recorded | — | 653 | — | — |
| Saliency_Y | todo | application not recorded | — | 653 | — | — |
| Scroll_Acceleration_Factor | applied | whole class (src/presentation/camera/controller.cpp) | none recorded | — | src/presentation/camera/controller.cpp | docs/camera.md: the ramp time constant of keyboard and edge pan; the loader is table-driven; basis: auto |
| Scroll_Deceleration_Factor | applied | whole class (src/presentation/camera/controller.cpp) | none recorded | — | src/presentation/camera/controller.cpp | docs/camera.md: the release time of keyboard and edge pan; the loader is table-driven; basis: auto |
| SetupPhaseFOWColor | todo | application not recorded | — | 653 | — | — |
| SetupPhaseInvalidDragColor | todo | application not recorded | — | 653 | — | — |
| Shield_Flash_Duration | applied | whole class (apps/viewer/src/live_session_frame.cpp) | BP-21 | — | apps/viewer/src/live_session_frame.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Shield_Flash_Scale | applied | whole class (apps/viewer/src/live_session_frame.cpp) | BP-21 | — | apps/viewer/src/live_session_frame.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| ShipNameTextFiles | todo | application not recorded | — | 653 | — | — |
| ShouldDisplayPotentialPath | todo | application not recorded | — | 653 | — | — |
| ShouldDisplayPredictionPaths | todo | application not recorded | — | 653 | — | — |
| ShouldDisplaySyncedPaths | todo | application not recorded | — | 653 | — | — |
| ShouldUseSpaceIdleMovement | todo | application not recorded | — | 653 | — | — |
| Space_Tactical_Camera_Locked | todo | application not recorded | — | 653 | — | — |
| SpaceFOWColor | applied | whole class (src/presentation/space/fog_field.cpp) | FW-01, FW-11, FWE-3, FWE-4 | — | src/presentation/space/fog_field.cpp | basis: auto |
| SpaceFOWHeight | applied | whole class (src/presentation/camera/controller.cpp) | AU-47, FW-01, FW-02, FW-03, FWE-4 | — | src/presentation/camera/controller.cpp | basis: auto |
| SpaceFOWRegrowTime | applied | whole class (src/presentation/space/fog_field.cpp) | FW-05, FWE-4, FWE-6, RO-1, V-15 | — | src/presentation/space/fog_field.cpp | basis: auto |
| SpaceIdleMovementSpeed | partial | ordinary space engine audio (apps/viewer/src/battle_audio_events.cpp); missing: locomotor movement implementation | BA-82 | 1501 | apps/viewer/src/battle_audio_events.cpp | Engine sound state compares published walk speed with the authored threshold; movement service is separate.; basis: reviewed |
| SpaceReinforceFeedbackOnlyWhileDragging | applied | whole class (apps/viewer/src/live_fog_view.cpp) | FW-23, WR-17 | — | apps/viewer/src/live_fog_view.cpp | — |
| SpaceReinforceFOWColor | applied | whole class (src/presentation/space/fog_field.cpp) | FW-08, FW-11, FW-12, FW-22, FWE-15 | — | src/presentation/space/fog_field.cpp | basis: auto |
| Speech_Text_Color | todo | application not recorded | — | 653 | — | — |
| Star_Wars_Crawl_Start_Fadeout_Frame | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| System_Text_Color | todo | application not recorded | — | 653 | — | — |
| Task_Text_Color | todo | application not recorded | — | 653 | — | — |
| Team_Healthbar_Offset | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar bracket offset; the viewer draws the offset from a literal (include/eawr/presentation/ui/world_ui.hpp), not from this tag |
| TeamCrouchMoveBlendTime | todo | application not recorded | — | 653 | — | — |
| TeamMoveBlendTime | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Hover_Height | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Max_Bob_Height | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Max_Wobble_Angle | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Transition_Time | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Wobble_Cycle_Time | todo | application not recorded | — | 653 | — | — |
| Telekinesis_Wobble_Fade_Time | todo | application not recorded | — | 653 | — | — |
| Text_Button_Default_Font_Name | todo | application not recorded | — | 653 | — | — |
| Text_Button_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the default font size of UI text-button components |
| Text_Reveal_Rate | todo | application not recorded | — | 653 | — | — |
| Tool_Tip_Font_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-52 | — | apps/viewer/src/world_ui_prepare.cpp | — |
| Tool_Tip_Font_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-52 | — | apps/viewer/src/world_ui_bars.cpp | — |
| Tool_Tip_Small_Font_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-52 | — | apps/viewer/src/world_ui_prepare.cpp | — |
| Tool_Tip_Small_Font_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-52 | — | apps/viewer/src/world_ui_bars.cpp | — |
| Tooltip_Delay | todo | application not recorded | — | 653 | — | — |
| Tractor_Beam_Color | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Frames | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-02, TBF-03 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Texture | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Width | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Unit_Command_Rankings_By_Category | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-20 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Use_Neutral_UI_Color | todo | application not recorded | — | 653 | — | — |
| WalkAnimationCutoff | todo | application not recorded | — | 653 | — | — |
| Water_Clip_Plane_Offset | todo | application not recorded | — | 653 | — | — |
| Water_Render_Target_Resolution | todo | application not recorded | — | 653 | — | — |
| WaypointFlagModelName | todo | application not recorded | — | 653 | — | — |
| WaypointLineTextureName | todo | application not recorded | — | 653 | — | — |
| Win_Lose_Message_Font | applied | whole class (src/presentation/godot/ui/battle_overlay.cpp) | BE-03, BX-04 | — | src/presentation/godot/ui/battle_overlay.cpp | basis: auto |
| Win_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | BE-03, BX-04 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Enhance_Defense_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Neutralize_Hero_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R) |
| Abilities/Neutralize_Hero_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-GALACTIC |
| Abilities/Remote_Bomb_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Toss_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/System_Spy_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Blob_Shadow_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Blob_Shadow_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Blob_Shadow_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Crouch_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Encyclopedia_Good_Against | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur) |
| Encyclopedia_Vulnerable_To | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur) |
| Idle_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Land_Model_Anim_Override_Name | applied | whole class (src/scene/scene_build.cpp) | UA-01 | — | src/scene/scene_build.cpp | basis: auto |
| LOD_Bias | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Rotation_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| SFXEvent_Guard | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Show_Hero_Head | partial | visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); missing: land and carried heroes | WU-47, WU-48 | 653 | apps/viewer/src/world_ui_groups.cpp | Explicit heads and named heroes share the loaded space world identity renderer.; basis: reviewed |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Walk_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Rebel) |

## GenericHeroUnit, HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attach_To_Flagship_During_Space_Battle | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. |
| Death_Fade_Time | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | land-or-galactic | application not recorded | — | — | — | BA-55: these effective hero classes author ground targeting payloads, not the M2 space heroes.; SCOPE-LAND |

## GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_SFXEvent_Start_Die | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); Projectile (apps/viewer/src/battle_audio_prepare.cpp) | AU-17, AU-18, BA-16, BA-17, SP-03; BA-85 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Fire | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-14, AU-15, BA-07, BA-14 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker, SecondaryStructure, SpaceProp, SpecialStructure, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | applied | whole class (src/scene/scene_build.cpp) | none recorded | — | src/scene/scene_build.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | applied | whole class (src/presentation/animation/idle_playback.cpp) | none recorded | — | src/presentation/animation/idle_playback.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Select | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-19, AU-23, BA-21 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Attack | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-22, BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Move | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-22, BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Clone | applied | whole class (apps/viewer/src/space_populate_report.cpp) | SP-02, UA-07, UA-10 | — | apps/viewer/src/space_populate_report.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## GenericHeroUnit, HeroUnit, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Fleet_Move | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion (+14 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam (+48 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam (+48 more). |

## GenericHeroUnit, HeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Play_SFXEvent_On_Sighting | partial | loaded space tactical types (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-70, SND-42 | 653 | apps/viewer/src/battle_audio_events.cpp | Logical snapshot visibility consumes type before WAV 2D busy admission; fog-service phase remains presentation-qualified.; basis: reviewed |

## GenericHeroUnit, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Group_Attack | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-22, BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| SFXEvent_Group_Move | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-22, BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Galactic_Stealth_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Tyber_Zann (+6 more). |

## GenericHeroUnit, SpecialStructure, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |

## GraphicDetailSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GraphicDetailLevelEnumeration/GraphicDetailLevel/@name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/Based_On | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/Bloom | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/DynamicLighting | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/EnvironmentDetail | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/HeatDistortions | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/MeshDetail | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ParticleDetail | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ScreenAALevel | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ScreenResolutionHeight | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ScreenResolutionWidth | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ShaderDetailLevel | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ShadowDetail | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/ShadowVolumes | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/SoftShadows | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/TextureMipLevel | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| GraphicDetailLevelEnumeration/GraphicDetailLevel/WaterDetailLevel | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/@name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/CPUSpeed | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/DefaultGraphicDetailLevel | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/FillRateMpsPS13 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/FillRateMpsPS14 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/FillRateMpsPS20 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/GraphicDetailSettingName | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/PixelShaderVersionHEX | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/TextureMemory | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/VendorIDHEX | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/VertexRateMvsVS11 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| HardwareConfigurations/HardwareConfiguration/VertexShaderVersionHEX | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |

## GroundBuildable, GroundInfantry, GroundStructure, HeroUnit, Indigenous_Unit, Props_Story, SecondaryStructure, Slave_Unit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bracket_Height | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundBuildable, GroundInfantry, GroundStructure, HeroUnit, Indigenous_Unit, Props_Story, SecondaryStructure, Slave_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bracket_Width | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Model | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bias | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundStructure, Props_Story, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Lighting_Effect_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundVehicle, HeroUnit, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Generic_Attack_Ability/SFXEvent_Apply_Damage | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundVehicle, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| IsDeathClone | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GUIDialogs

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fonts/*/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Combo_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Combo_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Combo_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Combo_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Combo_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Combo_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Combo_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Global_Default/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/R_Text/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/R_Text/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, MME-1, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/R_Text/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Textures/*/Button_Left | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Button_Left_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Left_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Left_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Middle | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Button_Middle_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Middle_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Middle_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Right | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Button_Right_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Right_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Button_Right_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Check_Off | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Check_On | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Combo_Box_Popdown_Button | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Combo_Box_Popdown_Button_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Combo_Box_Popdown_Button_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Combo_Box_Text_Box | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Left | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Middle | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Minus | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Minus_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Dial_Minus_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Dial_Plus | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Plus_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Dial_Plus_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Dial_Right | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Dial_Tab | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Frame_Background | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Bottom | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Bottom_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Frame_Bottom_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Frame_Bottom_Transition_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Bottom_Transition_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Left_Transition_Bottom | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Left_Transition_Top | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Right_Transition_Bottom | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Right_Transition_Top | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Top | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Top_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Frame_Top_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Frame_Top_Transition_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Frame_Top_Transition_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Progress_Bar_Left | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Progress_Bar_Middle_Off | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Progress_Bar_Middle_On | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Progress_Bar_Right | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Radio_Mouse_Over | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Radio_Off | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Radio_On | applied | whole class (src/presentation/godot/ui/kit_buttons.cpp) | none recorded | — | src/presentation/godot/ui/kit_buttons.cpp | basis: auto |
| Textures/*/Scanlines | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Down_Button | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Down_Button_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Down_Button_Mouse_Over | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Down_Button_Pressed | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Middle | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Middle_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Tab | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Tab_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Up_Button | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Up_Button_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Up_Button_Mouse_Over | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Up_Button_Pressed | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Small_Frame_Background | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Small_Frame_Bottom | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Small_Frame_Bottom_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Small_Frame_Bottom_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Small_Frame_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Small_Frame_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Small_Frame_Top | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Small_Frame_Top_Left | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Small_Frame_Top_Right | applied | whole class (src/presentation/godot/ui/kit_frames.cpp) | none recorded | — | src/presentation/godot/ui/kit_frames.cpp | basis: auto |
| Textures/*/Trackbar_Scroll_Down_Button | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Down_Button_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Down_Button_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Down_Button_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Middle | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Middle_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Tab | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Tab_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Up_Button | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Up_Button_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Up_Button_Mouse_Over | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Trackbar_Scroll_Up_Button_Pressed | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Tooltips/IDC_COMBAT_EFF_TIP/TextID | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Allow_Opportunity_Fire_When_Idle | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | A-02, DG-35, R-01, W-04, WCC-17; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Attachment_Bone | applied | whole class (src/scene/space_population.cpp) | BP-30, CF-06, DG-11, DG-31, DG-36 | — | src/scene/space_population.cpp | basis: auto |
| Collision_Mesh | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/units/unit_combat.cpp) | WAD-20, WAD-21, WAD-23; DG-11, DG-36, DG-39, PD-06, S-45 | — | src/sim/tactical/blast.cpp; src/units/unit_combat.cpp | basis: auto |
| Damage_Decal | applied | whole class (src/scene/space_population.cpp) | BP-15, BP-30, BP-41, BP-42, DG-38 | — | src/scene/space_population.cpp | basis: auto |
| Damage_Particles | applied | whole class (src/scene/space_population.cpp) | BP-15, BP-41, BP-42, DG-38, HD-04 | — | src/scene/space_population.cpp | basis: auto |
| Death_Explosion_Particles | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-15, BP-30, PB-30, PB-31, PB-46 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Death_Explosion_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-17, BA-17 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Engine_Particles | applied | whole class (src/scene/space_population.cpp) | BP-41, HD-04, PB-40, PB-41 | — | src/scene/space_population.cpp | basis: auto |
| Fire_Bone_A | applied | whole class (src/units/unit_combat.cpp) | A-06, AT-07, AT-09, BP-30, W-07 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Bone_B | applied | whole class (src/units/unit_combat.cpp) | IS-02, IS-04, W-06, W-06a, W-10 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-14, AU-15, BA-07, BA-14 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Model_To_Attach | applied | whole class (src/scene/scene_assets.cpp) | A-04, A-06, BP-35, BP-46, BP-47 | — | src/scene/scene_assets.cpp | basis: auto |
| Randomize_Between_Fire_Bones | todo | application not recorded | — | 653 | — | — |
| Tooltip_Text | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-51 | — | apps/viewer/src/world_ui_prepare.cpp | — |

## HardPoint, Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Barrel_Bone_Name | partial | one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-06, WAD-40 | 1075 | src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Bone_Name | partial | one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-06, WAD-40 | 1075 | src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |

## Hero_Clash

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Clash_Actions/Attack_Action/@Name | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Attack_Action/Attack_Animation_Speedup | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Attack_Action/Attack_Animation_Subindex | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Attack_Action/Attack_Animation_Type | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Attack_Action/Attack_Firing_Bone_Name | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Attack_Action/Attack_SFXEvent | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Move_Action/@Name | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Move_Action/Move_Animation_Override | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Move_Action/Move_Animation_Speedup | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Move_Action/Move_Animation_Subindex | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Move_Action/Move_Animation_Type | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Special_Ability_Action/@Name | todo | application not recorded | — | 653 | — | — |
| Clash_Actions/Special_Ability_Action/Ability_Name | todo | application not recorded | — | 653 | — | — |
| First_Hero_Conversation_Anim_Type | todo | application not recorded | — | 653 | — | — |
| First_Hero_Draw_Anim_Type | todo | application not recorded | — | 653 | — | — |
| First_Hero_Lose_Anim_Type | todo | application not recorded | — | 653 | — | — |
| First_Hero_Win_Anim_Type | todo | application not recorded | — | 653 | — | — |
| First_Hero_Win_Exchange_Chance | todo | application not recorded | — | 653 | — | — |
| First_Hero_Win_Speech | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Conversation_Anim_Type | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Draw_Anim_Type | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Lose_Anim_Type | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Win_Anim_Type | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Win_Exchange_Chance | todo | application not recorded | — | 653 | — | — |
| Second_Hero_Win_Speech | todo | application not recorded | — | 653 | — | — |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Absorb_Blaster_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Speedup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Subindex | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Berserker_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Drain_Life_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Drain_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Drain_Source_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Drain_Target_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Drain_Life_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Earthquake_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Damage_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Earthquake_Attack_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Earthquake_Attack_Ability/Terminate_Effect_On_Move_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Abilities/Find_Weakness_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Force_Cloak_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Force_Cloak_Ability/Force_Cloak_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Force_Confuse_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Confuse_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Owner_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Force_Healing_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Lightning_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Damage_Application_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Lightning_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Lightning_Max_Targets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Lightning_Source_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Lightning_Target_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Lightning_Targets_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Force_Lightning_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Start_Lightning_Frame_Number | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Lightning_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine) |
| Abilities/Force_Sight_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Force_Telekinesis_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Damage_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Force_Telekinesis_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Telekinesis_Ability/Terminate_Effect_On_Move_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Owner_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Galactic_Stealth_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Chewbacca, Droid_C3P0, Droid_R2D2, Han_Solo, IG-88, Jabba_The_Hutt (+10 more)) |
| Abilities/Generic_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Attack_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Attack_Animation_Speedup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Grenade_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Grenade_Toss_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Hack_Super_Weapon_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Hack_Super_Weapon_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Hack_Super_Weapon_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Hero_Protection_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Han_Solo, Jabba_The_Hutt) |
| Abilities/Infection_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Infection_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Shoot_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Neutralize_Hero_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Mara_Jade) |
| Abilities/Personal_Flame_Thrower_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Flame_Emitter_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Radioactive_Contaminate_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Radioactive_Contaminate_Ability/Contamination_Object_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Radioactive_Contaminate_Ability/SFXEvent_Radioactive | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Redirect_Blaster_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Retreat_Prevention_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Saber_Throw_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Saber_Throw_Ability/Saber_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Stealth_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stealth_Ability/Stealth_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Owner_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Abilities/Stun_Ability/Stunned_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Summon_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/SFXEvent_Unit_Summoned | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/System_Spy_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, General_Dodonna, Han_Solo (+5 more)); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/Tactical_Bribe_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/Effect_Z_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/SFXEvent_Fail | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/SFXEvent_Success | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Vehicle_Thief_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |
| Base_Shield_Penetration_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Blob_Shadow_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Blob_Shadow_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Blob_Shadow_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Death_Clone_Is_Obstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Dense_FOW_Reveal_Range_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Dynamic_Transform_End_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Dynamic_Transform_End_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Dynamic_Transform_Start_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Dynamic_Transform_Start_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Has_Pre_Turn_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Highlight_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+19 more)) |
| Holster_Drawn_Bone_Translation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Holster_Holstered_Bone_Translation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Holster_Minimum_Drawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Holster_Weapon_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Idle_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+22 more)) |
| Is_Visible_On_Enemy_Radar | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06, MME-4, MME-5 | — | src/presentation/ui/minimap.cpp | basis: auto |
| IsDeathCloneObstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| LOD_Bias | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88 (+12 more)) |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Primary_Locomotor_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88) |
| Rotation_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Droid_C3P0, Droid_R2D2, Kyle_Katarn, Mara_Jade) |
| Scale_Factor | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. |
| Secondary_Locomotor_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett) |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Selection_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+19 more)) |
| SFXEvent_Announce | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Attacked | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Deploy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Han_Solo, Jabba_The_Hutt) |
| SFXEvent_Draw_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| SFXEvent_Engine_Cinematic_Focus_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| SFXEvent_Guard | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. |
| SFXEvent_Holster_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88, Silri) |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, Emperor_Palpatine (+15 more)) |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88, Yoda) |
| Walk_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Mara_Jade) |

## HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Explosions | applied | Projectile (apps/viewer/src/battle_effects_prepare.cpp); whole class (apps/viewer/src/debris_props.cpp) | BP-70; BP-14, BP-34, PB-33, SP-03, SP-08 | — | apps/viewer/src/battle_effects_prepare.cpp; apps/viewer/src/debris_props.cpp | basis: auto |

## HeroUnit, MiscObject, SpaceUnit, TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Mouse_Collide_Override_Sphere_Radius | applied | whole class (apps/viewer/src/battle_input.cpp); whole class (src/presentation/ui/selection.cpp) | WSU-10, WSU-11, WSU-12; WSU-10, WSU-11, WSU-12 | — | apps/viewer/src/battle_input.cpp; src/presentation/ui/selection.cpp | basis: reviewed |

## HeroUnit, Mobile_Defense_Unit, Projectile, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Idle_Loop | partial | ordinary space locomotors (apps/viewer/src/battle_audio_events.cpp); missing: land, cinematic and non-locomotor callers | BA-82, BA-83 | 1501 | apps/viewer/src/battle_audio_events.cpp | Live ordinary-space idle/moving attachment; cinematic focus and other modes remain separate.; basis: reviewed |
| SFXEvent_Engine_Moving_Loop | partial | ordinary space locomotors (apps/viewer/src/battle_audio_events.cpp); missing: land, cinematic and non-locomotor callers | BA-82, BA-83 | 1501 | apps/viewer/src/battle_audio_events.cpp | Live ordinary-space idle/moving attachment; cinematic focus and other modes remain separate.; basis: reviewed |

## HeroUnit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bounds_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | ESU-20, WSU-36, WSU-54, WU-18 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## HeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Moving | partial | live space craft and ships with movement paths (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception, story-cinematic visibility bypass | BA-80, BA-81, SND-46 | 1501 | apps/viewer/src/battle_audio_events.cpp | Finite attached moving cues; normal engine and continuous ambient loops remain separate gaps.; basis: reviewed |
| SFXEvent_Health_Critical_Warning | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt (+9 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Hobbie_XWing (+27 more). Mixed ground/space, space carriers on HeroUnit: Luke_Skywalker. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Hobbie_XWing (+27 more). |
| SFXEvent_Health_Low_Warning | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt (+9 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Hobbie_XWing (+27 more). Mixed ground/space, space carriers on HeroUnit: Luke_Skywalker. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Biggs_XWing, Escort_TIE_Fighter, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Hobbie_XWing (+27 more). |

## HeroUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: effective authoring supplies only land or galactic payload consumers for these classes; no M2 space target.; SCOPE-LAND |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/@Name | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Enemy_Health_Critical_Warning | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Enemy_Health_Low_Warning | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Specific_Death_Anim_Index | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: auto |

## LandBombingUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Air_Vehicle_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Damage_Hit_Particles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Damaged_Smoke_Asset_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fires_Forward | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hover_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Lift | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxFacingLookAheadFrames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| No_Colorization_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Prepare_Strafe_Height | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Bomb_Run_Incoming | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Bomb_Run_Select_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Hit_Particles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Explosion | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_SFXEvent_Start_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| XSFXEvent_Ambient_Loop | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| No_Reflection_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| No_Refraction_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Scale_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Text_ID | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Props_Generic, Props_Story, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, Prop_Desert, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Layer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, Prop_Desert, Prop_Forest, Props_Snow, Props_Story, Props_Swamp, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Blob_Shadow_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Blob_Shadow_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AI_Combat_Power | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Armor_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| CategoryMask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Collidable_By_Projectile_Living | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Explosions | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_SFXEvent_Start_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Energy_Capacity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Energy_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Visible_On_Radar | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_FOW_Reveal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Mass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MovementClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ranged_Target_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Remove_Upon_Death | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Score_Cost_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Select_Box_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Points | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Size_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Health | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Victory_Relevant | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bank_Turn_Angle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Guard_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Idle_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Rate_Of_Roll | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Thrust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Min_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Pulse_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Fire | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Guard | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Select | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Max_Attack_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Elevate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rotate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandBombingUnit, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Exclude_From_Distance_Fade | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LandPrimarySkydome, LandSecondarySkydome, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Galactic_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LensFlares

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LensFlare/@Name | todo | application not recorded | — | 653 | — | — |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Color_End | todo | application not recorded | — | 653 | — | — |
| Color_Start | todo | application not recorded | — | 653 | — | — |
| Fadeout_Time_Max | todo | application not recorded | — | 653 | — | — |
| Fadeout_Time_Min | todo | application not recorded | — | 653 | — | — |
| Replace_Faded | todo | application not recorded | — | 653 | — | — |
| Texture_Repeat | todo | application not recorded | — | 653 | — | — |
| Texture_Scroll | todo | application not recorded | — | 653 | — | — |

## LightningEffect, Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Name | todo | application not recorded | — | 653 | — | — |
| Texture_Name | todo | application not recorded | — | 653 | — | — |

## LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Destruct_Fade_Time | todo | application not recorded | — | 653 | — | — |
| Blob_Color | todo | application not recorded | — | 653 | — | — |
| Blob_Intensity | todo | application not recorded | — | 653 | — | — |
| Blob_Radius | todo | application not recorded | — | 653 | — | — |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Faction_Anim_Subindex | partial | space capture-point ghosts (apps/viewer/src/live_session_prepare.cpp); missing: live owner animation and non-space models | FW-30 | 653 | apps/viewer/src/live_session_prepare.cpp | The validated mapping selects neutral IDLE art for remembered capture points.; basis: reviewed |
| Land_Model_Name | land-or-galactic | application not recorded | — | — | — | asset-formats.md: Land_Model_Name on a land map, Space_Model_Name on a space map |
| Reinforcement_Region_Blob_Name | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Debug_Test_Loop | todo | application not recorded | — | 653 | — | — |
| Tooltip_Text | todo | application not recorded | — | 653 | — | — |

## Marker, MiscObject, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Model_Name | applied | whole class (apps/viewer/src/live_session_view.cpp); whole class (apps/viewer/src/space_populate_compose.cpp); Projectile (apps/viewer/src/unit_emitters_prepare.cpp) | BP-01, BP-40, BP-60, PB-01, PB-63; MD-06; BP-70 | — | apps/viewer/src/live_session_view.cpp; apps/viewer/src/space_populate_compose.cpp; apps/viewer/src/unit_emitters_prepare.cpp | basis: reviewed |

## Marker, MiscObject, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Icon_Name | applied | whole class (src/presentation/ui/minimap.cpp); whole class (src/presentation/ui/minimap.cpp) | MM-16; MM-06, MME-4, MME-5 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Begin_Control_Transition_Radar_Event | todo | application not recorded | — | 653 | — | — |
| End_Control_Transition_Radar_Event | todo | application not recorded | — | 653 | — | — |

## Marker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Initial_State_Visible_Under_FOW | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (src/script/foc/ai_goals.cpp) | FW-25; WNO-13 | — | apps/viewer/src/live_session_frame.cpp; src/script/foc/ai_goals.cpp | Seeds local model knowledge when last-state retention is enabled; independently bypasses forced object-goal fog admission.; basis: reviewed |

## Marker, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Select_Box_Z_Adjust | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). |

## Marker, SecondaryStructure, SpaceBuildable, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Last_State_Visible_Under_FOW | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (src/script/foc/ai_goals.cpp) | FW-25, FW-26, FW-27, FW-28, FW-29; WNO-13 | — | apps/viewer/src/live_session_frame.cpp; src/script/foc/ai_goals.cpp | basis: reviewed |

## Marker, SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Loop | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+29 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Jedi_Cruiser (+8 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+29 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Jedi_Cruiser (+8 more). |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Multisample_FOW_Check | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Officer_Academy (+51 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Officer_Academy (+51 more). |
| Terrain_Texture_Modifier_Join_Distance | todo | application not recorded | — | 653 | — | — |
| Terrain_Texture_Modifier_Material | todo | application not recorded | — | 653 | — | — |
| Terrain_Texture_Modifier_Square | todo | application not recorded | — | 653 | — | — |

## Marker, SecondaryStructure, SpecialStructure, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Special_Weapon_Ready | deferred | application not recorded | — | 1505 | — | BA-56: source readiness cue, including space planetary-use upgrades, requires an implemented enabled/ready special-weapon state; build-complete stays separate. |

## Marker, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Show_Facing | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06, MME-4, MME-5 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Marker, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Range_Icon_Name | todo | application not recorded | — | 653 | — | — |

## Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Shader_Name | todo | application not recorded | — | 653 | — | — |
| Texture_Name_2 | todo | application not recorded | — | 653 | — | — |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Beacon_Lifetime_In_Secs | todo | application not recorded | — | 653 | — | — |
| Beacon_Radar_Map_Event_Name | todo | application not recorded | — | 653 | — | — |
| Encyclopedia_Unit_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |
| IsDeathCloneObstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |
| Model_Name | applied | whole class (apps/viewer/src/live_session_view.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp) | none recorded; WAD-38 | — | apps/viewer/src/live_session_view.cpp; src/units/unit_tables_decode.cpp | The existing viewer model-name binding has no named rule; WAD-38 adds the canonical target's collision geometry.; basis: reviewed |
| Not_Really_Selectable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine) |
| SFXEvent_Beacon_Placed | todo | application not recorded | — | 653 | — | — |

## MiscObject, Particle, SpacePrimarySkydome, SpaceProp, SpaceSecondarySkydome, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Exclude_From_Distance_Fade | todo | application not recorded | — | 653 | — | — |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Shield_Penetration_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Blob_Shadow_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Blob_Shadow_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Deployment_Anim_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Encyclopedia_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Encyclopedia_Unit_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SFXEvent_Fleet_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SFXEvent_Guard | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SFXEvent_Unit_Lost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Visible_To_Enemies_When_Empty | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Wind_Disturbance_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Wind_Disturbance_Sphere_Alpha | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Wind_Disturbance_Strength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |

## MousePointers

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| MousePointer/@Name | applied | whole class (apps/viewer/src/battle_cursor.cpp); whole class (src/data/ui/cursors.cpp) | CU-02, CU-03; CU-01 | — | apps/viewer/src/battle_cursor.cpp; src/data/ui/cursors.cpp | basis: reviewed |
| MousePointer/Anim_Frame_Delay | applied | whole class (apps/viewer/src/battle_cursor.cpp); whole class (src/data/ui/cursors.cpp) | CU-02, CU-03; CU-01 | — | apps/viewer/src/battle_cursor.cpp; src/data/ui/cursors.cpp | basis: reviewed |
| MousePointer/Base_Texture | applied | whole class (apps/viewer/src/battle_cursor.cpp); whole class (src/data/ui/cursors.cpp) | CU-02, CU-03; CU-01 | — | apps/viewer/src/battle_cursor.cpp; src/data/ui/cursors.cpp | basis: reviewed |
| MousePointer/Hot_X | applied | whole class (apps/viewer/src/battle_cursor.cpp); whole class (src/data/ui/cursors.cpp) | CU-02, CU-03; CU-01 | — | apps/viewer/src/battle_cursor.cpp; src/data/ui/cursors.cpp | basis: reviewed |
| MousePointer/Hot_Y | applied | whole class (apps/viewer/src/battle_cursor.cpp); whole class (src/data/ui/cursors.cpp) | CU-02, CU-03; CU-01 | — | apps/viewer/src/battle_cursor.cpp; src/data/ui/cursors.cpp | basis: reviewed |

## MOV_Cinematic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Asteroid_Damage_Hit_Particles | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Attack_Move_Response_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Autonomous_Move_Extension_Vs_Attacker | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Avoidance_Disabled | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Company_Units | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Death_Fade_Time | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Death_Persistence_Duration | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Dense_FOW_Reveal_Range_Multiplier | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Guard_Chase_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| GUI_Row | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Idle_Chase_Range | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Is_Escort | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Is_Valid_Target | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Land_Model_Anim_Override_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Maintenance_Cost | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| No_Colorization_Color | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| OverrideDeceleration | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Ambient_Loop | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Attack_Hardpoint | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Build_Cancelled | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Build_Complete | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Build_Started | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Damaged_By_Asteroid | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Engine_Cinematic_Focus_Loop | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Engine_Idle_Loop | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Engine_Moving_Loop | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Fleet_Move | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Move_Into_Asteroid_Field | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Move_Into_Nebula | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Targeting_Priority_Set | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Targeting_Stickiness_Time_Threshold | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Movie

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Alpha | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Cannot_Skip | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Caption_Duration | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Caption_Frame | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Commandbar_Offset | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Halt_Game_Music | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Has_Credits | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Movie_File | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Movie_Start_Frame | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Overlay | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Pause_Game_While_Playing | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SFXEvent_Frame | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| SpeechEvent_Frame | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Text_Crawl_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Text_Crawl_Start_Frame | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## MultiplayerStructureMarker, Particle, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | applied | whole class (apps/viewer/src/space_populate_animation.cpp); Projectile (apps/viewer/src/unit_emitters_prepare.cpp); whole class (apps/viewer/src/world_ui_prepare.cpp) | MD-06; BP-70; AV-05, E71-15, E71-18, E71-19, R-ROT-01 | — | apps/viewer/src/space_populate_animation.cpp; apps/viewer/src/unit_emitters_prepare.cpp; apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |

## MusicEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fade_In_Seconds | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | AU-28, BA-44 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fade_Out_Previous_Seconds | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | AU-28, BA-44 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |

## Particle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Allow_Particle_Model_Culling | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Ewok_Suicide_Bomb_Explosion, Huge_Explosion_Land, Large_Explosion_Land, Medium_Explosion_Land, p_orbital_impact, p_orbital_impact_ion, Remote_bomb. Mixed ground/space, space carriers on Particle: Ewok_Suicide_Bomb_Explosion, Huge_Explosion_Land, Large_Explosion_Land, Medium_Explosion_Land, p_orbital_impact, p_orbital_impact_ion, Remote_bomb. |
| Dynamic_Transform_End_Color | todo | application not recorded | — | 653 | — | — |
| Dynamic_Transform_End_Scale | todo | application not recorded | — | 653 | — | — |
| Dynamic_Transform_Start_Color | todo | application not recorded | — | 653 | — | — |
| Dynamic_Transform_Start_Scale | todo | application not recorded | — | 653 | — | — |
| Explosion_Jitter_Factor | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Friggin_Huge_Explosion_Space_Empire, Harmonic_Bomb_Explosion_Slave_I, Huge_Explosion_Land, Huge_Explosion_Space, Huge_Explosion_Space_Empire (+18 more). Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Friggin_Huge_Explosion_Space_Empire, Harmonic_Bomb_Explosion_Slave_I, Huge_Explosion_Land, Huge_Explosion_Space, Huge_Explosion_Space_Empire (+18 more). |
| Explosion_Scorch_Mark | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Huge_Explosion_Land, Huge_Explosion_Space, Large_Damage_Land, Large_Explosion_Land, Large_Explosion_Space (+20 more). Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Huge_Explosion_Land, Huge_Explosion_Space, Large_Damage_Land, Large_Explosion_Land, Large_Explosion_Space (+20 more). |
| Particle_Attach_To_Collision | partial | Damage_Hit_Particles and Shield_Hit_Particles entries (apps/viewer/src/battle_effects_impacts.cpp); projectile shield-hit facing (apps/viewer/src/battle_effects_impacts.cpp); missing: Other collision-attached particle hosts retain their existing ownership gap | BP-63, PS-02, PS-03, PS-04; BP-18 | 1481 | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |
| Particle_Lifetime_Frames | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-16, BP-33, PB-34 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |

## Particle, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Model_Visible_To_Enemy | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| IsDeathCloneObstacle | todo | application not recorded | — | 653 | — | — |
| Mouse_Collide_Override_Sphere_Radius | todo | application not recorded | — | 653 | — | — |
| Projectile_Absorbed_By_Shields_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-10, BP-17, BP-19, BP-20, PB-10 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Combat_Mod_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_MAL_Carbonite_Missile) |
| Projectile_Custom_Render | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-01, BP-40, BP-60, PB-01, PB-63 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Laser_Color | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-03, PB-02, PB-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Lifetime_Detonation_Particle | partial | ordinary space projectiles and HARMONIC_BOMB/WEAKEN_ENEMY spawned projectiles (apps/viewer/src/battle_effects_impacts.cpp); missing: land projectiles (&#35;653) | WAD-07, WHE-62 | 653 | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |
| Projectile_Object_Armor_Reduced_Detonation_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-11, PB-10 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Object_Creation_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Tyber_Zann_Blaster_Shotgun) |
| Projectile_Object_Detonation_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-11, PB-10 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_SFXEvent_Detonate | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-16, BA-15, BP-13; WPJ-35, WPJ-37, WPJ-40 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Projectile_SFXEvent_Detonate_Reduced_By_Armor | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-16, BA-15, BP-13 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Projectile_Texture_Slot | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-03, BP-06, PB-02, PB-03, PB-06 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 653 | — | — |

## Projectile, TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Dense_FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 653 | — | Hero and transport definitions are outside the traced M2 fog roster; their authored dense multipliers still need execution validation. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. |

## Prop_Desert, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_YExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Prop_Forest, Props_Snow, Props_Swamp, Props_Temperate, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_YExtent_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_X_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Y_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Discardable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Obstacle_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Props_Story, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Distance_From_Spawner | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Obstacle_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_In_Packs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Quantity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Desert, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Indigenous_Unit_Corruptible | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Prop_Forest, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Model_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Specific_Faction | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Stacking_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Combat_Bonus_Ability/Unit_Strength_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Avoidance_Disabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_By_TSW_Replacements | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Projectiles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationGrouping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationOrder | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ground_Vehicle_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Affected_By_Gravity_Control_Field | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Override_Bounty_SP | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Show_In_Sidebar_When_Complete | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Snap_Movement_Orders_To_Center | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Weather_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Sphere_Alpha | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Strength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Single_Target_Heal | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Object_Only | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Collidable_By_Projectile_Dead | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Encyclopedia_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Bounds_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Bracket_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Has_Land_Evaluator | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Influences_Capture_Point | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Branched_Map_Discardable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Valid_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MinimumPushReturnDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Occlusion_Silhouette_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Overall_Length | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Overall_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OverrideAcceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OverrideDeceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Political_Control | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Property_Flags | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reveal_During_Setup_Phase | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Engine_Idle_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Engine_Moving_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Generic, Props_Story, Props_Swamp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Unit_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Idle_Anim_00_Rate_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Heal_Percent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Block_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Stacking_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Additional_Population_Capacity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Apply_Y_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Apply_Z_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Asteroid_Damage_Hit_Particles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Attack_Move_Response_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Autonomous_Move_Extension_Vs_Attacker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Position | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Blob_Shadow_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Cost_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Limit_Current_For_All_Allies | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Limit_Lifetime_Per_Player | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Tab_Space_Station | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Tab_Space_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| CanCellStack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Blend_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Index | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Company_Transport_Unit | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Company_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Create_Team_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Soft_Footprint_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Clone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Clone_Is_Obstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Fade_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Persistence_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dense_FOW_Reveal_Range_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_End_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_End_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_Start_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dynamic_Transform_Start_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Encyclopedia_Good_Against | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Encyclopedia_Vulnerable_To | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Enemy_Spawn_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Inaccuracy_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Formation_Priority | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Friendly_Spawn_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GalacticBehavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Garrison_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ground_Infantry_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Hide_Health_Bar | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Row | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Velocity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| HardPoints | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Has_Space_Evaluator | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Highlight_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Disable_Engine_Loops | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Drawn_Bone_Translation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Holstered_Bone_Translation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Minimum_Drawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Transition_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Holster_Weapon_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hyperspace | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hyperspace_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Immune_To_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Include_In_UI_Map_Header | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Initial_State_Visible_Under_FOW | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Dummy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Escort | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Force_Sensitive | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Marker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Named_Hero | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Squashable_By_Supercrusher | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Stationary_When_Attacking | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Damage_Alternates | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Damage_SFX | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Damage_Thresholds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Model_Anim_Override_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Last_State_Visible_Under_FOW | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Lua_Script | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Maintenance_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Squad_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Mouse_Collide_Override_Sphere_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MP_Encyclopedia_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Multisample_FOW_Check | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Next_Level_Base | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Pause_During_Cinematic_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Play_SFXEvent_On_Sighting | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Political_Faction | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Population_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Prev_Level_Base | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Primary_Locomotor_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Appearance_Delay_Frames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Scale_Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Scale_Space | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Range_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Rotate_Icon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Show_Facing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ranking_In_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reinforcement_Prevention_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Ground_Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Planets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Special_Structures | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Star_Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Timeline | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reserve_Spawned_Units_Tech_0 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Retreat_Self_Destruct_Explosion | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Selection_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Assist_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Assist_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Attack_Hardpoint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Bombard_Incoming | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Bombard_Select_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Build_Cancelled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Build_Complete | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Build_Started | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Damaged_By_Asteroid | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Draw_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Engine_Cinematic_Focus_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Fleet_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Group_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Group_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Hardpoint_Destroyed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Health_Critical_Warning | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Health_Low_Warning | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Hero_Respawned | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Holster_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Move_Into_Asteroid_Field | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Move_Into_Nebula | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Cancelled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Complete | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Started | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Turret_Rotating_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Unit_Lost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Unit_Under_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Armor_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ship_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_FOW_Reveal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Full_Stop_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpaceBehavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Planet | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Pack_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Squadron_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Specific_Death_Anim_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Squadron_Capacity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Starting_Spawned_Units_Tech_0 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Bribe_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Cost_Multiplayer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Prerequisites | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Buildable_Objects_Campaign | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Production_Queue | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Priority_Set | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Stickiness_Time_Threshold | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tech_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tread_Scroll_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rest_Angle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rotate_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| User_Bound_Max | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| User_Bound_Min | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Visible_On_Radar_When_Fogged | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Props_Swamp, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Editor_Placed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## RadarMapEvents

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Map_Event/@name | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Duration | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Model_Name | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Model_Scale | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Persistent | todo | application not recorded | — | 653 | — | — |
| Radar_Map_Event/Event_Single_Instance | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |

## RadarMapSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-14 | — | src/presentation/ui/minimap.cpp | the entry named space fills the background layer; basis: reviewed |
| Color/@name | todo | application not recorded | — | 653 | — | — |
| Passability_Color_Settings/@name | todo | application not recorded | — | 653 | — | — |
| Passability_Color_Settings/Color | todo | application not recorded | — | 653 | — | nothing reads the passability colours; the minimap draws its own |
| Passability_Color_Settings/Color/@name | todo | application not recorded | — | 653 | — | — |
| Space_Asteroid_Field_Border_Color | partial | space tactical instances (src/presentation/ui/minimap.cpp); missing: land minimaps | WHZ-72 | 653 | src/presentation/ui/minimap.cpp | basis: reviewed |
| Space_Asteroid_Field_Color | partial | space tactical instances (src/presentation/ui/minimap.cpp); missing: land minimaps | WHZ-72 | 653 | src/presentation/ui/minimap.cpp | basis: reviewed |
| Space_Backdrop_Texture_Name | applied | whole class (src/presentation/godot/ui/minimap_view.cpp) | MM-01, MME-1 | — | src/presentation/godot/ui/minimap_view.cpp | basis: auto |
| Space_FOW_Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-01, MME-1 | — | src/presentation/ui/minimap.cpp | basis: auto |
| Space_Is_Guide_Rectangle | applied | whole class (src/presentation/ui/minimap.cpp) | MM-01, MME-1 | — | src/presentation/ui/minimap.cpp | basis: auto |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_FOW_Reveal_Range | applied | whole class (src/skirmish/start.cpp) | WBP-01 | — | src/skirmish/start.cpp | basis: reviewed |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| MP_Encyclopedia_Text | presentation-later | application not recorded | — | — | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Research_Facility, Empire_Ground_Mining_Facility (+24 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Research_Facility, Empire_Ground_Mining_Facility (+24 more).; SCOPE-MENU |

## SecondaryStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Unit_Under_Attack | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). |

## SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Clone_Is_Obstacle | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). |
| SFXEvent_Unit_Lost | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+3 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+3 more). |

## SecondaryStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Should_Death_Clone_Play_Idle | todo | application not recorded | — | 653 | — | — |

## SFXEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Chained_SFXEvent | applied | whole class (src/presentation/audio/sfx.cpp) | SND-09 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Is_Ambient_VO | applied | WAV events (src/presentation/audio/sfx.cpp) | SND-16, SND-17 | — | src/presentation/audio/sfx.cpp | Admission flags are independent of the Localize gain category; mode facts are supplied by the caller.; basis: reviewed |
| Is_GUI | applied | WAV events (src/presentation/audio/sfx.cpp) | SND-16, SND-17 | — | src/presentation/audio/sfx.cpp | Admission flags are independent of the Localize gain category; mode facts are supplied by the caller.; basis: reviewed |
| Is_HUD_VO | applied | WAV events (src/presentation/audio/sfx.cpp) | SND-16, SND-17 | — | src/presentation/audio/sfx.cpp | Admission flags are independent of the Localize gain category; mode facts are supplied by the caller.; basis: reviewed |
| Is_Unit_Response_VO | applied | WAV events (src/presentation/audio/sfx.cpp) | SND-16, SND-17 | — | src/presentation/audio/sfx.cpp | Admission flags are independent of the Localize gain category; mode facts are supplied by the caller.; basis: reviewed |
| Kills_Previous_Object_SFX | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-19 | — | apps/viewer/src/battle_audio_events.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Localize | partial | spatial and 2D WAV events (apps/viewer/src/battle_audio_events.cpp); WAV events (src/presentation/audio/sfx.cpp); missing: non-English sample localization | SND-60; SND-16, SND-17 | 1498 | apps/viewer/src/battle_audio_events.cpp; src/presentation/audio/sfx.cpp | Admission and speech/SFX gain routing apply; selected-language sample localization is separate.; basis: reviewed |
| Loop_Fade_In_Seconds | partial | ordinary space engine loops (apps/viewer/src/battle_audio_events.cpp); missing: general attached SFX lifecycle | BA-83 | 1509 | apps/viewer/src/battle_audio_events.cpp | Engine-loop starts/stops consume authored fades; general event fades remain separate.; basis: reviewed |
| Loop_Fade_Out_Seconds | partial | ordinary space engine loops (apps/viewer/src/battle_audio_events.cpp); missing: general attached SFX lifecycle | BA-83, SND-45 | 1509 | apps/viewer/src/battle_audio_events.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |

## SFXEvent, SpeechEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Text_ID | applied | whole class (src/data/ui/dialog_catalog.cpp) | none recorded | — | src/data/ui/dialog_catalog.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Blob_Color | todo | application not recorded | — | 760 | — | — |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Sensor_Jamming_Ability/Blob_Color | todo | application not recorded | — | 760 | — | — |
| Abilities/Sensor_Jamming_Ability/Blob_Material_Name | todo | application not recorded | — | 760 | — | — |
| Encyclopedia_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | WBP-36 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| GUI_Row | applied | whole class (apps/viewer/src/battle_input_cards.cpp) | WBP-36 | — | apps/viewer/src/battle_input_cards.cpp | basis: reviewed |
| Hides_When_Built_On | applied | whole class (apps/viewer/src/live_session_economy.cpp) | WBP-35 | — | apps/viewer/src/live_session_economy.cpp | basis: reviewed |
| MP_Encyclopedia_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | WBP-36 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| SFXEvent_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Turret_Bone_Name | todo | application not recorded | — | 1075 | — | The scene's four defense satellites author this field on SpaceBuildable objects. Manual turret fields are read through load_hardpoint's manual-turret branch, not the buildable object body; this class's fields are unread. Existing HardPoint and other proven class rows are preserved (WAD-40, MC-06). |
| Visible_To_Enemies_When_Empty | applied | whole class (apps/viewer/src/live_session_economy.cpp) | WBP-35 | — | apps/viewer/src/live_session_economy.cpp | basis: reviewed |

## SpaceBuildable, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/@Name | todo | application not recorded | — | 760 | — | — |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/@Name | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: IG-88. |

## SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | todo | application not recorded | — | 653 | — | — |
| Text_ID | todo | application not recorded | — | 653 | — | — |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | todo | application not recorded | — | 653 | — | — |
| Space_Model_Name | todo | application not recorded | — | 653 | — | — |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Debris_Attached_Particle | applied | whole class (apps/viewer/src/debris_props.cpp) | BP-33, PB-34 | — | apps/viewer/src/debris_props.cpp | basis: auto |
| Debris_Facing_Rotate_Vector | applied | whole class (apps/viewer/src/land_look.cpp) | BP-31, PB-32, PB-33, R-ROT-01 | — | apps/viewer/src/land_look.cpp | basis: auto |
| Debris_Max_Lifetime_Seconds | applied | whole class (src/presentation/space/debris.cpp) | BP-32, PB-34, PB-35 | — | src/presentation/space/debris.cpp | basis: auto |
| Debris_Min_Lifetime_Seconds | applied | whole class (src/presentation/space/debris.cpp) | BP-32, PB-34, PB-35 | — | src/presentation/space/debris.cpp | basis: auto |
| Debris_Movement_Vector | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | BP-31, PB-32, PB-33, R-ROT-01 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |
| Idle_Anim_00_Rate_Mod | applied | whole class (apps/viewer/src/idle_clips.hpp) | none recorded | — | apps/viewer/src/idle_clips.hpp | basis: auto |
| Layer_Z_Adjust | applied | whole class (src/scene/scene_build.cpp) | LZ-01 | — | src/scene/scene_build.cpp | SpaceProps take their height in the shared presentation transform; simulation input stays unchanged; basis: reviewed |
| Scale_Factor | applied | whole class (src/units/unit_motion.cpp) | AV-05, E71-15, E71-18, E71-19, R-ROT-01 | — | src/units/unit_motion.cpp | the map-object footprint loader reads it under an unrecorded trace; basis: reviewed; trace: unrecorded |

## SpaceProp, SpaceStructure, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Visible_On_Radar_When_Fogged | applied | whole class (src/presentation/ui/minimap.cpp) | WNO-41 | — | src/presentation/ui/minimap.cpp | Typed radar admission bypasses hidden-model and fade rejection only; ordinary final visibility gates remain.; basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_FOW_Reveal_Range | applied | ship (src/skirmish/start.cpp) | WHZ-50, WHZ-51 | — | src/skirmish/start.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Cluster_Bomb_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Cluster_Bomb_Ability/SFXEvent_Activate | deferred | application not recorded | — | 1505 | — | BA-55: authored space payload has no implemented M2 action consumer; no inferred activation cue. |
| Abilities/Laser_Defense_Ability/Bone_Names | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Frames | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Texture | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Width | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Color | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Lightning_Effect_Name | presentation-later | application not recorded | — | — | — | WLD-12: the particle is authored but its placement is unread; the beam draws without it |
| Abilities/Laser_Defense_Ability/Zap_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WLD-12 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Abilities/Leech_Shields_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Bone_Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Color | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Effect_Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Texture_Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Leech_Effect | todo | application not recorded | — | 760 | — | — |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | deferred | application not recorded | — | 1505 | — | BA-55: authored payload has no implemented M2 space action consumer; no inferred effect cue. |
| Abilities/Super_Laser_Ability/Attack_Anim_Sub_Index | todo | application not recorded | — | 760 | — | — |
| Radar_Clip_To_Visible_Region | todo | application not recorded | — | 653 | — | — |
| Selt_Destruct_SFXEvent_Start_Die | applied | SELF_DESTRUCT countdown start sound (apps/viewer/src/battle_audio_prepare.cpp) | WSD-11 | — | apps/viewer/src/battle_audio_prepare.cpp | — |
| SFXEvent_Attack_With_Non_Hero_Ability | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Move_Opposite | todo | application not recorded | — | 653 | — | — |
| TSW_Attack_Anim_Duration_Seconds | todo | application not recorded | — | 653 | — | — |
| TSW_Explosion_Debris_Creation_Frame_Delay | todo | application not recorded | — | 653 | — | — |
| TSW_MusicEvent_Activation_Start | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| TSW_Post_Music_Wait_Frames | todo | application not recorded | — | 653 | — | — |
| TSW_SFXEvent_Activate_Voice | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| TSW_SFXEvent_GUI_Bttton_Press | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |
| TSW_SFXEvent_Weapon_Power_Up | deferred | application not recorded | — | 1505 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX. |

## SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |

## SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | todo | application not recorded | — | 760 | — | — |

## SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Cinematic_Focus_Loop | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion. |

## SpaceUnit, Squadron, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Build_Cancelled | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-61 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/Owner_Particle_Effect | todo | application not recorded | — | 760 | — | — |
| Abilities/Stealth_Ability/@Name | applied | whole class (src/units/unit_abilities.cpp) | WST-01, WST-02 | — | src/units/unit_abilities.cpp | basis: reviewed |
| Abilities/Stealth_Ability/SFXEvent_Activate | deferred | application not recorded | — | 1505 | — | BA-55: authored space payload has no implemented M2 action consumer; no inferred activation cue. |
| Abilities/Stealth_Ability/Stealth_Color | presentation-later | application not recorded | — | 1876 | — | WST-12: parsed into the handler; the fade's tint is not drawn (every FoC value is white, no change). |
| LOD_Bias | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Silri, Silri_No_Abilities, Tyber_Zann (+7 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+5 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+5 more). |
| SFXEvent_Tactical_Build_Cancelled | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-61 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Tactical_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Tactical_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Multisample_FOW_Check | applied | craft, ship, squadron, station (src/sim/tactical/session_state.cpp) | V-21 | — | src/sim/tactical/session_state.cpp | basis: reviewed |
| SFXEvent_Attack_Hardpoint | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-25 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Hardpoint_Destroyed | todo | application not recorded | — | 653 | — | — |

## SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Asteroid_Damage_Hit_Particles | applied | space tactical instances (apps/viewer/src/battle_effects_impacts.cpp) | WHZ-14 | — | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |

## SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Beam_Texture_Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Line_Color | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Num_Anim_Frames | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 760 | — | — |
| Glory_Cinematics | todo | application not recorded | — | 653 | — | — |
| Radar_Register_As_Foreground_Object | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | partial | live space craft and ships with moving cues (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception | BA-80, SND-46 | 1501 | apps/viewer/src/battle_audio_events.cpp | Positive ordered integer seconds become logical frames for initialization and every due service.; basis: reviewed |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | partial | live space craft and ships with moving cues (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception | BA-80, SND-46 | 1501 | apps/viewer/src/battle_audio_events.cpp | Positive ordered integer seconds become logical frames for initialization and every due service.; basis: reviewed |
| SFXEvent_Assist_Attack | partial | live space selected object types (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | BA-29, SND-22 | 1500 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Assist_Move | partial | live space selected object types (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | BA-29, SND-22 | 1500 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Bombard_Incoming | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Bombard_Select_Target | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Damaged_By_Asteroid | applied | space tactical instances (apps/viewer/src/battle_audio_events.cpp) | WHZ-14 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Enemy_Damaged_Health_Critical_Warning | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Enemy_Damaged_Health_Low_Warning | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Guard | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-26 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Move_Into_Asteroid_Field | partial | live space ships and leading squadron craft (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | SND-24, SND-25 | 1500 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Move_Into_Nebula | partial | live space ships and leading squadron craft (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | SND-24, SND-25 | 1500 | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Stop | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-26 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Spin_Away_On_Death_Explosion | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | SP-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Spin_Away_On_Death_SFXEvent_Start_Die | applied | craft (apps/viewer/src/battle_audio_prepare.cpp) | BA-16, SP-03 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Target_Bones | applied | whole class (src/sim/tactical/combat.cpp); unit weapon aim draw (src/sim/tactical/combat_fire.cpp) | EWW-09, WWP-50; WWP-50 | — | src/sim/tactical/combat.cpp; src/sim/tactical/combat_fire.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Unit_Abilities_Data/Unit_Ability/Particle_Effect | partial | REPLENISH_WINGMEN (apps/viewer/src/battle_effects_impacts.cpp); missing: other ability particles | WHE-63 | 942 | apps/viewer/src/battle_effects_impacts.cpp | G9 WHE-63 emits the authored replenish particle on every current team member after successful refill; other ability emitters remain separate.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Base_Power_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Gravity_Control_Field_Effect | todo | application not recorded | — | 653 | — | — |
| HQ_Win_Condition_Relevant | todo | application not recorded | — | 653 | — | — |
| Lobbing_Superweapon_Chargeup_Particle | todo | application not recorded | — | 653 | — | — |
| Lobbing_Superweapon_Chargeup_Particle_Bone_Name | todo | application not recorded | — | 653 | — | — |
| Lobbing_Superweapon_SFXEvent_Chargeup | todo | application not recorded | — | 653 | — | — |
| Lobbing_Superweapon_SFXEvent_Fire | todo | application not recorded | — | 653 | — | — |
| Movie_Object | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_Powered_Active_Loop | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Sold_Tactical | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Turret_Rotating_Loop | todo | application not recorded | — | 653 | — | — |
| Shield_Normal_Color | todo | application not recorded | — | 653 | — | — |
| Shield_Off_Anim | todo | application not recorded | — | 653 | — | — |
| Shield_On_Anim | todo | application not recorded | — | 653 | — | — |

## SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Radar_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Enable_Radar_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Enable_Radar_Ability/Affects_All_Allies | todo | application not recorded | — | 760 | — | — |

## SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Material_Name | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). |
| Blob_Shadow_Scale | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). |

## SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt. |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Idle_Chase_Range | applied | squadron (src/units/unit_motion.cpp) | E452-14, E452-15, E75-22, FT-02, OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| Is_Named_Hero | partial | loaded local space battle (apps/viewer/src/live_session_frame.cpp); visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); space purchase catalog (src/skirmish/economy.cpp); space purchase catalog (src/skirmish/inputs.cpp); missing: objects and modes outside the loaded local space battle, squadron-authored presentation admission | WBF-45; WU-47, WU-48; WSS-29; WSS-29 | 653 | apps/viewer/src/live_session_frame.cpp; apps/viewer/src/world_ui_groups.cpp; src/skirmish/economy.cpp; src/skirmish/inputs.cpp | Squadron presentation currently reads admission from its spawned team container; the squadron-authored hero flag has no presentation consumer. Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate. Resolved inherited space hero policy filters the shared human/AI purchase catalog independently of presentation and results scoring. World identities cover visible standalone space heroes; carried heads remain separate.; basis: reviewed |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 653 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |

## Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Hero_Respawned | land-or-galactic | application not recorded | — | — | — | Do not reinterpret ordinary tactical arrival as campaign hero respawn.; debug build SND-E57 and WHE-38: local planetary-spawn cue; standalone skirmish automatic hero respawn rejects |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Radar_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | — |
| GUI_Angles | todo | application not recorded | — | 653 | — | — |
| GUI_X_Rot | todo | application not recorded | — | 653 | — | — |
| Is_Community_Property | applied | whole class (apps/viewer/src/battle_input.cpp) | WSU-16 | — | apps/viewer/src/battle_input.cpp | basis: reviewed |
| Radar_Rotate_Icon | todo | application not recorded | — | 653 | — | read by a loader, but nothing applies the value |
| Retreat_Self_Destruct_Explosion | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Hardpoint_All_Weapons_Destroyed | todo | application not recorded | — | 653 | — | — |
| Visible_To_Enemies_When_Empty | todo | application not recorded | — | 653 | — | — |

## StarWars3DTextScroll

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| TextScroll/@name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/@language | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Fadein_End_Frame | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Fadein_Start_Frame | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Fadeout_Start_Frame | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Font_Character_Padding | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Font_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Font_Size | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Font_Stretch_Factor | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Has_Header | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Header_Font_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Header_Font_Size | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Header_Text_IDs | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Header_Texture_Height_Pow_2 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Header_Texture_Width_Pow_2 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Model_Camera_Bone_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Model_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Polygon_Shader_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Polygon_Tex_Param_Name | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Text_IDs | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Texture_Height_Pow_2 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |
| TextScroll/Data_Specifications/Texture_Width_Pow_2 | presentation-later | application not recorded | — | — | — | SCOPE-SETTINGS |

## SurfaceEffects

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SurfaceFX/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Attach_To_SurfaceFX_Mesh | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Damage_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Decal_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Defense_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Dynamic_Track_Left_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Dynamic_Track_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Dynamic_Track_Right_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Fire_Range_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/FOW_Reveal_Range_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Particle_Min_Energy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Particle_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/SoundFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Speed_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Status_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Surface_Settings/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Surface_Settings/Decal_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Surface_Settings/SoundFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Terrain_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX/Surface_Settings/Terrain_Damage_Delay | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## TacticalCamera

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Distance_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Object_Fade_Begin | todo | application not recorded | — | 653 | — | — |
| Object_Fade_End | todo | application not recorded | — | 653 | — | — |
| Pitch_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | PI-9, RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Per_Zoom_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_When_Zoomed_In | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Zoom_Begin_Fraction | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Yaw_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Blast_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Blast_Ability/Charged_Effect | todo | application not recorded | — | 760 | — | — |
| Abilities/Blast_Ability/Charging_Effect | todo | application not recorded | — | 760 | — | — |
| Abilities/Blast_Ability/SFXEvent_Activate | deferred | application not recorded | — | 1505 | — | BA-55: authored payload has no implemented M2 space action consumer; no inferred effect cue. |
| Abilities/Concentrate_Fire_Attack_Ability/@Name | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/@Name | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Bone_Name | partial | ship (apps/viewer/src/battle_effects_projectiles.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-59 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Effect | partial | ship (apps/viewer/src/battle_effects_projectiles.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-59 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Shield_Flare_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Owner_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | BA-55: land shield-flare or galactic corruption payload; no M2 space target.; SCOPE-LAND |
| Abilities/Super_Laser_Ability/Alternate_Description_Text | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Owner_Particle_Effect | todo | application not recorded | — | 760 | — | — |
| Attach_To_Flagship_During_Space_Battle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Attack_Animation_Is_Overlay | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Base_Shield_Penetration_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Death_Explosions_End | todo | application not recorded | — | 653 | — | — |
| Death_SFXEvent_End_Die | todo | application not recorded | — | 653 | — | — |
| Display_Contained_Hero_Grab_Bars | todo | application not recorded | — | 653 | — | — |
| Has_Pre_Turn_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Idle_Chase_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 653 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Locomotor_Has_Animation_Priority | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Not_Really_Selectable | todo | application not recorded | — | 653 | — | — |
| Rotation_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| SFXEvent_Deploy | todo | application not recorded | — | 653 | — | — |
| SFXEvent_Turret_Rotating_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Specific_Death_Anim_Type | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | partial | UniqueUnit CONCENTRATE_FIRE (src/sim/tactical/session_step_commands.cpp); UniqueUnit SENSOR_JAMMING and MISSILE_SHIELD (src/sim/tactical/session_world.cpp); missing: other UniqueUnit abilities | WHE-24; WHE-32, WPJ-17 | 760 | src/sim/tactical/session_step_commands.cpp; src/sim/tactical/session_world.cpp | Concentrate fire applies its target-centred radius; shield and jamming sources apply authored spatial radii to flight and nonenemy recipient stamps.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | partial | UniqueUnit CONCENTRATE_FIRE (src/sim/tactical/session_step_commands.cpp); UniqueUnit ENERGY_WEAPON and TRACTOR_BEAM (src/sim/tactical/session_step_commands.cpp); missing: other UniqueUnit abilities | WHE-24; WHE-26, WHE-27, WHE-57, WHE-59, WHE-60 | 760 | src/sim/tactical/session_step_commands.cpp | Concentrate fire applies the authored target-centred radius and named nested-handler binding. G7 binds named beam handlers and services tracked targets.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Vehicle_Thief_Inside_Clone | todo | application not recorded | — | 653 | — | — |
| Wind_Disturbance_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Wind_Disturbance_Sphere_Alpha | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Wind_Disturbance_Strength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Enable_Ability/Ability_Name | todo | application not recorded | — | 760 | — | — |
| Abilities/Weatherproof_Ability/@Name | todo | application not recorded | — | 760 | — | — |
| Next_Upgrade_Level_Type | applied | whole class (include/eawr/presentation/ui/production.hpp) | WPR-63 | — | include/eawr/presentation/ui/production.hpp | basis: reviewed |
| Show_In_Sidebar_Tray | todo | application not recorded | — | 653 | — | — |
| Show_In_Sidebar_When_Complete | todo | application not recorded | — | 653 | — | — |

## Weather_Scenario

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @emitter_name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/Ease_Out_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/Emitter_Intensity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/Fog_Plane_Interpolation_Alpha | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Phase/Lightning_Intensity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## WeatherModifies

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| WeatherModifier/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Class_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Description_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Display_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Objective_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Sight_Range_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Sight_Range_Modifier/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Speed_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Speed_Modifier/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Weapon_Accuracy_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WeatherModifier/Weapon_Accuracy_Modifier/@name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
