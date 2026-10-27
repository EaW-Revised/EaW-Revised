# presentation tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 861 | 861 |
| partial | 497 | 497 |
| todo | 1174 | 1174 |
| presentation-later | 0 | 440 |
| foc-ignores | 95 | 95 |
| deferred | 45 | 45 |
| by-design | 8 | 8 |
| land-or-galactic | 0 | 32 |
| multiplayer | 0 | 0 |
| **Total** | **2680** | **3152** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Name | applied | whole class (src/script/foc/ai_data.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/script/foc/ai_data.cpp | basis: auto |

## AmbientMapSounds

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ambient_Map_Sound | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Ambient_Map_Sound/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No AmbientMapSounds definition-name decoder or map ambience scheduler exists; BattleAudio unit cues do not resolve this XML sound-list name.; basis: reviewed |
| AmbientMapSound/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No AmbientMapSounds definition-name decoder or map ambience scheduler exists; BattleAudio unit cues do not resolve this XML sound-list name.; basis: reviewed |
| AmbientMapSound/Ambient | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no ambient-map-sound consumer; only unrelated "ambient" music-mode strings match.; basis: reviewed |

## Audio

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ambient_Map_Sounds | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Audio_Space_3D_Listener_Z_Pullback_Dist | applied | whole class (src/presentation/audio/sfx.cpp) | BA-10, BA-12 | — | src/presentation/audio/sfx.cpp | basis: reviewed |
| Audio_Space_3D_Rolloff_Distance_Mod | applied | whole class (src/presentation/audio/sfx.cpp) | BA-10, BA-11 | — | src/presentation/audio/sfx.cpp | basis: reviewed |
| Audio_Space_3D_Saturation_Distance_Mod | applied | whole class (src/presentation/audio/sfx.cpp) | BA-10, BA-11 | — | src/presentation/audio/sfx.cpp | basis: reviewed |
| Delay_Between_Space_Base_Attack_Announcement_Seconds | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | SND-40 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| MSS_3D_Provider_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Music_Event_Battle_End_Summary_Screen_Lose | partial | loaded local space battle (apps/viewer/src/battle_audio_events.cpp); galactic battle result summary (apps/viewer/src/galactic/galactic_audio.cpp); missing: objects and modes outside the loaded local space battle and galactic result summary | WBF-47; WGP-185 | 653 | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/galactic/galactic_audio.cpp | Tactical and galactic result summaries consume the authored event; other mode consumers are separate.; basis: reviewed |
| Music_Event_Battle_End_Summary_Screen_Win | partial | loaded local space battle (apps/viewer/src/battle_audio_events.cpp); galactic battle result summary (apps/viewer/src/galactic/galactic_audio.cpp); missing: objects and modes outside the loaded local space battle and galactic result summary | WBF-47; WGP-185 | 653 | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/galactic/galactic_audio.cpp | Tactical and galactic result summaries consume the authored event; other mode consumers are separate.; basis: reviewed |
| Music_Space_Battle_To_Ambient_Peace_Seconds | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: read by the battle audio preparer (battle_audio_prepare.cpp); application to the music director not verified.; basis: reviewed |
| SFXEvent_Command_Bar_Attack | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Command_Bar_Attack_Move | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Command_Bar_Guard | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Command_Bar_Move | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Command_Bar_Stop | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-27 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Command_Bar_Waypoint | partial | local space command bar (apps/viewer/src/battle_audio_prepare.cpp); missing: land and galactic command bars | HCB-57 | 4532 | apps/viewer/src/battle_audio_prepare.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_GUI_Negative_Feedback | partial | local space command bar (apps/viewer/src/battle_audio_events.cpp); missing: land, galactic and unsupported command/refusal routes | BA-28 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Telekinesis_SFXEvent_Damage | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of Telekinesis_SFXEvent_Damage in src/, apps/ or include/; wire the authored value into the matching audio trigger and playback route.; basis: reviewed |
| Telekinesis_SFXEvent_Loop | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of Telekinesis_SFXEvent_Loop in src/, apps/ or include/; wire the authored value into the matching audio trigger and playback route.; basis: reviewed |
| Telekinesis_SFXEvent_Slam | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of Telekinesis_SFXEvent_Slam in src/, apps/ or include/; wire the authored value into the matching audio trigger and playback route.; basis: reviewed |
| Test_Disable_Weapon_Fire_SFX | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of Test_Disable_Weapon_Fire_SFX in src/, apps/ or include/; wire the authored value into the matching audio trigger and playback route.; basis: reviewed |

## Audio, Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Music_Event_Battle_Load_Screen | applied | whole class (apps/viewer/src/galactic/galactic_audio.cpp) | WGP-26 | — | apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |

## Campaign

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Description_Text | applied | whole class (apps/viewer/src/campaign_setup_screen.cpp) | WGP-29 | — | apps/viewer/src/campaign_setup_screen.cpp | basis: reviewed |
| Is_Listed | applied | whole class (apps/viewer/src/campaign_setup_screen.cpp) | WGP-65 | — | apps/viewer/src/campaign_setup_screen.cpp | basis: reviewed |
| Sort_Order | applied | whole class (src/presentation/ui/campaign_setup.cpp) | WGP-66 | — | src/presentation/ui/campaign_setup.cpp | basis: reviewed |

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
| LandBehavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |
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
| Behavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |
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

## Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceProp, Cin_SpaceUnit, Cin_TransportUnit, MOV_Cinematic, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | applied | galactic current-type membership (src/data/xml_merge.cpp) | WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |

## Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Pulse_Delay_Seconds | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |

## Cin_GroundProp, Cin_GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |
| Obstacle_Width | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |
| Obstacle_X_Offset | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |
| Obstacle_Y_Offset | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-CINEMATIC |

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
| Alternate_Font_Name | partial | g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); g_ground_level_pips, g_planet_name, g_political_control, g_space_level_pips (apps/viewer/src/galactic/galactic_hud.cpp); missing: other command-bar components | WGUI-96; WGUI-38, WGUI-39 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | Applied to the overview planet-name template; other command-bar uses remain pending.; basis: reviewed |
| Anim_FPS | applied | whole class (src/presentation/godot/ui/ability_buttons_view.cpp) | AB-06 | — | src/presentation/godot/ui/ability_buttons_view.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Animate_Back | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Animate_Upper_Effect | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Bar_Overlay_Name | applied | g_ground_level_pips, g_planet_name, g_political_control, g_space_level_pips (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-38, WGUI-39; none recorded | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Bar_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Blank_Texture_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); tooltip_back (apps/viewer/src/galactic/galactic_hud.cpp); encyclopedia_back, encyclopedia_header_text, encyclopedia_text (apps/viewer/src/galactic/galactic_hud.cpp); b_zoom, b_zoom_adv_map, g_radar_view, map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-42, WGUI-72, WGUI-73; WGUI-06, WGUI-38; WGUI-70, WGUI-81; WGUI-56, WGUI-65, WGUI-90; WGUI-41, WGUI-53, WGUI-54; AB-07 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Blink_Fade | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Can_Animate | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Click_SFX | partial | galactic command-bar buttons (src/presentation/godot/ui/tactical_hud_internal.hpp); missing: other galactic HUD panels, tactical command bar | WGP-182 | 653 | src/presentation/godot/ui/tactical_hud_internal.hpp | Each GC command-bar button plays its own authored event; no global default (WGP-182).; basis: reviewed |
| Click_Shift | partial | b_fast_forward, b_filters0, b_filters1, b_planet_tab, b_play_pause, b_scroll_left, b_scroll_right, b_space_tab (apps/viewer/src/galactic/galactic_hud.cpp); b_reinforcement (src/presentation/godot/ui/production_view.cpp); missing: other command bar components | GCF-01, HCB-12g, TM-14; PU-70 | 650 | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement pressed state; other components remain out of scope. Galactic command-bar controls also apply the authored state layering, selection blend and icon shift.; basis: reviewed |
| Color | partial | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); garrison_respawn_counter, reinforcement_counter (src/presentation/godot/ui/hud_timer.hpp); missing: other command bar components, other command-bar components | WGEP-141, WGEP-143; WGUI-47; HCB-62 | 653 | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; src/presentation/godot/ui/hud_timer.hpp | Applied to copied hero-action menu labels; other component colour consumers remain unqualified. Timer base, icon and dial use the authored tint; other component families remain unreviewed.; basis: reviewed |
| Cross_Fade | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Disabled_Texture_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-47; AB-02 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Drag_Back | partial | focused fleet unit icons, g_planet_fleet (apps/viewer/src/galactic/galactic_fleet_input.cpp); ground pads (apps/viewer/src/galactic/ground_hud.cpp); missing: other command-bar components | WGUI-54, WGUI-55; WGUI-54, WGUI-55 | 649 | apps/viewer/src/galactic/galactic_fleet_input.cpp; apps/viewer/src/galactic/ground_hud.cpp | GHUD-S97: permits the button backing at the cursor while dragged, retaining the authored planet stack backing. It does not create resting art: that belongs to Should_Ghost and Ghost_Base_Only.; basis: reviewed |
| Drag_Select | partial | Pad_00..09 (apps/viewer/src/galactic/ground_hud.cpp); missing: other command-bar components | WGUI-41, WGUI-53, WGUI-54 | 653 | apps/viewer/src/galactic/ground_hud.cpp | Applied to ground organization components; other consumers retain their provider seams.; basis: reviewed |
| Flash_Texture_Name | partial | GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); b_reinforcement (src/presentation/godot/ui/production_view.cpp); b_play_pause_t (src/presentation/godot/ui/tactical_hud_build.cpp); missing: other command bar components | WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; PU-71; TM-08 | 653 | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/godot/ui/production_view.cpp; src/presentation/godot/ui/tactical_hud_build.cpp | Reinforcement notifications and the paused tactical time button; other flashing components remain out of scope.; basis: reviewed |
| Font_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); encyclopedia_back, encyclopedia_header_text, encyclopedia_text (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_value (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); attack_button_single, choice_button_left, choice_button_right, text_attack_choice, text_attack_single (apps/viewer/src/galactic/pending_battle_hud.cpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp); generic_flytext (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-42, WGUI-72, WGUI-73; WGUI-96; WGUI-70, WGUI-81; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-39; WGUI-41, WGUI-53, WGUI-54; WGUI-47; WGM-174, WGM-175; HCB-65; HCB-65; HCB-65; HCB-65; HCB-64 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/godot/ui/tactical_hud_build.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Ghost_Base_Only | partial | focused fleet unit icons (apps/viewer/src/galactic/galactic_fleet_input.cpp); ground pads (apps/viewer/src/galactic/ground_hud.cpp); missing: other command-bar components | WGUI-54, WGUI-55; WGUI-54, WGUI-55 | 650 | apps/viewer/src/galactic/galactic_fleet_input.cpp; apps/viewer/src/galactic/ground_hud.cpp | GHUD-S97: Active GC drags retain only the authored resting base when requested, omitting its unit portrait and text.; basis: reviewed |
| Icon_Alternate_Texture_Name | applied | g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); g_planet_fleet, q1_fleet (apps/viewer/src/galactic/galactic_fleet_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); b_zoom, b_zoom_adv_map, g_radar_view, map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp); tactical special-weapon HUD button for every faction (src/presentation/ui/hud_shell.cpp) | WGCH-151, WGCH-154; WGUI-28, WGUI-42; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-56, WGUI-65, WGUI-90; AB-06, L-6; WSW-28 | — | apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; src/presentation/ui/hud_shell.cpp | Loaded through the command-bar field table; the special-weapon button selects the authored alternate by local faction.; basis: reviewed |
| Icon_Offset | applied | GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; AB-08 | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Icon_Texture_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); b_zoom, b_zoom_adv_map, g_radar_view, map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); g_radar_blip, g_radar_view (apps/viewer/src/galactic/galactic_radar_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-56, WGUI-65, WGUI-90; WGUI-56, WGUI-90; WGUI-41, WGUI-53, WGUI-54; WGUI-47; WU-39 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Left_Justified | partial | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); missing: other command bar components, other command-bar components | WGEP-141, WGEP-143; WGUI-96; WGUI-47; HCB-64 | 650 | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; src/presentation/godot/ui/hud_icon_text.hpp | Applied to the overview planet-name template; other command-bar uses remain pending. The tactical tech label starts at its authored origin; other component families remain unreviewed. HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Loop_Anim | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Lower_Effect_Additive | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Lower_Effect_Offset | partial | st_ability_icon, st_grab_bar (apps/viewer/src/world_ui_prepare.cpp); missing: other command bar components | WU-43, WU-44 | 653 | apps/viewer/src/world_ui_prepare.cpp | World squadron active ability and additional bracket ability lower-effect quads apply the authored offsets; other component lower effects remain in the command bar coverage ticket.; basis: reviewed |
| Lower_Effect_Texture_Name | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer draws the lower effect. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Manual_Offset | applied | pending_battle_shell, story_pending_battle_shell (apps/viewer/src/galactic/pending_battle_hud.cpp); whole class (src/presentation/ui/command_bar_placement.cpp) | WGM-174, WGM-175; GHUDL-02, GHUDL-03, GHUDL-04 | — | apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/ui/command_bar_placement.cpp | Shared shell placement selects manual or automatic model translation and gates its axes; main roots retain their explicit lower-left anchor.; basis: reviewed |
| Max_Text_Width | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Mega_Texture_Name | partial | GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); missing: other command-bar components | WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | DB-NOTAG |
| Model_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); i_main_organize (apps/viewer/src/galactic/galactic_fleet_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); pending_battle_shell, story_pending_battle_shell (apps/viewer/src/galactic/pending_battle_hud.cpp); whole class (src/assets/map.cpp); pause_shell (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGUI-42, WGUI-72, WGUI-73; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-56, WGUI-65, WGUI-90; WGUI-47; WGM-174, WGM-175; none recorded; TM-09 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; apps/viewer/src/galactic/pending_battle_hud.cpp; src/assets/map.cpp; src/presentation/ui/hud_shell.cpp | Pause shell model is consumed at its final mounted idle pose; other existing consumers retain their scope.; basis: reviewed |
| Model_Offset_X | applied | pending_battle_shell, story_pending_battle_shell (apps/viewer/src/galactic/pending_battle_hud.cpp); whole class (src/presentation/ui/command_bar_placement.cpp) | WGM-174, WGM-175; GHUDL-02, GHUDL-03, GHUDL-04 | — | apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/ui/command_bar_placement.cpp | Shared shell placement selects manual or automatic model translation and gates its axes; main roots retain their explicit lower-left anchor.; basis: reviewed |
| Model_Offset_Y | applied | pending_battle_shell, story_pending_battle_shell (apps/viewer/src/galactic/pending_battle_hud.cpp); whole class (src/presentation/ui/command_bar_placement.cpp) | WGM-174, WGM-175; GHUDL-02, GHUDL-03, GHUDL-04 | — | apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/ui/command_bar_placement.cpp | Shared shell placement selects manual or automatic model translation and gates its axes; main roots retain their explicit lower-left anchor.; basis: reviewed |
| Mouse_Over_Offset | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read into the command-bar table (command_bar.cpp); no renderer applies it. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Mouse_Over_SFX | partial | galactic command-bar buttons (src/presentation/godot/ui/tactical_hud_internal.hpp); missing: other galactic HUD panels, tactical command bar | WGP-182 | 653 | src/presentation/godot/ui/tactical_hud_internal.hpp | Each GC command-bar button plays its own authored event; no global default (WGP-182).; basis: reviewed |
| Mouse_Over_Texture_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); g_planet_fleet, q1_fleet (apps/viewer/src/galactic/galactic_fleet_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); b_zoom, b_zoom_adv_map, g_radar_view, map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); attack_button_single, choice_button_left, choice_button_right, text_attack_choice, text_attack_single (apps/viewer/src/galactic/pending_battle_hud.cpp); b_special_weapon2 (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-42, WGUI-72, WGUI-73; WGUI-28, WGUI-42; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-56, WGUI-65, WGUI-90; WGUI-41, WGUI-53, WGUI-54; WGUI-47; WGM-174, WGM-175; HCB-12g; TM-14; none recorded | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/godot/ui/tactical_hud_build.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| No_Shell | partial | generic_flytext (include/eawr/presentation/ui/hud_flytext.hpp); garrison_respawn_counter, reinforcement_counter (src/presentation/ui/hud_shell.cpp); missing: other command bar components | HCB-64; HCB-62 | 650 | include/eawr/presentation/ui/hud_flytext.hpp; src/presentation/ui/hud_shell.cpp | Independent timer styles are admitted without a shell anchor; other component families remain unreviewed. HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Offset_Render | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced Code review: confirmed gap; family=hud-radar. src/data/ui/command_bar.cpp decodes the flag; no Field::offset_render consumer applies the render offset. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Overlay2_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Overlay_Texture_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Scale | partial | st_ability_icon (apps/viewer/src/world_ui_prepare.cpp); garrison_respawn_counter, reinforcement_counter (src/presentation/godot/ui/hud_timer.hpp); r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | WU-44, WU-45; HCB-62; PU-72 | 653 | apps/viewer/src/world_ui_prepare.cpp; src/presentation/godot/ui/hud_timer.hpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement close button and independent timer art; other component uses retain their existing coverage.; basis: reviewed |
| Scale_Duration | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced Code review: confirmed gap; family=hud-radar. src/data/ui/command_bar.cpp decodes the number; no Field::scale_duration consumer advances component scaling. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Selected_Alpha | partial | b_fast_forward, b_filters0, b_filters1, b_planet_tab, b_play_pause, b_scroll_left, b_scroll_right, b_space_tab (apps/viewer/src/galactic/galactic_hud.cpp); b_reinforcement (src/presentation/godot/ui/production_view.cpp); missing: other command bar components | GCF-01, HCB-12g, TM-14; PU-70 | 653 | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement selected overlay; other components remain out of scope. Galactic command-bar controls also apply the authored state layering, selection blend and icon shift.; basis: reviewed |
| Selected_Texture_Name | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); b_zoom, b_zoom_adv_map, g_radar_view, map_shell (apps/viewer/src/galactic/galactic_radar_hud.cpp); g_radar_blip, g_radar_view (apps/viewer/src/galactic/galactic_radar_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGEP-141, WGEP-143; WGUI-42, WGUI-72, WGUI-73; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-56, WGUI-65, WGUI-90; WGUI-56, WGUI-90; WGUI-41, WGUI-53, WGUI-54; WGUI-47; AB-07 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/galactic_radar_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Should_Ghost | partial | focused fleet unit icons (apps/viewer/src/galactic/galactic_fleet_input.cpp); ground pads (apps/viewer/src/galactic/ground_hud.cpp); missing: other command-bar components | WGUI-54, WGUI-55; WGUI-54, WGUI-55 | 650 | apps/viewer/src/galactic/galactic_fleet_input.cpp; apps/viewer/src/galactic/ground_hud.cpp | GHUD-S97: GC unit drags keep an authored resting remainder for a single-unit lift from a larger stack; whole stacks vacate. Ground pads retain the authored base-only rest art.; basis: reviewed |
| Should_Render_At_Drag_Pos | partial | g_planet_fleet, q1..q3 unit icons, q1..q3_fleet (apps/viewer/src/galactic/galactic_fleet_input.cpp); ground pads (apps/viewer/src/galactic/ground_hud.cpp); missing: other command-bar components | WGUI-54, WGUI-55; WGUI-54, WGUI-55 | 653 | apps/viewer/src/galactic/galactic_fleet_input.cpp; apps/viewer/src/galactic/ground_hud.cpp | GHUD-S97: GC fleet and ground drags move their art to the pointer and lift source drawing unless the component sets it False; other drag sources retain their seams.; basis: reviewed |
| Swap_Texture | partial | b_fast_forward, b_filters0, b_filters1, b_planet_tab, b_play_pause, b_scroll_left, b_scroll_right, b_space_tab (apps/viewer/src/galactic/galactic_hud.cpp); r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | GCF-01, HCB-12g, TM-14; PU-72 | 653 | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/godot/ui/production_view.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage. Galactic command-bar controls also apply the authored state layering, selection blend and icon shift.; basis: reviewed |
| Text_Color | applied | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); encyclopedia_back, encyclopedia_header_text, encyclopedia_text (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_value (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); bm_close, bm_frame, bm_text_steal, bm_title, bm_title01, h_close, h_title, hero-action menu option components, target_type_back (apps/viewer/src/galactic/hero_action_hud.cpp); attack_button_single, choice_button_left, choice_button_right, text_attack_choice, text_attack_single (apps/viewer/src/galactic/pending_battle_hud.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-42, WGUI-72, WGUI-73; WGUI-70, WGUI-81; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-39; WGUI-41, WGUI-53, WGUI-54; WGUI-47; WGM-174, WGM-175; HCB-64; HCB-65 | — | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/hero_action_hud.cpp; apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/godot/ui/hud_icon_text.hpp; src/presentation/godot/ui/tactical_hud_build.cpp | matched by the field's name; the loader is table-driven HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Text_Color2 | todo | application not recorded | — | 653 | — | debug build: the default colour of a component's second text; the galactic build card price overwrites it on every refresh with the price-state colour (GHUD-S57), so it is never drawn there; other second-text consumers are not traced Code review: confirmed gap; family=hud-radar. Command-bar schema decodes the second-text colour, but no Field::text_color2 reader renders it; galactic build-price text uses its current price-state colour. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Text_Emboss | applied | attack_button_single, choice_button_left, choice_button_right, text_attack_choice, text_attack_single (apps/viewer/src/galactic/pending_battle_hud.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); r_close (src/presentation/godot/ui/production_view.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGM-174, WGM-175; HCB-64; PU-72; HCB-65 | — | apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/godot/ui/hud_icon_text.hpp; src/presentation/godot/ui/production_view.cpp; src/presentation/ui/hud_shell.cpp | Applied to the reinforcement close button with its authored art and text style; other component uses retain their existing coverage. HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Text_Offset | partial | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); q1_0000..q3_0406, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); galactic planet block text components (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); st_control_group, st_grab_bar (apps/viewer/src/world_ui_prepare.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); r_close (src/presentation/godot/ui/production_view.cpp); missing: other command-bar components | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-72, WGUI-73; WGUI-33, WGUI-34; WGUI-96; WU-44, WU-45; HCB-64; PU-72 | 653 | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/world_ui_prepare.cpp; src/presentation/godot/ui/hud_icon_text.hpp; src/presentation/godot/ui/production_view.cpp | Also applies the overview planet-name template text offset; other component uses retain existing coverage.; basis: reviewed |
| Text_Offset2 | applied | whole class (src/presentation/ui/hud_shell.cpp) | L-8 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Text_Outline | applied | i_main_organize, q1_0000..q3_0406, q1_fleet..q4_fleet, text_fleet1..3 (apps/viewer/src/galactic/galactic_fleet_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S09 ground organization components (apps/viewer/src/galactic/ground_hud.cpp); attack_button_single, choice_button_left, choice_button_right, text_attack_choice, text_attack_single (apps/viewer/src/galactic/pending_battle_hud.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); whole class (src/presentation/ui/hud_shell.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-42, WGUI-72, WGUI-73; WGUI-96; WGUI-41, WGUI-53, WGUI-54; WGM-174, WGM-175; HCB-64; HCB-65; none recorded | — | apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/pending_battle_hud.cpp; src/presentation/godot/ui/hud_icon_text.hpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tooltip_Text | applied | GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-35, WGUI-79; none recorded | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tutorial_Scene | todo | application not recorded | — | 653 | — | a data loader reads it; where the value goes is not traced Code review: confirmed gap; family=hud-radar. Command-bar schema decodes the flag; no Field::tutorial_scene consumer creates or controls the tutorial scene. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Upper_Effect_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, WU-20 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Hack_Ability/@Name | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; land hack not built; basis: reviewed |
| Abilities/Hack_Ability/Lightning_Effect_Name | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Lightning_Max_Targets | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Lightning_Source_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Lightning_Target_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Lightning_Targets_Radius | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/SFXEvent_Activate | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Hack_Ability/Target_Particle_Effect | todo | application not recorded | — | 3244 | — | no code reads Hack_Ability tags on origin/main; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/@Name | todo | application not recorded | — | 3239 | — | no code reads Permanent_Weapon_Swap_Ability on origin/main; basis: reviewed |
| Abilities/Proximity_Mines_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activate_SFX | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Repair_Ability/@Name | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; land repair not built; basis: reviewed |
| Abilities/Repair_Ability/Lightning_Effect_Name | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Lightning_Max_Targets | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Lightning_Source_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Lightning_Target_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Lightning_Targets_Radius | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/SFXEvent_Activate | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| Abilities/Repair_Ability/Target_Particle_Effect | todo | application not recorded | — | 3244 | — | no code reads Repair_Ability tags on origin/main; basis: reviewed |
| LOD_Bias | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Name_Adjust | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. Only galactic scene_content.cpp decodes the offset; galactic_layers.cpp leaves placement unresolved, and Container tactical labels never apply it.; basis: reviewed |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Show_Name | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Code review: confirmed gap; family=hud-radar. Only the galactic planet label route consumes this flag; Container tactical names do not use it.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | todo | application not recorded | — | 3776 | — | Parser branch only for CORRUPT_SYSTEMS (galactic ability type); basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## Container, Faction, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, MultiplayerStructureMarker, Projectile, ScriptMarker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Text_ID | applied | observer-local space production palette (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-91; none recorded | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | basis: auto |

## Container, Faction, GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Icon_Name | applied | observer-local space production palette (apps/viewer/src/galactic/galactic_hud.cpp); whole class (apps/viewer/src/world_ui_prepare.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-91; L-7, WU-20; P-4 | — | apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/world_ui_prepare.cpp; src/presentation/ui/hud_shell.cpp | GC faction affiliation icon is decoded; command-bar drawing remains pending &#35;1155.; basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Visible_On_Radar | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Projectile, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Text | partial | admitted galactic production types (apps/viewer/src/galactic/scene_content.cpp); whole class (src/presentation/ui/hud_shell.cpp); missing: objects outside admitted galactic production | WGUI-18, WGUI-81; P-1, P-2 | 4532 | apps/viewer/src/galactic/scene_content.cpp; src/presentation/ui/hud_shell.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Select_Box_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-02 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## Container, GenericHeroUnit, HeroUnit, Marker, Projectile, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Unit_Class | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); whole class (src/presentation/ui/hud_shell.cpp); missing: UI consumers outside galactic reference pages | GREF-06; P-3 | 1952 | apps/viewer/src/galactic/reference_catalog.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-20, BA-23, BA-24 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## Container, GenericHeroUnit, Marker, MiscObject, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | partial | land company members (include/eawr/skirmish/land_content.hpp); ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp); whole class (src/units/unit_combat.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-06; WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; L-32; L-32; AV-05, R-ROT-01 | 1450 | include/eawr/skirmish/land_content.hpp; src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp; src/units/unit_combat.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; basis: reviewed |

## Container, GenericHeroUnit, SecondaryStructure, SpaceUnit, SpecialStructure, Squadron, StarBase, TechBuilding, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Row | partial | observer-local ground production palette (apps/viewer/src/galactic/galactic_hud.cpp); observer-local space production palette (apps/viewer/src/galactic/planet_hud.cpp); missing: tactical/other GUI consumers and unavailable production providers | WGUI-34, WGUI-91; WGUI-91 | 653 | apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/galactic/planet_hud.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small (+67 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small (+67 more).; basis: reviewed |

## Container, HeroUnit, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Deactivated | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-20, BA-23, BA-24 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## Container, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Named_Hero | partial | loaded local space battle (apps/viewer/src/live_session_frame.cpp); visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); craft, ship, station (src/presentation/ui/world_ui.cpp); space purchase catalog (src/skirmish/economy.cpp); ship (src/skirmish/economy.cpp); space purchase catalog (src/skirmish/inputs.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects and modes outside the loaded local space battle, objects outside the simulated unit closure | WBF-45; WU-47, WU-48; WAR-24; WSU-50; WSS-29; WHE-01, WHE-49; WSS-29; WSU-50 | 653 | apps/viewer/src/live_session_frame.cpp; apps/viewer/src/world_ui_groups.cpp; src/campaign/auto_resolve_strength.cpp; src/presentation/ui/world_ui.cpp; src/skirmish/economy.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces. Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate. Resolved inherited space hero policy filters the shared human/AI purchase catalog independently of presentation and results scoring. World identities cover visible standalone space heroes; carried heads remain separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## Container, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Model_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |

## Container, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Distance | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Velocity | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |

## Container, SpaceBuildable, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+8 more). Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |

## Container, SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | partial | ship (apps/viewer/src/battle_audio_events.cpp); craft, ship, squadron, station (apps/viewer/src/battle_audio_events.cpp); missing: release-gated BARRAGE, unimplemented space abilities and land/galactic objects authored with hero/container classes | BA-54; BA-53 | 4532 | apps/viewer/src/battle_audio_events.cpp | M2 targeted confirmation and spawned harmonic/weaken effects attach to the issuer; unsupported actions stay silent.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_buzz_droids_ability stores buzz.name, but buzz_profile copies only the parameters and dispatches by ability kind; the authored name never selects a handler.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activate_SFX | partial | squadron (apps/viewer/src/battle_audio_prepare.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-75 | 4532 | apps/viewer/src/battle_audio_prepare.cpp | The droid object's owner hears it at the arming frame; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Idle_Chase_Range | todo | application not recorded | — | 653 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: loaded (unit_tables_profiles.cpp) but the space step never reads it for these classes.; basis: reviewed |

## Container, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_FOW_Reveal_Range | applied | space hunt fallback destinations (src/sim/tactical/session_cmd_lanes.cpp); whole class (src/skirmish/ai.cpp) | WAB-54; WAB-54 | — | src/sim/tactical/session_cmd_lanes.cpp; src/skirmish/ai.cpp | basis: auto |

## Container, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Ion_Cannon_Shot_Attack_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_nested_abilities records the attribute in inactive_abilities, but the nested-handler dispatch has no Ion_Cannon_Shot_Attack_Ability implementation consuming this name.; basis: reviewed |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Good_Against | applied | whole class (src/presentation/ui/hud_shell.cpp) | P-4 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Encyclopedia_Vulnerable_To | applied | whole class (src/presentation/ui/hud_shell.cpp) | P-4 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | CARD-15, CARD-16 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Icon_Name | applied | whole class (src/presentation/ui/hud_shell.cpp) | CARD-15, CARD-16 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | CARD-15, CARD-16 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |

## Container, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Dense_FOW_Reveal_Range_Multiplier | applied | craft, ship, squadron, station (src/sim/tactical/fog_cells.cpp) | V-22, WHZ-09 | — | src/sim/tactical/fog_cells.cpp | basis: reviewed |
| Radar_Icon_Scale_Space | applied | whole class (src/presentation/ui/minimap.cpp) | MM-17 | — | src/presentation/ui/minimap.cpp | conditional space radar scaling; types without Radar_Draw_To_Scale keep Radar_Icon_Size |

## Container, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Lucky_Shot_Attack_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_nested_abilities records the attribute in inactive_abilities, but the nested-handler dispatch has no Lucky_Shot_Attack_Ability implementation consuming this name.; basis: reviewed |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fadeout_Time | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Slot_Scale | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Slot_Scale in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |

## Decal, Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Render_Mode | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. Only galactic route_lines.cpp reads this spelling for route styling; Decal and Material XML have no render-mode consumer.; basis: reviewed |

## Decal, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. The Scale reader belongs to CommandBarComponent and camera bindings; Decal and SpaceProp XML Scale has no transform consumer.; basis: reviewed |

## Decals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Decal | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Decal/@name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No Decal XML definition-name catalog or presentation lookup exists; the generic model/material asset routes do not consume this named XML entry.; basis: reviewed |

## DynamicTrack

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| fade_begin_distance | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| fade_distance_per_second | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| fade_end_distance | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| min_geometry_lod | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| opacity | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| Render_Mode | todo | application not recorded | — | 3776 | — | DynamicTrack is galactic route styling; no code on the land path reads this tag; basis: reviewed |
| segment_length | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |
| Texture_Name | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |
| width | todo | application not recorded | — | 3776 | — | DynamicTrack fields; no code reads them on main; basis: reviewed |

## DynamicTrack, Weather_Scenario

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Alternate_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: read only by the HUD shell ability-button path (hud_shell.cpp); not applied to the space faction icons. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Alternate_Icon_Prefix | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Big_Fleet_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Bombardment_Lighting_Color_List | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Bombardment_Lighting_Intensity_List | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Bombardment_Lighting_Seconds_List | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Bombardment_Particle | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Bombardment_Shadow_Blob_Material_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Can_Win_By_Destroying_Super_Weapon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Carrier_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Color | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp); whole class (apps/viewer/src/land_look.cpp) | WGUI-38, WGUI-96; A-04, A-05, A-11, P-03, S-12 | — | apps/viewer/src/galactic/galactic_hud.cpp; apps/viewer/src/land_look.cpp | GC overview HUD uses observer-known primary faction colour or Neutral; WGP-05 geometry-colour path consumes only secondary colour on a separate object name, not planet materials.; basis: auto |
| Corvette_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Default_Transmission_Message | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Defeat_Text | applied | campaign score screen (src/platform/galactic/campaign_end_inputs.cpp) | WGP-37, WGP-38 | — | src/platform/galactic/campaign_end_inputs.cpp | The campaign score list shows the local faction's authored defeat text (GHUD-S48).; basis: reviewed |
| Display_Font_Color | partial | campaign loading owner/title (apps/viewer/src/loading_screen.cpp); missing: other tactical font consumers | WGP-31 | 653 | apps/viewer/src/loading_screen.cpp | Campaign loading applies the actual owner font colour supplied by the scene catalog in the space/land ordinary/mission resources (WGP-31/75); other tactical font consumers remain pending.; basis: reviewed |
| Finale_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Finale_Movie_2 | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Fleet_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located (Faction+0x1d8); consumer not traced.; basis: reviewed |
| Frigate_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Generic_Win_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Helper_Icon_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Is_Debug_Switchable_To | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Land_Lose_Image | partial | campaign score screen (src/platform/galactic/campaign_end_inputs.cpp); missing: standalone tactical result pictures | WGP-37, WGP-38 | 653 | src/platform/galactic/campaign_end_inputs.cpp | The campaign score screen draws it for the presentation-only land variant; standalone tactical pictures remain pending.; basis: reviewed |
| Land_Win_Image | partial | campaign score screen (src/platform/galactic/campaign_end_inputs.cpp); missing: standalone tactical result pictures | WGP-37, WGP-38 | 653 | src/platform/galactic/campaign_end_inputs.cpp | The campaign score screen draws it for the presentation-only land variant; standalone tactical pictures remain pending.; basis: reviewed |
| Multiplayer_Beacon_Type | partial | space faction beacons (src/skirmish/inputs.cpp); space faction beacons (src/skirmish/start.cpp); missing: land and galactic beacons | HCB-12f; HCB-12f | 4532 | src/skirmish/inputs.cpp; src/skirmish/start.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Multiplayer_Map_Preview_Icon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Music_Event_List_Ambient | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-41 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Music_Event_List_Battle | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-41 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Music_Event_Space_Ambient_Super_Weapon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Music_Event_Space_Battle_Super_Weapon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Music_Event_Strategic_Lose | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-25 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Music_Event_Strategic_Lose_Vs_Faction | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-25 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Music_Event_Strategic_Win | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-25 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Music_Event_Strategic_Win_Vs_Faction | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-25 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Music_Event_Tactical_Lose | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Lose_Vs_Faction | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Space_Battle_Pending | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-26 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Music_Event_Tactical_Win | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Music_Event_Tactical_Win_Vs_Faction | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-54 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Post_Credits_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Reinforcements_Cancelled_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-16 | 4532 | apps/viewer/src/battle_audio_events.cpp | Failed space placement drops and explicit right-click cancellation request the authored faction event; ordinary overlap admission may refuse playback.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Reinforcements_Enroute_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-16 | 4532 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Reinforcements_Ready_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: pending-garrison/transport reinforcement notification, not the ordinary space pool (BA-84, WR walk U-9); the space path has no caller |
| Reinforcements_Requesting_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: pending-garrison/transport reinforcement notification, not the ordinary space pool (BA-84, WR walk U-9); the space path has no caller |
| Reinforcements_Selection_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-07 | 4532 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Reinforcements_Shadow_Blob_Material_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Reinforcements_Shadow_Blob_Material_Name in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Selection_Blob_RGBA | applied | local-player faction (apps/viewer/src/world_ui_prepare.cpp) | WU-01 | — | apps/viewer/src/world_ui_prepare.cpp | The effective faction RGBA tints the selected-unit blob material.; basis: reviewed |
| SFX_Event_Tactical_Space_Battle_Pending | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-26 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Arrive_From_Hyperspace | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-62 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Base_Shield_Absorb_Damage | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Base_Shield_Absorb_Damage in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bombard_Ally_Available | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Ally_Available in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bombard_Available | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Available in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bombard_Cancelled | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Cancelled in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bombard_Enemy_Available | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Enemy_Available in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bunker_Vacated | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bunker_Vacated in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Enemy_Spotted | partial | standalone space skirmish (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-71 | 653 | apps/viewer/src/battle_audio_events.cpp | Local faction cue; consumed eligibility is not restored on busy/missing/refused playback.; basis: reviewed |
| SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_Off | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | none recorded | — | apps/viewer/src/battle_audio_events.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| SFXEvent_GUI_Enemy_Toggle_Non_Hero_Ability_On | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-50 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_GUI_Toggle_Non_Hero_Ability_Off | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | none recorded | — | apps/viewer/src/battle_audio_events.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| SFXEvent_GUI_Toggle_Non_Hero_Ability_On | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-50, WAB-11 | — | apps/viewer/src/battle_audio_events.cpp | basis: auto |
| SFXEvent_Hack_Success | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Hack_Success in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Advisor_Hint | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_HUD_Advisor_Message | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| SFXEvent_HUD_Advisor_Urgent | partial | galactic approach advisor (apps/viewer/src/galactic/galactic_audio.cpp); missing: other advisor producers and full shared popup presentation | WGUI-114, WGUI-66 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The minimal galactic approach advisor consumes this field; shared advisor graphics, history and other producers remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_HUD_Base_Shield_Offline | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Base_Shield_Offline in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Base_Shield_Online | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Base_Shield_Online in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Base_Shield_Penetrated | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Base_Shield_Penetrated in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Build_Pad_Captured | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-37 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_HUD_Build_Pad_Lost | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-37 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_HUD_Enemy_Base_Shield_Offline | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Enemy_Base_Shield_Offline in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Enemy_Base_Shield_Online | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Enemy_Base_Shield_Online in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Enemy_Base_Shield_Penetrated | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Enemy_Base_Shield_Penetrated in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Enemy_Special_Weapon_Charging | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Enemy_Special_Weapon_Charging in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Enemy_Special_Weapon_Firing | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WSW-33 | — | apps/viewer/src/battle_audio_events.cpp | — |
| SFXEvent_HUD_Enemy_Special_Weapon_Ready | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WSW-34 | — | apps/viewer/src/battle_audio_events.cpp | — |
| SFXEvent_HUD_Gravity_Control_Generator_Off | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Gravity_Control_Generator_Off in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Gravity_Control_Generator_On | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Gravity_Control_Generator_On in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Lost_Space_Battle | applied | local faction (apps/viewer/src/battle_audio_prepare.cpp) | WBF-38 | — | apps/viewer/src/battle_audio_prepare.cpp | Local faction space outcome HUD cue, once on first presented outcome; land and story/TSW variants have separate rows.; basis: reviewed |
| SFXEvent_HUD_Lost_Space_Battle_Enemy_TSW_Present | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Lost_Space_Battle_Enemy_TSW_Present in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_05_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_15_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_30_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Ally_Owned_60_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Contested | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_05_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_15_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_30_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Enemy_Owned_60_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Owned_05_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Owned_15_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Owned_30_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Reinforcement_Point_Owned_60_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: reinforcement-point ownership countdown announcements belong to the ground-combat reinforcement-point system (LC-44, Use_Reinforcement_Points); the space skirmish has no capturable reinforcement points |
| SFXEvent_HUD_Repairing | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Repairing in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Special_Weapon_Charging | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Special_Weapon_Charging in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Special_Weapon_Firing | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WSW-33 | — | apps/viewer/src/battle_audio_events.cpp | — |
| SFXEvent_HUD_Special_Weapon_Ready | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WSW-34 | — | apps/viewer/src/battle_audio_events.cpp | — |
| SFXEvent_HUD_Tactical_Victory_Near | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Tactical_Victory_Near in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_HUD_Won_Space_Battle | applied | local faction (apps/viewer/src/battle_audio_prepare.cpp) | WBF-38 | — | apps/viewer/src/battle_audio_prepare.cpp | Local faction space outcome HUD cue, once on first presented outcome; land and story/TSW variants have separate rows.; basis: reviewed |
| SFXEvent_HUD_Won_Space_Battle_Enemy_TSW_Present | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_HUD_Won_Space_Battle_Enemy_TSW_Present in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Mission_Added | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-27 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_New_Construction_Options_Available | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); qualified galactic technology notifications (src/platform/galactic/live_game_session.cpp); missing: other slicer/black-market notification callers (ability walk) | WGP-27; SND-68, WGEP-130 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp; src/platform/galactic/live_game_session.cpp | The shared galactic tech setter qualifies newly unlocked registered types and the bounded platform feed retains one local request per batch; the viewer resolves the authored faction SFX. Other ability callers retain their owning walk scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Player_Taunt | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Player_Taunt in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Space_Base_Under_Attack_Announcement | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | SND-40 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Gain_Enemy_Control | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Tactical_Gain_Enemy_Control in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Tactical_Gain_Friendly_Control | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Tactical_Gain_Friendly_Control in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Tactical_Lose_Enemy_Control | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Tactical_Lose_Enemy_Control in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Tactical_Lose_Friendly_Control | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Tactical_Lose_Friendly_Control in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Tactical_Object_Building_Complete | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Building_Loop | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Building_Started | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Tactical_Object_Sold | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WBP-31 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Unit_Type_Spotted | partial | standalone space skirmish (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-70 | 653 | apps/viewer/src/battle_audio_events.cpp | Local faction cue; consumed eligibility is not restored on busy/missing/refused playback.; basis: reviewed |
| SFXEvent_Weather_Begin | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Weather_Begin in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Weather_End | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Weather_End in src/, apps/ or include/; wire the authored value into the matching audio trigger and playback route.; basis: reviewed |
| Ship_Icon_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Ship_Icon_Name in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation. Reader walk pending (10-25): FoC debug-build tag-table row located (Faction+0x1f8); consumer not traced.; basis: reviewed |
| Space_Advisor_Hints | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Space_Lose_Image | partial | campaign space summary (apps/viewer/src/galactic/galactic_battle_dialog.cpp); campaign score screen (src/platform/galactic/campaign_end_inputs.cpp); missing: standalone tactical result pictures | WGP-34; WGP-37, WGP-38 | 653 | apps/viewer/src/galactic/galactic_battle_dialog.cpp; src/platform/galactic/campaign_end_inputs.cpp | Campaign space summaries and the campaign score screen use the local faction picture; standalone tactical pictures remain pending.; basis: reviewed |
| Space_Mode_Selection_Blob_Material_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-01 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |
| Space_Retreat_Begin_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-03; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Retreat_Cancel_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-03; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Retreat_Countdown_Color_RGBA | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (apps/viewer/src/map_mode.cpp) | RP-08; RP-08 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/map_mode.cpp | The retreating faction's line text and colour.; basis: reviewed |
| Space_Retreat_Countdown_Text_ID | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (apps/viewer/src/map_mode.cpp) | RP-07; RP-07 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/map_mode.cpp | The retreating faction's line text and colour.; basis: reviewed |
| Space_Retreat_Enemy_Begin_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-03; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Retreat_Not_Allowed_Reason_1_SFXEvent | partial | cue name read from the faction (apps/viewer/src/battle_audio_prepare.cpp); missing: gravity-well refusal cue (RP-05 reason 1): the retreat refusals have no gravity-well source yet | RP-01 | 4532 | apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers, but never played: no refusal maps to reason 1.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Space_Retreat_Not_Allowed_Reason_2_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-05; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Retreat_Not_Allowed_Reason_3_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-05; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Retreat_Not_Allowed_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | RP-02, RP-05; RP-01 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | Read per faction through the catalog layers; the local type cue takes precedence (WR-44).; basis: reviewed |
| Space_Surrender_SFXEvent | foc-ignores | application not recorded | — | — | — | debug build: the tag-table row has no accessor or reader (RP-10) |
| Space_Win_Image | partial | campaign space summary (apps/viewer/src/galactic/galactic_battle_dialog.cpp); campaign score screen (src/platform/galactic/campaign_end_inputs.cpp); missing: standalone tactical result pictures | WGP-34; WGP-37, WGP-38 | 653 | apps/viewer/src/galactic/galactic_battle_dialog.cpp; src/platform/galactic/campaign_end_inputs.cpp | Campaign space summaries and the campaign score screen use the local faction picture; standalone tactical pictures remain pending.; basis: reviewed |
| SpeechEvent_Super_Weapon_Enemy_Moved_Into_Range | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Into_Range | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_05_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_15_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_30_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Enemy_Moving_Range_60_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moved_Into_Range | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moving_Into_Range | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moving_Range_05_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moving_Range_15_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moving_Range_30_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Super_Weapon_Moving_Range_60_Seconds | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| SpeechEvent_Tactical_Intro_Space_Attacker | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| SpeechEvent_Tactical_Intro_Space_Attacker_Conditional_And | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| SpeechEvent_Tactical_Intro_Space_Attacker_Conditional_Or | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| SpeechEvent_Tactical_Intro_Space_Defender | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| SpeechEvent_Tactical_Intro_Space_Defender_Conditional_And | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| SpeechEvent_Tactical_Intro_Space_Defender_Conditional_Or | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp | Campaign parent, cinematic/queue gates and conditional object persistence are outside standalone space skirmish. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; debug build SND-E35/E36: parented campaign briefing only; no standalone skirmish intro (SND-41/49, BA-72); basis: reviewed |
| Star_Base_Icon_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Star_Base_Icon_Name in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Strategic_Map_Music_Event | applied | whole class (apps/viewer/src/galactic/galactic_audio.cpp) | WGP-23 | — | apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |
| Superweapon_Win_Movie | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Tactical_Intro_Command_Bar_Movie_Name | partial | prepared campaign battle briefings (src/campaign/space_entry_stock.cpp); missing: Runtime briefing and cinematic presentation gates | SND-41, SND-49 | 2007 | src/campaign/space_entry_stock.cpp |  Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; SCOPE-MENU; basis: reviewed |
| Text_Nickname_ID | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Text_Nickname_ID in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Vehicle_Icon_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Vehicle_Icon_Name in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation. Reader walk pending (10-25): FoC debug-build tag-table row located; consumer not traced.; basis: reviewed |
| Victory_Text | applied | campaign score screen (src/platform/galactic/campaign_end_inputs.cpp) | WGP-37, WGP-38 | — | src/platform/galactic/campaign_end_inputs.cpp | The campaign score list shows the local faction's authored victory text (GHUD-S48).; basis: reviewed |

## Faction, GenericHeroUnit, SpaceStructure, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| No_Colorization_Color | partial | campaign battle unit models (apps/viewer/src/live_session_economy.cpp); capture-point radar (apps/viewer/src/map_mode_hud.cpp); missing: skirmish neutral-owner models | WGT-120; WNO-43 | 653 | apps/viewer/src/live_session_economy.cpp; apps/viewer/src/map_mode_hud.cpp | Nonneutral unassigned community capture radar falls back to nonzero type then faction no-colourisation data; campaign battle models use nonzero type then faction no-colourisation colour (WGT-120); skirmish neutral owners remain pending.; basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Activated_Hack_Super_Weapon_Ability_Names | partial | live galactic hack ability routing (src/campaign/live_setup.cpp); missing: faction hack-success sound and attached particle consumers | WGCH-139, WGCH-180, WGCH-57 | 653 | src/campaign/live_setup.cpp | Routes named activated hack abilities into the live prepared host; presentation consumers remain seams.; basis: reviewed |
| Activated_Neutralize_Hero_Ability_Names | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-20, WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | basis: reviewed |
| Activated_System_Spy_Ability_Names | applied | ground-activated galactic spy preview (apps/viewer/src/galactic/ability_drop.hpp) | WGCH-152 | — | apps/viewer/src/galactic/ability_drop.hpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Advisor_Hint_Duration | partial | galactic approach advisor (apps/viewer/src/galactic/galactic_hud.cpp); missing: other advisor producers and full shared popup presentation | WGUI-68 | 4532 | apps/viewer/src/galactic/galactic_hud.cpp | The minimal galactic approach advisor consumes this field; shared advisor graphics, history and other producers remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Advisor_Hint_Interval | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Animate_During_Galactic_Mode_Pause | applied | whole class (apps/viewer/src/galactic/galactic_layers.cpp) | WGP-03 | — | apps/viewer/src/galactic/galactic_layers.cpp | basis: reviewed |
| Battle_Load_Planet_Ambient | applied | Independent loading preview (apps/viewer/src/loading_planet.cpp) | WGP-32, WGP-51 | — | apps/viewer/src/loading_planet.cpp | basis: reviewed |
| Battle_Load_Planet_Direction | applied | Independent loading preview (apps/viewer/src/loading_planet.cpp) | WGP-32, WGP-51 | — | apps/viewer/src/loading_planet.cpp | basis: reviewed |
| Battle_Load_Planet_Viewport | applied | Independent loading preview (apps/viewer/src/loading_planet.cpp) | WGP-32, WGP-51 | — | apps/viewer/src/loading_planet.cpp | basis: reviewed |
| Battle_Pending_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | TM-09 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| Battle_Pending_Message_Font | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Battle_Pending_Message_Pos_X | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Battle_Pending_Message_Pos_Y | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| BeaconPlaceDelay | partial | space tactical HUD (apps/viewer/src/battle_input.cpp); space tactical HUD (src/presentation/ui/hud_shell.cpp); missing: land and galactic HUD | HCB-12a, HCB-12f; HCB-12a, HCB-12f | 4532 | apps/viewer/src/battle_input.cpp; src/presentation/ui/hud_shell.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Bink_Player_Caption_Font_Name | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Bink_Player_Caption_Font_Size | presentation-later | application not recorded | — | — | — | SCOPE-CINEMATIC |
| Camera_FX_Manager_Letterbox_Height | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp); whole class (src/presentation/ui/hud_shell.cpp) | HCB-11c; HCB-11c | — | src/presentation/godot/ui/tactical_hud_build.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Camera_Stop_Left | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Camera_Stop_Right | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Camera_Z_Position | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| CB_Flash_Count | todo | application not recorded | — | 653 | — | debug build: pool addition passes this value but the continuous flag bypasses its application (PU-71); other command bar flashes remain pending sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| CB_Flash_Duration | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| CB_Movie_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| CB_Movie_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Command_Bar_Default_Font_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Command_Bar_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the default font size of UI text components |
| Corruption_Line_Grow_Seconds | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/corruption_particles.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-49; WGP-20, WGP-21, WGP-49 | — | apps/viewer/src/galactic/corruption_particles.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Line_Radius | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/corruption_particles.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-49; WGP-20, WGP-21, WGP-49 | — | apps/viewer/src/galactic/corruption_particles.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Line_Start_End_Offset | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/corruption_particles.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-49; WGP-20, WGP-21, WGP-49 | — | apps/viewer/src/galactic/corruption_particles.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Particle_Line_Name | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/corruption_particles.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-49; WGP-20, WGP-21, WGP-49 | — | apps/viewer/src/galactic/corruption_particles.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Particle_Name | applied | whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-19 | — | apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Path_Color | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-48; WGP-20, WGP-21, WGP-48 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Path_Offset | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-48; WGP-20, WGP-21, WGP-48 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Corruption_Path_Width | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-20, WGP-21, WGP-48; WGP-20, WGP-21, WGP-48 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Countdowns_Font_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Credits_Bottom_Color | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Font | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Font_Size | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Header_Bottom_Color | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Header_Top_Color | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_1_Height | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_1_Name | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_1_Width | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_1_Y_Offset | applied | whole class (src/presentation/ui/credits.cpp) | CRD-07 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Logo_2_Height | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_2_Name | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_2_Width | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_2_Y_Offset | applied | whole class (src/presentation/ui/credits.cpp) | CRD-07 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Logo_3_Height | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_3_Name | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_3_Width | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-07 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| Credits_Logo_3_Y_Offset | applied | whole class (src/presentation/ui/credits.cpp) | CRD-07 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Margin | applied | whole class (src/presentation/ui/credits.cpp) | CRD-05 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Scroll_Rate | applied | whole class (src/presentation/ui/credits.cpp) | CRD-04 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Spacing | applied | whole class (src/presentation/ui/credits.cpp) | CRD-04 | — | src/presentation/ui/credits.cpp | basis: reviewed |
| Credits_Top_Color | applied | whole class (src/presentation/godot/ui/credits_view.cpp) | CRD-02 | — | src/presentation/godot/ui/credits_view.cpp | basis: reviewed |
| CrouchIdleWalkBlendTime | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: no reader under src/apps/include.; basis: reviewed |
| CrouchMoveBlendTime | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: no reader under src/apps/include.; basis: reviewed |
| Debug_Hot_Key_Load_Map | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Debug_Hot_Key_Load_Map_Script | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Demo_Attract_Map_Cycle_Delay_Seconds | applied | whole class (src/skirmish/attract.cpp) | MAT-04 | — | src/skirmish/attract.cpp | basis: reviewed |
| Demo_Attract_Maps | applied | whole class (src/skirmish/attract.cpp) | MAT-03 | — | src/skirmish/attract.cpp | basis: reviewed |
| Demo_Attract_Start_Timeout_Seconds | applied | whole class (src/skirmish/attract.cpp) | MAT-05 | — | src/skirmish/attract.cpp | basis: reviewed |
| Display_Bink_Movie_Frames | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Droid_Date_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Droid_Encyclopedia_Offset | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Droid_Seperator_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Droid_Text_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Earthquake_Shake_Magnitude | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Earthquake_Shake_Speed | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Earthquake_Transition_Time | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Encyclopedia_Class_Y_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Encyclopedia_Cost_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Encyclopedia_Delay | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-70, WGUI-80; CARD-18, WGUI-70, WGUI-80 | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Encyclopedia_Fade_Rate | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-70, WGUI-80; CARD-18, WGUI-70, WGUI-80 | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Encyclopedia_Icon_X_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Encyclopedia_Icon_Y_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Encyclopedia_Min_Display_Time | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-70, WGUI-80; CARD-18, WGUI-70, WGUI-80 | — | apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |
| Encyclopedia_Name_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Encyclopedia_Population_Offset | applied | whole class (src/presentation/godot/ui/tactical_hud_build.cpp) | CARD-19 | — | src/presentation/godot/ui/tactical_hud_build.cpp | basis: reviewed |
| Enemy_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Energy_Beam_Color | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Energy_Beam_Texture | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Energy_Beam_Width | partial | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp); missing: Energy-beam highlight rendering remains deferred. | TBF-01 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | TBF-01 corrects the shared simple-line geometry and global colour; energy highlights and per-ability overrides remain separate.; basis: reviewed |
| Event_Message_Default_Font_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Evil_Side_Leader_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Evil_Side_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: read for galactic/story setup only (live_setup.cpp, story_plots.cpp); no skirmish consumer.; basis: reviewed |
| First_Strike_Particle | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;weapons-shields&#93;: no reader under src/apps/include.; basis: reviewed |
| Fleet_Hyperspace_Band_Texture_Name | applied | whole class (apps/viewer/src/galactic/galactic_routes.cpp); additive galactic corruption paths (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-18; WGP-48 | — | apps/viewer/src/galactic/galactic_routes.cpp; apps/viewer/src/galactic/galactic_scene.cpp | Authored band texture is applied to each route hop during native fleet dragging and to the bounded empty-world pointer guide; preview and release share the movement planner. Galactic corruption paths also use the band texture with authored width and RGB modulation.; basis: reviewed |
| Fleet_Movement_Line_Texture_Name | applied | whole class (apps/viewer/src/galactic/galactic_routes.cpp) | WGP-17 | — | apps/viewer/src/galactic/galactic_routes.cpp | WGP-17 texture and geometry are applied; locally visible is a presentation adapter pending the live observer visibility input (legacy &#35;1114).; basis: reviewed |
| Galactic_Map_Score_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Galactic_Right_Button_Scroll_Speed_Factor | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-59, WGUI-78 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Galactic_Scroll_Plane | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-41, WGP-42, WGUI-57 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Galactic_Zoom_In_Light_Angle | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-62 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Galactic_Zoom_In_Station_Rotation | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | GFC-47, WGP-186 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Galactic_Zoom_Light_Level | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-62 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Galactic_Zoom_Out_Light_Angle | applied | whole class (apps/viewer/src/galactic/galactic_layers.cpp); whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-01; WGP-01, WGUI-62 | — | apps/viewer/src/galactic/galactic_layers.cpp; apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Game_Object_Name_Font_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Game_Object_Name_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the 3D game initialisation (object name font) |
| Game_Scoring_Script_Name | partial | loaded local space battle (src/platform/battle_scoring.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); missing: objects and modes outside the loaded local space battle | WBF-46; WGP-178 | 653 | src/platform/battle_scoring.cpp; src/platform/galactic/campaign_scoring.cpp | Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate.; basis: reviewed |
| GMC_Battle_Fade_Time | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| GMC_Battle_Zoom_Time | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| GMC_EdgeHorizontalRotateRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_EdgeVerticalTiltRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialDesiredLookdownPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialDiscRotationAngleDegrees | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialiLookatDistanceFromOrigin | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialLookdownAngleDegrees | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialLookdownPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_InitialPitchAngleDegrees | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-41, WGUI-63 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| GMC_InitialPullbackDistance | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-41, WGUI-63 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| GMC_LookdownPullbackRate | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxLookdownAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxRollAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MaxRotationAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinLookdownAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinPullbackDistance | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_MinRollAngle | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_PullbackFromOriginDistFactor | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| GMC_ZoomedPitchAngleDegrees | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-63 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| GMC_ZoomedPositionOffsetPlanetRadiusFractions | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-63 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| GMC_ZoomedPullbackPlanetRadiusFraction | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-63 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| GMC_ZoomTime | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGUI-62 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Good_Side_Leader_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Good_Side_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: read for galactic/story setup only (live_setup.cpp, story_plots.cpp); no skirmish consumer.; basis: reviewed |
| GripperCombatGridSnapDistance | applied | whole class (apps/viewer/src/world_ui_groups.cpp); whole class (apps/viewer/src/world_ui_prepare.cpp) | WSU-36; WSU-36 | — | apps/viewer/src/world_ui_groups.cpp; apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| GUI_Attack_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Attack_Movement_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Cycle_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Cycle_Speed | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Double_Click_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Flash_Duration | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Flash_Level | presentation-later | application not recorded | — | 828 | — | debug build: read only by the selection flash lighting effect of an object |
| GUI_Guard_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Move_Acknowledge_Scale_Space | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Move_Command_Ack_Effect | applied | space destination acknowledgements (apps/viewer/src/battle_effects_prepare.cpp) | OF-01, OF-02, OF-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| GUI_Movement_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Movement_Double_Click_Radar_Event_Name | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |
| GUI_Rapid_Flash_Duration | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Strategic_Countdown_Timers_Screen_Spacing | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Strategic_Countdown_Timers_Screen_X | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Strategic_Countdown_Timers_Screen_Y | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Tactical_Countdown_Timers_Screen_Spacing | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_Tactical_Countdown_Timers_Screen_X | applied | whole class (src/presentation/ui/battle_messages.cpp) | RP-06 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| GUI_Tactical_Countdown_Timers_Screen_Y | applied | whole class (src/presentation/ui/battle_messages.cpp) | RP-06 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| Hack_Super_Weapon_Particle_Effect | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;weapons-shields&#93;: no reader under src/apps/include.; basis: reviewed |
| HardPoint_Target_Reticle_Enemy_Screen_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-31 | — | apps/viewer/src/world_ui_bars.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Enemy_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Enemy_Tracked_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Disabled_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Disabled_Tracked_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Repairing_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Screen_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-31 | — | apps/viewer/src/world_ui_bars.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| HardPoint_Target_Reticle_Friendly_Tracked_Texture | applied | whole class (src/presentation/ui/world_ui.cpp) | WU-32 | — | src/presentation/ui/world_ui.cpp | basis: reviewed |
| Health_Bar_Scale | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Health_Bar_Spacing | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar bracket layout; the viewer draws the spacing from a literal (include/eawr/presentation/ui/world_ui.hpp), not from this tag |
| Hint_Text_Color | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Icons_Per_Column | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| IdleMovementFrames | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: no reader under src/apps/include.; basis: reviewed |
| IdleWalkBlendTime | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: no reader under src/apps/include.; basis: reviewed |
| In_Game_Cinematics | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| In_Game_Message_Default_Font_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| In_Game_Message_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the in-game message window set-up |
| Japanese_Line_Percent | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Japanese_ST_Line_Percent | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Land_Tactical_Camera_Locked | todo | application not recorded | — | 3776 | — | src/presentation/camera/constants_source.cpp:332 reads the tag but no land camera use is shown; basis: reviewed |
| Laser_Beam_Z_Scale_Factor | applied | whole class (apps/viewer/src/battle_effects_projectiles.cpp) | BP-07 | — | apps/viewer/src/battle_effects_projectiles.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Laser_Kite_Z_Scale_Factor | applied | whole class (apps/viewer/src/battle_effects_projectiles.cpp) | BP-04 | — | apps/viewer/src/battle_effects_projectiles.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Left_Queue_Tint | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar build-queue display; the viewer production view uses a literal default, not this tag |
| Localized_Menu_Overlay | applied | whole class (apps/viewer/src/front_end_mode.cpp) | FES-09 | — | apps/viewer/src/front_end_mode.cpp | The front end draws the texture the entry for the start-up language names over the whole screen, as the main menu does; the demo battle retail plays behind it is Main_Menu_Demo_Attract_Mode (todo).; basis: reviewed |
| Localized_Splash_Screen | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Localized_UK_English_Splash_Screen | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Long_Encyclopedia_Delay | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| LoopWaypointLineTextureName | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Lose_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | BE-03 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |
| Main_Menu_Demo_Attract_Mode | applied | whole class (src/skirmish/attract.cpp) | MAT-01 | — | src/skirmish/attract.cpp | basis: reviewed |
| Main_Menu_Score_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Map_Preview_Image_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the map preview image generator |
| Message_Of_The_Day_URL | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Min_Accuracy_For_Icon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Min_Health_Bar_Scale | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Min_Sight_Range_For_Icon | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| MinimumDragSelectDistance | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Mouse_Over_Highlight_Scale | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| MoveBlendTime | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;movement&#93;: no reader under src/apps/include.; basis: reviewed |
| Nebula_Effect_Color | partial | space tactical instances (apps/viewer/src/live_session_frame.cpp); missing: land and cinematic rendering | WHZ-71 | 653 | apps/viewer/src/live_session_frame.cpp | basis: reviewed |
| Neutral_UI_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Neutral_UI_Color in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| Num_Structures_For_Large_Planet_Name | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp) | WGUI-38 | — | apps/viewer/src/galactic/galactic_hud.cpp | basis: reviewed |
| Num_Structures_For_Medium_Planet_Name | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp) | WGUI-38 | — | apps/viewer/src/galactic/galactic_hud.cpp | basis: reviewed |
| Object_Visual_Status_Particle_Attach_Bone_Names | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Object_Visual_Status_Particle_Attach_Bone_Names in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Particle_Brightness_Per_Corruption_Level | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-47; WGP-47 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Particle_Energy_Per_Corruption_Level | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-47; WGP-47 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Particle_Scale_Per_Corruption_Level | applied | observer-admitted galactic corruption (apps/viewer/src/galactic/galactic_scene.cpp); whole class (apps/viewer/src/galactic/scene_content.cpp) | WGP-47; WGP-47 | — | apps/viewer/src/galactic/galactic_scene.cpp; apps/viewer/src/galactic/scene_content.cpp | basis: reviewed |
| Player_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Player_Color in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| PlayModeSwitchMovies | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of PlayModeSwitchMovies in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| Radar_Colorize_Multiplayer_Enemy | applied | space radar blips (src/presentation/ui/minimap.cpp) | MM-05 | — | src/presentation/ui/minimap.cpp | Read in minimap_settings; a hostile blip takes the enemy colour while the stock-No switch is Yes (unit-tested only).; basis: reviewed |
| Radar_Colorize_Selected_Units | applied | whole class (src/presentation/ui/minimap.cpp) | MM-05 | — | src/presentation/ui/minimap.cpp | basis: auto |
| Radar_Multiplayer_Enemy_Color | applied | space radar blips (src/presentation/ui/minimap.cpp) | MM-05 | — | src/presentation/ui/minimap.cpp | Read in minimap_settings; a hostile blip takes the enemy colour while the stock-No switch is Yes (unit-tested only).; basis: reviewed |
| Radar_Selected_Units_Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-05 | — | src/presentation/ui/minimap.cpp | basis: auto |
| ReinforcementOverlayBadColor | applied | whole class (src/presentation/ui/production.cpp) | WR-14 | — | src/presentation/ui/production.cpp | basis: reviewed |
| ReinforcementOverlayGoodColor | applied | whole class (src/presentation/ui/production.cpp) | WR-14 | — | src/presentation/ui/production.cpp | basis: reviewed |
| Right_Queue_Tint | applied | whole class (src/presentation/godot/ui/production_view.cpp) | PU-63 | — | src/presentation/godot/ui/production_view.cpp | basis: reviewed |
| Saliency_Health | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_Health in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Saliency_Power | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_Power in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Saliency_Speed | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_Speed in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Saliency_Targets | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_Targets in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Saliency_X | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_X in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Saliency_Y | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=tactical-ai. No named reader of Saliency_Y in src/, apps/ or include/; wire the authored value into tactical target saliency scoring.; basis: reviewed |
| Scroll_Acceleration_Factor | applied | whole class (src/presentation/camera/controller.cpp) | none recorded | — | src/presentation/camera/controller.cpp | docs/camera.md: the ramp time constant of keyboard and edge pan; the loader is table-driven; basis: auto |
| Scroll_Deceleration_Factor | applied | whole class (src/presentation/camera/controller.cpp) | none recorded | — | src/presentation/camera/controller.cpp | docs/camera.md: the release time of keyboard and edge pan; the loader is table-driven; basis: auto |
| SetupPhaseFOWColor | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of SetupPhaseFOWColor in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| SetupPhaseInvalidDragColor | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of SetupPhaseInvalidDragColor in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| Shield_Flash_Duration | applied | whole class (apps/viewer/src/live_session_frame.cpp) | BP-21 | — | apps/viewer/src/live_session_frame.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| Shield_Flash_Scale | applied | whole class (apps/viewer/src/live_session_frame.cpp) | BP-21 | — | apps/viewer/src/live_session_frame.cpp | Presentation-only; effective-VFS GPU perturbations in live_session_effect_cases.py::test_effective_constants_change_drawn_width_and_flash.; basis: reviewed |
| ShipNameTextFiles | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of ShipNameTextFiles in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| ShouldDisplayPotentialPath | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of ShouldDisplayPotentialPath in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| ShouldDisplayPredictionPaths | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of ShouldDisplayPredictionPaths in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| ShouldDisplaySyncedPaths | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of ShouldDisplaySyncedPaths in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| ShouldUseSpaceIdleMovement | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=movement. No named reader of ShouldUseSpaceIdleMovement in src/, apps/ or include/; wire the authored value into space idle-motion policy.; basis: reviewed |
| Space_Tactical_Camera_Locked | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. constants_source.cpp only rejects this tag in override validation; XML camera-lock behavior is not loaded or applied.; basis: reviewed |
| SpaceFOWColor | applied | whole class (src/presentation/space/fog_field.cpp) | FW-01, FW-11 | — | src/presentation/space/fog_field.cpp | basis: auto |
| SpaceFOWHeight | applied | whole class (src/presentation/camera/controller.cpp) | FW-01, FW-02, FW-03 | — | src/presentation/camera/controller.cpp | basis: auto |
| SpaceFOWRegrowTime | applied | whole class (src/presentation/space/fog_field.cpp) | FW-05, RO-1, V-15 | — | src/presentation/space/fog_field.cpp | basis: auto |
| SpaceIdleMovementSpeed | partial | ordinary space engine audio (apps/viewer/src/battle_audio_events.cpp); missing: locomotor movement implementation | BA-82 | 4532 | apps/viewer/src/battle_audio_events.cpp | Engine sound state compares published walk speed with the authored threshold; movement service is separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SpaceReinforceFeedbackOnlyWhileDragging | applied | whole class (apps/viewer/src/live_fog_view.cpp) | FW-23, WR-17 | — | apps/viewer/src/live_fog_view.cpp | — |
| SpaceReinforceFOWColor | applied | whole class (src/presentation/space/fog_field.cpp) | FW-08, FW-11, FW-12, FW-22 | — | src/presentation/space/fog_field.cpp | basis: auto |
| Speech_Text_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Speech_Text_Color in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Star_Wars_Crawl_Start_Fadeout_Frame | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Strategic_Command_Bar_Buildable_Row_Count | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp) | WGUI-91 | — | apps/viewer/src/galactic/galactic_hud.cpp | Mod-authored (Stargate and the remake mod); stock FoC leaves it unauthored. The galactic command bar reads it as the choice-slot limit.; debug build: typed game-constants XML table (engine-tag catalog ET-T052); basis: reviewed |
| System_Text_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of System_Text_Color in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Tactical_Command_Bar_Buildable_Row_Count | presentation-later | application not recorded | — | — | — | Mod-authored (Stargate and the remake mod); stock FoC leaves it unauthored. The tactical command bar does not read it yet; its row count is fixed.; debug build: typed game-constants XML table (engine-tag catalog ET-T052); basis: reviewed |
| Task_Text_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Task_Text_Color in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Team_Healthbar_Offset | presentation-later | application not recorded | — | 828 | — | debug build: read only by the command bar bracket offset; the viewer draws the offset from a literal (include/eawr/presentation/ui/world_ui.hpp), not from this tag |
| TeamCrouchMoveBlendTime | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of TeamCrouchMoveBlendTime in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| TeamMoveBlendTime | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of TeamMoveBlendTime in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Hover_Height | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Hover_Height in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Max_Bob_Height | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Max_Bob_Height in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Max_Wobble_Angle | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Max_Wobble_Angle in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Transition_Time | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Transition_Time in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Wobble_Cycle_Time | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Wobble_Cycle_Time in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Telekinesis_Wobble_Fade_Time | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Telekinesis_Wobble_Fade_Time in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Text_Button_Default_Font_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Text_Button_Default_Font_Name in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Text_Button_Default_Font_Size | presentation-later | application not recorded | — | 828 | — | debug build: read only by the default font size of UI text-button components |
| Text_Reveal_Rate | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Text_Reveal_Rate in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Tool_Tip_Font_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-52 | — | apps/viewer/src/world_ui_prepare.cpp | — |
| Tool_Tip_Font_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-52 | — | apps/viewer/src/world_ui_bars.cpp | — |
| Tool_Tip_Small_Font_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-52 | — | apps/viewer/src/world_ui_prepare.cpp | — |
| Tool_Tip_Small_Font_Size | applied | whole class (apps/viewer/src/world_ui_bars.cpp) | WU-52 | — | apps/viewer/src/world_ui_bars.cpp | — |
| Tooltip_Delay | partial | galactic construction tooltip (apps/viewer/src/galactic/galactic_hud.cpp); missing: other plain pointer tooltips | WGUI-40 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | Applied to the construction hammer hover; other pointer tooltip consumers remain open. |
| Tractor_Beam_Color | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Frames | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-02, TBF-03 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Texture | applied | GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01 | — | apps/viewer/src/battle_effects_projectiles.cpp | Simple tractor line: authored full width, byte RGB, complete fixed texture and serviced reverse highlights; TBF-01..03.; basis: reviewed |
| Tractor_Beam_Width | applied | GameConstants (apps/viewer/src/battle_effects_prepare.cpp); GameConstants (apps/viewer/src/battle_effects_projectiles.cpp) | TBF-01; TBF-01 | — | apps/viewer/src/battle_effects_prepare.cpp; apps/viewer/src/battle_effects_projectiles.cpp | TBF-01: battle_effects_prepare.cpp builds the Tractor_Beam_ prefix plus Width and loads hero_beams_&#91;1&#93;.width; draw_ability_beams uses look.width * 0.5F for both quad sides. The complete tag is not a source literal.; basis: reviewed |
| Unit_Command_Rankings_By_Category | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); galactic fleet members (apps/viewer/src/galactic/galactic_audio.cpp) | BA-20; WGP-181 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |
| Use_Neutral_UI_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Use_Neutral_UI_Color in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| WalkAnimationCutoff | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of WalkAnimationCutoff in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Water_Clip_Plane_Offset | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Water_Clip_Plane_Offset in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Water_Render_Target_Resolution | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Water_Render_Target_Resolution in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| WaypointFlagModelName | applied | space waypoint display (apps/viewer/src/world_ui_prepare.cpp) | OF-05 | — | apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| WaypointLineLandDashLength | applied | space waypoint display (apps/viewer/src/world_ui_prepare.cpp) | OF-05 | — | apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| WaypointLineLandDashVelocity | applied | space waypoint display (apps/viewer/src/world_ui_prepare.cpp) | OF-05 | — | apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| WaypointLineLandGapLength | applied | space waypoint display (apps/viewer/src/world_ui_prepare.cpp) | OF-05 | — | apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| WaypointLineTextureName | applied | space waypoint display (apps/viewer/src/world_ui_prepare.cpp) | OF-05 | — | apps/viewer/src/world_ui_prepare.cpp | basis: reviewed |
| Win_Lose_Message_Font | applied | whole class (src/presentation/godot/ui/battle_overlay.cpp) | BE-03 | — | src/presentation/godot/ui/battle_overlay.cpp | basis: auto |
| Win_Message_Color | applied | whole class (src/presentation/ui/battle_messages.cpp) | BE-03 | — | src/presentation/ui/battle_messages.cpp | basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/FOW_Reveal_Range_Multiplier | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Enhance_Defense_Ability/@Name | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/@Name | applied | whole class (src/campaign/live_setup.cpp) | WGCH-52 | — | src/campaign/live_setup.cpp | The activated-name list selects the authored neutralize ability by its Name attribute, which gives the host its profile.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/SFXEvent_Activate | todo | application not recorded | — | 653 | — | The neutralize activation cue is not played by the galactic ability host.; basis: reviewed |
| Abilities/Remote_Bomb_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Toss_Anim | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/System_Spy_Ability/@Name | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Blob_Shadow_Bone_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Crouch_Animation_Speed | todo | application not recorded | — | 3776 | — | LR13-14 multiplier is not in load_land_body or land_movement.; basis: reviewed |
| Encyclopedia_Good_Against | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| Encyclopedia_Vulnerable_To | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| Idle_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Land_Model_Anim_Override_Name | applied | whole class (src/scene/scene_build.cpp) | UA-01 | — | src/scene/scene_build.cpp | basis: auto |
| LOD_Bias | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Rotation_Animation_Speed | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Guard | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Show_Hero_Head | partial | first admitted carried space hero head, visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); missing: land and stacked carried hero heads | WHE-45, WHE-47, WU-47, WU-48 | 653 | apps/viewer/src/world_ui_groups.cpp | Explicit heads and named heroes share the loaded space world identity renderer; carried heads use visible carrier health, position and selection.; basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Walk_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |

## GenericHeroUnit, HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attach_To_Flagship_During_Space_Battle | partial | prepared campaign space entry (src/campaign/space_entry.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-21 | 2006 | src/campaign/space_entry.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Death_Fade_Time | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_SFXEvent_Start_Die | applied | Projectile (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-85; BA-16, BA-17, SP-03 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Fire | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-07, BA-14 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker, SecondaryStructure, SpaceProp, SpecialStructure, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | partial | land company members (include/eawr/skirmish/land_content.hpp); ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); whole class (src/scene/scene_build.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-06; WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; none recorded | 1450 | include/eawr/skirmish/land_content.hpp; src/scene/scene_build.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; basis: reviewed |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | applied | whole class (src/presentation/animation/idle_playback.cpp) | none recorded | — | src/presentation/animation/idle_playback.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Select | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-21 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Attack | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Move | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); galactic fleet members (apps/viewer/src/galactic/galactic_audio.cpp) | BA-22; WGP-181 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/galactic_audio.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Clone | applied | whole class (apps/viewer/src/space_populate_report.cpp) | SP-02, UA-07, UA-10 | — | apps/viewer/src/space_populate_report.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## GenericHeroUnit, HeroUnit, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Fleet_Move | applied | galactic fleet members (apps/viewer/src/galactic/galactic_audio.cpp) | WGP-181 | — | apps/viewer/src/galactic/galactic_audio.cpp | The accepted local galactic move plays the best-ranked member's fleet-move event (WGP-181).; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Play_SFXEvent_On_Sighting | partial | loaded space tactical types (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign observers | BA-70, SND-42 | 653 | apps/viewer/src/battle_audio_events.cpp | Logical snapshot visibility consumes type before WAV 2D busy admission; fog-service phase remains presentation-qualified.; basis: reviewed |

## GenericHeroUnit, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Group_Attack | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| SFXEvent_Group_Move | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-22 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Galactic_Stealth_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Tyber_Zann (+6 more). Code review: confirmed gap; family=abilities. galactic_types.cpp reads the handler children but not this name attribute; the tactical nested-handler table has no galactic-stealth binding. Leave GC implementation to its lane.; basis: reviewed |

## GenericHeroUnit, SpecialStructure, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/@Name | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

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

## GroundBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Special_Weapon_Ready | partial | ship (apps/viewer/src/battle_audio_events.cpp); missing: land | WSW-34 | 4532 | apps/viewer/src/battle_audio_events.cpp | Standalone planetary sources consume the source ready cue; land readiness is outside space skirmish.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |

## GroundBuildable, GroundInfantry, GroundStructure, HeroUnit, Indigenous_Unit, Props_Story, SecondaryStructure, Slave_Unit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bracket_Height | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundBuildable, GroundInfantry, GroundStructure, HeroUnit, Indigenous_Unit, Props_Story, SecondaryStructure, Slave_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bracket_Width | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundBuildable, GroundStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Sold_Tactical | applied | tactical sale audio (apps/viewer/src/battle_audio_prepare.cpp); galactic identity sale (apps/viewer/src/galactic/ground_hud.cpp); galactic identity sale (apps/viewer/src/galactic/scene_content.cpp) | WBP-31; none recorded; none recorded | 653 | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/ground_hud.cpp; apps/viewer/src/galactic/scene_content.cpp | Galactic sale confirm follows HUD recipe GHUD-S93 in docs/behaviour/gc-hud-inventory.md. Tactical sale resolves the authored object cue and plays it for the local owner through the sale event; the owning faction supplies the spatial cue (WBP-31). |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Model | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Lighting_Effect_Name | foc-ignores | application not recorded | — | — | — | Stock and the wiki example misspell Lightning_Effect_Name; the debug build has only the correct spelling, so this misspelt tag is never read (Focumentation: Unit Abilities, FORCE_HEAL).; DB-NOTAG |
| GUI_Bias | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

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
| Fonts/*/Combo_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Combo_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Combo_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Edit_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Edit_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Global_Default/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Global_Default/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/IME_Edit_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/IME_Edit_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/L_Text/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/L_Text/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/List_Box/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/List_Box/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/Push_Button/Stretch_Factor | applied | whole class (src/presentation/ui/layout.cpp) | none recorded | — | src/presentation/ui/layout.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/Push_Button/Top_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Bottom_Color | applied | whole class (src/presentation/godot/ui/kit_text.cpp) | none recorded | — | src/presentation/godot/ui/kit_text.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Character_Padding | applied | whole class (src/presentation/godot/ui/theme_builder.cpp) | none recorded | — | src/presentation/godot/ui/theme_builder.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fonts/*/R_Text/Emboss | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/R_Text/Name | applied | whole class (src/presentation/ui/theme.cpp) | AB-44, DG-11, R-SKY-01, S-45, S-49 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Fonts/*/R_Text/Outline | applied | whole class (src/presentation/ui/theme.cpp) | BP-14, HD-20, MM-09, WCC-70 | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
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
| Textures/*/Scanlines | todo | application not recorded | — | 653 | — | Consumer review: theme.cpp only classifies/registers this texture slot; no dialog/frame renderer requests the Scanlines icon or KitPart::scanlines. Registration alone does not apply the value.; basis: reviewed |
| Textures/*/Scroll_Down_Button | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Down_Button_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Down_Button_Mouse_Over | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Down_Button_Pressed | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Middle | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Middle_Disabled | applied | whole class (src/presentation/ui/theme.cpp) | none recorded | — | src/presentation/ui/theme.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Textures/*/Scroll_Tab | applied | whole class (src/presentation/godot/ui/kit_controls.cpp) | none recorded | — | src/presentation/godot/ui/kit_controls.cpp | basis: auto |
| Textures/*/Scroll_Tab_Disabled | todo | application not recorded | — | 653 | — | Consumer review: theme.cpp only registers this texture slot. EawrUiList::draw_scroll_tab always requests Scroll_Tab, and apply_skin does not bind a disabled thumb texture. No disabled-tab draw consumer exists.; basis: reviewed |
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
| Attachment_Bone | applied | whole class (src/scene/space_population.cpp) | BP-30, DG-11, DG-31, DG-36 | — | src/scene/space_population.cpp | basis: auto |
| Collision_Mesh | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/units/unit_combat.cpp) | WAD-20, WAD-21, WAD-23; DG-11, DG-36, DG-39, S-45 | — | src/sim/tactical/blast.cpp; src/units/unit_combat.cpp | basis: auto |
| Damage_Decal | applied | whole class (src/scene/space_population.cpp) | BP-15, BP-30, BP-41, BP-42, DG-38 | — | src/scene/space_population.cpp | basis: auto |
| Damage_Particles | applied | whole class (src/scene/space_population.cpp) | BP-15, BP-41, BP-42, DG-38, HD-04 | — | src/scene/space_population.cpp | basis: auto |
| Death_Explosion_Particles | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-15, BP-30 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Death_Explosion_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-17 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Engine_Particles | applied | whole class (src/scene/space_population.cpp) | BP-41, HD-04 | — | src/scene/space_population.cpp | basis: auto |
| Fire_Bone_A | applied | whole class (src/units/unit_combat.cpp) | A-06, BP-30, W-07 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Bone_B | applied | whole class (src/units/unit_combat.cpp) | IS-02, IS-04, W-06, W-06a, W-10 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-07, BA-14 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Model_To_Attach | applied | whole class (src/scene/space_population.cpp) | A-04, A-06, BP-35, BP-46, BP-47 | — | src/scene/space_population.cpp | basis: auto |
| Randomize_Between_Fire_Bones | applied | space weapon hardpoints (src/sim/tactical/combat_fire.cpp); space weapon hardpoints (src/units/unit_combat.cpp) | WWP-22; WWP-22 | — | src/sim/tactical/combat_fire.cpp; src/units/unit_combat.cpp | Defaults true; samples the posed world-coordinate box with three synchronized draws per attempt. Explicit false retains endpoint selection. An unavailable second endpoint uses A without a draw.; basis: reviewed |
| Tooltip_Text | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-51 | — | apps/viewer/src/world_ui_prepare.cpp | — |

## HardPoint, Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Barrel_Bone_Name | partial | ordinary attached and parent-model space hardpoint turrets (apps/viewer/src/space_populate_animation.cpp); attached one-pulse manual space hardpoints (apps/viewer/src/space_populate_animation.cpp); ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: land hardpoints and remaining non-turret object interfaces | HPT-07; MC-06, WAD-40; HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; UA-05, WWP-17; MC-06, WAD-40 | 4532 | apps/viewer/src/space_populate_animation.cpp; src/sim/tactical/combat_aim.cpp; src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames retain their existing rules; UA-05 ordinary TURRET space objects also apply mechanical readiness and viewer articulation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Attached manual hardpoint models apply the snapshot yaw/pitch to their authored turret/barrel bones before drawing. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## HardPoint, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Bone_Name | partial | ordinary attached and parent-model space hardpoint turrets (apps/viewer/src/space_populate_animation.cpp); attached one-pulse manual space hardpoints (apps/viewer/src/space_populate_animation.cpp); ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: land hardpoints and remaining non-turret object interfaces | HPT-07; MC-06, WAD-40; HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; UA-05, WWP-17; MC-06, WAD-40 | 4532 | apps/viewer/src/space_populate_animation.cpp; src/sim/tactical/combat_aim.cpp; src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames retain their existing rules; UA-05 ordinary TURRET space objects also apply mechanical readiness and viewer articulation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Attached manual hardpoint models apply the snapshot yaw/pitch to their authored turret/barrel bones before drawing. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## Hero_Clash

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Clash_Actions/Attack_Action/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No Hero_Clash action-name decoder or clash presentation service exists; tactical ship abilities do not consume these hero-clash actions.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Animation_Speedup | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Animation_Subindex | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Animation_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Firing_Bone_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_SFXEvent | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Move_Action/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No Hero_Clash action-name decoder or clash presentation service exists; tactical ship abilities do not consume these hero-clash actions.; basis: reviewed |
| Clash_Actions/Move_Action/Move_Animation_Override | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Move_Action/Move_Animation_Speedup | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Move_Action/Move_Animation_Subindex | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Move_Action/Move_Animation_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No Hero_Clash action-name decoder or clash presentation service exists; tactical ship abilities do not consume these hero-clash actions.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Ability_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Conversation_Anim_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Draw_Anim_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Lose_Anim_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Win_Anim_Type | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Win_Exchange_Chance | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| First_Hero_Win_Speech | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Second_Hero_Conversation_Anim_Type | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Second_Hero_Conversation_Anim_Type in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Second_Hero_Draw_Anim_Type | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Second_Hero_Draw_Anim_Type in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Second_Hero_Lose_Anim_Type | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Second_Hero_Lose_Anim_Type in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Second_Hero_Win_Anim_Type | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Second_Hero_Win_Anim_Type in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Second_Hero_Win_Exchange_Chance | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Second_Hero_Win_Exchange_Chance in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Second_Hero_Win_Speech | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of Second_Hero_Win_Speech in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Absorb_Blaster_Ability/@Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/SFXEvent_Activate | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/@Name | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Speedup | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Subindex | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Berserker_Ability/@Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/@Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Effect_Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Source_Bone_Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Target_Bone_Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/SFXEvent_Activate | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Drain_Life_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/@Name | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Damage_Interval_In_Secs | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/SFXEvent_Activate | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Target_Particle_Effect | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Earthquake_Attack_Ability/Terminate_Effect_On_Move_Command | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Find_Weakness_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. The name is retained in SpecialAbilityProfile, but no find-weakness name lookup applies it; HeroUnit remains carried metadata in the space loader.; basis: reviewed |
| Abilities/Force_Cloak_Ability/@Name | todo | application not recorded | — | 3246 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Cloak_Ability/Force_Cloak_Color | todo | application not recorded | — | 3246 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/@Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Confuse_Effect | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/Owner_Particle_Effect | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Confuse_Ability/SFXEvent_Activate | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3776 | — | no presentation reader found yet; combat heal path WLAB-72 only; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Lightning_Ability/@Name | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Activation_Max_Range | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Activation_Min_Range | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Damage_Amount | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Damage_Application_Time_In_Secs | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Lightning_Effect_Name | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Lightning_Max_Targets | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Lightning_Source_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Lightning_Target_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Lightning_Targets_Radius | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Start_Lightning_Frame_Number | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Lightning_Ability/Target_Particle_Effect | todo | application not recorded | — | 3776 | — | combat WLAB-42 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Sight_Ability/@Name | todo | application not recorded | — | 3776 | — | combat WLAB-57 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/@Name | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Activation_Chance | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Activation_Max_Range | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Activation_Min_Range | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Damage_Amount | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Damage_Interval_In_Secs | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Damage_Percentage | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Target_Particle_Effect | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Telekinesis_Ability/Terminate_Effect_On_Move_Command | todo | application not recorded | — | 3776 | — | combat WLAB-44 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/@Name | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Owner_Particle_Effect | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat WLAB-45/46 exists; presentation reader not found; basis: reviewed |
| Abilities/Galactic_Stealth_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Tyber_Zann (+6 more).; basis: reviewed |
| Abilities/Generic_Attack_Ability/@Name | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Generic_Attack_Ability/Attack_Animation | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Generic_Attack_Ability/Attack_Animation_Speedup | todo | application not recorded | — | 3242 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Grenade_Attack_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Toss_Anim | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Grenade_Attack_Ability/SFXEvent_Activate | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/@Name | applied | live galactic hack ability binding (src/campaign/live_setup.cpp) | WGCH-139, WGCH-180, WGCH-57 | — | src/campaign/live_setup.cpp | basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3244 | — | no reader for Hack_Super_Weapon_Ability on origin/main; no walk row for it found; basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/Target_Particle_Effect | todo | application not recorded | — | 3244 | — | no reader for Hack_Super_Weapon_Ability on origin/main; no walk row for it found; basis: reviewed |
| Abilities/Hero_Protection_Ability/@Name | todo | application not recorded | — | 760 | — | Galaxy-map hero protection tag is not read: Neutralization_Cost is read by src/campaign/live_setup.cpp but no reader for this protection tag or multiplier exists.; basis: reviewed |
| Abilities/Infection_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Infection_Ability/Infection_Particle | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Infection_Ability/Shoot_Anim | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/@Name | applied | whole class (src/campaign/live_setup.cpp) | WGCH-52 | — | src/campaign/live_setup.cpp | The activated-name list selects the authored neutralize ability by its Name attribute, which gives the host its profile. The ability host is keyed by authored name and profile, so the same reader serves the HeroUnit carrier.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Flame_Emitter_Model_Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/SFXEvent_Activate | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/Contamination_Object_Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/SFXEvent_Radioactive | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/@Name | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Retreat_Prevention_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_nested_abilities records the attribute in inactive_abilities, but the nested-handler dispatch has no Retreat_Prevention_Ability implementation consuming this name.; basis: reviewed |
| Abilities/Saber_Throw_Ability/@Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Saber_Throw_Ability/Saber_Name | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Stealth_Ability/@Name | todo | application not recorded | — | 3246 | — | Stealth_Ability maps to a special kind in src/sim/tactical/abilities.cpp:139 but no land stealth carrier reads it; land stealth not built; basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Color | todo | application not recorded | — | 3246 | — | parsed by src/units/unit_tables_abilities_special.cpp:128 but no land render use found; basis: reviewed |
| Abilities/Stun_Ability/@Name | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Owner_Particle_Effect | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Stun_Ability/Stunned_Effect | todo | application not recorded | — | 3776 | — | combat WLAB-52 exists; presentation reader not found; basis: reviewed |
| Abilities/Summon_Ability/@Name | todo | application not recorded | — | 3240 | — | no code reads Summon_Ability on origin/main; land summon not built; basis: reviewed |
| Abilities/Summon_Ability/SFXEvent_Unit_Summoned | todo | application not recorded | — | 3240 | — | no code reads Summon_Ability on origin/main; basis: reviewed |
| Abilities/Summon_Ability/Target_Particle_Effect | todo | application not recorded | — | 3240 | — | no code reads Summon_Ability on origin/main; basis: reviewed |
| Abilities/System_Spy_Ability/@Name | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/Tactical_Bribe_Ability/@Name | todo | application not recorded | — | 3244 | — | no code reads Tactical_Bribe_Ability on origin/main; land bribe not built; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/Effect_Z_Offset | todo | application not recorded | — | 3244 | — | no code reads Tactical_Bribe_Ability on origin/main; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/SFXEvent_Fail | todo | application not recorded | — | 3244 | — | no code reads Tactical_Bribe_Ability on origin/main; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/SFXEvent_Success | todo | application not recorded | — | 3244 | — | no code reads Tactical_Bribe_Ability on origin/main; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/@Name | todo | application not recorded | — | 3244 | — | no code reads Vehicle_Thief_Ability on origin/main; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/SFXEvent_Activate | todo | application not recorded | — | 3244 | — | no code reads Vehicle_Thief_Ability on origin/main; basis: reviewed |
| Base_Shield_Penetration_Particle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Blob_Shadow_Bone_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Death_Clone_Is_Obstacle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Dense_FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Dynamic_Transform_End_Color | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_End_Scale | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Start_Color | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Start_Scale | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Has_Pre_Turn_Anim | todo | application not recorded | — | 3776 | — | named only in required-tag list (land_content.hpp:298); no animation consumer found; basis: reviewed |
| Highlight_Blob_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Holster_Drawn_Bone_Translation | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Holstered_Bone_Translation | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Minimum_Drawn_Time_In_Secs | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Weapon_Bone_Name | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Idle_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Is_Visible_On_Enemy_Radar | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06 | — | src/presentation/ui/minimap.cpp | basis: auto |
| IsDeathCloneObstacle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| LOD_Bias | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Primary_Locomotor_Name | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Rotation_Animation_Speed | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Scale_Factor | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-06 | 1450 | include/eawr/skirmish/land_content.hpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Secondary_Locomotor_Name | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Selection_Blob_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Announce | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Attacked | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Deploy | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| SFXEvent_Draw_Weapon | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| SFXEvent_Engine_Cinematic_Focus_Loop | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Guard | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Code review: confirmed gap; family=audio. BattleAudio::respond plays deployed-unit guard responses; HeroUnit is carried metadata in Loader::load_unit, so its own guard cue has no deployed tactical speaker.; basis: reviewed |
| SFXEvent_Holster_Weapon | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | todo | application not recorded | — | 3776 | — | Reader in hud_shell.cpp:918-920 (HUD title/description/icon); land HUD use not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | todo | application not recorded | — | 3776 | — | Reader in hud_shell.cpp:918-920 (HUD title/description/icon); land HUD use not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | todo | application not recorded | — | 3776 | — | Parser branch only for CORRUPT_SYSTEMS (galactic ability type); basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |
| Walk_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |

## HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Explosions | applied | Projectile (apps/viewer/src/battle_effects_impacts.cpp); whole class (apps/viewer/src/debris_props.cpp) | BP-70; BP-14, BP-34, SP-03, SP-08 | — | apps/viewer/src/battle_effects_impacts.cpp; apps/viewer/src/debris_props.cpp | basis: reviewed |

## HeroUnit, MiscObject, SpaceUnit, TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Mouse_Collide_Override_Sphere_Radius | applied | whole class (apps/viewer/src/battle_input.cpp); whole class (src/presentation/ui/selection.cpp) | WSU-10, WSU-11, WSU-12; WSU-10, WSU-11, WSU-12 | — | apps/viewer/src/battle_input.cpp; src/presentation/ui/selection.cpp | basis: reviewed |

## HeroUnit, Mobile_Defense_Unit, Projectile, SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Idle_Loop | partial | ordinary space locomotors (apps/viewer/src/battle_audio_events.cpp); missing: land, cinematic and non-locomotor callers | BA-82, BA-83 | 4532 | apps/viewer/src/battle_audio_events.cpp | Live ordinary-space idle/moving attachment; cinematic focus and other modes remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Engine_Moving_Loop | partial | ordinary space locomotors (apps/viewer/src/battle_audio_events.cpp); missing: land, cinematic and non-locomotor callers | BA-82, BA-83 | 4532 | apps/viewer/src/battle_audio_events.cpp | Live ordinary-space idle/moving attachment; cinematic focus and other modes remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## HeroUnit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bounds_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WSU-36, WSU-54, WU-18 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## HeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Moving | partial | live space craft and ships with movement paths (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception, story-cinematic visibility bypass | BA-80, BA-81, SND-46 | 4532 | apps/viewer/src/battle_audio_events.cpp | Finite attached moving cues; normal engine and continuous ambient loops remain separate gaps.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Health_Critical_Warning | partial | durable standalone and campaign-hosted tactical space objects (apps/viewer/src/battle_audio_prepare.cpp); durable standalone and campaign-hosted tactical space objects (include/eawr/presentation/audio/health_warning.hpp); missing: durable squadron-container damage (&#35;4887), land consumers | SND-33, WCC-54; SND-33 | 653 | apps/viewer/src/battle_audio_prepare.cpp; include/eawr/presentation/audio/health_warning.hpp | Standalone and campaign-hosted durable tactical space objects use per-delivery hull warnings through the retained battle journal. Land and durable squadron-container damage (&#35;4887) remain outside this consumer.; basis: reviewed |
| SFXEvent_Health_Low_Warning | partial | durable standalone and campaign-hosted tactical space objects (apps/viewer/src/battle_audio_prepare.cpp); durable standalone and campaign-hosted tactical space objects (include/eawr/presentation/audio/health_warning.hpp); missing: durable squadron-container damage (&#35;4887), land consumers | SND-33, WCC-54; SND-33 | 653 | apps/viewer/src/battle_audio_prepare.cpp; include/eawr/presentation/audio/health_warning.hpp | Standalone and campaign-hosted durable tactical space objects use per-delivery hull warnings through the retained battle journal. Land and durable squadron-container damage (&#35;4887) remain outside this consumer.; basis: reviewed |

## HeroUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/@Name | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Enemy_Health_Critical_Warning | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| SFXEvent_Enemy_Health_Low_Warning | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Specific_Death_Anim_Index | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: auto |

## LandBombingUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Air_Vehicle_Turret_Target | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Damage_Hit_Particles | todo | application not recorded | — | 3776 | — | Presentation prep exists but the land live path creates no BattleEffects.; basis: reviewed |
| Damaged_Smoke_Asset_Name | todo | application not recorded | — | 1459 | — | no code reads the tag on main; basis: reviewed |
| Fires_Forward | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Read to choose the weapon cone in the land weapon row; no aim/fire code on land yet.; basis: reviewed |
| Hover_Offset | todo | application not recorded | — | 3776 | — | only space_entry_stock reads Hover_Offset; no land reader; basis: reviewed |
| Max_Lift | todo | application not recorded | — | 3776 | — | read in profile decode only (unit_tables_profiles.cpp:379); no land consumer; basis: reviewed |
| MaxFacingLookAheadFrames | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| No_Colorization_Color | todo | application not recorded | — | 3776 | — | only faction-level minimap readers, and no minimap on land; no code on the land path reads this tag; basis: reviewed |
| Prepare_Strafe_Height | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SFXEvent_Bomb_Run_Incoming | todo | application not recorded | — | 3776 | — | No reader on main; basis: reviewed |
| SFXEvent_Bomb_Run_Select_Target | todo | application not recorded | — | 3776 | — | No reader on main; basis: reviewed |
| Shield_Hit_Particles | todo | application not recorded | — | 3776 | — | Presentation prep exists but the land live path creates no BattleEffects.; basis: reviewed |
| Spin_Away_On_Death | todo | application not recorded | — | 3776 | — | Only the space load_body reads it; land has no BattleEffects.; basis: reviewed |
| Spin_Away_On_Death_Chance | todo | application not recorded | — | 3776 | — | Only the space load_body reads it.; basis: reviewed |
| Spin_Away_On_Death_Explosion | todo | application not recorded | — | 3776 | — | Read by BattleEffects, absent on land.; basis: reviewed |
| Spin_Away_On_Death_SFXEvent_Start_Die | todo | application not recorded | — | 3776 | — | Read by BattleAudio, absent on land.; basis: reviewed |
| Spin_Away_On_Death_Time | todo | application not recorded | — | 3776 | — | Only the space load_body reads it.; basis: reviewed |
| XSFXEvent_Ambient_Loop | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| No_Reflection_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| No_Refraction_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66 | 4822 | include/eawr/skirmish/land_content.hpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Scale_Factor | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66 | 4822 | include/eawr/skirmish/land_content.hpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Text_ID | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Props_Generic, Props_Story, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-LAND |

## LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Layer_Z_Adjust | todo | application not recorded | — | 3776 | — | read in skirmish inputs (inputs.cpp:191) and start.cpp layer_position; land-specific application not pinned to a symbol; basis: reviewed |

## LandBombingUnit, Prop_Desert, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandBehavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-LAND |
| Space_Layer | todo | application not recorded | — | 3776 | — | parsed into movement profile and used for skirmish footprint (economy.cpp); land-specific consumer not pinned; basis: reviewed |

## LandBombingUnit, Prop_Desert, Prop_Forest, Props_Snow, Props_Story, Props_Swamp, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |

## LandBombingUnit, Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |
| Deploys | todo | application not recorded | — | 1459 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |

## LandBombingUnit, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 3776 | — | Only galactic battle dialog reads the tag; land bombing units have no AI power consumer; basis: reviewed |
| Armor_Type | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into the land UnitType; step_land runs movement only, no damage code consumes it yet.; basis: reviewed |
| CategoryMask | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | mask_flags runs in load_land_body, only for props passing land_ground_object (Land_Model_Name, no Space_Model_Name).; basis: reviewed |
| Collidable_By_Projectile_Living | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no projectile flight on land until shoot PR3.; basis: reviewed |
| Damage | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Death_Explosions | todo | application not recorded | — | 3776 | — | Read by BattleEffects, which the land live path does not create.; basis: reviewed |
| Death_SFXEvent_Start_Die | todo | application not recorded | — | 3776 | — | Read by BattleAudio, which the land live path does not create.; basis: reviewed |
| Energy_Capacity | todo | application not recorded | — | 1459 | — | no land consumer of energy for bombing runs; registry applied is for space units; basis: reviewed |
| Energy_Refresh_Rate | todo | application not recorded | — | 1459 | — | no land consumer of energy refresh; basis: reviewed |
| Icon_Name | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | land path: map_mode_land_live.cpp ready_land_live -&gt; BattleInput::prepare -&gt; WorldUiView::prepare over live.tables() units; tag read from the catalog object, only for types in the land unit table; basis: reviewed |
| Is_Visible_On_Radar | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Land_FOW_Reveal_Range | applied | whole class (src/units/domain_tags.cpp) | none recorded | — | src/units/domain_tags.cpp | BF-2 (&#35;3779): a land table reads this spelling through the domain selector (src/units/domain_tags.cpp&#35;domain_tag); no land battle runs yet, so no rule mentions it.; basis: reviewed |
| Mass | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Max_Rate_Of_Turn | partial | land battle objects (src/sim/tactical/land_movement.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/sim/tactical/land_movement.cpp | Max_Rate_Of_Turn is converted to LandLocomotionProfile.turn_rate (land_content.hpp, deg/s over 360) and LandMovement::step applies it as the per-tick turn; LandBombingUnit bombers have no land movement path; basis: reviewed |
| Max_Speed | partial | land battle objects (src/sim/tactical/land_movement.cpp); missing: land battle consumers (see note) | LC-06 | 3776 | src/sim/tactical/land_movement.cpp | Max_Speed is read per member (unit_tables_land.cpp land_member) and converted in land_content.hpp to LandLocomotionProfile.maximum_speed, which LandMovement::step uses for desired speed, squad cap and team min; LandBombingUnit bombers have no land movement path; basis: reviewed |
| MovementClass | partial | land battle objects (src/sim/tactical/land_movement.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/sim/tactical/land_movement.cpp | MovementClass resolves to an authored passability mask (land_content.hpp movement_masks) used by admitted(grid,...,m.mask) in LandMovement::step; LandBombingUnit has no land path; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Ranged_Target_Z_Adjust | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no aim code on land yet.; basis: reviewed |
| Remove_Upon_Death | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | Read from the clone in the land-capable death prep; no land deaths yet.; basis: reviewed |
| Score_Cost_Credits | partial | live galactic automatic battle summary (apps/viewer/src/galactic/galactic_battle_dialog.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); missing: consumers outside loaded space and live galactic battle summaries | WAR-88, WBF-45; WBF-45, WGP-178 | 4532 | apps/viewer/src/galactic/galactic_battle_dialog.cpp; src/platform/galactic/campaign_scoring.cpp |  Live GC summary filters confirmed original-type losses using the resolved Score_Cost_Credits threshold.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Select_Box_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | land path: map_mode_land_live.cpp ready_land_live -&gt; BattleInput::prepare -&gt; WorldUiView::prepare over live.tables() units; tag read from the catalog object, only for types in the land unit table; basis: reviewed |
| Shield_Points | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read when SHIELDED in land behaviour; no shield sim on land yet.; basis: reviewed |
| Shield_Refresh_Rate | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read for shielded land bodies; no shield sim on land yet.; basis: reviewed |
| Size_Value | todo | application not recorded | — | 3776 | — | tag is not read anywhere on origin/main; basis: reviewed |
| Tactical_Health | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read as the land hull; step_land has no health/damage consumer yet.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 3776 | — | no presentation reader found; basis: reviewed |

## LandBombingUnit, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bank_Turn_Angle | todo | application not recorded | — | 3776 | — | read in profile decode (unit_tables_profiles.cpp:376) only; no land consumer; basis: reviewed |
| Guard_Chase_Range | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no guard behaviour on land yet.; basis: reviewed |
| Idle_Chase_Range | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no chase behaviour on land yet.; basis: reviewed |
| Max_Rate_Of_Roll | todo | application not recorded | — | 3776 | — | read in profile decode only (unit_tables_profiles.cpp:377); no land consumer; basis: reviewed |
| Max_Thrust | todo | application not recorded | — | 3776 | — | read in profile decode only; no land consumer; basis: reviewed |
| Min_Speed | todo | application not recorded | — | 3776 | — | read in profile decode only; no land consumer; basis: reviewed |
| Projectile_Fire_Pulse_Count | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Loaded into the land weapon row; no firing on land yet.; basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Loaded into the land weapon row; no firing on land yet.; basis: reviewed |
| Projectile_Fire_Recharge_Seconds | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Loaded into the land weapon row; no firing on land yet.; basis: reviewed |
| Projectile_Types | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | First projectile type loaded for the land weapon (others noted only); no firing on land yet.; basis: reviewed |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Ambient_Moving | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:419 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:435 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:435 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Fire | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:408 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Guard | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Move | todo | application not recorded | — | 3776 | — | galactic.cpp:189 reads it for galactic fleets only; no land reader; basis: reviewed |
| SFXEvent_Select | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Targeting_Max_Attack_Distance | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no target selection on land yet.; basis: reviewed |
| Turret_Elevate_Extent_Degrees | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Read as weapon cone height (180 default); no aim code on land yet.; basis: reviewed |
| Turret_Rotate_Extent_Degrees | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Read as weapon cone width (360 default); no aim code on land yet.; basis: reviewed |
| Type | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Props/bombing types become a land body only with Land_Model_Name and no Space_Model_Name.; basis: reviewed |

## LandBombingUnit, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | partial | land battle objects (src/skirmish/start.cpp); missing: land battle consumers (see note) | WLEN-40, WLEN-53 | 3230 | src/skirmish/start.cpp | Is_Decoration/Is_Discardable read in skirmish/inputs.cpp and applied by the shared owner remap (start.cpp:633, used by land_start.cpp); ground-prop variants not verified per WLEN-276; basis: reviewed |

## LandPrimarySkydome, LandSecondarySkydome, Prop_Desert, Prop_Felucia, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Exclude_From_Distance_Fade | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |

## LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | applied | whole class (src/scene/idle_tags.cpp) | none recorded | — | src/scene/idle_tags.cpp | land map placements: map_mode_placements.cpp idle_placement calls idle_tags(..., MapKind::land) (static placed props); basis: reviewed |

## LandPrimarySkydome, LandSecondarySkydome, Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | todo | application not recorded | — | 3776 | — | only read by space placement (space_population.cpp); no code on the land path reads this tag; basis: reviewed |

## LandPrimarySkydome, LandSecondarySkydome, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Galactic_Model_Name | todo | application not recorded | — | 1157 | — | The galactic model is loaded for every game object but only stations and planets are drawn; skydome and story-prop objects have no galactic consumer.; basis: reviewed |

## LensFlares

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LensFlare/@Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No LensFlare XML definition-name catalog or presentation lookup exists; the generic model/material asset routes do not consume this named XML entry.; basis: reviewed |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Color_End | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Color_Start | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Fadeout_Time_Max | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Fadeout_Time_Min | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Replace_Faded | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 653 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; basis: reviewed |
| Texture_Repeat | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 653 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor/leech zero-valued defaults are supported; nonzero repeat/scroll remain limited by the shared clamped sampler.; basis: reviewed |
| Texture_Scroll | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 653 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor/leech zero-valued defaults are supported; nonzero repeat/scroll remain limited by the shared clamped sampler.; basis: reviewed |

## LightningEffect, Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named LightningEffect or XML Material catalog consumer exists; model material shaders use a separate asset route, not these XML definition names.; basis: reviewed |
| Texture_Name | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Destruct_Fade_Time | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Blob_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Blob_Intensity | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Blob_Radius | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Faction_Anim_Subindex | partial | space capture-point ghosts (apps/viewer/src/live_session_prepare.cpp); missing: live owner animation and non-space models | FW-30 | 653 | apps/viewer/src/live_session_prepare.cpp | The validated mapping selects neutral IDLE art for remembered capture points.; basis: reviewed |
| Land_Model_Name | applied | whole class (src/units/unit_tables_land.cpp) | none recorded | — | src/units/unit_tables_land.cpp | load_land_body reads the land model via domain_tag(model_name); ground=6 Marker carriers go through land_ground_object (Land_Model_Name set, no Space_Model_Name); basis: reviewed |
| Reinforcement_Region_Blob_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |
| SFXEvent_Debug_Test_Loop | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Debug_Test_Loop in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| Tooltip_Text | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. WorldUi::prepare reads HardPoint tooltip keys only; Marker tooltips have no corresponding presentation route.; basis: reviewed |

## Marker, MiscObject, MultiplayerStructureMarker, Particle, Projectile, ScriptMarker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Model_Name | applied | whole class (apps/viewer/src/live_session_view.cpp); whole class (apps/viewer/src/space_populate_arrivals.cpp); Projectile (apps/viewer/src/unit_emitters_prepare.cpp) | BP-01, BP-40, BP-60; MD-06; BP-70, MD-06 | — | apps/viewer/src/live_session_view.cpp; apps/viewer/src/space_populate_arrivals.cpp; apps/viewer/src/unit_emitters_prepare.cpp | basis: reviewed |

## Marker, MiscObject, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Icon_Name | applied | whole class (src/presentation/ui/minimap.cpp); whole class (src/presentation/ui/minimap.cpp) | MM-16; MM-06 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Begin_Control_Transition_Radar_Event | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |
| End_Control_Transition_Radar_Event | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |

## Marker, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Initial_State_Visible_Under_FOW | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (apps/viewer/src/space_populate_animation.cpp); whole class (src/script/foc/ai_goals.cpp) | FW-25; FW-25; WNO-13 | — | apps/viewer/src/live_session_frame.cpp; apps/viewer/src/space_populate_animation.cpp; src/script/foc/ai_goals.cpp | Seeds local model knowledge and a drawable battle-start model copy when last-state retention is enabled; independently bypasses forced object-goal fog admission.; basis: reviewed |

## Marker, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Select_Box_Z_Adjust | by-design | application not recorded | — | — | — | WSU-66 |

## Marker, SecondaryStructure, SpaceBuildable, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Last_State_Visible_Under_FOW | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (src/script/foc/ai_goals.cpp) | FW-25, FW-26, FW-27, FW-28, FW-29; WNO-13 | — | apps/viewer/src/live_session_frame.cpp; src/script/foc/ai_goals.cpp | basis: reviewed |

## Marker, SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Loop | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+29 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Jedi_Cruiser (+8 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+29 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Jedi_Cruiser (+8 more). Code review: confirmed gap; family=audio. No named reader of SFXEvent_Ambient_Loop in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Multisample_FOW_Check | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Officer_Academy (+51 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Heavy_Vehicle_Factory, E_Ground_Light_Vehicle_Factory, E_Ground_Officer_Academy (+51 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: loaded into the unit table (unit_tables_profiles.cpp); the fog sampler does not read it.; basis: reviewed |
| Terrain_Texture_Modifier_Join_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |
| Terrain_Texture_Modifier_Material | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |
| Terrain_Texture_Modifier_Square | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |

## Marker, SecondaryStructure, SpecialStructure, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Special_Weapon_Ready | partial | ship (apps/viewer/src/battle_audio_events.cpp); missing: land | WSW-34 | 4532 | apps/viewer/src/battle_audio_events.cpp | Ready planetary sources prefer this cue; completion of a use upgrade retains its separate production cue.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |

## Marker, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Show_Facing | applied | whole class (src/presentation/ui/minimap.cpp) | MM-06 | — | src/presentation/ui/minimap.cpp | basis: auto |

## Marker, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Range_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |

## Material

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Shader_Name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Shader_Name in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Texture_Name_2 | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Texture_Name_2 in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Beacon_Lifetime_In_Secs | partial | space faction beacons (src/sim/tactical/session_step_commands.cpp); space faction beacons (src/skirmish/inputs.cpp); missing: land and galactic beacons | HCB-12d, HCB-12f; HCB-12d, HCB-12f | 4532 | src/sim/tactical/session_step_commands.cpp; src/skirmish/inputs.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Beacon_Radar_Map_Event_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Encyclopedia_Unit_Class | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); missing: UI consumers outside galactic reference pages | GREF-06 | 1952 | apps/viewer/src/galactic/reference_catalog.cpp | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proximity_Mine); basis: reviewed |
| IsDeathCloneObstacle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Model_Name | applied | whole class (apps/viewer/src/live_session_view.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp) | none recorded; WAD-38 | — | apps/viewer/src/live_session_view.cpp; src/units/unit_tables_decode.cpp | The existing viewer model-name binding has no named rule; WAD-38 adds the canonical target's collision geometry.; basis: reviewed |
| Not_Really_Selectable | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Beacon_Placed | partial | space faction beacons (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic beacons | HCB-12c, HCB-12f | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## MiscObject, Particle, SpacePrimarySkydome, SpaceProp, SpaceSecondarySkydome, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Exclude_From_Distance_Fade | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Shield_Penetration_Particle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Blob_Shadow_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Deployment_Anim_Rate | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Encyclopedia_Text | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| Encyclopedia_Unit_Class | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); missing: UI consumers outside galactic reference pages | GREF-06 | 1952 | apps/viewer/src/galactic/reference_catalog.cpp | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue); basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Fleet_Move | todo | application not recorded | — | 653 | — | Fleet-move audio plays from the moving fleet member types (apps/viewer/src/galactic/galactic_audio.cpp&#35;GalacticAudio::fleet_move); this carrier class is not a fleet member, so no consumer for it is verified.; basis: reviewed |
| SFXEvent_Guard | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Unit_Lost | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |
| Visible_To_Enemies_When_Empty | todo | application not recorded | — | 3776 | — | Parsed only; no land reader for mobile defence units; basis: reviewed |
| Wind_Disturbance_Radius | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Sphere_Alpha | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Strength | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |

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

## MOV_Cinematic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Company_Units | partial | galactic current-type membership (src/data/xml_merge.cpp); missing: other company consumers | WGA-363, WGA-364 | 4532 | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

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
| Scale_Factor | applied | CONCENTRATE_FIRE (apps/viewer/src/battle_effects_impacts.cpp); CONCENTRATE_FIRE (apps/viewer/src/battle_effects_prepare.cpp); whole class (apps/viewer/src/space_populate_animation.cpp); Projectile (apps/viewer/src/unit_emitters_prepare.cpp); whole class (apps/viewer/src/world_ui_prepare.cpp); whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp) | BP-15, WHE-78; BP-15, WHE-78; MD-06; BP-70, MD-06; AV-05, R-ROT-01; L-32; L-32 | — | apps/viewer/src/battle_effects_impacts.cpp; apps/viewer/src/battle_effects_prepare.cpp; apps/viewer/src/space_populate_animation.cpp; apps/viewer/src/unit_emitters_prepare.cpp; apps/viewer/src/world_ui_prepare.cpp; src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp | basis: reviewed |

## MusicEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fade_In_Seconds | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | BA-44 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |
| Fade_Out_Previous_Seconds | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | BA-44 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |

## Particle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Allow_Particle_Model_Culling | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Ewok_Suicide_Bomb_Explosion, Huge_Explosion_Land, Large_Explosion_Land, Medium_Explosion_Land, p_orbital_impact, p_orbital_impact_ion, Remote_bomb. Mixed ground/space, space carriers on Particle: Ewok_Suicide_Bomb_Explosion, Huge_Explosion_Land, Large_Explosion_Land, Medium_Explosion_Land, p_orbital_impact, p_orbital_impact_ion, Remote_bomb. sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Dynamic_Transform_End_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Dynamic_Transform_End_Scale | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Dynamic_Transform_Start_Color | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Dynamic_Transform_Start_Scale | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Explosion_Jitter_Factor | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Friggin_Huge_Explosion_Space_Empire, Harmonic_Bomb_Explosion_Slave_I, Huge_Explosion_Land, Huge_Explosion_Space, Huge_Explosion_Space_Empire (+18 more). Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Friggin_Huge_Explosion_Space_Empire, Harmonic_Bomb_Explosion_Slave_I, Huge_Explosion_Land, Huge_Explosion_Space, Huge_Explosion_Space_Empire (+18 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Explosion_Scorch_Mark | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Huge_Explosion_Land, Huge_Explosion_Space, Large_Damage_Land, Large_Explosion_Land, Large_Explosion_Space (+20 more). Mixed ground/space, space carriers on Particle: Empire_Bombing_Run_Explosion_Land, Ewok_Suicide_Bomb_Explosion, Friggin_Huge_Explosion_Space, Huge_Explosion_Land, Huge_Explosion_Space, Large_Damage_Land, Large_Explosion_Land, Large_Explosion_Space (+20 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Galactic_Model_Name | partial | Corruption particle (apps/viewer/src/galactic/scene_content.cpp); missing: Other galactic action particles await admitted producers and U-06 placement. | WGP-19 | 4532 | apps/viewer/src/galactic/scene_content.cpp | Family: Galactic UI / camera / map. WGP-19 applies the authored corruption particle model; scene_content.cpp&#35;load_galactic_particle also resolves models for admitted dynamic particles. Other action-particle producers and placement remain qualified separately; model decoding alone does not cover every Particle author.; basis: reviewed |
| In_Midforeground | partial | galactic corruption attachments and line particles (apps/viewer/src/galactic/scene_content.cpp); missing: other particle producers | WGP-01b, WGP-19, WGP-21 | 650 | apps/viewer/src/galactic/scene_content.cpp | Effective authored flags route galactic corruption attachments and lines; other producers remain separate.; basis: reviewed |
| In_Midground | partial | galactic corruption attachments and line particles (apps/viewer/src/galactic/scene_content.cpp); missing: other particle producers | WGP-01b, WGP-19, WGP-21 | 650 | apps/viewer/src/galactic/scene_content.cpp | Effective authored flags route galactic corruption attachments and lines; other producers remain separate.; basis: reviewed |
| Model_Visible_To_Enemy | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: read by the effects preparer (battle_effects_prepare.cpp); application to fog visibility not verified.; basis: reviewed |
| Particle_Attach_To_Collision | partial | Damage_Hit_Particles and Shield_Hit_Particles entries (apps/viewer/src/battle_effects_impacts.cpp); projectile shield-hit facing (apps/viewer/src/battle_effects_impacts.cpp); missing: Other collision-attached particle hosts retain their existing ownership gap | BP-63, PS-02, PS-03, PS-04; BP-18 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Particle_Lifetime_Frames | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-16, BP-33 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Describe_History | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-03, BLD-05 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Describe_Population | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-03 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Describe_Terrain | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-03 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Describe_Weather | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-03 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Encyclopedia_Weather_Icon | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-04 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Galactic_Model_Name | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-04 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Idle_Anim_00_Rate_Mod | todo | application not recorded | — | 3772 | — | Family: Galactic UI / camera / map. The galactic planet animation clock does not consume the recognized rate tag (ET-T053); the SpaceProp consumer does not cover Planet. Implement planet idle-rate scaling through its admitted animation clock.; basis: reviewed |
| In_Background | applied | whole class (apps/viewer/src/galactic/galactic_layers.cpp) | WGP-02 | — | apps/viewer/src/galactic/galactic_layers.cpp | basis: reviewed |
| Loop_Idle_Anim_00 | applied | whole class (apps/viewer/src/galactic/galactic_layers.cpp) | WGP-04 | — | apps/viewer/src/galactic/galactic_layers.cpp | basis: reviewed |
| Mouse_Collide_Override_Sphere_Radius | applied | whole class (apps/viewer/src/galactic/galactic_mode.cpp) | WGP-04 | — | apps/viewer/src/galactic/galactic_mode.cpp | basis: reviewed |
| Native_Icon_Name | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-04 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Scale_Factor | applied | whole class (apps/viewer/src/galactic/galactic_scene.cpp) | WGP-04 | — | apps/viewer/src/galactic/galactic_scene.cpp | basis: reviewed |
| Show_Name | applied | whole class (apps/viewer/src/galactic/galactic_hud.cpp) | WGUI-97 | — | apps/viewer/src/galactic/galactic_hud.cpp | Omitted Show_Name inherits the parser's false default; that mod default is unsourced. All stock planet types author the tag.; basis: reviewed |
| Text_ID | applied | whole class (src/presentation/ui/loading_screen.cpp) | BLD-02 | — | src/presentation/ui/loading_screen.cpp | the skirmish battle loading screen (docs/behaviour/loading-screen.md); the galactic uses stay out of scope; basis: reviewed |
| Zoomed_Terrain_Index | applied | whole class (apps/viewer/src/galactic/galactic_fleet_hud.cpp) | WGP-04 | — | apps/viewer/src/galactic/galactic_fleet_hud.cpp | selects the focused organize shell's land_art_ALT&lt;n&gt; panel; behaviour rows GHUD-S52..S54 in gc-hud-inventory.md; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Dense_FOW_Reveal_Range_Multiplier | partial | FOW_REVEAL_PING owner projectile (src/sim/tactical/session_step_finish.cpp); missing: Other revealing projectiles outside the supported space ping closure | WFP-01, WFP-02, WFP-03 | 653 | src/sim/tactical/session_step_finish.cpp | Creates the owner reveal projectile; absent lifetime ends the stock zero-distance ping on its first flight step (WFP-03), followed by ordinary fog regrowth. WFP-U1 regrowth appearance remains capture-needed.; basis: reviewed |
| IsDeathCloneObstacle | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Model_Visible_To_Enemy | partial | FOW_REVEAL_PING owner projectile (apps/viewer/src/live_session_frame.cpp); missing: Other revealing projectiles outside the supported space ping closure | WFP-01, WFP-02, WFP-03 | 653 | apps/viewer/src/live_session_frame.cpp | Creates the owner reveal projectile; absent lifetime ends the stock zero-distance ping on its first flight step (WFP-03), followed by ordinary fog regrowth. WFP-U1 regrowth appearance remains capture-needed.; basis: reviewed |
| Mouse_Collide_Override_Sphere_Radius | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: loaded into the unit table (unit_tables_profiles.cpp); the tactical pick path is not traced to it.; basis: reviewed |
| Projectile_Absorbed_By_Shields_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-10, BP-17, BP-19, BP-20 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Combat_Mod_Effect | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Projectile_Custom_Render | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-01, BP-40, BP-60 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Laser_Color | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Lifetime_Detonation_Particle | partial | ordinary space projectiles and HARMONIC_BOMB/WEAKEN_ENEMY spawned projectiles (apps/viewer/src/battle_effects_impacts.cpp); hero ability detonation projectiles (apps/viewer/src/battle_effects_impacts.cpp); missing: land projectiles (&#35;653) | WAD-07, WHE-62; WHE-62 | 653 | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |
| Projectile_Object_Armor_Reduced_Detonation_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-11 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Object_Creation_Particle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Projectile_Object_Detonation_Particle | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-11 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_SFXEvent_Detonate | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-15, BP-13; WPJ-35, WPJ-37, WPJ-40 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Projectile_SFXEvent_Detonate_Reduced_By_Armor | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-15, BP-13 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Projectile_Texture_Slot | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-03, BP-06 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Space_FOW_Reveal_Range | partial | FOW_REVEAL_PING owner projectile (src/sim/tactical/session_step_finish.cpp); missing: Other revealing projectiles outside the supported space ping closure | WFP-01, WFP-02, WFP-03 | 653 | src/sim/tactical/session_step_finish.cpp | Creates the owner reveal projectile; absent lifetime ends the stock zero-distance ping on its first flight step (WFP-03), followed by ordinary fog regrowth. WFP-U1 regrowth appearance remains capture-needed.; basis: reviewed |

## Prop_Desert, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read for land members; only hard_y feeds spawn spacing (LC-79), hard_x is not used yet.; basis: reviewed |
| Custom_Hard_YExtent | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read and used for squad spawn-slot spacing (LC-79, session_land.cpp); movement footprint uses model extents.; basis: reviewed |
| UnitCollisionClass | partial | land battle objects (include/eawr/units/land_companies.hpp); missing: land battle consumers (see note) | WLPA-73 | 3776 | include/eawr/units/land_companies.hpp | class read and ordered in land content (unit_tables_land.cpp:190); collision use in land sim not pinned; basis: reviewed |

## Prop_Desert, Prop_Forest, Props_Snow, Props_Swamp, Props_Temperate, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Hard_XExtent_Offset | todo | application not recorded | — | 3776 | — | fog_box_offset is read in the space load_body only.; basis: reviewed |
| Custom_Hard_YExtent_Offset | todo | application not recorded | — | 3776 | — | fog_box_offset is read in the space load_body only.; basis: reviewed |

## Prop_Desert, Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Obstacle_Height | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_Width | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_X_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_Y_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |

## Prop_Desert, Props_Generic, Props_Story, Props_Temperate, Props_Urban

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Discardable | partial | land battle objects (src/skirmish/start.cpp); missing: land battle consumers (see note) | WLEN-40, WLEN-53 | 3230 | src/skirmish/start.cpp | Is_Decoration/Is_Discardable read in skirmish/inputs.cpp and applied by the shared owner remap (start.cpp:633, used by land_start.cpp); ground-prop variants not verified per WLEN-276; basis: reviewed |

## Prop_Desert, Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Obstacle_Radius | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |

## Prop_Desert, Props_Story, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Distance_From_Spawner | todo | application not recorded | — | 3776 | — | WLEN-47 marks it missing in code.; basis: reviewed |
| Space_Obstacle_Offset | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Spawn_Indigenous_Units_Chance | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawn_Indigenous_Units_In_Packs | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawn_Indigenous_Units_Radius | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Delay_Seconds | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Quantity | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Type | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |

## Prop_Desert, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Indigenous_Unit_Corruptible | todo | application not recorded | — | 3776 | — | WLEN-52 marks it missing in code.; basis: reviewed |

## Prop_Forest, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Model_Name | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Props_Generic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/@Name | todo | application not recorded | — | 3776 | — | loader reads the tag (src/units/unit_tables_abilities_bonuses.cpp:48-70) but production.combat_bonuses is consumed only by skirmish economy (src/skirmish/economy.cpp:355-397) and unit_identity; no land battle use; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | gate read by the loader (unit_tables_abilities_bonuses.cpp:48) for the domain automatic style; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | loader reads it into bonus.categories; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | loader reads it into bonus.types; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Movement_Speed_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | todo | application not recorded | — | 3776 | — | loader reads it into bonus.percentages; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Specific_Faction | todo | application not recorded | — | 3776 | — | loader stores specific_faction; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Stacking_Category | todo | application not recorded | — | 3776 | — | loader reads stacking category; no land battle consumer; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Unit_Strength_Category | todo | application not recorded | — | 3776 | — | galactic auto-resolve reads it (src/units/galactic_types.cpp:338-341); no land battle consumer; basis: reviewed |
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3776 | — | no presentation reader found yet; combat heal path WLAB-72 only; basis: reviewed |
| Abilities/Force_Healing_Ability/Single_Target_Heal | todo | application not recorded | — | 3776 | — | mechanic row; walk lists Force_Healing one row as foc-ignores (unclear which) so not marked; verify; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | todo | application not recorded | — | 3776 | — | no presentation reader found yet; basis: reviewed |
| Avoidance_Disabled | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Death_By_TSW_Replacements | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Death_Projectiles | todo | application not recorded | — | 3776 | — | Decoded after the land early return in unit_tables_decode.cpp (space path only).; basis: reviewed |
| FormationGrouping | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| FormationOrder | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Ground_Vehicle_Turret_Target | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Is_Affected_By_Gravity_Control_Field | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Override_Bounty_SP | todo | application not recorded | — | 1458 | — | bounty read by space economy and auto_resolve_live only; no land reader; basis: reviewed |
| Political_Control | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Show_In_Sidebar_When_Complete | todo | application not recorded | — | 1458 | — | team_dock reads it for UpgradeObject only; no land reader; basis: reviewed |
| Snap_Movement_Orders_To_Center | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Weather_Category | todo | application not recorded | — | 3225 | — | WLEN-15: no loader or sim field for Weather_Category on land; basis: reviewed |
| Wind_Disturbance_Radius | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Sphere_Alpha | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Strength | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |

## Props_Generic, Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Cinematic_Object_Only | todo | application not recorded | — | 3776 | — | only campaign production admission (galactic); no code on the land path reads this tag; basis: reviewed |
| Collidable_By_Projectile_Dead | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no projectile flight on land until shoot PR3.; basis: reviewed |
| Encyclopedia_Text | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| FormationSpacing | applied | whole class (src/sim/tactical/land_placement.cpp) | LP-20, WLPA-24, WLPA-27 | 3776 | src/sim/tactical/land_placement.cpp | FormationSpacing x20 drives landing slot spacing and nearest-open padding (session_land.cpp slot spacing, land_placement_padding); formation-move slot geometry is a separate F5 item; basis: reviewed |
| GUI_Bounds_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | land path: map_mode_land_live.cpp ready_land_live -&gt; BattleInput::prepare -&gt; WorldUiView::prepare over live.tables() units; tag read from the catalog object, only for types in the land unit table; basis: reviewed |
| GUI_Bracket_Size | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | land path: map_mode_land_live.cpp ready_land_live -&gt; BattleInput::prepare -&gt; WorldUiView::prepare over live.tables() units; tag read from the catalog object, only for types in the land unit table; basis: reviewed |
| Has_Land_Evaluator | todo | application not recorded | — | 3776 | — | Same enrolment gap as the combat row; basis: reviewed |
| Influences_Capture_Point | todo | application not recorded | — | 1458 | — | influences_capture decoded; no land consumer; basis: reviewed |
| Is_Branched_Map_Discardable | partial | GC simulated map objects (src/campaign/conflict_entry.cpp); missing: land tactical and presentation-only placements | WGT-121 | 4532 | src/campaign/conflict_entry.cpp | WGT-121 reads authored flags and their debug-build defaults through skirmish inputs, then applies them only to GC simulated map-object admission. Land entry and inert presentation scenery retain their separate scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Is_Valid_Target | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | WCC-25 | 3776 | src/units/unit_tables_land.cpp | Read in load_land_body; props with a Space_Model_Name never reach it.; basis: reviewed |
| MinimumPushReturnDistance | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Occlusion_Silhouette_Enabled | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Overall_Length | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Overall_Width | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| OverrideAcceleration | applied | whole class (src/sim/tactical/land_movement.cpp) | none recorded | 3776 | src/sim/tactical/land_movement.cpp | OverrideAcceleration is converted to LandLocomotionProfile.acceleration (default Max_Speed, land_content.hpp) and applied by the speed ramp in land_movement.cpp (profile.acceleration * dt); basis: reviewed |
| OverrideDeceleration | applied | whole class (src/sim/tactical/land_movement.cpp) | none recorded | 3776 | src/sim/tactical/land_movement.cpp | OverrideDeceleration is converted to LandLocomotionProfile.deceleration (default Max_Speed, land_content.hpp) and applied by the speed ramp in land_movement.cpp (profile.deceleration * dt); basis: reviewed |
| Property_Flags | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | mask_flags runs in load_land_body, only for props passing land_ground_object (Land_Model_Name, no Space_Model_Name).; basis: reviewed |
| Radar_Icon_Name | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Radar_Icon_Size | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Reveal_During_Setup_Phase | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_Engine_Idle_Loop | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:420 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Engine_Moving_Loop | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:421 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| Space_Model_Name | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Only routes a Land_Model_Name prop to the land body (heuristic); the space model is not applied on land.; basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |

## Props_Generic, Props_Story, Props_Swamp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Unit_Class | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); missing: UI consumers outside galactic reference pages | GREF-06 | 1952 | apps/viewer/src/galactic/reference_catalog.cpp | SCOPE-LAND; basis: reviewed |

## Props_Generic, Props_Temperate

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Idle_Anim_00_Rate_Mod | applied | whole class (src/scene/idle_tags.cpp) | none recorded | — | src/scene/idle_tags.cpp | land map placements: map_mode_placements.cpp idle_placement calls idle_tags(..., MapKind::land) (static placed props); basis: reviewed |

## Props_Story

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Activation_Style | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Heal_Amount | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Heal_Percent | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Heal_Range | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Lighting_Effect_Name | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Single_Target_Heal | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | deferred | application not recorded | — | 4532 | — | WSH-01 space automatic healing is available to loaded types; this neutral story marker is outside the skirmish roster, and its story/neutral-defender lifecycle is unverified (UFS-28). Other Props_Story healing authors use land activation. No runtime coverage is claimed for these props.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; Effective props_story.xml: UM12_Marker_Healing authors Space_Automatic healing and inherits the space model from Marker_Generic_Purple in markers.xml. UC-01 acquisition closure in data/census/space-units.json contains no UM12_Marker_Healing; the story marker has no space skirmish roster target. |
| Abilities/Redirect_Blaster_Ability/@Name | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Block_Chance | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | todo | application not recorded | — | 3776 | — | combat redirect applied (WLAB-60); presentation row not verified; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/@Name | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Stacking_Category | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent | todo | application not recorded | — | 3240 | — | Tractor beam is applied only by the space session (src/sim/tactical/session_cmd_abilities.cpp&#35;apply_ability, ship types); no land session applies it.; basis: reviewed |
| Additional_Population_Capacity | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); missing: UI consumers outside galactic reference pages | GREF-14 | 1129 | apps/viewer/src/galactic/reference_catalog.cpp | GC-only census application: original-type population/capacity contributes through src/sim/galactic/production.cpp&#35;population_totals (WGEP-48/62/68/69). Outside the tactical M2 scene; GC coverage is recorded in statuses-galactic.json. Live owned-list lifecycle/usability remains supplied by the owning services.; basis: reviewed |
| Affiliation | applied | whole class (src/campaign/live_setup.cpp) | none recorded | — | src/campaign/live_setup.cpp | live_setup.cpp:114 admission.affiliated = contains(type.affiliation, owner-&gt;faction); setup.cpp:146 uses it to select affected types; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Apply_Y_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Apply_Z_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Asteroid_Damage_Hit_Particles | todo | application not recorded | — | 3776 | — | read only by battle_effects_prepare.cpp (space impact looks); no land view reads it; basis: reviewed |
| Attack_Move_Response_Range | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no attack-move on land yet.; basis: reviewed |
| Autonomous_Move_Extension_Vs_Attacker | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Base_Level | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:300 admission.base_level = *type.base_level; live_setup.cpp:156 reads type.base_level for the station rule; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Base_Position | todo | application not recorded | — | 3776 | — | no land reader of Base_Position; props are not placed from this tag on land; basis: reviewed |
| Blob_Shadow_Bone_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Build_Cost_Credits | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:197 and 265 use type.build_cost_credits for the authored and resolved price; live_setup.cpp:202 scales it; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Build_Limit_Current_For_All_Allies | todo | application not recorded | — | 1458 | — | build limit read by space economy and galactic types only; basis: reviewed |
| Build_Limit_Lifetime_Per_Player | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:302 sets admission.lifetime_limit from the type's build_limit_lifetime_per_player; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Build_Tab_Space_Station | todo | application not recorded | — | 1458 | — | space planet HUD only; no land reader; basis: reviewed |
| Build_Tab_Space_Units | todo | application not recorded | — | 1458 | — | space planet HUD and skirmish start only; no land reader; basis: reviewed |
| Build_Time_Seconds | applied | whole class (src/campaign/live_setup.cpp) | none recorded | — | src/campaign/live_setup.cpp | live_setup.cpp:198-200 scales type.build_time_seconds by production speed into the built time; no behaviour-note rule mentions this tag yet; basis: reviewed |
| CanCellStack | todo | application not recorded | — | 3776 | — | CanCellStack read nowhere on origin/main; basis: reviewed |
| Cinematic_Anim_Blend_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Cinematic_Anim_Index | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Cinematic_Anim_Speed | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Company_Transport_Unit | todo | application not recorded | — | 1149 | — | Galactic company producer reads it for company types (src/campaign/starting_forces.cpp, galactic_types.cpp); Props_Story objects are not companies, so no consumer is verified.; basis: reviewed |
| Create_Team_Type | todo | application not recorded | — | 3776 | — | load_land_company only handles *Company types; a Props_Story carrier is never read.; basis: reviewed |
| Custom_Soft_Footprint_Radius | applied | whole class (include/eawr/skirmish/land_content.hpp) | none recorded | 3776 | include/eawr/skirmish/land_content.hpp | Soft radius x Scale_Factor becomes profile.radius, used for admitted() terrain checks, member-member contact and landing padding (land_movement.cpp, session_land.cpp); basis: reviewed |
| Death_Clone | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | Viewer resolves the clone on the land live path (options_.land); land has no kill source until shoot PR3.; basis: reviewed |
| Death_Clone_Is_Obstacle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Death_Fade_Time | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | Read from the clone (also loaded in load_land_body); no land deaths yet.; basis: reviewed |
| Death_Persistence_Duration | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | Read from the clone (also loaded in load_land_body); no land deaths yet.; basis: reviewed |
| Dense_FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Dynamic_Transform_Delay_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_End_Color | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_End_Scale | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Start_Color | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Start_Scale | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Encyclopedia_Good_Against | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| Encyclopedia_Vulnerable_To | partial | land battle objects (src/presentation/ui/hud_shell.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/presentation/ui/hud_shell.cpp | land HUD (sync_cards in map_mode_land_live.cpp) shares the encyclopedia hover card via BattleInput::encyclopedia_unit; unverified that every carrier type is hoverable on land; basis: reviewed |
| Enemy_Spawn_Text | todo | application not recorded | — | 3776 | — | no code reads spawn text tags on main; basis: reviewed |
| Fire_Inaccuracy_Distance | todo | application not recorded | — | 3776 | — | Read only for space hardpoints; land weapon uses Targeting_Fire_Inaccuracy.; basis: reviewed |
| Formation_Priority | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Friendly_Spawn_Text | todo | application not recorded | — | 3776 | — | no code reads spawn text tags on main; basis: reviewed |
| GalacticBehavior | todo | application not recorded | — | 1157 | — | Unit galactic behaviour names are only read for planets.; basis: reviewed |
| Garrison_Category | todo | application not recorded | — | 3776 | — | Same compatibility gate gap as the combat row; basis: reviewed |
| Ground_Infantry_Turret_Target | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| GUI_Distance | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| GUI_Hide_Health_Bar | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | land path: map_mode_land_live.cpp ready_land_live -&gt; BattleInput::prepare -&gt; WorldUiView::prepare over live.tables() units; tag read from the catalog object, only for types in the land unit table; basis: reviewed |
| GUI_Model_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| GUI_Row | todo | application not recorded | — | 3776 | — | gui_row set only in space body; no code on the land path reads this tag; basis: reviewed |
| GUI_Velocity | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| HardPoints | todo | application not recorded | — | 3776 | — | space twin applied but no land reader of HardPoints on origin/main; basis: reviewed |
| Has_Space_Evaluator | todo | application not recorded | — | 3776 | — | Only read by space decode and space AI perception; the land body returns before it.; basis: reviewed |
| Highlight_Blob_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Holster_Disable_Engine_Loops | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Drawn_Bone_Translation | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Holstered_Bone_Translation | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Minimum_Drawn_Time_In_Secs | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Transition_Time_In_Secs | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Weapon_Bone_Name | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Hyperspace | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |
| Hyperspace_Speed | applied | whole class (src/campaign/fleets.cpp) | none recorded | — | src/campaign/fleets.cpp | fleets.cpp:153 validates original-&gt;hyperspace_speed and carries it into the fleet type used by GC movement; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Immune_To_Damage | todo | application not recorded | — | 3776 | — | Only the space loader and campaign auto-resolve read it; neither is a land-battle anchor.; basis: reviewed |
| Include_In_UI_Map_Header | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| Initial_State_Visible_Under_FOW | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Is_Dummy | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Is_Escort | todo | application not recorded | — | 1455 | — | no reader on origin/main; basis: reviewed |
| Is_Force_Sensitive | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |
| Is_Marker | partial | land battle objects (src/skirmish/land_start.cpp); missing: land battle consumers (see note) | WLEN-53, WLEN-55 | 3230 | src/skirmish/land_start.cpp | Is_Marker read via scene::space_object_tags (inputs.cpp:176) and used by land_start.cpp (marker placements skipped as static types; Team_NN_ markers drive spawns); Props_Story markers get no land role; basis: reviewed |
| Is_Named_Hero | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read in load_land_body and land_ground_object; only props routed to the land body count.; basis: reviewed |
| Is_Squashable_By_Supercrusher | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Is_Stationary_When_Attacking | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Land_Damage_Alternates | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Land_Damage_SFX | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Land_Damage_Thresholds | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Land_Model_Anim_Override_Name | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | read with options_.land for death clone clip counts; no land idle/anim playback override yet; basis: reviewed |
| Last_State_Visible_Under_FOW | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Lua_Script | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |
| Maintenance_Cost | todo | application not recorded | — | 1458 | — | maintenance read by space AI economy and galactic data only; land has no upkeep; basis: reviewed |
| Max_Squad_Size | todo | application not recorded | — | 3776 | — | load_land_company only handles *Company types; a Props_Story carrier is never read.; basis: reviewed |
| Mouse_Collide_Override_Sphere_Radius | todo | application not recorded | — | 3776 | — | only read in space body (unit_tables_profiles.cpp:472), after the land early return at unit_tables_decode.cpp:198; no code on the land path reads this tag; basis: reviewed |
| MP_Encyclopedia_Text | todo | application not recorded | — | 3776 | — | MP branch of the shared card, land MP not traced; no code on the land path reads this tag; basis: reviewed |
| Multisample_FOW_Check | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Next_Level_Base | applied | whole class (src/campaign/ai_selection_content.cpp) | none recorded | — | src/campaign/ai_selection_content.cpp | ai_selection_content.cpp:279-280 resolves type.original-&gt;next_level_base to the next starbase level; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Pause_During_Cinematic_Anim | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Play_SFXEvent_On_Sighting | todo | application not recorded | — | 3776 | — | BattleAudio is not created on the land live path; no code on the land path reads this tag; basis: reviewed |
| Political_Control | applied | whole class (src/campaign/control.cpp) | none recorded | — | src/campaign/control.cpp | control.cpp:90-105 turns type-&gt;political_control and planet-&gt;political_control into the presence and leaning the GC control step uses; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Political_Faction | applied | whole class (src/campaign/live_setup.cpp) | none recorded | — | src/campaign/live_setup.cpp | live_setup.cpp:359-362 sets control.leaning and matches source-&gt;political_faction to the faction at setup; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Population_Value | applied | whole class (src/sim/galactic/production.cpp) | WGEP-48, WGEP-62, WGEP-68, WGEP-69 | — | src/sim/galactic/production.cpp | GC-only census application: original-type population contributes through population_totals (WGEP-48/62/68/69). Outside the tactical scene.; basis: reviewed |
| Prev_Level_Base | todo | application not recorded | — | 1458 | — | previous base level read by space AI only; basis: reviewed |
| Primary_Locomotor_Name | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Projectile_Appearance_Delay_Frames | todo | application not recorded | — | 3776 | — | Read only for space hardpoints.; basis: reviewed |
| Projectile_Damage | partial | land battle objects (src/units/unit_tables_profiles.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_profiles.cpp | Object damage read for land weapons (shoot PR1); no damage application on land yet.; basis: reviewed |
| Radar_Icon_Scale_Land | todo | application not recorded | — | 3776 | — | no reader found; only Radar_Icon_Scale_Space is read; basis: reviewed |
| Radar_Icon_Scale_Space | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Radar_Range_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: carried only by ground objects (Has_Land_Evaluator Marker/SecondaryStructure, Ground_* special structures and buildables, UMP drop point); no space-skirmish type authors it (retail effective XML scan, 10-25) |
| Radar_Rotate_Icon | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Radar_Show_Facing | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Ranking_In_Category | todo | application not recorded | — | 3776 | — | ranking read only by space and galactic audio; basis: reviewed |
| Reinforcement_Prevention_Radius | todo | application not recorded | — | 3776 | — | Land prevention query missing (LC-45); space applies the radius; basis: reviewed |
| Required_Ground_Base_Level | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:298 copies type.required_ground_base_level into admission.required_ground_level; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Required_Planets | applied | whole class (src/campaign/live_setup.cpp) | none recorded | — | src/campaign/live_setup.cpp | live_setup.cpp:147 refuses direct build unless a required planet is held; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Required_Special_Structures | applied | whole class (src/campaign/live_setup.cpp) | none recorded | — | src/campaign/live_setup.cpp | live_setup.cpp:135 iterates words(type.required_special_structures) as the admission requirement; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Required_Star_Base_Level | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:299 copies type.required_star_base_level into admission.required_space_level; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Required_Timeline | todo | application not recorded | — | 3776 | — | no reader on origin/main; basis: reviewed |
| Reserve_Spawned_Units_Tech_0 | todo | application not recorded | — | 3776 | — | Space fighter-bay field exists (fighters.hpp:80) but M2 ignores it (unit_motion.cpp:323); no land use; basis: reviewed |
| Retreat_Self_Destruct_Explosion | todo | application not recorded | — | 3776 | — | Decoded after the land early return (space path only).; basis: reviewed |
| Selection_Blob_Material_Name | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Ambient_Loop | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Assist_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Assist_Move | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Attack_Hardpoint | todo | application not recorded | — | 3776 | — | mechanic row; no land reader on main; basis: reviewed |
| SFXEvent_Bombard_Incoming | todo | application not recorded | — | 3776 | — | No reader on main; basis: reviewed |
| SFXEvent_Bombard_Select_Target | todo | application not recorded | — | 3776 | — | No reader on main; basis: reviewed |
| SFXEvent_Build_Cancelled | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:108); no land reader; basis: reviewed |
| SFXEvent_Build_Complete | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:107); no land reader; basis: reviewed |
| SFXEvent_Build_Started | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:106); no land reader; basis: reviewed |
| SFXEvent_Damaged_By_Asteroid | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| SFXEvent_Draw_Weapon | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| SFXEvent_Engine_Cinematic_Focus_Loop | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Fleet_Move | todo | application not recorded | — | 653 | — | Fleet-move audio plays from the moving fleet member types (apps/viewer/src/galactic/galactic_audio.cpp&#35;GalacticAudio::fleet_move); this carrier class is not a fleet member, so no consumer for it is verified.; basis: reviewed |
| SFXEvent_Group_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Group_Move | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Hardpoint_Destroyed | todo | application not recorded | — | 3776 | — | mechanic row; no land reader on main; basis: reviewed |
| SFXEvent_Health_Critical_Warning | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp health warnings are space-battle only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Health_Low_Warning | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:414 health warnings are space-battle only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Hero_Respawned | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| SFXEvent_Holster_Weapon | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| SFXEvent_Move_Into_Asteroid_Field | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| SFXEvent_Move_Into_Nebula | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| SFXEvent_Tactical_Build_Cancelled | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; basis: reviewed |
| SFXEvent_Tactical_Build_Complete | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; basis: reviewed |
| SFXEvent_Tactical_Build_Started | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; basis: reviewed |
| SFXEvent_Turret_Rotating_Loop | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| SFXEvent_Unit_Lost | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Unit_Under_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Shield_Armor_Type | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read with the Shield_Default fallback; no consumer in step_land yet.; basis: reviewed |
| Ship_Class | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 3776 | — | space-only twin of Land_FOW_Reveal_Range; no land reader, no retail citation found for foc-ignores; basis: reviewed |
| Space_Full_Stop_Command | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| SpaceBehavior | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |
| Spawn_Planet | todo | application not recorded | — | 1116 | — | The hero spawn planet is not read; respawn placement uses its own choice.; basis: reviewed |
| Spawned_Indigenous_Pack_Type | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Squadron_Delay_Seconds | todo | application not recorded | — | 3776 | — | Applied only to space station launches (WSL-18); no land spawner reader; basis: reviewed |
| Specific_Death_Anim_Type | partial | land battle objects (apps/viewer/src/live_session_death.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_death.cpp | Read from the clone in the land-capable death prep (UA-08); no land deaths yet.; basis: reviewed |
| Squadron_Capacity | applied | whole class (src/campaign/fleets.cpp) | none recorded | — | src/campaign/fleets.cpp | fleets.cpp:158-161 validates original-&gt;squadron_capacity and carries it into the fleet type (t.squadron_capacity); no behaviour-note rule mentions this tag yet; basis: reviewed |
| Starting_Spawned_Units_Tech_0 | todo | application not recorded | — | 3776 | — | Applied only to space stations; not reached for this class in the M2 scene; basis: reviewed |
| Tactical_Bribe_Cost | todo | application not recorded | — | 3244 | — | LR11-30 covers the land tactical bribe cost; no reader in src on origin/main; basis: reviewed |
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 1458 | — | space economy only; no land reader; basis: reviewed |
| Tactical_Build_Prerequisites | todo | application not recorded | — | 1458 | — | production_allowed is space-only; no land reader; basis: reviewed |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 1458 | — | build_time_seconds read by space economy only; basis: reviewed |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 1458 | — | space and campaign only; no land reader; basis: reviewed |
| Tactical_Production_Queue | todo | application not recorded | — | 1458 | — | production queue read by space economy only; no land reader; basis: reviewed |
| Targeting_Priority_Set | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read and its priority set queued for loading; no targeting consumer on land yet.; basis: reviewed |
| Targeting_Stickiness_Time_Threshold | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | Read into UnitType; no target selection on land yet.; basis: reviewed |
| Tech_Level | applied | whole class (src/campaign/setup.cpp) | none recorded | — | src/campaign/setup.cpp | setup.cpp:295 copies type.tech_level into admission.tech; no behaviour-note rule mentions this tag yet; basis: reviewed |
| Tread_Scroll_Rate | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Turret_Bone_Name | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |
| Turret_Rest_Angle | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |
| Turret_Rotate_Speed | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Type | todo | application not recorded | — | 3776 | — | ability_kind() used by viewer audio (battle_audio_prepare.cpp:485); no land apply found; basis: reviewed |
| User_Bound_Max | todo | application not recorded | — | 3776 | — | camera bound of cinematic prop; no code reads the tag; basis: reviewed |
| User_Bound_Min | todo | application not recorded | — | 3776 | — | camera bound of cinematic prop; no code reads the tag; basis: reviewed |
| Visible_On_Radar_When_Fogged | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |

## Props_Swamp, Props_Volcanic

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Editor_Placed | todo | application not recorded | — | 3230 | — | no code reads the tag on main; basis: reviewed |

## RadarMapEvents

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Map_Event/@name | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Duration | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Model_Name | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Model_Scale | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |
| Radar_Map_Event/Event_Persistent | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include. Reader walk pending (10-25): FoC debug-build tag-table row located (parser id 0x4b1 (only Interdict_Activated in stock XML, no XML/Lua reference)); consumer not traced.; basis: reviewed |
| Radar_Map_Event/Event_Single_Instance | partial | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp); missing: other radar event types | OF-04 | 653 | src/presentation/godot/ui/minimap_view.cpp | Click feedback only; other radar event consumers remain deferred.; basis: reviewed |

## RadarMapSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-14 | — | src/presentation/ui/minimap.cpp | the entry named space fills the background layer; basis: reviewed |
| Color/@name | applied | space minimap surface (src/presentation/ui/minimap.cpp) | MM-14 | — | src/presentation/ui/minimap.cpp | Code review: applied; family=hud-radar. The Color name attribute selects the space surface; load_settings assigns its colour to settings.background and MinimapView draws that background rectangle.; basis: reviewed |
| Passability_Color_Settings/@name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. load_settings reads top-level space radar colours only; no Passability_Color_Settings name lookup or passability-colour overlay consumes this attribute.; basis: reviewed |
| Passability_Color_Settings/Color | todo | application not recorded | — | 653 | — | nothing reads the passability colours; the minimap draws its own sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Passability_Color_Settings/Color/@name | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. load_settings reads top-level space radar colours only; no Passability_Color_Settings name lookup or passability-colour overlay consumes this attribute.; basis: reviewed |
| Space_Asteroid_Field_Border_Color | partial | space tactical instances (src/presentation/ui/minimap.cpp); missing: land minimaps | WHZ-72 | 653 | src/presentation/ui/minimap.cpp | basis: reviewed |
| Space_Asteroid_Field_Color | partial | space tactical instances (src/presentation/ui/minimap.cpp); missing: land minimaps | WHZ-72 | 653 | src/presentation/ui/minimap.cpp | basis: reviewed |
| Space_Backdrop_Texture_Name | applied | whole class (src/presentation/godot/ui/minimap_view.cpp) | MM-01 | — | src/presentation/godot/ui/minimap_view.cpp | basis: auto |
| Space_FOW_Color | applied | whole class (src/presentation/ui/minimap.cpp) | MM-01 | — | src/presentation/ui/minimap.cpp | basis: auto |
| Space_Is_Guide_Rectangle | applied | whole class (src/presentation/ui/minimap.cpp) | MM-01 | — | src/presentation/ui/minimap.cpp | basis: auto |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_FOW_Reveal_Range | applied | whole class (src/skirmish/start.cpp) | WBP-01 | — | src/skirmish/start.cpp | basis: reviewed |

## SecondaryStructure, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| MP_Encyclopedia_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | P-2 | — | src/presentation/ui/hud_shell.cpp | basis: reviewed |

## SecondaryStructure, SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Unit_Under_Attack | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Code review: confirmed gap; family=audio. No named reader of SFXEvent_Unit_Under_Attack in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |

## SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Clone_Is_Obstacle | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| SFXEvent_Unit_Lost | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+3 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small (+61 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+3 more). Code review: confirmed gap; family=audio. No named reader of SFXEvent_Unit_Lost in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |

## SecondaryStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Should_Death_Clone_Play_Idle | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Should_Death_Clone_Play_Idle in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |

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
| Loop_Fade_In_Seconds | partial | ordinary space engine loops (apps/viewer/src/battle_audio_events.cpp); missing: general attached SFX lifecycle | BA-83 | 4532 | apps/viewer/src/battle_audio_events.cpp | Engine-loop starts/stops consume authored fades; general event fades remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Loop_Fade_Out_Seconds | partial | ordinary space engine loops (apps/viewer/src/battle_audio_events.cpp); missing: general attached SFX lifecycle | BA-83, SND-45 | 4532 | apps/viewer/src/battle_audio_events.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SFXEvent, SpeechEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Text_ID | applied | whole class (src/data/ui/dialog_catalog.cpp) | none recorded | — | src/data/ui/dialog_catalog.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Blob_Color | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Abilities/Force_Healing_Ability/Lighting_Effect_Name | partial | automatic space repair satellites (apps/viewer/src/battle_effects_impacts.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | partial | automatic space repair satellites (apps/viewer/src/battle_effects_impacts.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | partial | automatic space repair satellites (apps/viewer/src/battle_audio_events.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/battle_audio_events.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Code review: confirmed gap; family=vfx-models. Loader::load_special_healing reads only the first owner/target light colour; the second authored colour has no decoder or light-effect consumer.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | partial | automatic space repair satellites (apps/viewer/src/live_session_frame.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/live_session_frame.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | partial | automatic space repair satellites (apps/viewer/src/battle_effects_impacts.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | partial | automatic space repair satellites (apps/viewer/src/battle_effects_impacts.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-08 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Blob_Color | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Blob_Material_Name | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Encyclopedia_Text | applied | admitted galactic production types (apps/viewer/src/galactic/scene_content.cpp); whole class (src/presentation/ui/hud_shell.cpp) | WGUI-18, WGUI-81; P-1, P-2 | — | apps/viewer/src/galactic/scene_content.cpp; src/presentation/ui/hud_shell.cpp | basis: reviewed |
| GUI_Row | applied | whole class (apps/viewer/src/battle_input_cards.cpp) | WBP-36 | — | apps/viewer/src/battle_input_cards.cpp | basis: reviewed |
| Hides_When_Built_On | applied | whole class (apps/viewer/src/live_session_economy.cpp) | WBP-35 | — | apps/viewer/src/live_session_economy.cpp | basis: reviewed |
| SFXEvent_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | WBP-20 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Turret_Bone_Name | partial | space objects declaring TURRET (src/units/unit_combat.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17 | 4532 | src/units/unit_combat.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Visible_To_Enemies_When_Empty | applied | whole class (apps/viewer/src/live_session_economy.cpp) | WBP-35 | — | apps/viewer/src/live_session_economy.cpp | basis: reviewed |

## SpaceBuildable, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_special_ability stores the name, but jammer registration uses the outer ability slot/radius rather than matching this nested name; automatic satellite binding remains missing.; basis: reviewed |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Tractor_Beam_Attack_Ability/@Name | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop | partial | Kedalbe_Battleship (apps/viewer/src/battle_audio_events.cpp); missing: Loop cues on other authored ability kinds remain outside the shield-leech adapter. | WLS-05 | 760 | apps/viewer/src/battle_audio_events.cpp | Mixed ground/space, space carriers on HeroUnit: IG-88. |

## SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_healing reads only the first owner/target light colour; the second authored colour has no decoder or light-effect consumer.; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Loop_Idle_Anim_00 | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: read for the galactic scene and idle tags (idle_tags.cpp); not traced for these skydome classes.; basis: reviewed |
| Text_ID | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. Skydome model selection reads model names only; primary and secondary sky Text_ID values have no displayed or localized label consumer.; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. Generic placed-object scaling exists in src/scene/scene_build.cpp, but the sky planner does not load type scale and live SpecialEffect creation has no scale consumer.; basis: reviewed |
| Space_Model_Name | partial | SpacePrimarySkydome, SpaceSecondarySkydome (apps/viewer/src/space_environment_view.cpp); missing: standalone SpecialEffect lifecycle outside explicit referenced particles | none recorded | 653 | apps/viewer/src/space_environment_view.cpp | Code review: partial application; family=vfx-models. Primary and secondary sky references pass through space::select_model and compose_object into rendered geometry; standalone SpecialEffect lifecycle remains missing. No behaviour rule specifically names this XML model tag.; basis: reviewed |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Debris_Attached_Particle | applied | whole class (apps/viewer/src/debris_props.cpp) | BP-33 | — | apps/viewer/src/debris_props.cpp | basis: auto |
| Debris_Facing_Rotate_Vector | applied | whole class (apps/viewer/src/land_look.cpp) | BP-31, R-ROT-01 | — | apps/viewer/src/land_look.cpp | basis: auto |
| Debris_Max_Lifetime_Seconds | applied | whole class (src/presentation/space/debris.cpp) | BP-32 | — | src/presentation/space/debris.cpp | basis: auto |
| Debris_Min_Lifetime_Seconds | applied | whole class (src/presentation/space/debris.cpp) | BP-32 | — | src/presentation/space/debris.cpp | basis: auto |
| Debris_Movement_Vector | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | BP-31, R-ROT-01 | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |
| Idle_Anim_00_Rate_Mod | applied | whole class (apps/viewer/src/idle_clips.hpp) | none recorded | — | apps/viewer/src/idle_clips.hpp | basis: auto |
| Layer_Z_Adjust | applied | whole class (src/scene/scene_build.cpp); source-bearing map placements (src/skirmish/start.cpp) | LZ-01; WSW-01 | — | src/scene/scene_build.cpp; src/skirmish/start.cpp | SpaceProps take their height in presentation; a source-bearing backdrop also supplies the height-adjusted planetary firing transform (WSW-01).; basis: reviewed |
| Scale_Factor | applied | whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp); whole class (src/units/unit_motion.cpp) | L-32; L-32; AV-05, R-ROT-01 | — | src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp; src/units/unit_motion.cpp | the map-object footprint loader reads it under an unrecorded trace; basis: reviewed; trace: unrecorded |

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
| Abilities/Cluster_Bomb_Ability/@Name | applied | MC30_Frigate CLUSTER_BOMB (src/sim/tactical/session_cmd_abilities.cpp) | WCB-01, WCB-02, WCB-03 | — | src/sim/tactical/session_cmd_abilities.cpp | Named self-targeted ring launcher binds count, type and countdown; object-script autofire cadence remains separate.; basis: reviewed |
| Abilities/Cluster_Bomb_Ability/SFXEvent_Activate | applied | MC30_Frigate CLUSTER_BOMB (apps/viewer/src/battle_audio_events.cpp) | WCB-02 | — | apps/viewer/src/battle_audio_events.cpp | WCB-02: successful ring spawn plays the named handler launch cue through existing ability-spawn audio.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Bone_Names | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Frames | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Texture | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Width | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Color | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | WLD-12 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Lightning_Effect_Name | presentation-later | application not recorded | — | — | — | WLD-12: the particle is authored but its placement is unread; the beam draws without it |
| Abilities/Laser_Defense_Ability/Zap_SFXEvent | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WLD-12 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Abilities/Leech_Shields_Ability/@Name | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp) | WLS-01 | — | src/sim/tactical/session_abilities.cpp | — |
| Abilities/Leech_Shields_Ability/Beam_Bone_Name | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Leech_Shields_Ability/Beam_Color | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Leech_Shields_Ability/Beam_Effect_Name | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | TBF-06 | — | apps/viewer/src/battle_effects_prepare.cpp | Resolves the authored shield-leech lightning definition.; basis: reviewed |
| Abilities/Leech_Shields_Ability/Beam_Texture_Name | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Leech_Shields_Ability/Leech_Effect | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | WRB-12 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Abilities/Super_Laser_Ability/Attack_Anim_Sub_Index | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Radar_Clip_To_Visible_Region | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Radar_Clip_To_Visible_Region in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation. Reader walk pending (10-25): FoC debug-build tag-table row located (parser id 0x11f (only Death_Star in stock XML)); consumer not traced.; basis: reviewed |
| Selt_Destruct_SFXEvent_Start_Die | applied | SELF_DESTRUCT countdown start sound (apps/viewer/src/battle_audio_prepare.cpp) | WSD-11 | — | apps/viewer/src/battle_audio_prepare.cpp | — |
| SFXEvent_Attack_With_Non_Hero_Ability | applied | Broadside_Class_Cruiser (apps/viewer/src/battle_audio_events.cpp) | SND-77 | — | apps/viewer/src/battle_audio_events.cpp | SND-77: while the speaking unit has an ability switched on, its attack acknowledgement uses this variant (Select/Move variants read the same way); unauthored falls back.; basis: reviewed |
| SFXEvent_Move_Opposite | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Move_Opposite in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| TSW_Attack_Anim_Duration_Seconds | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of TSW_Attack_Anim_Duration_Seconds in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| TSW_Explosion_Debris_Creation_Frame_Delay | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of TSW_Explosion_Debris_Creation_Frame_Delay in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| TSW_MusicEvent_Activation_Start | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| TSW_Post_Music_Wait_Frames | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of TSW_Post_Music_Wait_Frames in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| TSW_SFXEvent_Activate_Voice | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| TSW_SFXEvent_GUI_Bttton_Press | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| TSW_SFXEvent_Weapon_Power_Up | deferred | application not recorded | — | 4532 | — | BA-56: no playable M2 space tactical-superweapon stage; approach warnings are speech, power-up cues are unattached SFX.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |

## SpaceUnit, SpecialStructure, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |

## SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Cinematic_Focus_Loop | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion. Code review: confirmed gap; family=audio. No named reader of SFXEvent_Engine_Cinematic_Focus_Loop in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |

## SpaceUnit, Squadron, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Build_Cancelled | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); galactic production types (apps/viewer/src/galactic/galactic_audio.cpp) | BA-61; WGEP-96, WGEP-97, WGP-183, WGP-63 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |
| SFXEvent_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); galactic production types (apps/viewer/src/galactic/galactic_audio.cpp) | BA-60; WGEP-96, WGEP-97, WGP-183, WGP-63 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |
| SFXEvent_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); galactic production types (apps/viewer/src/galactic/galactic_audio.cpp) | BA-60; WGEP-96, WGEP-97, WGP-183, WGP-63 | — | apps/viewer/src/battle_audio_prepare.cpp; apps/viewer/src/galactic/galactic_audio.cpp | basis: reviewed |

## SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/Owner_Particle_Effect | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=vfx-models. Loader::load_special_beam reads owner particles only for beam handlers; sensor-jamming does not decode or spawn this owner particle.; basis: reviewed |
| Abilities/Stealth_Ability/@Name | applied | whole class (src/units/unit_abilities.cpp) | WST-01, WST-02 | — | src/units/unit_abilities.cpp | basis: reviewed |
| Abilities/Stealth_Ability/SFXEvent_Activate | deferred | application not recorded | — | 4532 | — | BA-55: authored space payload has no implemented M2 action consumer; no inferred activation cue.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| Abilities/Stealth_Ability/Stealth_Color | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (src/presentation/live/unit_fade.cpp) | WST-12; WST-12 | — | apps/viewer/src/live_session_frame.cpp; src/presentation/live/unit_fade.cpp | basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Transition_Time | applied | whole class (apps/viewer/src/live_session_frame.cpp); whole class (src/presentation/live/unit_fade.cpp) | WST-12; WST-12 | — | apps/viewer/src/live_session_frame.cpp; src/presentation/live/unit_fade.cpp | basis: reviewed |
| LOD_Bias | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Silri, Silri_No_Abilities, Tyber_Zann (+7 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+5 more). Mixed ground/space, space carriers on UniqueUnit: Biggs_XWing, Hobbie_XWing, Luke_Jedi_XWing, Luke_XWing, Porkins_XWing, Red_Leader_XWing, Rogue_10_XWing, Rogue_11_XWing (+5 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| SFXEvent_Tactical_Build_Cancelled | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-61 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Tactical_Build_Complete | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| SFXEvent_Tactical_Build_Started | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-60 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Multisample_FOW_Check | applied | craft, ship, squadron, station (src/sim/tactical/session_state.cpp) | V-21 | — | src/sim/tactical/session_state.cpp | basis: reviewed |
| SFXEvent_Attack_Hardpoint | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-25 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Hardpoint_Destroyed | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-64 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |

## SpaceUnit, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Asteroid_Damage_Hit_Particles | applied | space tactical instances (apps/viewer/src/battle_effects_impacts.cpp) | WHZ-14 | — | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |

## SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. special_ability_kind has no Super_Laser_Ability dispatch; the authored name, animation frame count and owner particle/bone parameters do not reach a nested super-laser consumer.; basis: reviewed |
| Abilities/Super_Laser_Ability/Beam_Texture_Name | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: loaded into the leech-ability table (unit_tables_abilities_special.cpp) but the Super_Laser beam is not drawn with it.; basis: reviewed |
| Abilities/Super_Laser_Ability/Line_Color | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: no reader under src/apps/include.; basis: reviewed |
| Abilities/Super_Laser_Ability/Num_Anim_Frames | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=vfx-models. special_ability_kind has no Super_Laser_Ability dispatch; the authored name, animation frame count and owner particle/bone parameters do not reach a nested super-laser consumer.; basis: reviewed |
| Abilities/Super_Laser_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=vfx-models. special_ability_kind has no Super_Laser_Ability dispatch; the authored name, animation frame count and owner particle/bone parameters do not reach a nested super-laser consumer.; basis: reviewed |
| Glory_Cinematics | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Radar_Register_As_Foreground_Object | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Radar_Register_As_Foreground_Object in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation. Reader walk pending (10-25): FoC debug-build tag-table row located (parser id 0x11e); consumer not traced.; basis: reviewed |
| Ranking_In_Category | applied | space craft, space ship (apps/viewer/src/battle_audio_events.cpp); galactic fleet members (apps/viewer/src/galactic/galactic_audio.cpp); locally owned named galactic heroes (apps/viewer/src/galactic/hero_map_bar.hpp) | BA-20; WGP-181; WGUI-89 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/galactic/galactic_audio.cpp; apps/viewer/src/galactic/hero_map_bar.hpp | The effective type ranking selects the speaking space ship or craft after category admission and orders locally owned named heroes in the galaxy map bar.; basis: reviewed |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | partial | live space craft and ships with moving cues (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception | BA-80, SND-46 | 4532 | apps/viewer/src/battle_audio_events.cpp | Positive ordered integer seconds become logical frames for initialization and every due service.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | partial | live space craft and ships with moving cues (apps/viewer/src/battle_audio_events.cpp); missing: ground units, no-locomotor buzz-droid exception | BA-80, SND-46 | 4532 | apps/viewer/src/battle_audio_events.cpp | Positive ordered integer seconds become logical frames for initialization and every due service.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Assist_Attack | partial | live space selected object types (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | BA-29, SND-22 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Assist_Move | partial | live space selected object types (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | BA-29, SND-22 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Bombard_Incoming | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Incoming in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Bombard_Select_Target | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Bombard_Select_Target in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Damaged_By_Asteroid | applied | space tactical instances (apps/viewer/src/battle_audio_events.cpp) | WHZ-14 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Enemy_Damaged_Health_Critical_Warning | partial | durable standalone and campaign-hosted tactical space objects (apps/viewer/src/battle_audio_prepare.cpp); durable standalone and campaign-hosted tactical space objects (include/eawr/presentation/audio/health_warning.hpp); missing: durable squadron-container damage (&#35;4887), land consumers, ordinary obstacle-source health-cue binding (&#35;4909/&#35;4902) | SND-33, WCC-54; SND-33 | 653 | apps/viewer/src/battle_audio_prepare.cpp; include/eawr/presentation/audio/health_warning.hpp | Standalone and campaign-hosted durable tactical space objects use per-delivery hull warnings through the retained battle journal. Land and durable squadron-container damage (&#35;4887) remain outside this consumer. Environmental receipts retain the current field identity (WHZ-14), but ordinary fields live in UnitTables.obstacles while BattleAudio::prepare binds health cues only from tables.units. The field type lookup therefore has no cue entry: locally owned mod obstacles cannot request their authored enemy warning until obstacle preparation is added (&#35;4909/&#35;4902). Identity propagation is preparatory; current field tests supply cue pointers manually and do not cover the missing binding. A removed projectile shooter intentionally supplies no enemy cue because HW-E01 requires the current source object/type; retained projectile.owner serves WHE-51 difficulty and kill attribution. Target-local own warnings retain precedence.; basis: reviewed |
| SFXEvent_Enemy_Damaged_Health_Low_Warning | partial | durable standalone and campaign-hosted tactical space objects (apps/viewer/src/battle_audio_prepare.cpp); durable standalone and campaign-hosted tactical space objects (include/eawr/presentation/audio/health_warning.hpp); missing: durable squadron-container damage (&#35;4887), land consumers, ordinary obstacle-source health-cue binding (&#35;4909/&#35;4902) | SND-33, WCC-54; SND-33 | 653 | apps/viewer/src/battle_audio_prepare.cpp; include/eawr/presentation/audio/health_warning.hpp | Standalone and campaign-hosted durable tactical space objects use per-delivery hull warnings through the retained battle journal. Land and durable squadron-container damage (&#35;4887) remain outside this consumer. Environmental receipts retain the current field identity (WHZ-14), but ordinary fields live in UnitTables.obstacles while BattleAudio::prepare binds health cues only from tables.units. The field type lookup therefore has no cue entry: locally owned mod obstacles cannot request their authored enemy warning until obstacle preparation is added (&#35;4909/&#35;4902). Identity propagation is preparatory; current field tests supply cue pointers manually and do not cover the missing binding. A removed projectile shooter intentionally supplies no enemy cue because HW-E01 requires the current source object/type; retained projectile.owner serves WHE-51 difficulty and kill attribution. Target-local own warnings retain precedence.; basis: reviewed |
| SFXEvent_Guard | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-26 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| SFXEvent_Move_Into_Asteroid_Field | partial | live space ships and leading squadron craft (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | SND-24, SND-25 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Move_Into_Nebula | partial | live space ships and leading squadron craft (apps/viewer/src/battle_audio_events.cpp); missing: land and galactic response consumers | SND-24, SND-25 | 4532 | apps/viewer/src/battle_audio_events.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_Stop | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-26 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Spin_Away_On_Death_Explosion | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | SP-03 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Spin_Away_On_Death_SFXEvent_Start_Die | applied | craft (apps/viewer/src/battle_audio_prepare.cpp) | BA-16, SP-03 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Target_Bones | applied | whole class (src/sim/tactical/combat.cpp); unit weapon aim draw (src/sim/tactical/combat_fire.cpp) | WWP-50; WWP-50 | — | src/sim/tactical/combat.cpp; src/sim/tactical/combat_fire.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Unit_Abilities_Data/Unit_Ability/Particle_Effect | partial | REPLENISH_WINGMEN (apps/viewer/src/battle_effects_impacts.cpp); CONCENTRATE_FIRE (apps/viewer/src/battle_effects_impacts.cpp); LURE (apps/viewer/src/battle_effects_impacts.cpp); missing: other ability particles | WHE-63; WHE-78; WLU-09 | 4532 | apps/viewer/src/battle_effects_impacts.cpp | G9 WHE-63 emits the authored replenish particle on every current team member after successful refill; other ability emitters remain separate. WHE-78 emits the authored concentrate-fire reticle once at the tracked target and drains it through the shared particle lifecycle; the active-duration HUD dial is independent.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Base_Power_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_nested_abilities records the attribute in inactive_abilities, but the nested-handler dispatch has no Base_Power_Ability implementation consuming this name.; basis: reviewed |
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=abilities. Loader::load_nested_abilities stores the name, but service_healing dispatches by kind and activation style; SpecialStructure names have no tactical binding consumer.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=vfx-models. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Code review: confirmed gap; family=audio. Loader::load_special_ability accepts only the battle-domain automatic healing style; SpecialStructure healing outside that style never reaches the light, bone or audio pulse consumers.; basis: reviewed |
| Gravity_Control_Field_Effect | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;weapons-shields&#93;: no reader under src/apps/include.; basis: reviewed |
| HQ_Win_Condition_Relevant | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;other&#93;: no reader under src/apps/include.; basis: reviewed |
| Lobbing_Superweapon_Chargeup_Particle | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;weapons-shields&#93;: no reader under src/apps/include.; basis: reviewed |
| Lobbing_Superweapon_Chargeup_Particle_Bone_Name | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;weapons-shields&#93;: no reader under src/apps/include.; basis: reviewed |
| Lobbing_Superweapon_SFXEvent_Chargeup | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Lobbing_Superweapon_SFXEvent_Fire | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Movie_Object | presentation-later | application not recorded | — | — | — | SCOPE-MENU |
| Scale_Factor | applied | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp); whole class (src/units/unit_combat.cpp) | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; L-32; L-32; AV-05, R-ROT-01 | — | include/eawr/skirmish/land_content.hpp; src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp; src/units/unit_combat.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; basis: reviewed |
| SFXEvent_Powered_Active_Loop | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Powered_Active_Loop in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Turret_Rotating_Loop | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Turret_Rotating_Loop in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| Shield_Normal_Color | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Shield_Normal_Color in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Shield_Off_Anim | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Shield_Off_Anim in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Shield_On_Anim | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Shield_On_Anim in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |

## SpecialStructure, StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Radar_Ability/@Name | partial | space skirmish stations (src/sim/tactical/session_abilities.cpp); missing: ground structure radar | WHD-05, WRAD-01, WRAD-02, WRAD-04 | 760 | src/sim/tactical/session_abilities.cpp | Station radar is applied through named owner/type locks; world fog is unchanged.; basis: reviewed |
| Abilities/Enable_Radar_Ability/Activation_Style | partial | space skirmish stations (src/sim/tactical/session_abilities.cpp); missing: ground structure radar | WHD-05, WRAD-01, WRAD-02, WRAD-04 | 760 | src/sim/tactical/session_abilities.cpp | Station radar is applied through named owner/type locks; world fog is unchanged.; basis: reviewed |
| Abilities/Enable_Radar_Ability/Affects_All_Allies | partial | space skirmish stations (src/sim/tactical/session_abilities.cpp); missing: ground structure radar | WHD-05, WRAD-01, WRAD-02, WRAD-04 | 760 | src/sim/tactical/session_abilities.cpp | Station radar is applied through named owner/type locks; world fog is unchanged.; basis: reviewed |

## SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Material_Name | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 653 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). Mixed ground/space, space carriers on UniqueUnit: Houndstooth, Houndstooth_Ground_Prop, Houndstooth_Landing, IG-2000, IG2000_Ground_Prop, IG2000_Landing, Luke_Jedi_XWing, Luke_XWing (+17 more). sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |

## SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/@Name | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Idle_Chase_Range | applied | squadron (src/units/unit_motion.cpp) | FT-02, OR-13 | — | src/units/unit_motion.cpp | basis: auto |
| Is_Named_Hero | partial | loaded local space battle (apps/viewer/src/live_session_frame.cpp); visible standalone space heroes (apps/viewer/src/world_ui_groups.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); space purchase catalog (src/skirmish/economy.cpp); space purchase catalog (src/skirmish/inputs.cpp); missing: objects and modes outside the loaded local space battle, squadron-authored presentation admission | WBF-45; WU-47, WU-48; WAR-24; WSS-29; WSS-29 | 653 | apps/viewer/src/live_session_frame.cpp; apps/viewer/src/world_ui_groups.cpp; src/campaign/auto_resolve_strength.cpp; src/skirmish/economy.cpp; src/skirmish/inputs.cpp | Squadron presentation currently reads admission from its spawned team container; the squadron-authored hero flag has no presentation consumer. Results/scoring consumer covers the loaded local space closure; other gameplay and mode consumers are separate. Resolved inherited space hero policy filters the shared human/AI purchase catalog independently of presentation and results scoring. World identities cover visible standalone space heroes; carried heads remain separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 653 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Code review: confirmed gap; family=hud-radar. units::sensor_range uses the spawned team container range for squadrons, not the Squadron-authored value; bind or explicitly qualify this separate authored field.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |

## Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Hero_Respawned | todo | application not recorded | — | 1116 | — | The hero respawn cue is not played for a campaign hero respawn.; basis: reviewed |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Radar_Ability/Initially_Enabled | applied | space skirmish stations (src/sim/tactical/session_abilities.cpp) | WHD-05, WRAD-01, WRAD-02, WRAD-04 | — | src/sim/tactical/session_abilities.cpp | Station radar is applied through named owner/type locks; world fog is unchanged.; basis: reviewed |
| Galactic_Model_Name | applied | station (apps/viewer/src/galactic/orbit_hud.cpp) | none recorded | — | apps/viewer/src/galactic/orbit_hud.cpp | Focused observer-admitted station display uses the current starbase type's authored model name; it has no fixed level-to-model table. Station display follows HUD recipe GHUD-S78 in docs/behaviour/gc-hud-inventory.md; this inventory recipe is not a numeric-rule ID.; basis: reviewed |
| GUI_Angles | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| GUI_X_Rot | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;hud-radar&#93;: no reader under src/apps/include.; basis: reviewed |
| Is_Community_Property | applied | whole class (apps/viewer/src/battle_input.cpp) | WSU-16 | — | apps/viewer/src/battle_input.cpp | basis: reviewed |
| Radar_Rotate_Icon | applied | space station (src/presentation/godot/ui/minimap_view.cpp) | MM-06 | — | src/presentation/godot/ui/minimap_view.cpp | The minimap carries the station flag into the blip and turns its texture a quarter turn while swapping its half extents. The quarter-turn direction remains unverified.; basis: reviewed |
| SFXEvent_Hardpoint_All_Weapons_Destroyed | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-64 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Visible_To_Enemies_When_Empty | applied | whole class (apps/viewer/src/live_session_economy.cpp) | WBP-35 | — | apps/viewer/src/live_session_economy.cpp | basis: reviewed |

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
| SurfaceFX/@name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/@name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Attach_To_SurfaceFX_Mesh | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Damage_Mod | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Decal_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Defense_Mod | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Dynamic_Track_Left_Bone | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Dynamic_Track_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Dynamic_Track_Right_Bone | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Fire_Range_Mod | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/FOW_Reveal_Range_Mod | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SurfaceFX/Surface_Settings/Particle_Min_Energy | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Particle_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/SoundFX_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Speed_Mod | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Status_Icon_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Surface_Settings/@name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Surface_Settings/Decal_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Surface_Settings/SoundFX_Name | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Terrain_Damage | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |
| SurfaceFX/Surface_Settings/Terrain_Damage_Delay | todo | application not recorded | — | 3228 | — | SurfaceEffects settings (LC-16 family); no loader reads them; basis: reviewed |

## TacticalCamera

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Distance_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Object_Fade_Begin | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Object_Fade_Begin in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Object_Fade_End | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Object_Fade_End in src/, apps/ or include/; wire the authored value into the class-specific effect or animation presentation.; basis: reviewed |
| Pitch_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | PI-9, RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Per_Zoom_Unit | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_When_Zoomed_In | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Pitch_Zoom_Begin_Fraction | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Yaw_Per_Mouse_Unit | applied | whole class (src/presentation/camera/camera.cpp) | RO-7 | — | src/presentation/camera/camera.cpp | basis: auto |

## TradeRoute

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Visible_Line_Name | applied | whole class (apps/viewer/src/galactic/galactic_routes.cpp) | WGP-15 | — | apps/viewer/src/galactic/galactic_routes.cpp | basis: reviewed |

## TradeRouteLine

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Settings_For_Faction/@Name | applied | whole class (apps/viewer/src/galactic/route_lines.cpp) | WGP-10 | — | apps/viewer/src/galactic/route_lines.cpp | basis: reviewed |
| Settings_For_Faction/Color_Zoomed_In | applied | whole class (apps/viewer/src/galactic/route_lines.cpp) | WGP-10, WGP-12, WGP-13 | — | apps/viewer/src/galactic/route_lines.cpp | basis: reviewed |
| Settings_For_Faction/Color_Zoomed_Out | applied | whole class (apps/viewer/src/galactic/route_lines.cpp) | WGP-10, WGP-12, WGP-13 | — | apps/viewer/src/galactic/route_lines.cpp | basis: reviewed |
| Settings_For_Faction/Render_Mode | applied | whole class (apps/viewer/src/galactic/route_lines.cpp) | WGP-10, WGP-12 | — | apps/viewer/src/galactic/route_lines.cpp | basis: reviewed |
| Settings_For_Faction/Width | applied | whole class (apps/viewer/src/galactic/route_lines.cpp) | WGP-10, WGP-12 | — | apps/viewer/src/galactic/route_lines.cpp | basis: reviewed |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Dense_FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 653 | — | Hero and transport definitions are outside the traced M2 fog roster; their authored dense multipliers still need execution validation. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. Mixed ground/space, space carriers on Projectile: Proj_Special_Space_FOW_Reveal_Ping_Blast. sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: loaded (unit_tables_profiles.cpp); the M2 company resolver does not apply it to these scene objects.; basis: reviewed |
| Scale_Factor | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. Code review: confirmed gap; family=vfx-models. Loader::load_body reads scale for selected transports, but unresolved or unselected company transports remain metadata; no complete class-wide deployed transform route.; basis: reviewed |
| Space_FOW_Reveal_Range | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. Code review: confirmed gap; family=hud-radar. Loader::load_company resolves unselected transport metadata without loading its body; sensor_table has no sensor for that metadata-only type.; basis: reviewed |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Blast_Ability/@Name | partial | The_Peacebringer (src/units/unit_abilities.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 760 | src/units/unit_abilities.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; basis: reviewed |
| Abilities/Blast_Ability/Charged_Effect | partial | The_Peacebringer (apps/viewer/src/battle_effects_impacts.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 760 | apps/viewer/src/battle_effects_impacts.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; basis: reviewed |
| Abilities/Blast_Ability/Charging_Effect | partial | The_Peacebringer (apps/viewer/src/battle_effects_impacts.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 760 | apps/viewer/src/battle_effects_impacts.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; basis: reviewed |
| Abilities/Blast_Ability/SFXEvent_Activate | partial | The_Peacebringer (apps/viewer/src/battle_audio_events.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 4532 | apps/viewer/src/battle_audio_events.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/@Name | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Owner_Particle_Effect | applied | UniqueUnit CORRUPT_SYSTEMS (apps/viewer/src/battle_effects_impacts.cpp) | WHE-33 | — | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/SFXEvent_Activate | applied | UniqueUnit CORRUPT_SYSTEMS (apps/viewer/src/battle_audio_events.cpp) | WHE-33 | — | apps/viewer/src/battle_audio_events.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Target_Particle_Effect | applied | UniqueUnit CORRUPT_SYSTEMS (apps/viewer/src/battle_effects_impacts.cpp) | WHE-33, WHE-34 | — | apps/viewer/src/battle_effects_impacts.cpp | basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/@Name | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Bone_Name | partial | ship (apps/viewer/src/battle_effects_projectiles.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-59 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Owner_Particle_Effect | partial | ship (apps/viewer/src/battle_effects_projectiles.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-59 | 940 | apps/viewer/src/battle_effects_projectiles.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Shield_Flare_Ability/@Name | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Owner_Particle_Effect | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/SFXEvent_Activate | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Super_Laser_Ability/Alternate_Description_Text | todo | application not recorded | — | 760 | — | sk-tagrev B1 10-25: confirmed gap &#91;abilities&#93;: read only by the HUD shell (hud_shell.cpp) for the galactic/ability card text; no space-battle consumer.; basis: reviewed |
| Abilities/Super_Laser_Ability/Owner_Particle_Effect | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=vfx-models. special_ability_kind has no Super_Laser_Ability dispatch; the authored name, animation frame count and owner particle/bone parameters do not reach a nested super-laser consumer.; basis: reviewed |
| Attach_To_Flagship_During_Space_Battle | todo | application not recorded | — | 3776 | — | Space/galactic reader only (or map placement of other types); not read by load_land_body or the land HUD.; basis: reviewed |
| Attack_Animation_Is_Overlay | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Base_Shield_Penetration_Particle | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Death_Explosions_End | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: no reader under src/apps/include.; basis: reviewed |
| Death_SFXEvent_End_Die | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;audio&#93;: no reader under src/apps/include.; basis: reviewed |
| Display_Contained_Hero_Grab_Bars | todo | application not recorded | — | 653 | — | sk-tagrev B1 10-25: confirmed gap &#91;vfx-models&#93;: loaded into the unit table (unit_tables_decode.cpp); no HUD consumer traced.; basis: reviewed |
| Has_Pre_Turn_Anim | todo | application not recorded | — | 3776 | — | named only in required-tag list (land_content.hpp:298); no animation consumer found; basis: reviewed |
| Idle_Chase_Range | partial | loaded fighter locomotor craft, including solo heroes (src/units/unit_motion.cpp); missing: nonfighter ships and containers | FT-02, WHE-SQ-02 | 653 | src/units/unit_motion.cpp | Fighter flight applies authored diversion ranges through the self-represented group; ship and container consumers remain open.; basis: reviewed |
| Locomotor_Has_Animation_Priority | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | no land reader found; basis: reviewed |
| Not_Really_Selectable | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Not_Really_Selectable in src/, apps/ or include/; wire the authored value into the corresponding tactical UI or radar presentation.; basis: reviewed |
| Rotation_Animation_Speed | todo | application not recorded | — | 3776 | — | no code on the land path reads this tag; basis: reviewed |
| SFXEvent_Deploy | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=audio. No named reader of SFXEvent_Deploy in src/, apps/ or include/; wire the authored value into the matching event trigger and playback route.; basis: reviewed |
| SFXEvent_Turret_Rotating_Loop | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Specific_Death_Anim_Type | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | partial | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp); missing: other area abilities | WHE-33 | 760 | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | partial | UniqueUnit CONCENTRATE_FIRE (src/sim/tactical/session_cmd_abilities.cpp); UniqueUnit SENSOR_JAMMING and MISSILE_SHIELD (src/sim/tactical/session_world.cpp); missing: other UniqueUnit abilities | WHE-24; WHE-32, WPJ-17 | 760 | src/sim/tactical/session_cmd_abilities.cpp; src/sim/tactical/session_world.cpp | Concentrate fire applies its target-centred radius; shield and jamming sources apply authored spatial radii to flight and nonenemy recipient stamps.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | partial | UniqueUnit CONCENTRATE_FIRE (src/sim/tactical/session_cmd_abilities.cpp); UniqueUnit ENERGY_WEAPON and TRACTOR_BEAM (src/sim/tactical/session_cmd_abilities.cpp); UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp); missing: other UniqueUnit abilities | WHE-24; WHE-26, WHE-27, WHE-57, WHE-59, WHE-60; WHE-33 | 760 | src/sim/tactical/session_cmd_abilities.cpp | Concentrate fire applies the authored target-centred radius and named nested-handler binding. G7 binds named beam handlers and services tracked targets.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); WIRING todo; basis: reviewed |
| Vehicle_Thief_Inside_Clone | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=vfx-models. No named reader of Vehicle_Thief_Inside_Clone in src/, apps/ or include/; wire the authored value into the class-specific model, animation or effect presentation.; basis: reviewed |
| Wind_Disturbance_Radius | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Sphere_Alpha | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Strength | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Enable_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_bonus_abilities applies Ability_Name and filters; the Enable_Ability name attribute is used only as diagnostic context, not as an upgrade or handler key.; basis: reviewed |
| Abilities/Enable_Ability/Ability_Name | partial | redirect-blaster nested handlers (src/sim/tactical/session_abilities.cpp); redirect-blaster nested handlers (src/skirmish/economy.cpp); missing: nested abilities other than the redirect blaster | WRB-05; WRB-05 | 760 | src/sim/tactical/session_abilities.cpp; src/skirmish/economy.cpp | The debug build's enable-ability type switches any named nested ability; the remake applies it to the redirect-blaster handlers, the only stock target (WRB-05; Focumentation: XML Tags).; basis: reviewed |
| Abilities/Weatherproof_Ability/@Name | todo | application not recorded | — | 760 | — | Code review: confirmed gap; family=abilities. Loader::load_nested_abilities records the attribute in inactive_abilities, but the nested-handler dispatch has no Weatherproof_Ability implementation consuming this name.; basis: reviewed |
| Next_Upgrade_Level_Type | applied | whole class (include/eawr/presentation/ui/production.hpp) | WPR-63 | — | include/eawr/presentation/ui/production.hpp | basis: reviewed |
| Show_In_Sidebar_Tray | todo | application not recorded | — | 653 | — | Code review: confirmed gap; family=hud-radar. No named reader of Show_In_Sidebar_Tray in src/, apps/ or include/; wire the authored value into the matching UI, path overlay or text presentation.; basis: reviewed |
| Show_In_Sidebar_When_Complete | partial | tactical allied UpgradeObject ledgers (src/presentation/ui/team_dock.cpp); missing: non-ledger campaign and land-only callers | HCB-30, WPR-70, WPR-72, WPR-73 | 653 | src/presentation/ui/team_dock.cpp | The tactical research dock reads inherited sidebar flags through the resolved presentation catalog; no simulation schema change. Unrelated corruption trays remain deferred.; basis: reviewed |

## Weather_Scenario

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @emitter_name | todo | application not recorded | — | 3227 | — | weather emitter field; no code reads it; basis: reviewed |
| Phase/@name | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |
| Phase/Duration | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |
| Phase/Ease_Out_Duration | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |
| Phase/Emitter_Intensity | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |
| Phase/Fog_Plane_Interpolation_Alpha | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |
| Phase/Lightning_Intensity | todo | application not recorded | — | 3225 | — | WLEN weather scenario phases; no loader or sim field; basis: reviewed |

## WeatherModifies

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| WeatherModifier/@name | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Class_Text | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Description_Text | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Display_Text | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Icon_Name | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Objective_Text | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Sight_Range_Modifier | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Sight_Range_Modifier/@name | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Speed_Modifier | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Speed_Modifier/@name | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Weapon_Accuracy_Modifier | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
| WeatherModifier/Weapon_Accuracy_Modifier/@name | todo | application not recorded | — | 3226 | — | WLEN weather modifier fields; no loader or sim field; basis: reviewed |
