# combat tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 394 | 394 |
| partial | 133 | 133 |
| todo | 524 | 524 |
| presentation-later | 0 | 3 |
| foc-ignores | 43 | 43 |
| deferred | 0 | 0 |
| land-or-galactic | 0 | 2095 |
| multiplayer | 0 | 1 |
| **Total** | **1094** | **3193** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandFreeStoreScript | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Personality/Aggressiveness | todo | application not recorded | — | 737 | — | — |
| Personality/Focus | todo | application not recorded | — | 737 | — | — |
| SpaceFreeStoreScript | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Templates/Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Templates/Space | applied | whole class (src/script/foc/ai_data.cpp) | DG-24, DG-26, S-15, V-03, W-10 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## AITemplates

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Priority | todo | application not recorded | — | 737 | — | — |
| */Trigger | todo | application not recorded | — | 737 | — | — |

## Audio

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Audio_Land_3D_Listener_Z_Pullback_Dist | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Audio_Land_3D_Rolloff_Distance_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Audio_Land_3D_Saturation_Distance_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Audio_Space_3D_Listener_Z_Pullback_Dist | todo | application not recorded | — | 650 | — | — |
| Audio_Space_3D_Saturation_Distance_Mod | todo | application not recorded | — | 650 | — | — |
| Default_MSS_DIG_DS_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |
| Default_MSS_DIG_DS_MIX_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |
| Delay_Between_Land_Base_Attack_Announcement_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Music_Land_Battle_To_Ambient_Peace_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Sustained_IO_MSS_DIG_DS_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |
| Sustained_IO_MSS_DIG_DS_MIX_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |

## Campaign

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Evil_Victory_Conditions | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Evil_Victory_Cycle_Limit | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Galactic_Control_Percentage_For_Victory | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Good_Victory_Conditions | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Good_Victory_Cycle_Limit | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_GroundInfantry, GroundInfantry, GroundVehicle, HeroUnit, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Sensor_Range | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Cin_GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Scan_Range | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Cin_Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Is_Missile | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CommandBar_Component_Files, Faction_Files, Game_Object_Files, GameXMLFiles, Hard_Point_Files, Mega_Files, MousePointerFiles, SFXEvent_Files, Targeting_Priority_Set_Files

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| File | applied | whole class (src/data/xml.cpp) | AU-107, C-01, L-03b, R-07, SHA-256 | — | src/data/xml.cpp | a registry file entry, read by the catalog's file lists; basis: reviewed |

## CommandBarComponent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blink_Duration | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Blink_Rate | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Default_Offset | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Default_Offset_Widescreen | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Dialog_Scene | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Disable_Darken | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Disabled | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Disabled_Darken | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Disabled_Offset | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Ghost_Base_Only | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Group | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Hidden | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Left_Justified | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Manual_Offset | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| No_Shell | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Offset | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Outlined_Bar | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Overlay2_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, ABE-9 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Overlay_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08, ABE-9 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Pixel_Align | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Right_Justified | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Should_Ghost | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Smooth_Bar | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Snap_Location | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Tab | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Toggle | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Type | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Hack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Hack_Ability/Team_Member_With_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Permanent_Weapon_Swap_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Disruptor_Merc_Team) |
| Abilities/Permanent_Weapon_Swap_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Disruptor_Merc_Team) |
| Abilities/Permanent_Weapon_Swap_Ability/Weapon_Index | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Disruptor_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Activation_Time_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Mine_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Number_Of_Mines | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Proximity_Mines_Ability/Trigger_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Merc_Team) |
| Abilities/Repair_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| Abilities/Repair_Ability/Team_Member_With_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |
| ContainerArrangement | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+33 more)) |
| Is_Squashable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Arctic_Human_Civ_Team, Bothan_Slave_Team, Bothan_Team, Civilian_Independent_AI_Team, Civilian_Urban_Usable_Team, Desert_Human_Civ_Team, Empire_Field_Commander_Team, Ewok_Team (+32 more)) |
| Pre_Lit | todo | application not recorded | — | 650 | — | — |
| Unit_Abilities_Data/Unit_Ability/Damage_Percent_When_Activated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Disruptor_Merc_Team, Underworld_Merc_Team) |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tactical_R2_3PO_Team) |

## Container, GenericHeroUnit, GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, HeroUnit, Indigenous_Unit, Marker, Mobile_Defense_Unit, Projectile, SecondaryStructure, Slave_Unit, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_FOW_Reveal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Container, GenericHeroUnit, GroundInfantry, GroundVehicle, HeroUnit, Indigenous_Unit, Mobile_Defense_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | applied | whole class (src/data/xml_merge.cpp) | R-08, R-11 | — | src/data/xml_merge.cpp | basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+12 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+50 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Home_One, Houndstooth (+14 more). Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+50 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Home_One, Houndstooth (+14 more). |

## Container, GenericHeroUnit, MiscObject, Mobile_Defense_Unit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Type | applied | HUNT craft and lone locomotor units (src/sim/tactical/session_step_commands.cpp); UniqueUnit ENERGY_WEAPON and TRACTOR_BEAM (src/sim/tactical/session_step_commands.cpp); REPLENISH_WINGMEN (src/sim/tactical/session_step_commands.cpp); space MISSILE_SHIELD and SENSOR_JAMMING (src/units/unit_abilities.cpp); space ships with BARRAGE (src/units/unit_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAB-04, WAB-50, WAB-51, WAB-55, WAB-56; WHE-26, WHE-27, WHE-57, WHE-59, WHE-60; WHE-63; WHE-31, WHE-32, WPJ-17; WAD-38; DG-05, DG-06, EWW-09, FO-09, WSU-38 | — | src/sim/tactical/session_step_commands.cpp; src/units/unit_abilities.cpp | G7 binds named beam handlers and services tracked targets.; basis: reviewed |

## Container, GenericHeroUnit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | applied | space MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/abilities.cpp); space ships with BARRAGE (src/sim/tactical/session_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAB-03, WAB-05, WHE-31; WAD-38; AB-01, AB-60, BP-23, BP-24, WAB-11 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_abilities.cpp; src/units/unit_abilities.cpp | basis: reviewed |

## Container, GenericHeroUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | applied | whole class (src/sim/tactical/abilities.cpp); whole class (src/sim/tactical/session_step_commands.cpp) | AB-45, WAB-35; AB-01, AB-40, AB-60, AB-68, WAB-30 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## Container, HeroUnit, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, E_Ground_Turbolaser_Tower, Ground_Empire_Hypervelocity_Gun, Ground_Magnepulse_Cannon, R_Ground_Turbolaser_Tower, Template_Ground_Turbolaser_Tower, U_Ground_Turbolaser_Tower. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, E_Ground_Turbolaser_Tower, Ground_Empire_Hypervelocity_Gun, Ground_Magnepulse_Cannon, R_Ground_Turbolaser_Tower, Template_Ground_Turbolaser_Tower, U_Ground_Turbolaser_Tower. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). |

## Container, HeroUnit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+10 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+41 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+41 more). |

## Container, Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |
| Targeting_Max_Attack_Distance | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |

## Container, Marker, MultiplayerStructureMarker, SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Marker: AI Hint Zone Center, Attacker Entry Position, Attacker Setup Phase Reveal, Base Shield Structure Position, Captureable_Turret, Defender Setup Phase Reveal, Defending Forces Position, DEMO_CONTROLLER (+85 more). Mixed ground/space, space carriers on Marker: AI Hint Zone Center, Attacker Entry Position, Attacker Setup Phase Reveal, Base Shield Structure Position, Captureable_Turret, Defender Setup Phase Reveal, Defending Forces Position, DEMO_CONTROLLER (+85 more). |

## Container, Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Property_Flags | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## Container, SecondaryStructure, SpaceBuildable, SpecialStructure, Squadron, StarBase, TechBuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Dummy | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks (+47 more). Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks (+47 more). |

## Container, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 760 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. Mixed ground/space, space carriers on Container: Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/Activation_Max_Range | partial | squadron (src/sim/tactical/session_step_commands.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-70 | 1884 | src/sim/tactical/session_step_commands.cpp | Planar ordinary attack-range test against the point; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activation_Min_Range | partial | squadron (src/sim/tactical/session_step_commands.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-70 | 1884 | src/sim/tactical/session_step_commands.cpp | Exclusive planar minimum; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Buzz_Droids_Ability/Activation_Time_In_Seconds | partial | squadron (src/units/unit_abilities.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-71 | 1884 | src/units/unit_abilities.cpp | Arming delay of the created droids, truncated to frames; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Damage_Radius | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-73 | 1884 | src/sim/tactical/session_step_combat.cpp | Strict reach to the victim's nearest standing hardpoint; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Enemy_Damage_Per_Second | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-72 | 1884 | src/sim/tactical/session_step_combat.cpp | Per-frame miscellaneous damage to every victim in reach; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Object_Type | partial | squadron (src/sim/tactical/session_step_commands.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-71 | 1884 | src/sim/tactical/session_step_commands.cpp | The created droid object type; the squadron team container's nested ability (AB-60).; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Own_Damage_Per_Second | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-72 | 1884 | src/sim/tactical/session_step_combat.cpp | Per-frame self wear before the victims; the squadron team container's nested ability (AB-60).; basis: reviewed |

## Container, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Ion_Cannon_Shot_Attack_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Ion_Cannon_Shot_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Ion_Cannon_Shot_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Projectile_Types_Override | applied | space ships with BARRAGE (src/units/unit_combat.cpp); whole class (src/units/unit_tables_profiles.cpp) | WAD-38; AB-60, IR-11 | — | src/units/unit_combat.cpp; src/units/unit_tables_profiles.cpp | Team ion shots and BARRAGE weapon overrides retain the normal projectile profile separately.; basis: reviewed |

## Container, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Icon_Scale_Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## Container, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | todo | application not recorded | — | 760 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. Mixed ground/space, space carriers on Container: Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. |

## Container, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Lucky_Shot_Attack_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Lucky_Shot_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Lucky_Shot_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Category | todo | application not recorded | — | 650 | — | — |
| Permanent | todo | application not recorded | — | 650 | — | — |
| UV_Slot | todo | application not recorded | — | 650 | — | — |
| Z_Angle | todo | application not recorded | — | 650 | — | — |

## Decal, LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Intensity | todo | application not recorded | — | 650 | — | — |

## Difficulty_Adjustment

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage_Multiplier | todo | application not recorded | — | 737 | — | — |
| Health_Multiplier | todo | application not recorded | — | 737 | — | — |
| Land_AI_Contrast_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_AI_Goal_Cycle_Sleep_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Build_Time_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Multiplier | todo | application not recorded | — | 737 | — | — |

## EnumDefinition

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | applied | whole class (src/units/unit_support.cpp) | EWW-01, EWW-03, G-03, S-28, U-02 | — | src/units/unit_support.cpp | basis: reviewed |

## Equations

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | todo | application not recorded | — | 737 | — | — |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Allies | todo | application not recorded | — | 650 | — | — |
| Bombardment_Initial_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Number_Of_Salvos | todo | application not recorded | — | 650 | — | — |
| Bombardment_Projectile | todo | application not recorded | — | 650 | — | — |
| Bombardment_Salvo_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Shot_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Shots_Per_Salvo | todo | application not recorded | — | 650 | — | — |
| Create_Player_In_Multiplayer_Games | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |
| Debug_Ground_Structures | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Enemies | todo | application not recorded | — | 650 | — | — |
| Faction_Leader | todo | application not recorded | — | 650 | — | — |
| Faction_Leader_Company | todo | application not recorded | — | 650 | — | — |
| Faction_Super_Weapon | todo | application not recorded | — | 650 | — | — |
| Garrison_Reinforcement_Delay_Seconds | applied | whole class (src/skirmish/inputs.cpp) | FL-14 | — | src/skirmish/inputs.cpp | basis: reviewed |
| Ground_Base_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ground_Transport_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Infantry_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Neutral | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/session_lifecycle.cpp); whole class (src/skirmish/ai.cpp) | WHZ-51, WWP-66; WHZ-51; none recorded | — | src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_lifecycle.cpp; src/skirmish/ai.cpp | Every combat session applies authored neutral factions to ordinary attack commands, ship and opportunity targeting, cursor classification and projectile contact; captured objects use the current owner relationship.; basis: reviewed |
| Is_Playable | applied | whole class (src/skirmish/inputs.cpp); whole class (src/skirmish/start.cpp) | SC-01; none recorded | — | src/skirmish/inputs.cpp; src/skirmish/start.cpp | basis: auto |
| Land_Ability_Targeting_Range_Overlay_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Ability_Targeting_Range_Overlay_RGBA | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Ability_Targeting_Range_Overlay_Scale_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Advisor_Hints | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Area_Effect_Range_Overlay_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Area_Effect_Range_Overlay_RGBA | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Area_Effect_Range_Overlay_Scale_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Lose_Image | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Mode_Garrison_Selection_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Mode_Selection_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Begin_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Cancel_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Countdown_Color_RGBA | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Countdown_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Countdown_Text_ID | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Enemy_Begin_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Not_Allowed_Reason_1_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Not_Allowed_Reason_2_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Not_Allowed_Reason_3_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Not_Allowed_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Pursue_Max_Speed_Mod_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Units_Damaged_Mod_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Skirmish_AI_Default_Forces | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Skirmish_Unit_Buy_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Skirmish_Unit_Cap_By_Player_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Surrender_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Win_Image | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Music_Event_Land_Ambient_Super_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Music_Event_Land_Battle_Super_Weapon | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Music_Event_Tactical_Land_Battle_Pending | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Primary_Enemy | todo | application not recorded | — | 650 | — | — |
| SFX_Event_Tactical_Land_Battle_Pending | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_HUD_Lost_Land_Battle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_HUD_Lost_Land_Battle_Enemy_TSW_Present | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_HUD_Won_Land_Battle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_HUD_Won_Land_Battle_Enemy_TSW_Present | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Land_Base_Under_Attack_Announcement | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Land_Invasion_Commencing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Skirmish_Land_Bomber | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Mode_Garrison_Selection_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Attacker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Attacker_Conditional_Or | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Attacker_Last_Location | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Defender | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Defender_Conditional_Or | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Defender_Last_Location | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Raid_Attacker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SpeechEvent_Tactical_Intro_Land_Raid_Defender | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Standalone_Space_Maps_Special_Weapon_A | todo | application not recorded | — | 650 | — | — |
| Standalone_Space_Maps_Special_Weapon_B | todo | application not recorded | — | 650 | — | — |

## FunctionSet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Function | applied | whole class (src/script/foc/ai_data.cpp) | CRC-32, FT-01, GS-02, L-10, L-20 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_LandAreaThreatScaleFactor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AI_LandEvaluatorRegionSize | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AI_LandThreatDistanceFactor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AI_LandThreatLookAheadTime | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AI_LandThreatTurnRateFactor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| AIUsesFogOfWarLand | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Allow_Reinforcement_Percentage_Normalized | todo | application not recorded | — | 650 | — | — |
| ApproximationForwardDistance | todo | application not recorded | — | 650 | — | — |
| ApproximationSmoothCosAngle | todo | application not recorded | — | 650 | — | — |
| Armor_Types | todo | application not recorded | — | 650 | — | — |
| Asteroid_Field_Damage | applied | eligible tactical frigates and capitals (src/sim/tactical/session_step_systems.cpp) | WHZ-11, WHZ-12 | 924 | src/sim/tactical/session_step_systems.cpp | basis: reviewed |
| Asteroid_Field_Damage_Rate | applied | eligible tactical frigates and capitals (src/sim/tactical/session_step_systems.cpp) | WHZ-11, WHZ-12 | 924 | src/sim/tactical/session_step_systems.cpp | basis: reviewed |
| Base_Land_Targeting_Arc_Angle_Coefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Shield_Delay_Time | land-or-galactic | application not recorded | — | — | — | debug build: read only by the ground-structure base shield behavior; the only stock base shields are ground structures (E_Ground_Base_Shield, R_Ground_Base_Shield) |
| Base_Shield_Vulnerability_Modifier | land-or-galactic | application not recorded | — | — | — | debug build: read only by the damage scaling for a base-shielded object; the only stock base shields are ground structures |
| Battle_Pending_Timeout_Seconds | land-or-galactic | application not recorded | — | — | — | debug build: read only by the campaign pending-battle flow (battle-end dialog timeout, pending-battle message, pending-battle service) |
| Blockade_Run_Attrition_Factor | land-or-galactic | application not recorded | — | — | — | debug build: read only by the ground retreat coordinator (blockade-run attrition) |
| Bombardment_Distribution | land-or-galactic | application not recorded | — | — | — | no getter reference in the debug build; ground bombardment placement (docs/behaviour/walks/land-combat.md LC-67) |
| Bombardment_Offset | land-or-galactic | application not recorded | — | — | — | no getter reference in the debug build; ground bombardment placement (docs/behaviour/walks/land-combat.md LC-67) |
| Bribery_Fleet_Reveal_Range | land-or-galactic | application not recorded | — | — | — | debug build: read only by the galactic fleet visibility test (bribed planets) |
| Control_Point_Domination_Victory_Time_In_Secs | todo | application not recorded | — | 650 | — | — |
| Damage_To_Armor_Mod | applied | whole class (src/units/unit_support.cpp) | DG-06, DG-10, DG-12, DG-20, WC-04 | — | src/units/unit_support.cpp | basis: reviewed |
| Damage_Types | todo | application not recorded | — | 650 | — | — |
| Default_Defense_Adjust | land-or-galactic | application not recorded | — | — | — | debug build: read only by the auto-resolve fire steps (unit fire, escort fire) of the campaign solver |
| Default_Hero_Respawn_Time | land-or-galactic | application not recorded | — | — | — | debug build: read only by hero defeat handling and the hero respawn scheduler (a defeated hero is respawned by the campaign) |
| Depleted_Shield_Damage_Increment | applied | whole class (src/units/unit_durability.cpp) | BP-19, BP-21, DG-06, DG-08, DG-09 | — | src/units/unit_durability.cpp | basis: auto |
| Depleted_Shield_Disable_Time | applied | whole class (src/units/unit_durability.cpp) | AB-17, DG-08, PD-09, S-15 | — | src/units/unit_durability.cpp | basis: auto |
| DesiredLandFOWCellSize | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Diminishing_Firepower | applied | whole class (src/units/unit_durability.cpp) | DG-05, DG-26, PD-07, S-15 | — | src/units/unit_durability.cpp | basis: auto |
| DynamicLandQuotaResetInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Elevated_Vulnerability_Duration | land-or-galactic | application not recorded | — | — | — | no getter reference in the debug build; the non-space elevated-vulnerability time; the set-up swaps to the Space_ pair in space mode (the skirmish reads that pair, PU-38) |
| Elevated_Vulnerability_Factor | land-or-galactic | application not recorded | — | — | — | debug build: the non-space pair of the elevated-vulnerability set-up, which swaps to Space_Elevated_Vulnerability_Factor/Duration in space mode (the skirmish reads that pair, PU-38) |
| Energy_Beam_Frames | todo | application not recorded | — | 650 | — | — |
| EnergyRechargeIntervalInSecs | applied | whole class (src/sim/tactical/damage.cpp) | DG-13, EN-02, PD-22, PD-26 | — | src/sim/tactical/damage.cpp | basis: auto |
| EnergyToShieldExchangeRate | applied | whole class (src/units/unit_durability.cpp) | DG-13, DG-15, EN-04, PD-23 | — | src/units/unit_durability.cpp | basis: auto |
| First_Strike_Extra_Damage_Percent | todo | application not recorded | — | 650 | — | — |
| Fleeing_Infantry_Speed_Bonus | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fleet_Maintenance_Update_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | no getter reference in the debug build; galactic fleet maintenance service (docs/behaviour/walks/gc-movement-control.md) |
| Force_Ability_Disable_Time | todo | application not recorded | — | 650 | — | — |
| FramesPerPositionApproximationRebuild | todo | application not recorded | — | 650 | — | — |
| Good_Ground_Color_Tint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Move_Acknowledge_Scale_Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hardpoint_Recharge_Cutoff_For_Opportunity_Fire | applied | whole class (src/sim/tactical/combat_fire.cpp); whole class (src/units/unit_combat.cpp) | R-01; R-01 | — | src/sim/tactical/combat_fire.cpp; src/units/unit_combat.cpp | basis: reviewed |
| Health_Critical_Percent_Threshold | todo | application not recorded | — | 650 | — | — |
| Health_Low_Percent_Threshold | applied | whole class (src/units/unit_motion.cpp) | AB-42, E72-03, E72-13, EX-01, T-07 | — | src/units/unit_motion.cpp | basis: auto |
| High_Ground_Color_Tint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hull_Vs_Hard_Points_Health_Constraint | applied | whole class (src/units/unit_motion.cpp) | E72-06, E72-07, HD-21, HS-02, S-45 | — | src/units/unit_motion.cpp | basis: auto |
| Infantry_Ground_Color_Tint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| InfantryFormationRecruitmentDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| InfantryTurnBlendTime | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ion_Storm_Shield_Disable_Time | applied | shield-bearing tactical combatants (src/sim/tactical/damage.cpp); mesh collision targets (src/sim/tactical/projectiles.cpp); shield-bearing tactical combatants (src/sim/tactical/session_step_systems.cpp); GameConstants (src/units/unit_durability.cpp) | WHZ-32; WHZ-33; WHZ-30, WHZ-31; WHZ-31 | — | src/sim/tactical/damage.cpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_durability.cpp | basis: reviewed |
| Land_Auto_Resolve_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Base_Destruction_Forces_Retreat | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Capture_Allowed_Countdown_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Collidable_Grid_Cull_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Guard_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Health_Bar_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Allowed_Countdown_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Retreat_Attrition_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Landbase_Damage_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Landbase_Health_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| LandDestinationProximity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandFOWColor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandFOWRegrowTime | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandObjectTrackingInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandObjectTrackingTreeCount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandPredictionTimeInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandTemporaryDestinationProximity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandWaitOperatorSpeedCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Lava_Ground_Color_Tint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Bombard_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | debug build: read only by the planetary bombardment manager set-up (galactic) |
| Max_Ground_Forces_On_Planet | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Remote_Bombs_Per_Player | land-or-galactic | application not recorded | — | — | — | debug build: read only by the unit-ability enable check of the remote-bomb ability, which the effective XML authors only on ground infantry and indigenous types |
| MaximumSpecialStructuresLand | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxLandFormationFormupFrames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxObstacleCostLand | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Melee_Cutoff_Range | land-or-galactic | application not recorded | — | — | — | debug build: read only by the melee test of an object type (ground) |
| Min_Bombard_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | debug build: read only by the planetary bombardment manager set-up (galactic) |
| MinLandPredictionDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MinObstacleCostLand | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MP_Default_Land_Tactical_Win_Condition | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Nebula_Ability_Disable_Time | applied | loaded tactical space units, craft and static obstacles (src/sim/tactical/session_step_systems.cpp); GameConstants (src/units/unit_motion.cpp) | WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-21 | — | src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp | basis: reviewed |
| Object_Max_Health_Multiplier_Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Object_Max_Health_Multiplier_Space | applied | M2 squadron containers (apps/sim_headless/scenario_run.cpp); whole class (src/units/unit_durability.cpp) | WSQ-60; E72-12, ETA-05, ETA-11, G-07, SK-42 | — | apps/sim_headless/scenario_run.cpp; src/units/unit_durability.cpp | Container Tactical_Health and Shield_Points are absent from the loaded stock XML; WSQ-60 applies the native hull default to traces with the space multiplier.; basis: auto |
| Object_Max_Speed_Multiplier_Land | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Occlusion_Silhouettes_Enabled | todo | application not recorded | — | 650 | — | — |
| Override_Death_Persistence_Duration | todo | application not recorded | — | 650 | — | — |
| Quickmatch_Map_Exclusion_List | multiplayer | application not recorded | — | — | — | debug build: read only by the quickmatch map-name lookup (online matchmaking) |
| Raid_Force_Free_Object_Category_Mask | land-or-galactic | application not recorded | — | — | — | debug build: read only by the galactic fleet raid-capability test |
| Raid_Force_Limited_Object_Category_Mask | land-or-galactic | application not recorded | — | — | — | debug build: read only by the galactic fleet raid-capability test |
| Raid_Force_Max_Heros | land-or-galactic | application not recorded | — | — | — | debug build: read only by the galactic fleet raid-capability test |
| Raid_Force_Max_Limited_Objects | land-or-galactic | application not recorded | — | — | — | debug build: read only by the galactic fleet raid-capability test |
| Sensor_Jamming_Time | todo | application not recorded | — | 650 | — | — |
| SetupPhaseCountdownSeconds | todo | application not recorded | — | 650 | — | — |
| SetupPhaseEnabled | todo | application not recorded | — | 650 | — | — |
| ShieldRechargeIntervalInSecs | applied | whole class (src/units/unit_durability.cpp) | DG-13, PD-08, PD-26, S-15 | — | src/units/unit_durability.cpp | basis: auto |
| ShouldInfantryTeamsSplitAcrossFormations | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| ShouldSkipLandFormup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Skirmish_Reinforcement_Delay_Frames | todo | application not recorded | — | 650 | — | — |
| Slow_Ground_Color_Tint | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Solo_Attack_Range | land-or-galactic | application not recorded | — | — | — | debug build: read only by the land movement coordinator (single-unit attack range) |
| Space_Elevated_Vulnerability_Duration | applied | whole class (src/sim/tactical/session_step_combat.cpp) | WR-41 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| Space_Elevated_Vulnerability_Factor | applied | whole class (src/sim/tactical/session_step_combat.cpp) | WR-41 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| SpecialAlignedOperatorBonus | todo | application not recorded | — | 650 | — | — |
| Starbase_Damage_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Starbase_Health_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Strategic_Edge_Scroll_Region | land-or-galactic | application not recorded | — | — | — | debug build: read only by the strategic (galactic map) camera scroll-speed calculation |
| Strategic_Offscreen_Scroll_Region | land-or-galactic | application not recorded | — | — | — | debug build: read only by the strategic (galactic map) camera scroll-speed calculation |
| SyncedFrameInterval | todo | application not recorded | — | 650 | — | — |
| Tactical_Edge_Scroll_Region | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tactical_Offscreen_Scroll_Region | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| TacticalEdgeScrollRegion | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| TacticalOffscreenScrollRegion | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Terrain_Resurface_Rand | todo | application not recorded | — | 650 | — | — |
| Terrain_Resurface_Tolerance | todo | application not recorded | — | 650 | — | — |
| Under_Construction_Damage_Multiplier | todo | application not recorded | — | 650 | — | — |
| Use_Reinforcement_Points | land-or-galactic | application not recorded | — | — | — | debug build: read only by land mode company placement (docs/behaviour/walks/land-combat.md LC-44) |
| Value | todo | application not recorded | — | 650 | — | — |
| WaypointLineLandDashLength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WaypointLineLandDashVelocity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| WaypointLineLandGapLength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| XYExpansionDistanceLand | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GameConstants, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Grenade_Gravity | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. |
| Projectile_Grenade_Gravity_Lob_Mod | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. |

## GameXMLFiles

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| File/@filename | todo | application not recorded | — | 650 | — | — |
| File/@metafile | todo | application not recorded | — | 650 | — | — |
| File/@type | todo | application not recorded | — | 650 | — | — |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Enhance_Defense_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Enhance_Defense_Ability/Apply_To_All_Allies | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Enhance_Defense_Ability/Enhancement_Percentage | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Enhance_Defense_Ability/Unit_Strength_Category | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Major_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R) |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Minor_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R) |
| Abilities/Neutralize_Hero_Ability/Owner_Respawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R) |
| Abilities/Neutralize_Hero_Ability/Target_Respawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R) |
| Abilities/Remote_Bomb_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Bomb_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/System_Spy_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Abilities/System_Spy_Ability/Causes_Despawn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Abilities/System_Spy_Ability/Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Abilities/System_Spy_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Abilities/System_Spy_Ability/See_Fleet_Contents | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Probe_Droid) |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid) |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid) |
| Abilities/System_Spy_Ability/See_Num_Fleets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Generic_Bounty_Hunter_E, Generic_Bounty_Hunter_R, Generic_Probe_Droid, Generic_Smuggler_E, Generic_Smuggler_R, Generic_Smuggler_R_Tutorial) |
| Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| Execute_Script_On_Type | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Is_Generic_Hero | partial | craft, ship, station (src/presentation/ui/world_ui.cpp); ship (src/skirmish/economy.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects outside the simulated unit closure | WSU-50; WHE-01, WHE-49; WSU-50 | 650 | src/presentation/ui/world_ui.cpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces.; basis: reviewed |
| Is_Squashable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)) |
| MaxJiggleDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel) |
| Occlusion_Silhouette_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (R_General_Rieekan_Commander) |
| Ranged_Target_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, R_General_Rieekan_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur (+1 more)); basis: reviewed |
| Targeting_Stickiness_Time_Threshold | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Generic_Commander_00, Generic_Field_Commander_Empire, Generic_Field_Commander_Rebel, Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Unit_Abilities_Data/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Hutt_Field_Commander, Squad_Generic_Field_Commander_Empire, Squad_Generic_Field_Commander_Rebel, Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |

## GenericHeroUnit, GroundInfantry, GroundVehicle, HeroUnit, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ground_Infantry_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Living | partial | craft, ship, station (src/presentation/ui/selection.cpp); craft, ship, station (src/sim/tactical/blast.cpp); loaded tactical units (src/sim/tactical/combat_algorithms.hpp); loaded non-team combat candidates (src/sim/tactical/combat_targeting.cpp); loaded tactical units (src/sim/tactical/pads.cpp); missing: objects outside the simulated unit closure | WSU-13; WAD-14; R-08; WCC-25; WBP-50, WBP-51 | 649 | src/presentation/ui/selection.cpp; src/sim/tactical/blast.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/pads.cpp | Effective living-projectile admission defaults false for each loaded type. R-08 rejects noncollidable weapon opportunities, including hostile captured mining pads, before priority and aim. WBP-50/51 gates capture and palette candidates; WAD-14 gates blast recipients and WHZ-51 gates ordinary contact. Squadron records take the spawned team type flag, with independently admitted craft. WSU-13 mouse admission already uses false; objects outside the simulated closure remain deferred. WCC-25 independently rejects noncollidable ship-scan candidates before weapon/priority/range checks; team containers bypass that type check.; basis: reviewed |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, SpaceProp, SpaceUnit, SpecialStructure, StarBase, TechBuilding, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, MiscObject, Mobile_Defense_Unit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | applied | whole class (src/units/unit_combat.cpp) | PL-13 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Priority_Set | applied | whole class (include/eawr/units/unit_tables.hpp) | E75-22, FT-03, R-09, S-01, S-32 | — | include/eawr/units/unit_tables.hpp | matched by the field's name; the loader is table-driven; basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Persistence_Duration | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-01, UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team | partial | loaded local space battle (apps/viewer/src/battle_scoring.cpp); missing: land companies and non-scoring team creation | WBF-46 | 650 | apps/viewer/src/battle_scoring.cpp | Results scoring applies the guarded first-member team size clamp; authoritative team creation is separate.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/See_Ground_Company_Contents | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/System_Spy_Ability/See_Num_Ground_Companies | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GenericHeroUnit, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Can_Be_Neutralized_By_Major_Heroes | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Sundered_Heart, Sundered_Heart_Cinematic_Clone. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Sundered_Heart, Sundered_Heart_Cinematic_Clone. |
| Can_Be_Neutralized_By_Minor_Heroes | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Sundered_Heart, Sundered_Heart_Cinematic_Clone. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Sundered_Heart, Sundered_Heart_Cinematic_Clone. |
| Stay_In_Transport_During_Ground_Battle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GenericHeroUnit, MiscObject, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Health | applied | whole class (src/skirmish/start.cpp) | E72-12, HD-01, SK-42, U-04 | — | src/skirmish/start.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | applied | whole class (src/skirmish/content.cpp) | VE-06, VE-10, VT-03 | — | src/skirmish/content.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, PD-05, WC-04, WCC-46, WCC-50 | — | src/units/unit_durability.cpp | basis: auto |
| Shield_Points | applied | whole class (src/units/unit_durability.cpp) | DG-06, PD-02, PD-03, S-15, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |
| Shield_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, PD-08, PD-26, S-15, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, PD-22, PD-26, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Max_Attack_Distance | applied | whole class (src/units/unit_combat.cpp) | A-06, AT-10, DG-24, S-26, S-27, WWP-48 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Recharge_Seconds | applied | whole class (src/units/unit_combat.cpp) | EN-05, S-97, W-06, W-06a, W-10 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Types | applied | whole class (src/sim/tactical/damage.cpp) | EN-05, EN-06, EWW-07, PD-24, PD-25 | — | src/sim/tactical/damage.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Galactic_Stealth_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Jabba_The_Hutt. |
| Abilities/Galactic_Stealth_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Jabba_The_Hutt. |
| Abilities/Galactic_Stealth_Ability/Evade_Detection_Chance | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Tyber_Zann (+6 more). |
| Abilities/Galactic_Stealth_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Jabba_The_Hutt. |

## GenericHeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | applied | space ships with BARRAGE (src/sim/tactical/combat_fire.cpp); Millennium_Falcon (src/sim/tactical/damage.cpp); Admonitor_Star_Destroyer, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing, Rogue_7_XWing, Wedge_XWing_Rogue (src/sim/tactical/session_step_combat.cpp); Admonitor_Star_Destroyer, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing, Rogue_7_XWing, Wedge_XWing_Rogue (src/units/unit_abilities.cpp); Millennium_Falcon (src/units/unit_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAD-38; WHE-51; WHE-51; WHE-22, WHE-23; WHE-22, WHE-52; AB-01, WAB-03, WAB-11, WAB-30 | — | src/sim/tactical/combat_fire.cpp; src/sim/tactical/damage.cpp; src/sim/tactical/session_step_combat.cpp; src/units/unit_abilities.cpp | basis: reviewed |

## GenericHeroUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy | applied | whole class (src/units/unit_combat.cpp) | CF-03, DG-24, EWW-09, S-97, S-98 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, SpecialStructure, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |
| Abilities/Battlefield_Modifier_Ability/Apply_To_All_Allies | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |

## GenericHeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Type | applied | whole class (src/units/unit_abilities.cpp) | DG-05, DG-06, EWW-09, FO-09, WSU-38 | — | src/units/unit_abilities.cpp | basis: auto |

## Goals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Category | applied | whole class (src/script/foc/ai_goals.cpp) | AI-05, AI-14, FH-20, GS-30, R-09 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */GameMode | applied | whole class (src/script/foc/ai_service.cpp) | none recorded | — | src/script/foc/ai_service.cpp | basis: auto |
| */Global_Exclusions | todo | application not recorded | — | 737 | — | — |
| */Is_Like | applied | whole class (src/script/foc/ai_data.hpp) | GS-12 | — | src/script/foc/ai_data.hpp | matched by the field's name; the loader is table-driven; basis: auto |
| */Per_Activation_Failure_Desire_Adjust | applied | whole class (src/script/foc/ai_goals.cpp) | GS-04, PL-01 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */Per_Failure_Desire_Adjust | applied | whole class (src/script/foc/ai_goals.cpp) | GS-04, PL-01 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */Reachability | todo | application not recorded | — | 737 | — | read by a loader, but nothing applies the value |
| */Time_Limit | partial | space tactical goals (src/script/foc/ai_engine.hpp); missing: galactic and land goals | SAE-05, SAE-09 | 737 | src/script/foc/ai_engine.hpp | Space tactical activation applies the limit; the tactical estimate remains an explicitly unverified conservative policy.; basis: reviewed |

## GraphicDetailSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HardwareConfigurations/HardwareConfiguration/FillRateGPs | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## GroundBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Tab_Outpost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Next_Level_Base | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Prev_Level_Base | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Behavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Text_ID | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Build_Cancelled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Build_Complete | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Build_Started | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Cost_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, GroundStructure, HeroCompany, HeroUnit, Indigenous_Unit, SecondaryStructure, SlaveCompany, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TechBuilding, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Ground_Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Dummy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tech_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundBuildable, GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Size_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Velocity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundCompany, GroundInfantry, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundCompany, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Row | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundCompany, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Star_Base_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBase, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Income_Stream_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Income_Stream_Ability/Base_Income_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Income_Stream_Ability/Base_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Income_Stream_Ability/Full_Amount_To_Everyone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Income_Stream_Ability/Split_Favors_Owner | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Income_Stream_Ability/Split_Income_With_Allies | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Position | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Capture_Point_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Capture_Point_Transition_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fine_Tune_Occupied_Passability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hides_When_Built_On | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Immune_To_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Include_In_UI_Map_Header | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Community_Property | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Minimum_Time_Before_Pad_Can_Build_Again | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MP_Encyclopedia_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ownership_Sticks | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Show_Facing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Respects_Skirmish_Pre_Build_Bases_Option | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reveal_During_Setup_Phase_Only | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Special_Weapon_Ready | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Unit_Under_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Additional_Structure_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Cost_Campaign | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Start_Lower_Z | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Buildable_Constructed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Buildable_Objects_Campaign | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Buildable_Objects_Multiplayer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Terrain_Texture_Modifier_Join_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Terrain_Texture_Modifier_Material | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Terrain_Texture_Modifier_Square | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Visible_To_Enemies_When_Empty | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Encyclopedia_Unit_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Score_Cost_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Select_Box_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit, SpacePrimarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LOD_Bias | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Build_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bounds_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Can_Be_Unlocked_By_Slicer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Initially_Locked | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Explosions | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Property_Flags | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Points | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Capacity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Energy_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| No_Reflection_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| No_Refraction_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Occlusion_Silhouette_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Collidable_By_Projectile_Living | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_XExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_YExtent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_SFXEvent_Start_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Dense_FOW_Reveal_Range_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Bracket_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Influences_Capture_Point | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Visible_On_Radar | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LandBehavior | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Loop_Idle_Anim_00 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Mass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ranged_Target_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Select_Box_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Select | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Layer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Health | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Max_Attack_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| UnitCollisionClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Damage_Alternates | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Damage_SFX | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Damage_Thresholds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Political_Control | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Soft_Footprint_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundInfantry, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Movie_Object | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Percent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Single_Target_Heal | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Shield_Always_Off | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Exclude_From_Distance_Fade | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Height | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Proxy_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_X_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Obstacle_Y_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Icon_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reveal_During_Setup_Phase | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Sold_Tactical | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundStructure, GroundVehicle, HeroUnit, Marker, Mobile_Defense_Unit, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Land_Evaluator | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundStructure, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Initial_State_Visible_Under_FOW | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Last_State_Visible_Under_FOW | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundBuildable, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Unit_Lost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ignore_For_Reoptimization | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Maintenance_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Piracy_Value_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Required_Orbiting_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Slice_Cost_Credits | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Permanent_Weapon_Swap_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Permanent_Weapon_Swap_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Permanent_Weapon_Swap_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Permanent_Weapon_Swap_Ability/Must_Be_Bought_On_Black_Market | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Permanent_Weapon_Swap_Ability/Weapon_Index | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Activate_SFX | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Activation_Time_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Mine_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Must_Be_Bought_On_Black_Market | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Number_Of_Mines | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Proximity_Mines_Ability/Trigger_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Bomb_Countdown_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Damage_Percent_When_Activated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Good_Against | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Encyclopedia_Vulnerable_To | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Stealth_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Stealth_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Stealth_Ability/Must_Be_Bought_On_Black_Market | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Stealth_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Stealth_Ability/Stealth_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Stealth_Ability/Stealth_Transition_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Cancelled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Complete | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Tactical_Build_Started | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Must_Be_Bought_On_Black_Market | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Cheap_Bribe_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Fleet_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Timeline | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Prerequisites | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Production_Queue | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Charging_Time_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Damage_Per_Second | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Drain_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Drain_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Drain_Source_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Duration_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Max_Drain_Victims | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Should_Heal | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| No_Colorization_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundStructure, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Special_Structures | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Block_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Is_Pulsing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Max_Number_Of_Pulses | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Pulse_Frequency_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Stop_When_Activated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Target_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Targeting_Max_Attack_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Alternate_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Expensive_Bribe_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Homogeneous | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Planets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team_Type | land-or-galactic | application not recorded | — | — | — | WHE-05: the HeroCompany occurrences are ground teams, including Rebel_Field_Commander_Team, Empire_Field_Commander_Team and Tactical_R2_3PO_Team. The five space purchase companies have no effective Create_Team_Type; their selected ship supplies any creation team under WHE-49.; SCOPE-LAND |

## GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Time_Reduced_By_Multiple_Factories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Formation_Priority | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Squad_Size | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Population_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Indigenous_Unit_Corruptible | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Tab_Land_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Company_Transport_Unit | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Company_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Escort | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ship_Class | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Grenade_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Grenade_Explode_Timer_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Grenade_Toss_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Grenade_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Grenade_Attack_Ability/Requires_Direct_Player_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Creation_Delay_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Explosion_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Object_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Recharge_Time_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Self_Destruct_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Spawn_Ability/Spawn_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Blend_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Index | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cinematic_Anim_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Conversion_Ability_Changes_To_Enemy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Formation_Formup_Wait_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Terrain_Model_Mapping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| LateralAcceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Pause_During_Cinematic_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Presence_Induced_Animations | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Cough_Override | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Health_Critical_Warning | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Health_Low_Warning | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Squashable_By_Supercrusher | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Engine_Cinematic_Focus_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Target_Stealth_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Stickiness_Time_Threshold | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundStructure, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Min_Attack_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Elevate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rotate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Below_Detail_Level | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Blob_Shadow_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Blob_Shadow_Scale | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Clone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Pulse_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Fire_Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Fire | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Fire_Inaccuracy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Priority_Set | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autonomous_Move_Extension_Vs_Attacker | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Destruction_Survivors | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Affected_By_Gravity_Control_Field | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Selt_Destruct_SFXEvent_Start_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Surface_Type_Cover_Damage_Shield | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tactical_Bribe_Cost | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Deactivated | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Move_Response_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Guard_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Idle_Chase_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bank_Turn_Angle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Shield_Penetration_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Close_Enough_Angle_For_Move_Start | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Crouch_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Category_Restrictions | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Stationary_When_Attacking | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Rate_Of_Roll | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxJiggleDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Min_Speed_Fraction_For_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Specific_Death_Anim_Index | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Specific_Death_Anim_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Walk_Transition | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Sphere_Alpha | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Wind_Disturbance_Strength | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| CanCellStack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Fade_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Persistence_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationGrouping | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationOrder | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationRaggedness | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| FormationSpacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Squashable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Model_Anim_Override_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Min_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MinimumPushReturnDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Mouse_Collide_Override_Sphere_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Movement_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MovementClass | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OccupationStyle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OverrideAcceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OverrideDeceleration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Appearance_Delay_Frames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Rotation_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Assist_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Assist_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Attack | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Engine_Moving_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Guard | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SurfaceFX_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Walk_Animation_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Weather_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Force_Sensitive | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Can_Indigenous_Unit_Stop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundInfantry, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Burning_Damage_Per_Second | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Create_Team | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Combustible | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| On_Fire_Speed_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Allowed_When_Burning | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Sensor_Jamming_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Sensor_Jamming_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Sensor_Jamming_Ability/Blob_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Sensor_Jamming_Ability/Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Sensor_Jamming_Ability/Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Base_Shield_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Disable_Force_Abilities_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Friendly_Spawn_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| GUI_Hide_Health_Bar | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Branched_Map_Discardable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Decoration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Loads_When_Faction_Present | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Distance_From_Spawner | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Modifies_Reveal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Multisample_FOW_Check | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| OverridePassability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Passive_Missile_Shield_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Radar_Range_Icon_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reinforcement_Prevention_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Reveal_Range_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Normal_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Off_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_On_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Requires_Base_Power | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Model_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Obstacle_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_In_Packs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawn_Indigenous_Units_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Pack_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Quantity | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spawned_Indigenous_Units_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Apply_Y_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Apply_Z_Turret_Rotate_To_Axis | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Attack_Category_Restrictions | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Remove_Upon_Death | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Turret_Rotating_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Drain_Target_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Barrel_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Clone_Is_Obstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rotate_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundStructure, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Enemy_Spawn_Text | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Discardable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Valid_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Cable_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_End_Speed_Fraction | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_Fadeout_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_Length_Fraction | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_Render_Mode | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_Texture_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Cable_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Drop_Phase_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Paralyze_Phase_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Pull_Phase_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Cable_Ability/Terminate_Effect_On_Move_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Bone_Names | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Defense_Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Laser_Beam_Frames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Laser_Beam_Texture | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Laser_Beam_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Laser_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Lightning_Effect_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Projectile_Dist_Travelled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Projectile_Types_Targeted | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Protection_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Recharge_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Laser_Defense_Ability/Zap_SFXEvent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Alternate_Max_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Alternate_Max_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Asteroid_Damage_Hit_Particles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Attack_Animation_Is_Overlay | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Auto_Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Cache_Crusher_Boxes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Converted_To_Enemy_Die_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_YExtent_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_YExtent_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Damaged_Smoke_Asset_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_Explosions_End | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Death_SFXEvent_End_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployed_Max_Attack_Distance_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployed_Turret_Elevate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployed_Turret_Rotate_Extent_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployed_Walk_Anim_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployed_Walk_Speed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Deployment_Anim_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Destroy_When_Stunned_Over_Water | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Exit_Door_Angle_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Exit_Door_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Inaccuracy_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Weapon_When_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Weapon_When_In_Normal_Attack_Mode | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Weapon_When_In_Rocket_Attack_Mode | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fire_Weapon_When_Undeployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fires_Forward | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Glory_Cinematics | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| HardPoints | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hover_Height | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Is_Supercrusher | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Locomotor_Has_Animation_Priority | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Dist_Walk_While_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxSecondaryTurnROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MinSecondaryTurnROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MovementBoxExpansionFactor | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MovementPredictionInterval | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Must_Face_Target_To_Attack_When_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Overall_Length | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Overall_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| PathFrameDelay | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Presence_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Primary_Locomotor_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Secondary_Locomotor_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SecondaryTurnAngle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SecondaryTurnInPlaceROTCoefficient | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SecondaryTurnLookaheadDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Armor_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shield_Sub_Mesh | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Shielded_Only_When_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Should_Cause_Limited_Turrets_To_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Should_Reinforcements_Move | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Space_Full_Stop_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Explosion | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_SFXEvent_Start_Die | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Spin_Away_On_Death_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Stationary_Space_Layer | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Stopped_Rate_Of_Turn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Target_Bones | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Fire_Inaccuracy_Fixed_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Targeting_Land_Model_Stay_Horiz_Flat | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Tread_Scroll_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Deployed_Rest_Angle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Rest_Angle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Disable_Movement | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Max_Secs_For_AE_Delayed_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Uses_Multiple_Locomotors | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| UseSecondaryFacing | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Vehicle_Thief_Inside_Clone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ranking_In_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Projectile_Types_Override | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Arc_Sweep_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Apply_Damage_Right_To_Left | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Width_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Speedup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Subindex | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Begin_Applying_Damage_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Sweep_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Arc_Sweep_Attack_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Drain_Life_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Attachment_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Attack_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Attack_Animation_Speedup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Health_Per_Victim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Owner_Attachment_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Target_Destruction_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Target_Grabbed_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Attack_Animation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Attack_Animation_Speedup | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Damage_Frame_Number | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Generic_Attack_Ability/Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Air_Vehicle_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Collidable_By_Projectile_Dead | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Custom_Hard_XExtent_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Has_Pre_Turn_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Hover_Offset | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| IsDeathCloneObstacle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Layer_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Lift | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Max_Thrust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| MaxFacingLookAheadFrames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Prepare_Strafe_Height | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Targets_Air_Vehicles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Targets_Anything_Else | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, Indigenous_Unit, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Ground_Infantry | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_Targets_Ground_Vehicles | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Idle_Loop | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Ground_Vehicle_Turret_Target | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## GroundVehicle, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Bone_Names | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Garrison_Enter_Dist | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Garrison_Exit_Dist | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Garrison_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Num_Garrison_Slots | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Allow_Opportunity_Fire_When_Targeting | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | A-02, DG-35, R-01, W-04, WCC-17; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Allows_Special_Weapon_Use | todo | application not recorded | — | 650 | — | — |
| Blast_Ability_Fire_Projectile_Type | todo | application not recorded | — | 650 | — | — |
| Death_Breakoff_Prop | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | AU-17, BA-17, BP-30, DG-38, WCC-62 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Engine_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Fire_Cone_Height | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | P-04, W-07, W-09, WA-01, WA-02; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Cone_Width | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | A-06, AT-07, AT-09, P-04, W-07; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Inaccuracy_Distance | applied | whole class (src/units/unit_tables_decode.cpp) | AU-82, CF-03, DG-24, S-97, S-98 | — | src/units/unit_tables_decode.cpp | basis: reviewed |
| Fire_Max_Recharge_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-21, EWW-03, S-01, W-06, W-06a; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Min_Recharge_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-21, EWW-03, W-06, W-06a, W-10; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Projectile_Type | applied | whole class (src/sim/tactical/combat_fire.cpp); whole class (src/sim/tactical/damage.cpp) | WAD-31, WAD-32; EWW-03, WWP-18 | — | src/sim/tactical/combat_fire.cpp; src/sim/tactical/damage.cpp | SPECIAL uses the selected authored projectile through ordinary weapon service; a projectile-free attachment has no firing slot.; basis: auto |
| Fire_Pulse_Count | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | EWW-03, W-06, W-06a, W-10, W-11; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Pulse_Delay_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | EWW-03, W-06, W-06a, W-10, W-11; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Range_Distance | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-66, DG-23, P-04, S-97, S-98; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_When_Deployed | todo | application not recorded | — | 650 | — | — |
| Fire_When_In_Normal_Attack_Mode | todo | application not recorded | — | 650 | — | — |
| Fire_When_In_Power_To_Weapons_Mode | todo | application not recorded | — | 650 | — | — |
| Fire_When_In_Rocket_Attack_Mode | todo | application not recorded | — | 650 | — | — |
| Fire_When_Undeployed | todo | application not recorded | — | 650 | — | — |
| Full_Salvo_Weapon_Delay_Multiplier | applied | loaded space hardpoint weapons (src/sim/tactical/combat_fire.cpp) | WAB-80 | — | src/sim/tactical/combat_fire.cpp | — |
| Health | applied | whole class (src/units/unit_durability.cpp) | CARD-1, DG-38, HD-02, HD-21, WU-22 | — | src/units/unit_durability.cpp | basis: auto |
| Is_Destroyable | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/units/unit_durability.cpp) | WAD-14, WAD-19; none recorded | — | src/sim/tactical/blast.cpp; src/units/unit_durability.cpp | basis: auto |
| Is_Targetable | applied | whole class (src/units/unit_combat.cpp) | none recorded | — | src/units/unit_combat.cpp | basis: auto |
| Is_Turret | partial | one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-06, WAD-39 | 1075 | src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |
| Manual_Hardpoint_Firing_Cooldown_Secs | partial | one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-04, MC-05, WAD-40 | 1075 | src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |
| Power_To_Weapons_Mode_Weapon_Delay_Multiplier | todo | application not recorded | — | 650 | — | — |
| Repair_Amount_Per_Frame | applied | whole class (src/sim/tactical/durability.cpp); whole class (src/units/unit_durability.cpp) | WSL-41, WSL-42; AU-29, E72-08, E72-09, HR-01 | — | src/sim/tactical/durability.cpp; src/units/unit_durability.cpp | Live station command and partitioned repair service apply the authored values; zero amount still pays without gain.; CHECK-842; basis: reviewed |
| Requires_Manual_Target_Assignment | partial | one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); station automatic-fire guard (src/sim/tactical/combat_fire.cpp); missing: nonmanual or other manual burst hardpoints | MC-01, MC-02, WAD-39; WAD-39 | 1075 | src/sim/tactical/combat_aim.cpp; src/sim/tactical/combat_fire.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |
| Shield_Generator_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Special_Ability_Name | todo | application not recorded | — | 715 | — | loaded into the hardpoint, never used (&#35;797) |
| Type | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp); whole class (src/units/unit_durability.cpp) | DG-05, DG-06, EWW-09, FO-09, WSU-38; MD-01; WAD-31, WAD-32; WAD-31 | — | apps/viewer/src/battle_audio_prepare.cpp; src/units/unit_combat.cpp; src/units/unit_durability.cpp | basis: reviewed |
| Weapon_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## HardPoint, HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Category_Restrictions | applied | whole class (src/units/unit_combat.cpp) | EWW-01, EWW-02, EWW-03, R-09, R-10 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, Projectile, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-25, G-02, G-3, WCC-22, WWP-26 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, Projectile, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25, EWW-03 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rest_Angle | partial | one-pulse manual space hardpoints (src/sim/tactical/combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-06, WAD-40 | 1075 | src/sim/tactical/combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; basis: reviewed |

## HardPoint, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Appearance_Delay_Frames | partial | loaded space hardpoint weapons (src/sim/tactical/projectiles.cpp); missing: non-space objects | MC-07, WAD-37, WAD-40 | 1075 | src/sim/tactical/projectiles.cpp | Positive delay controls projectile visibility and first movement independently of manual assignment.; basis: reviewed |

## Hero_Clash

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Clash_Actions/@SubObjectList | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Distance_From_Target_Curve | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Health_Percentage_Curve | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Object | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Relevance_Type | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Target | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Attack_Accuracy_Modifier | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Attack_Number_Of_Shots | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Attack_Projectile_Type | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Ability_Cooldown_Time_In_Secs | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Action_Distance_From_Target_Curve | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Action_Health_Percentage_Curve | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Action_Object | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Action_Relevance_Type | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Special_Ability_Action/Action_Target | todo | application not recorded | — | 650 | — | — |
| Clash_Range | todo | application not recorded | — | 650 | — | — |
| Clash_Type | todo | application not recorded | — | 650 | — | — |
| Combat_Distance | todo | application not recorded | — | 650 | — | — |
| Damage_Amount | todo | application not recorded | — | 650 | — | — |
| Damage_Percentage | todo | application not recorded | — | 650 | — | — |
| First_Hero_Damage_Multiplier | todo | application not recorded | — | 650 | — | — |
| First_Hero_Type | todo | application not recorded | — | 650 | — | — |
| Involved_Hero_Types | todo | application not recorded | — | 650 | — | — |
| Play_Conversation_Events | todo | application not recorded | — | 650 | — | — |
| Second_Hero_Damage_Multiplier | todo | application not recorded | — | 650 | — | — |
| Second_Hero_Type | todo | application not recorded | — | 650 | — | — |

## HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Remote_Bomb_Ability/@Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Bomb_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Remote_Bomb_Ability/Toss_Anim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Always_Spawn_In_Orbit | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Available_In_Skirmish | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Limit_Current_Per_Player | partial | ship (include/eawr/presentation/ui/production.hpp); ship (src/sim/tactical/session_economy.cpp); missing: land and campaign companies | WPR-61, WPR-62, WPR-63; WHE-40, WPR-33 | 934 | include/eawr/presentation/ui/production.hpp; src/sim/tactical/session_economy.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate.; basis: reviewed |
| Build_Limit_Lifetime_For_All_Allies | partial | ship (include/eawr/presentation/ui/production.hpp); ship (include/eawr/sim/tactical/economy.hpp); missing: land and campaign companies | WPR-61, WPR-62, WPR-63; WHE-05, WHE-40, WPR-33 | 935 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp | Space companies retain the authored purchase type through queue, pool and deployment; negative lifetime limits remain unbounded. Ground and campaign consumers are separate.; basis: reviewed |
| Build_Limit_Lifetime_Per_Player | partial | ship (include/eawr/presentation/ui/production.hpp); ship (include/eawr/sim/tactical/economy.hpp); missing: land and campaign companies | WPR-61, WPR-62, WPR-63; WHE-05, WHE-40, WPR-33 | 935 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp | Space companies retain the authored purchase type through queue, pool and deployment; negative lifetime limits remain unbounded. Ground and campaign consumers are separate.; basis: reviewed |
| Build_Tab_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Build_Tab_Space_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Can_Be_Only_One | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Combat_Power_Value | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Company_Transport_Unit | partial | ship (src/skirmish/economy.cpp); missing: land and campaign companies | WHE-05, WHE-49 | 934 | src/skirmish/economy.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate.; basis: reviewed |
| Company_Units | partial | ship (src/skirmish/economy.cpp); missing: land and campaign companies | WHE-05, WHE-49 | 934 | src/skirmish/economy.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate.; basis: reviewed |
| Is_Generic_Hero | todo | application not recorded | — | 935 | — | WHE-01/05: hero identity has independent named and generic flags in space. Company generic-identity consumers remain unimplemented; parsing alone is not application.; basis: reviewed |
| Is_Named_Hero | partial | ship (src/skirmish/inputs.cpp); missing: land and campaign companies | WHE-01, WHE-04 | 934 | src/skirmish/inputs.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate.; basis: reviewed |
| Is_Stealth_Company | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Override_Population_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| SFXEvent_Hero_Respawned | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Show_Hero_Head | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Unit_Abilities_Data/Unit_Ability/Mod_Flag | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Absorb_Blaster_Ability/Absorb_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Damage_Absorb_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Absorb_Blaster_Ability/Damage_Absorb_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Yoda) |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Apply_Damage_Right_To_Left | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Width_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Begin_Applying_Damage_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Sweep_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25, EWW-03 | — | src/units/unit_combat.cpp | basis: auto |
| Abilities/Arc_Sweep_Attack_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Berserker_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Berserker_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Berserker_Damage_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Berserker_Ability/Berserker_Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Combat_Bonus_Ability/Activation_Style | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Drain_Life_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Damage_Per_Second | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Drain_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Duration_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Drain_Life_Ability/Should_Heal | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Find_Weakness_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Find_Weakness_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Find_Weakness_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |
| Abilities/Find_Weakness_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | — |
| Abilities/Find_Weakness_Ability/Damage_Bonus_Percentage | todo | application not recorded | — | 760 | — | — |
| Abilities/Find_Weakness_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | — |
| Abilities/Force_Cloak_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Force_Cloak_Ability/Force_Cloak_Transition_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Force_Confuse_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Confuse_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Confuse_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Confuse_Ability/Confuse_Travel_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Silri) |
| Abilities/Force_Healing_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Heal_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Heal_Percent | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Heal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Emperor_Palpatine, Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Healing_Ability/Single_Target_Heal | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Force_Sight_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Force_Sight_Ability/Force_Sight_Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Yoda) |
| Abilities/Force_Whirlwind_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Damage_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Force_Whirlwind_Ability/Travel_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion) |
| Abilities/Galactic_Stealth_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Droid_C3P0, Droid_R2D2, Han_Solo, Jabba_The_Hutt) |
| Abilities/Galactic_Stealth_Ability/Causes_Despawn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Droid_C3P0, Droid_R2D2, Han_Solo, Jabba_The_Hutt) |
| Abilities/Galactic_Stealth_Ability/Evade_Detection_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Chewbacca, Droid_C3P0, Droid_R2D2, Han_Solo, IG-88, Jabba_The_Hutt (+10 more)) |
| Abilities/Galactic_Stealth_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Droid_C3P0, Droid_R2D2, Han_Solo, Jabba_The_Hutt) |
| Abilities/Generic_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Damage_Frame_Number | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Damage_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Abilities/Generic_Attack_Ability/Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25, EWW-03 | — | src/units/unit_combat.cpp | basis: auto |
| Abilities/Grenade_Attack_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Grenade_Explode_Timer_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Grenade_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Requires_Direct_Player_Command | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Hack_Super_Weapon_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Hack_Super_Weapon_Ability/Causes_Despawn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Hack_Super_Weapon_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Damage_Per_Second | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Distance_To_Infect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Duration_Of_Infection | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Infection_CategoryMask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Projectile_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Seconds_To_Infect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Major_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Mara_Jade) |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Minor_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Mara_Jade) |
| Abilities/Neutralize_Hero_Ability/Owner_Respawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Mara_Jade) |
| Abilities/Neutralize_Hero_Ability/Target_Respawn_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, IG-88, Mara_Jade) |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Arc_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Delay_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Personal_Flame_Thrower_Ability/Fire_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk) |
| Abilities/Radioactive_Contaminate_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Radioactive_Contaminate_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Radioactive_Contaminate_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Radioactive_Contaminate_Ability/Spray_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Redirect_Blaster_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Block_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Urai_Fen (+2 more)) |
| Abilities/Saber_Throw_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Saber_Throw_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Saber_Throw_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Saber_Throw_Ability/Catch_Dist | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Luke_Skywalker_Jedi) |
| Abilities/Stealth_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stealth_Ability/Stealth_Transition_Time | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Stun_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Stun_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Stun_Ability/Stun_Travel_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Urai_Fen, Urai_Fen_Prologue) |
| Abilities/Summon_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/Creation_Delay_In_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/Summon_Ability/Object_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Silri) |
| Abilities/System_Spy_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, General_Dodonna, Han_Solo (+5 more)); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/Causes_Despawn | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, General_Dodonna, Han_Solo (+5 more)); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/Duration_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, General_Dodonna, Han_Solo (+5 more)); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/Initially_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, General_Dodonna, Han_Solo (+5 more)); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Fleet_Contents | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_R2D2, General_Dodonna); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Force_Sensitive_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Obi_Wan_Kenobi) |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Droid_R2D2, General_Dodonna, IG-88, Kyle_Katarn, Mara_Jade); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Droid_R2D2, General_Dodonna, IG-88, Kyle_Katarn, Mara_Jade); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Most_Powerful_Ship | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_R2D2, General_Dodonna); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Num_Fleets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Droid_R2D2, General_Dodonna, Han_Solo, IG-88, Jabba_The_Hutt); SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability (Grand_Admiral_Thrawn) |
| Abilities/System_Spy_Ability/See_Super_Weapons | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Obi_Wan_Kenobi) |
| Abilities/Tactical_Bribe_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Tactical_Bribe_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann) |
| Abilities/Vehicle_Thief_Ability/Activation_Chance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/Activation_Max_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/Activation_Min_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Abilities/Vehicle_Thief_Ability/Applicable_Unit_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Armor_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Han_Solo) |
| Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Kyle_Katarn (+3 more)) |
| Dynamic_Transform_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Dynamic_Transform_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Energy_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Holster_Disable_Engine_Loops | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Holster_Transition_Time_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Yoda) |
| Is_Force_Sensitive | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Luke_Skywalker. Mixed ground/space, space carriers on HeroUnit: Luke_Skywalker. |
| Is_Squashable | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Han_Solo, Jabba_The_Hutt) |
| Is_Stationary_When_Attacking | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Silri, Silri_No_Abilities (+1 more)) |
| MaxJiggleDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_C3P0, Droid_R2D2, Kyle_Katarn, Mara_Jade) |
| MinimumPushReturnDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+19 more)) |
| Occlusion_Silhouette_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+20 more)) |
| Presence_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine) |
| Projectile_Appearance_Delay_Frames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Luke_Skywalker_Jedi (+6 more)) |
| Projectile_Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Han_Solo) |
| Projectile_Fire_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Han_Solo, IG-88, Jabba_The_Hutt, Kyle_Katarn (+12 more)) |
| Projectile_Fire_Pulse_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Han_Solo, IG-88, Jabba_The_Hutt, Kyle_Katarn (+12 more)) |
| Projectile_Fire_Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+20 more)) |
| Projectile_Types | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Ranged_Target_Z_Adjust | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+22 more)); basis: reviewed |
| Respawn_Whole_Team_When_Killed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_C3P0, Droid_R2D2) |
| Share_Damage_With_Teammates | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_C3P0, Droid_R2D2) |
| Shield_Points | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88 (+13 more)) |
| Shield_Refresh_Rate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Chewbacca) |
| Tactical_Health | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Target_Stealth_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, IG-88, Luke_Skywalker_Jedi, Obi_Wan_Kenobi, Silri (+3 more)) |
| Targeting_Fire_Inaccuracy | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion (+5 more)) |
| Targeting_Max_Attack_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+21 more)) |
| Targeting_Stickiness_Time_Threshold | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine (+19 more)) |
| Type | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Captain_Piet, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+2 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. |
| Unique_Ground_Unit | partial | craft, ship, squadron (src/units/unit_tables_profiles.cpp); missing: ground deployment | WHE-13, WHE-56 | 937 | src/units/unit_tables_profiles.cpp | The authored ground container is excluded from a carried hero's space command recipients; ground deployment remains separate.; basis: reviewed |
| Unique_Space_Unit | partial | craft, ship, squadron (src/skirmish/economy.cpp); craft, ship, squadron (src/units/unit_tables_profiles.cpp); missing: campaign and ground conversions | WHE-49; WHE-13, WHE-56 | 934 | src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | The space company resolver selects the authored unique ship and its creation team; command recipients exclude the unique container. Other modes remain separate.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen (+1 more). |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Droid_R2D2, Obi_Wan_Kenobi) |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo. |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+8 more). |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_R2D2, Emperor_Palpatine, Han_Solo (+14 more)) |
| Unit_Abilities_Data/Unit_Ability/Type | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+8 more). |
| Victory_Relevant | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01, Darth_Vader (+23 more)) |
| Walk_Transition | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |
| Weather_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Cuddles_The_Rancor, Cuddles_The_Rancor_Death_Clone_00, Cuddles_The_Rancor_Death_Clone_01) |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |

## HeroUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Unit_Strength_Category | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing (+3 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Home_One, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing (+3 more). |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Super_Weapon_Killer | todo | application not recorded | — | 650 | — | — |

## HintSets

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */* | todo | application not recorded | — | 650 | — | — |

## Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Eat_Attack_Ability/Needs_To_Rotate | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Eat_Attack_Ability/Rotation_Base_Bone_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Allow_Idle_When_Moving | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Avoidance_Disabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Begin_Turn_Towards_Distance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Harass_Enemy_Exclusion_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Squash_Damage_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Turret_XY_Only | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |

## LensFlares

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LensFlare/Flare/Diffuse | todo | application not recorded | — | 650 | — | — |
| LensFlare/Flare/Fraction | todo | application not recorded | — | 650 | — | — |
| LensFlare/Flare/Num_Slots | todo | application not recorded | — | 650 | — | — |
| LensFlare/Flare/Radius | todo | application not recorded | — | 653 | — | no loader reads it (the scan's match is an unrelated particle property) |
| LensFlare/Flare/Slot_X | todo | application not recorded | — | 650 | — | — |
| LensFlare/Flare/Slot_Y | todo | application not recorded | — | 650 | — | — |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bolt_Creation_Interval_Max | todo | application not recorded | — | 650 | — | — |
| Bolt_Creation_Interval_Min | todo | application not recorded | — | 650 | — | — |
| Cycles_Per_Distance | todo | application not recorded | — | 650 | — | — |
| Detail | todo | application not recorded | — | 650 | — | — |
| Displace | todo | application not recorded | — | 650 | — | — |
| Number_Bolts | todo | application not recorded | — | 650 | — | — |
| Update_All | todo | application not recorded | — | 650 | — | — |
| Width_Max | todo | application not recorded | — | 650 | — | — |
| Width_Min | todo | application not recorded | — | 650 | — | — |

## LightningEffect, LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radius | todo | application not recorded | — | 653 | — | no loader reads it (the scan's match is an unrelated particle property) |

## LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Destruct_Time | todo | application not recorded | — | 650 | — | — |
| Diffuse | todo | application not recorded | — | 650 | — | — |
| Falloff_Start | todo | application not recorded | — | 650 | — | — |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Cash_Point_Radius | todo | application not recorded | — | 650 | — | — |
| Cash_Point_Transition_Time_Seconds | todo | application not recorded | — | 650 | — | — |
| Control_Point_Domination_Condition_Relevant | todo | application not recorded | — | 650 | — | — |
| Influences_Cash_Point | todo | application not recorded | — | 650 | — | — |
| Reinforcement_Enabling_Radius | todo | application not recorded | — | 650 | — | — |
| Reveal_For_Attacker | todo | application not recorded | — | 650 | — | — |
| Reveal_For_Defender | todo | application not recorded | — | 650 | — | — |
| Shield_Points | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Victory_Relevant | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## Marker, MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Immune_To_Damage | partial | Dummy_Barrage_Target (src/units/unit_tables_decode.cpp); missing: other markers and misc objects | WAD-38 | 1074 | src/units/unit_tables_decode.cpp | The canonical BARRAGE target must be immune and receives no durability profile; its model collision remains available to projectiles.; basis: reviewed |

## Marker, MiscObject, MultiplayerStructureMarker, SpaceProp, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Discardable | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |

## Marker, MultiplayerStructureMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Include_In_UI_Map_Header | todo | application not recorded | — | 650 | — | — |
| Marker_For_Specific_Object_Type | applied | whole class (src/skirmish/inputs.cpp) | none recorded | — | src/skirmish/inputs.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## Marker, MultiplayerStructureMarker, ScriptMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Marker | applied | whole class (src/scene/space_population.cpp) | none recorded | — | src/scene/space_population.cpp | basis: auto |

## Marker, Particle, SpaceProp, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | applied | whole class (src/scene/space_population.cpp) | none recorded | — | src/scene/space_population.cpp | basis: auto |

## Marker, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Health | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## Marker, Projectile, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Marker: Captureable_Turret, Cash_Point_Large, Cash_Point_Medium, Cash_Point_Small, Reinforcement_Point, Reinforcement_Point_Empire, Reinforcement_Point_Empire_Plus4_Cap, Reinforcement_Point_Plus10_Cap (+9 more). Mixed ground/space, space carriers on Marker: Captureable_Turret, Cash_Point_Large, Cash_Point_Medium, Cash_Point_Small, Reinforcement_Point, Reinforcement_Point_Empire, Reinforcement_Point_Empire_Plus4_Cap, Reinforcement_Point_Plus10_Cap (+9 more). |

## Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## Marker, SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Refresh_Rate | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Projectile_Types | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## Marker, SecondaryStructure, SpaceStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Reveal_During_Setup_Phase | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Anything_Else | todo | application not recorded | — | 650 | — | — |

## Marker, SecondaryStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ranged_Target_Z_Adjust | partial | external redirect targets (src/sim/tactical/projectiles.cpp); projectile defence sources (src/sim/tactical/session_world.cpp); missing: ordinary weapon and missile aiming | WPJ-42; WPJ-17 | 1760 | src/sim/tactical/projectiles.cpp; src/sim/tactical/session_world.cpp | — |

## Marker, SpaceProp, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Valid_Target | partial | craft, ship, station (src/presentation/ui/selection.cpp); loaded hardpoint opportunity candidates (src/sim/tactical/combat_algorithms.hpp); loaded non-team combat candidates (src/sim/tactical/combat_targeting.cpp); loaded direct attack targets after team promotion (src/sim/tactical/session_step_commands.cpp); loaded map props (src/units/unit_tables_decode.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects outside the simulated unit closure | WSU-13; R-08; WCC-25; WCC-25; WSU-13; WSU-13 | 650 | src/presentation/ui/selection.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_commands.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces. Loaded map props apply the same mouse admission before picking: neutral noncollidable fields pass a right click through to the move point; impassable asteroids retain contacts. WCC-25 applies effective valid-target admission (default true) to ship scan acquisition and non-direct retention/replacement. R-08 also rejects invalid effective types before turret pointing, priority and aim. WCC-25 direct assignment refuses invalid effective types without changing the previous combat target; outer command acceptance and movement remain separate.; basis: reviewed |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Abilities/Force_Healing_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Abilities/Force_Healing_Ability/Heal_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Abilities/Force_Healing_Ability/Heal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |
| Is_Decoration | todo | application not recorded | — | 650 | — | The Barrage proxy Dummy_Barrage_Target takes its dedicated load_unit branch and returns before load_selection; its authored Is_Decoration is not read. Decoration consumers for other classes are unchanged (WAD-38, WSU-13/21). Mixed ground/space, space carriers on MiscObject: Dummy_Barrage_Target, Space_Special_Weapon_Source_Marker. Mixed ground/space, space carriers on MiscObject: Dummy_Barrage_Target, Space_Special_Weapon_Source_Marker. |
| Unit_Abilities_Data/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Plasma_Grenade_Contamination, Radioactive_Contamination) |

## MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Remove_Upon_Death | applied | whole class (apps/viewer/src/live_session_death.cpp) | BP-34, PB-33, SP-01, SP-03, UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: auto |

## MiscObject, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | applied | space MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/abilities.cpp); space ships with BARRAGE (src/sim/tactical/session_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAB-03, WAB-05, WHE-31; WAD-38; AB-01, AB-04, BP-23, BP-24, WAB-03 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_abilities.cpp; src/units/unit_abilities.cpp | basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Fire_Weapon_When_Deployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Fire_Weapon_When_Undeployed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Is_Affected_By_Gravity_Control_Field | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Is_Squashable_By_Supercrusher | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| MaxJiggleDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| MinimumPushReturnDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Occlusion_Silhouette_Enabled | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Overall_Length | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Overall_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Tactically_Built_Child_Object_Persists | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Unit_Abilities_Data/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |
| Weather_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Abstract_Mobile_Defense_Unit, Empire_Mobile_Defense_Unit, Rebel_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit, Underworld_Mobile_Defense_Unit_Prologue) |

## MovementClassType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | todo | application not recorded | — | 650 | — | — |

## MusicEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Files | applied | whole class (src/presentation/audio/sfx.cpp) | AI-01, AI-02, R-09, S-28 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Loop | applied | whole class (src/presentation/audio/sfx.cpp) | A-01, AI-05, AI-50, S-28, U-01 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| MSS_Internal_Loop | todo | application not recorded | — | 650 | — | — |
| Volume_Percent | applied | whole class (src/presentation/audio/sfx.cpp) | AU-28, BA-40 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |

## Particle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Abilities/Force_Healing_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Abilities/Force_Healing_Ability/Heal_Amount | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Abilities/Force_Healing_Ability/Heal_Range | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bossk_Plasma_Grenade_Burn) |
| Dynamic_Transform_Delay_Seconds | todo | application not recorded | — | 650 | — | — |
| Dynamic_Transform_Seconds | todo | application not recorded | — | 650 | — | — |
| In_Midforeground | todo | application not recorded | — | 650 | — | — |
| In_Midground | todo | application not recorded | — | 650 | — | — |
| Is_Editor_Placed | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Splash_Wake_Lava) |

## Particle, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | applied | Particle (apps/viewer/src/battle_effects_prepare.cpp); whole class (src/skirmish/start.cpp) | WPJ-40; U-04 | — | apps/viewer/src/battle_effects_prepare.cpp; src/skirmish/start.cpp | basis: auto |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Ground_Base | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Priority_Set

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Priorities | applied | whole class (src/units/unit_priority.cpp) | PI-1, R-09 | — | src/units/unit_priority.cpp | basis: reviewed |
| Category_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| Hard_Point_Priorities | todo | application not recorded | — | 702 | — | loaded into the priority set; the ship-level hardpoint choice does not use it (&#35;702) |
| Property_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| Unit_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Description | todo | application not recorded | — | 650 | — | — |
| CategoryMask | todo | application not recorded | — | 650 | — | the projectile loader does not read it |
| Causes_Infection | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Infection_Projectile) |
| Explode_When_Reached_Target_Radius | partial | DEFAULT space projectiles, zero-offset target-terminating space rockets (src/sim/tactical/projectiles.cpp); missing: LIGHTSABER and MPTL_ROCKET, other rocket constructions | RFL-05, RFL-08, WAD-04 | 1072 | src/sim/tactical/projectiles.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; basis: reviewed |
| Friendly_Damage_Amount | todo | application not recorded | — | 650 | — | — |
| Internal_Damage_Type | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Remote_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Remote_Bomb. |
| Max_Secs_For_AE_Delayed_Damage | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/blast.cpp) | WAD-17, WAD-21; WAD-17 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Damage | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/blast.cpp) | WAD-18, WAD-21; WAD-01, WAD-02 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Dropoff | applied | whole class (src/sim/tactical/blast.cpp) | WAD-15, WAD-16 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Dropoff_Tiers | applied | whole class (src/sim/tactical/blast.cpp) | WAD-16 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Immune_Faction | applied | whole class (src/sim/tactical/blast.cpp) | WAD-09 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Range | applied | whole class (src/sim/tactical/blast.cpp); ship, station (src/sim/tactical/blast.cpp) | WAD-01, WAD-10, WAD-12; WAD-19 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Block_Chance_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_BobaFett_Blaster_Vs_ObiWan_Hero_Clash) |
| Projectile_Category | applied | ordinary space projectiles, zero-offset target-terminating space rockets (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | RFL-01, RFL-08, WAD-04; DG-22, EWW-10, MS-01, PD-30, PD-31 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Cause_Invulnerability_Duration_Frames | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Cause_Invulnerability_Max_Targets | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Cause_Invulnerability_On_Detonation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Cause_Invulnerability_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Cause_Invulnerability_Targets_Category_Mask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Combat_Mod | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_MAL_Carbonite_Missile) |
| Projectile_Combat_Mod_Category_Mask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_MAL_Carbonite_Missile) |
| Projectile_Combat_Mod_Duration | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_MAL_Carbonite_Missile) |
| Projectile_Combat_Mod_Value | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_MAL_Carbonite_Missile) |
| Projectile_Convert_Enemy_On_Detonation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Emperor_Instant_Dark_Side_Corrupt_Blast, Proj_Special_Mara_Corrupt_Blast) |
| Projectile_Convert_Enemy_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Emperor_Instant_Dark_Side_Corrupt_Blast, Proj_Special_Mara_Corrupt_Blast) |
| Projectile_Convert_Enemy_Targets_Category_Mask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Emperor_Instant_Dark_Side_Corrupt_Blast, Proj_Special_Mara_Corrupt_Blast) |
| Projectile_Damage_Delay_Secs | applied | whole class (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/session_step_combat.cpp) | WAD-26, WNO-32; WAD-26, WFO-14 | — | src/sim/tactical/blast.cpp; src/sim/tactical/session_step_combat.cpp | Positive explicit delay overrides area-distance delay with a separate synchronized draw per delivery.; basis: reviewed |
| Projectile_Damages_Random_Hard_Points | todo | application not recorded | — | 650 | — | — |
| Projectile_Disable_Engines_Duration | partial | nonnegative duration up to 3600 seconds (src/units/unit_combat.cpp); missing: negative (indefinite) duration | EN-08, EN-09 | 561 | src/units/unit_combat.cpp | The stock small-ion projectile authors 20 seconds; unsupported indefinite durations are rejected.; basis: reviewed |
| Projectile_Disables_Engines_When_Power_Drained | applied | whole class (src/sim/tactical/session_step_combat.cpp) | EN-07, EN-08, EN-09 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| Projectile_Does_Energy_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-06, DG-07, DG-09, EN-07, IR-02 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Does_Hitpoint_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-10, PD-05, WC-04, WCC-50 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Does_Shield_Damage | applied | whole class (src/units/unit_combat.cpp) | BP-19, DG-06, DG-09, DG-38, S-45 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Energy_Per_Shot | applied | whole class (src/units/unit_combat.cpp) | EN-05, EN-06, EWW-07, PD-24, PD-25 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Fire_Pulse_Count | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Bossk_Plasma_Grenade, Proj_Hand_Blaster_Bossk, Proj_Hand_Blaster_Merc, Proj_Hand_Disruptor_Cannon, Proj_MAL_Carbonite_Missile, Proj_MAL_Concussion_Missile) |
| Projectile_Fire_Pulse_Delay_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Bossk_Plasma_Grenade, Proj_Hand_Blaster_Bossk, Proj_Hand_Blaster_Merc, Proj_Hand_Disruptor_Cannon, Proj_MAL_Carbonite_Missile, Proj_MAL_Concussion_Missile) |
| Projectile_Fire_Recharge_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Bossk_Plasma_Grenade, Proj_Hand_Blaster_Bossk, Proj_Hand_Blaster_Merc, Proj_Hand_Disruptor_Cannon, Proj_MAL_Carbonite_Missile, Proj_MAL_Concussion_Missile) |
| Projectile_Grenade_Can_Lob_Slower | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. |
| Projectile_Ground_Detonation_Particle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Ground_Detonation_SurfaceFX | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Projectile_Instant_Heal_Duration_Frames | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Projectile_Instant_Heal_Health_Increase | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Instant_Heal_On_Detonation | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Instant_Heal_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Instant_Heal_Targets_Category_Mask | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Ion_Stun_Duration | applied | whole class (src/units/unit_combat.cpp) | DG-01, IR-04, IR-05, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Ion_Stun_On_Detonation | applied | whole class (src/units/unit_combat.cpp) | DG-01, IR-04, IR-05, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Ion_Stun_Radius | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. |
| Projectile_Ion_Stun_Shot_Rate_Reduction_Percent | applied | whole class (src/units/unit_combat.cpp) | DG-01, IR-04, IR-05, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Length | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-02, PB-02, PB-04, PB-14, R-ROT-01 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Max_Flight_Distance | applied | zero-offset target-terminating space rockets (src/sim/tactical/projectiles.cpp); whole class (src/units/unit_combat.cpp) | RFL-05, RFL-08; DG-23, EWW-03, G-03, P-04, PD-10 | — | src/sim/tactical/projectiles.cpp; src/units/unit_combat.cpp | basis: auto |
| Projectile_Max_Lifetime | partial | ordinary space projectiles (src/sim/tactical/projectiles.cpp); missing: grenades and land projectiles | RFL-08 | 1072 | src/sim/tactical/projectiles.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; basis: reviewed |
| Projectile_Max_Scan_Range | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Drunk_Missile, Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Ship_Concussion_Missile, Proj_Ship_Concussion_Missile_Bossk, Proj_Ship_Concussion_Missile_Dummy, Proj_Ship_Concussion_Missile_Satellite, Proj_Ship_Concussion_Missile_SSD (+6 more). Mixed ground/space, space carriers on Projectile: Proj_Drunk_Missile, Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Ship_Concussion_Missile, Proj_Ship_Concussion_Missile_Bossk, Proj_Ship_Concussion_Missile_Dummy, Proj_Ship_Concussion_Missile_Satellite, Proj_Ship_Concussion_Missile_SSD (+6 more). |
| Projectile_Redirect_Chance_Modifier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_BobaFett_Blaster_Vs_ObiWan_Hero_Clash) |
| Projectile_Rocket_Curve_Distance | partial | zero-offset target-terminating space rockets (src/sim/tactical/rocket.cpp); missing: other rocket constructions | RFL-03 | 1072 | src/sim/tactical/rocket.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; basis: reviewed |
| Projectile_Rocket_Curve_Offset | partial | zero-offset target-terminating space rockets (src/sim/tactical/rocket.cpp); missing: nonzero-offset rocket constructions | RFL-03 | 1072 | src/sim/tactical/rocket.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; basis: reviewed |
| Projectile_Rocket_Straight_Distance | partial | zero-offset target-terminating space rockets (src/sim/tactical/rocket.cpp); missing: other rocket constructions | RFL-02 | 1072 | src/sim/tactical/rocket.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; basis: reviewed |
| Projectile_Stun_Duration_Frames | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Stun_On_Detonation | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Stun_Radius | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Stun_Victims_Category_Mask | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Stun_Victims_Unit_Types | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. |
| Projectile_Target_Point_On_Terrain | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Infection_Projectile, Proj_Dark_Trooper_Missile, Proj_Ground_Proton_Torpedo, Proj_Ground_Proton_Torpedo_ZC_Turret, Proj_MAL_Carbonite_Missile, Proj_MAL_Concussion_Missile, Proj_Plex_Missile) |
| Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Weaken_Enemy_Duration_Seconds | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Weaken_Enemy_On_Detonation | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Weaken_Enemy_Radius | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Weaken_Enemy_Take_Damage_Increase_Percent | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Weaken_Enemy_Targets_Category_Mask | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Projectile_Width | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-04, BP-07, G-08, PB-04, PB-07 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |

## RadarMapSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Backdrop_Texture_Name | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_FOW_Color | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Land_Is_Guide_Rectangle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Use_Event_System | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Minimum_Destruction_Survivor_Count | todo | application not recorded | — | 650 | — | — |
| Secondary_Objective | todo | application not recorded | — | 650 | — | — |
| Shield_Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | whole class (src/sim/tactical/pads.cpp) | WBP-04 | — | src/sim/tactical/pads.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WBP-14 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| CategoryMask | applied | whole class (src/units/unit_combat.cpp) | R-09, WBP-01 | — | src/units/unit_combat.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Shield_Points | applied | whole class (src/units/unit_durability.cpp) | DG-06, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Shield_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-06, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Tactical_Respawn_Time_In_Secs | applied | whole class (src/sim/tactical/pads.cpp) | WBP-29, WBP-49, WHZ-52 | — | src/sim/tactical/pads.cpp | basis: reviewed |
| Targeting_Max_Attack_Distance | applied | whole class (src/units/unit_combat.cpp) | W-09, WBP-18 | — | src/units/unit_combat.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## SecondaryStructure, SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Living | partial | craft, ship, station (src/presentation/ui/selection.cpp); craft, ship, station (src/sim/tactical/blast.cpp); loaded tactical units (src/sim/tactical/combat_algorithms.hpp); loaded non-team combat candidates (src/sim/tactical/combat_targeting.cpp); loaded tactical units (src/sim/tactical/pads.cpp); missing: objects outside the simulated unit closure | WSU-13; WAD-14; R-08; WCC-25; WBP-50, WBP-51 | 649 | src/presentation/ui/selection.cpp; src/sim/tactical/blast.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/pads.cpp | Effective living-projectile admission defaults false for each loaded type. R-08 rejects noncollidable weapon opportunities, including hostile captured mining pads, before priority and aim. WBP-50/51 gates capture and palette candidates; WAD-14 gates blast recipients and WHZ-51 gates ordinary contact. Squadron records take the spawned team type flag, with independently admitted craft. WSU-13 mouse admission already uses false; objects outside the simulated closure remain deferred. WCC-25 independently rejects noncollidable ship-scan candidates before weapon/priority/range checks; team containers bypass that type check.; basis: reviewed |

## SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HardPoints | applied | whole class (src/scene/space_population.cpp) | DG-25, DG-39, E72-06, HD-21, HS-02 | — | src/scene/space_population.cpp | basis: auto |

## SecondaryStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ship_Class | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | none recorded | — | apps/viewer/src/world_ui_prepare.cpp | basis: auto |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Position | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station (+69 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station (+69 more). |
| Destruction_Survivors | todo | application not recorded | — | 650 | — | — |
| Modifies_Reveal_Range | todo | application not recorded | — | 650 | — | — |
| Reveal_Range_Modifier | todo | application not recorded | — | 650 | — | — |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Shutter_Shield, Ground_Gravity_Generator, Ground_Ion_Cannon, Ground_Magnepulse_Cannon, Hutt_Ground_Base_Shield (+8 more). Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Shutter_Shield, Ground_Gravity_Generator, Ground_Ion_Cannon, Ground_Magnepulse_Cannon, Hutt_Ground_Base_Shield (+8 more). |

## SFXEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_2D | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Is_3D | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | basis: auto |
| Is_Preset | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | basis: auto |
| Max_Instances | applied | whole class (src/presentation/audio/sfx.cpp) | AU-03, AU-05, BA-04, BA-05 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Max_Predelay | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Max_Volume | applied | whole class (src/presentation/audio/sfx.cpp) | AU-04, BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Min_Predelay | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Min_Volume | applied | whole class (src/presentation/audio/sfx.cpp) | AU-04, BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Overlap_Test | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | AU-03, AU-05, AU-33, BA-05, BA-23 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |
| Play_Count | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Play_Sequentially | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Post_Samples | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05, SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Pre_Samples | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05, SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Priority | applied | whole class (src/presentation/audio/sfx.cpp) | R-08, R-09, S-01, WCC-18, WTA-21 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Probability | applied | whole class (src/presentation/audio/sfx.cpp) | AI-06, AI-10, AI-53, AU-01, AU-02 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Samples | applied | whole class (src/presentation/audio/sfx.cpp) | BP-21, G-04, T-01, UA-08, V-03 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Use_Preset | applied | whole class (src/presentation/audio/sfx.cpp) | AU-01, AU-02, BA-02 | — | src/presentation/audio/sfx.cpp | basis: reviewed |
| Volume_Saturation_Distance | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | AU-06, AU-09, BA-11 | — | apps/viewer/src/battle_audio_events.cpp | basis: auto |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Sensor_Jamming_Ability/Duration_In_Secs | todo | application not recorded | — | 760 | — | — |
| Destroy_When_Child_Dies | applied | whole class (src/sim/tactical/session_step_economy.cpp) | WBP-27, WBP-28 | — | src/sim/tactical/session_step_economy.cpp | basis: reviewed |
| Passive_Missile_Shield_Radius | applied | passive space sources (src/sim/tactical/session_world.cpp) | WPJ-17 | — | src/sim/tactical/session_world.cpp | — |
| Projectile_Damage | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Turret_Rest_Angle | todo | application not recorded | — | 1075 | — | The scene's four defense satellites author this field on SpaceBuildable objects. Manual turret fields are read through load_hardpoint's manual-turret branch, not the buildable object body; this class's fields are unread. Existing HardPoint and other proven class rows are preserved (WAD-40, MC-06). |

## SpaceBuildable, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy_Fixed_Radius | todo | application not recorded | — | 650 | — | — |

## SpaceBuildable, SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Min_Attack_Distance | partial | ship beam targets (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-58 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |

## SpaceBuildable, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Inaccuracy_Distance | foc-ignores | application not recorded | — | — | — | DG-24 |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Target_Stealth_Units | applied | whole class (src/units/unit_combat.cpp) | WST-07, WST-08, WST-11 | — | src/units/unit_combat.cpp | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Silri, Urai_Fen, Urai_Fen_Prologue.; basis: reviewed |

## SpaceBuildable, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Heal_Percent | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |
| Abilities/Force_Healing_Ability/Single_Target_Heal | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Urai_Fen, Urai_Fen_Prologue. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+46 more). |

## SpaceBuildable, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 760 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | todo | application not recorded | — | 650 | — | — |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | todo | application not recorded | — | 650 | — | — |
| Is_Discardable | todo | application not recorded | — | 650 | — | — |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Death_By_TSW_Replacements | todo | application not recorded | — | 650 | — | — |
| Is_Asteroid_Field | partial | space tactical hazards (src/presentation/ui/minimap.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); selected-map asteroid fields (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp); map object (src/units/unit_tables_decode.cpp); missing: objects outside loaded tactical closure | WHZ-70, WHZ-72; WHZ-04, WHZ-08; WHZ-11, WHZ-12; WHZ-01; WHZ-01 | 924 | src/presentation/ui/minimap.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp | Loaded fields participate in center queries and independent per-frame damage rolls; flags remain independent of tracking category. The field flag is read in selected-map profiles; these hazard types are outside the pinned M2 table closure and recorded trace.; basis: reviewed; trace: unrecorded |
| Is_Ion_Storm | applied | space tactical hazards (src/presentation/ui/minimap.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); shield-bearing tactical combatants (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp); map object (src/units/unit_tables_decode.cpp) | WHZ-70, WHZ-72; WHZ-04, WHZ-08; WHZ-30, WHZ-31; WHZ-01; WHZ-01 | — | src/presentation/ui/minimap.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp | Selected-map independent storm flags feed shield-service center XY contacts; cached disable predicates preserve shield pools while gating absorption and mesh collision.; basis: reviewed; trace: unrecorded |
| Is_Nebula | applied | space tactical hazards (src/presentation/ui/minimap.cpp); whole class (src/scene/space_population.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); loaded tactical space units, craft and static obstacles (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp) | WHZ-70, WHZ-72; none recorded; WHZ-04, WHZ-08; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-01 | — | src/presentation/ui/minimap.cpp; src/scene/space_population.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp | Selected-map independent flags feed circular affected-unit service and the hard-box public fallback; team queries union member state.; basis: reviewed |

## SpaceProp, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Impassable_Asteroid | partial | craft, ship, station (src/presentation/ui/selection.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); map object (src/skirmish/inputs.cpp); loaded map props (src/units/unit_tables_decode.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: affected-unit environmental services | WSU-13; WHZ-04, WHZ-08; WHZ-01; WSU-13; WSU-13 | 923 | src/presentation/ui/selection.cpp; src/sim/tactical/pathfind.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp | Selected-map profiles retain independent flags; tracking applies one precedence category and fighter avoidance excludes field, storm and nebula categories. Loaded map props apply the same mouse admission before picking: neutral noncollidable fields pass a right click through to the move point; impassable asteroids retain contacts.; basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | applied | ship (src/units/unit_durability.cpp) | DG-10, PD-05, WC-04, WCC-46, WCC-50, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| CategoryMask | applied | ship (src/units/unit_combat.cpp) | PL-13, R-09, WHZ-50, WHZ-51 | — | src/units/unit_combat.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Collidable_By_Projectile_Living | applied | ship (src/sim/tactical/blast.cpp); ship (src/sim/tactical/combat_algorithms.hpp) | WAD-14, WHZ-50, WHZ-51; R-08, WHZ-50, WHZ-51 | — | src/sim/tactical/blast.cpp; src/sim/tactical/combat_algorithms.hpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Death_Projectiles | applied | whole class (src/sim/tactical/session_step_fighters.cpp) | WNO-29, WNO-30 | — | src/sim/tactical/session_step_fighters.cpp | basis: reviewed |
| Energy_Refresh_Rate | applied | ship (src/units/unit_durability.cpp) | DG-13, EN-02, PD-22, PD-26, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Property_Flags | applied | ship (src/units/unit_priority.cpp) | G-03, PL-13, R-09, WHZ-50, WHZ-51, WNO-09, WNO-10 | — | src/units/unit_priority.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it. WNO-10: the container's NotOpportunityTarget keeps it out of the automatic fire of every shooter with a priority set; a parent without a set scores it 1.0 (R-09), as in FoC.; basis: reviewed |
| Shield_Points | applied | ship (src/units/unit_durability.cpp) | DG-06, PD-02, PD-03, S-15, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Shield_Refresh_Rate | applied | ship (src/units/unit_durability.cpp) | DG-13, PD-08, PD-26, S-15, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

## SpaceStructure, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ranged_Target_Z_Adjust | applied | whole class (src/sim/tactical/combat_aim.cpp); whole class (src/sim/tactical/combat_fire.cpp); whole class (src/sim/tactical/projectiles.cpp); external redirect targets (src/sim/tactical/projectiles.cpp); ordinary death payloads (src/sim/tactical/session_step_fighters.cpp); projectile defence sources (src/sim/tactical/session_world.cpp) | WWP-19, WWP-50, WWP-72; WWP-16, WWP-72; MS-07, WWP-64, WWP-72; WPJ-42; WNO-29, WNO-30; WPJ-17 | — | src/sim/tactical/combat_aim.cpp; src/sim/tactical/combat_fire.cpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_fighters.cpp; src/sim/tactical/session_world.cpp | — |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Cluster_Bomb_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Cluster_Bomb_Ability/Bomb_Type | todo | application not recorded | — | 760 | — | — |
| Abilities/Cluster_Bomb_Ability/Detonation_Time_In_Secs | todo | application not recorded | — | 760 | — | — |
| Abilities/Cluster_Bomb_Ability/Number_Of_Bombs | todo | application not recorded | — | 760 | — | — |
| Abilities/Laser_Defense_Ability/@Name | applied | whole class (src/units/unit_tables_profiles.cpp) | WLD-01 | — | src/units/unit_tables_profiles.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Activation_Style | applied | whole class (src/units/unit_tables_profiles.cpp) | WLD-02 | — | src/units/unit_tables_profiles.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Defense_Duration_In_Secs | applied | whole class (src/units/unit_abilities.cpp) | WLD-02 | — | src/units/unit_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Dist_Travelled | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Types_Targeted | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04, WLD-08 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Protection_Radius | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Recharge_Time_In_Secs | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-03 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Leech_Shields_Ability/Activation_Max_Range | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Activation_Min_Range | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Frames | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Beam_Width | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Damage_Multiplier | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Duration_In_Secs | todo | application not recorded | — | 760 | — | — |
| Abilities/Leech_Shields_Ability/Shield_Damage_Per_Second | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Face_Target | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Fire_Continuation | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Fire_Delay | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Firing_Cone | todo | application not recorded | — | 760 | — | — |
| Damage_Type | applied | craft (src/units/unit_combat.cpp) | DG-12, WWP-48 | — | src/units/unit_combat.cpp | &#35;851 reassessment: the unit-authored tag supplies only its own Projectile_Types weapon (WWP-48); ship hardpoint shots use each hardpoint's Damage_Type, then its projectile's (DG-12). The checked ships do not author the unit tag; an absent baseline node is not proof of a failed read.; CHECK-842; basis: reviewed |
| Death_Leave_Hulk_Behind | todo | application not recorded | — | 650 | — | — |
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, PD-22, PD-26, WCC-46 | — | src/units/unit_durability.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. This includes craft energy regeneration (EN-02, AB-23); no craft-specific gap was established.; CHECK-842; basis: reviewed |
| Land_Bomber_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Minimum_Follow_Distance | partial | craft (src/units/unit_motion.cpp); missing: ship | EWSQ-29, FM-03, FM-04, WSQ-33 | 843 | src/units/unit_motion.cpp | the sim applies it to craft only; FoC's per-type read is unverified in the debug build; basis: auto |
| Moniker | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Out_Of_Combat_Defense_Adjustment | partial | craft (src/units/unit_motion.cpp); missing: ship | CF-04, CF-05, DG-05, FA-07, S-28 | 843 | src/units/unit_motion.cpp | the sim applies it to craft only; FoC's per-type read is unverified in the debug build; basis: auto |
| Should_Attacker_Hold_Fire_For_Special_Ability | todo | application not recorded | — | 650 | — | — |
| TSW_Attack_Distance_From_Target | todo | application not recorded | — | 650 | — | — |
| TSW_Power_Up_Countdown_Seconds | todo | application not recorded | — | 650 | — | — |
| TSW_Start_Distance_From_Target | todo | application not recorded | — | 650 | — | — |
| TSW_Start_Pos_Match_Targets_Z | todo | application not recorded | — | 650 | — | — |
| TSW_Z_Adjust_Relative_To_Target_Pos | todo | application not recorded | — | 650 | — | — |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | partial | SpaceUnit MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/session_world.cpp); missing: other space ability radii | WHE-32, WPJ-17 | 760 | src/sim/tactical/session_world.cpp | — |
| Unit_Abilities_Data/Unit_Ability/Is_Pulsing | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Max_Number_Of_Pulses | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Pulse_Frequency_Secs | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | applied | whole class (src/sim/tactical/abilities.cpp); whole class (src/sim/tactical/session_step_commands.cpp) | AB-45, WAB-35; AB-01, AB-40, AB-60, AB-68, WAB-30 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Ship ability profiles retain the flag for the ability service (AB-40).; CHECK-842; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_step_commands.cpp); space ships with BARRAGE (src/sim/tactical/session_step_commands.cpp); missing: Other ability kinds; nonzero spawned placement offsets; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62; WAD-38 | 1074 | src/sim/tactical/session_step_commands.cpp | BARRAGE applies its authored point Z offset. G8 binds spawned projectile types, countdowns, timed category-0 modifiers and status particles; nonzero spawned placement Z offsets remain unsupported.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Types | todo | application not recorded | — | 760 | — | — |
| Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override | partial | space ships with BARRAGE (src/sim/tactical/combat_aim.cpp); missing: other ability kinds | WAD-38 | 1074 | src/sim/tactical/combat_aim.cpp | The BARRAGE point interface applies this value; other point abilities remain gated.; basis: reviewed |
| Victory_Relevant | applied | whole class (src/skirmish/content.cpp) | WBF-35 | — | src/skirmish/content.cpp | The selected all-enemy-units condition applies ship/craft relevance; starbase-only retains its role filter.; basis: reviewed |

## SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Block_Chance | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | — |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Urai_Fen, Urai_Fen_Prologue. |
| Special_Weapon_Valid_Targets | partial | ship (src/units/unit_abilities.cpp); missing: special-weapon structures and other spawned objects | WAB-73 | 650 | src/units/unit_abilities.cpp | Applied as the Buzz_Droids object's victim categories; other users stay with combat tag coverage.; basis: reviewed |

## SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Property_Flags | applied | whole class (src/units/unit_priority.cpp) | G-03, PL-13 | — | src/units/unit_priority.cpp | basis: auto |

## SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Pulse_Count | applied | whole class (src/units/unit_combat.cpp) | none recorded | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Fire_Pulse_Delay_Seconds | applied | whole class (src/units/unit_combat.cpp) | none recorded | — | src/units/unit_combat.cpp | basis: auto |

## SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); missing: concrete nested-handler effects | WHE-09, WHE-10 | 936 | src/sim/tactical/abilities.cpp | WHE-09/10/12/14: typed handler infrastructure applies gates, exact-type filter precedence, scheduling and cleanup. Concrete effects and unknown default activation policies remain separate; no hero ability is enabled by metadata alone.; basis: reviewed |
| Abilities/Stealth_Ability/Activation_Style | applied | whole class (src/units/unit_abilities.cpp) | WST-02 | — | src/units/unit_abilities.cpp | basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Transition_Time | presentation-later | application not recorded | — | 1876 | — | WST-03/12: parsed and validated; it times only the tint fade, which is not drawn (the sub-object swap is immediate in FoC). |

## SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Shield_Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-06, DG-07, DG-09, DG-20, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/Activation_Max_Range | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Activation_Min_Range | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Damage_Per_Frame | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Fire_Frame | todo | application not recorded | — | 760 | — | — |
| Abilities/Super_Laser_Ability/Line_Width | todo | application not recorded | — | 760 | — | — |
| Defend_Mode_Energy_Regen_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Shield_Regen_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Weapon_Delay_Multiplier | todo | application not recorded | — | 650 | — | — |
| Fires_Forward | applied | whole class (src/units/unit_tables_profiles.cpp) | DG-24, G-08, OW-01, OW-02, OW-03, S-28, W-09, WWP-53 | — | src/units/unit_tables_profiles.cpp | basis: reviewed |
| Max_Thrust | partial | craft (apps/viewer/src/world_ui_groups.cpp); craft (src/units/unit_motion.cpp); missing: ship | WSU-34; E75-10, E75-14, E75-23, ESU-16, ESU-28 | 843 | apps/viewer/src/world_ui_groups.cpp; src/units/unit_motion.cpp | craft thrust drives flight and the squadron icon anchor; ship motion coverage remains incomplete; basis: auto |
| Spin_Away_On_Death | applied | craft (src/units/unit_motion.cpp) | UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Spin_Away_On_Death_Chance | applied | craft (src/units/unit_motion.cpp) | UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Spin_Away_On_Death_Time | applied | craft (src/units/unit_motion.cpp) | SP-05, UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Targeting_Stickiness_Time_Threshold | todo | application not recorded | — | 701 | — | read by a loader, but nothing applies the value Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+31 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+31 more). |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Base_Power_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Battlefield_Modifier_Ability/Reinforcement_Time_Multiplier | todo | application not recorded | — | 760 | — | — |
| Abilities/Battlefield_Modifier_Ability/Reverse_Application_Logic | todo | application not recorded | — | 760 | — | — |
| Base_Shield_Always_Off | todo | application not recorded | — | 650 | — | — |
| Base_Shield_Radius | todo | application not recorded | — | 650 | — | — |
| Can_Contain_Heroes_During_Ground_Battle | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Fine_Tune_Occupied_Passability | todo | application not recorded | — | 650 | — | — |
| Gravity_Control_Field_Range | todo | application not recorded | — | 650 | — | — |
| Gravity_Control_Field_Slow_Fraction | todo | application not recorded | — | 650 | — | — |
| Include_In_UI_Map_Header | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Garrison_Bunker_Capturable) |
| Is_Special_Weapon_In_Space | todo | application not recorded | — | 650 | — | — |
| Land_Victory_Relevant | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Lobbing_Superweapon_Chargeup_Frames | todo | application not recorded | — | 650 | — | — |
| Num_Garrison_Guns | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Place_At_Every_Specific_Marker_Position | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Place_Other_Type_At_Every_Specific_Marker_Position | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (E_Galactic_Turbolaser_Tower_Defenses, R_Galactic_Turbolaser_Tower_Defenses) |
| Prevents_Blockade_Run_Attrition | todo | application not recorded | — | 650 | — | — |
| Requires_Base_Power | todo | application not recorded | — | 650 | — | — |
| Should_Be_Destroyed_When_All_Hardpoints_Destroyed | applied | whole class (src/units/unit_durability.cpp) | E72-01, HD-21, IS-05, IS-06, WC-04 | — | src/units/unit_durability.cpp | basis: auto |
| Space_Victory_Relevant | todo | application not recorded | — | 650 | — | — |
| Spawn_Garrison_On_Load | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Special_Weapon_Index | todo | application not recorded | — | 650 | — | — |
| Special_Weapon_Target_Action_Index | todo | application not recorded | — | 650 | — | — |
| Weapon_Quantity | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Weapon_Type | todo | application not recorded | — | 650 | — | — |

## SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-09, WHE-10; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-14; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-14; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 937 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; basis: reviewed |

## SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/Activation_Style | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt. |
| Abilities/System_Spy_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt. |
| Abilities/System_Spy_Ability/Duration_In_Secs | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt. |
| Abilities/System_Spy_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88, Jabba_The_Hutt. |
| Abilities/System_Spy_Ability/See_Fleet_Contents | todo | application not recorded | — | 760 | — | — |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, IG-88. |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, IG-88. |
| Abilities/System_Spy_Ability/See_Most_Powerful_Ship | todo | application not recorded | — | 760 | — | — |
| Abilities/System_Spy_Ability/See_Num_Fleets | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Han_Solo, IG-88, Jabba_The_Hutt. |
| Unit_Abilities_Data/Unit_Ability/Active_By_Default | todo | application not recorded | — | 760 | — | — |

## SpeechEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Files | partial | English space tactical production speech (apps/viewer/src/battle_audio_events.cpp); missing: scripted/campaign speech and other languages | BA-73, BA-74, SND-44 | 1504 | apps/viewer/src/battle_audio_events.cpp | Stream execution applies reachable speech files and volume; Text_ID/movie lifecycle is separate.; basis: reviewed |
| Volume_Percent | partial | English space tactical production speech (apps/viewer/src/battle_audio_events.cpp); missing: scripted/campaign speech and other languages | BA-73, BA-74, SND-44 | 1504 | apps/viewer/src/battle_audio_events.cpp | Stream execution applies reachable speech files and volume; Text_ID/movie lifecycle is separate.; basis: reviewed |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | foc-ignores | application not recorded | — | — | — | &#35;851 reassessment: the check changes structural tables without changing the battle. AB-15 establishes why: non-team abilities use each craft's Mod_Multiplier, not the squadron's; team ability data instead comes from Create_Team_Type (AB-60).; AB-15; basis: reviewed |

## Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team_Type | partial | Squadron (apps/viewer/src/map_mode_hud.cpp); craft (src/sim/tactical/session_economy.cpp); craft, ship, squadron (src/skirmish/economy.cpp); missing: campaign and ground conversions | MM-15; WHE-30, WHE-63; WHE-49 | 934 | apps/viewer/src/map_mode_hud.cpp; src/sim/tactical/session_economy.cpp; src/skirmish/economy.cpp | The space company resolver selects the authored unique ship and its creation team; other modes remain separate. Loaded squadron radar independently applies the team type (MM-15).; basis: reviewed |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Available_In_Skirmish | todo | application not recorded | — | 650 | — | — |
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, PD-22, PD-26, WCC-46 | — | src/units/unit_durability.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| Victory_Relevant | applied | station (src/skirmish/content.cpp) | VE-06, VE-10, VT-03 | — | src/skirmish/content.cpp | &#35;851 reassessment: the default enemy-starbase-destroyed condition builds relevant targets only from stations (VT-01 to VT-03); no altered victory result occurs in the check.; CHECK-842; basis: reviewed |

## TacticalCamera

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bottom_Bounds_Buffer | todo | application not recorded | — | 650 | — | — |
| Distance_Default | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Max | applied | whole class (src/presentation/camera/camera.cpp) | C-6, CAM-1, CAM-2, CAM-4, V-1 | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Min | applied | whole class (src/presentation/camera/camera.cpp) | EAWR-CAMERA-0001 | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Smooth_Time | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Spline | applied | whole class (src/presentation/camera/camera.cpp) | SHA-256 | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Far_Clip | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Default | applied | whole class (src/presentation/camera/camera.cpp) | EAWR-CAMERA-0402 | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Max | applied | whole class (src/presentation/camera/camera.cpp) | EAWR-CAMERA-0402 | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Min | applied | whole class (src/presentation/camera/camera.cpp) | EAWR-CAMERA-0402 | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Smooth_Time | todo | application not recorded | — | 650 | — | — |
| Location_Follows_Terrain | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Location_Height_Down_Smooth_Time | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Location_Height_Smooth_Time | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Location_Height_Up_Smooth_Time | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Min_Height_Above_Terrain | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| MinHeightAboveTerrain | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Near_Clip | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Side_Bounds_Buffer | todo | application not recorded | — | 650 | — | — |
| Spline_Steps | todo | application not recorded | — | 650 | — | — |
| Tactical_Overview_Click_Time | applied | whole class (src/presentation/camera/overview.cpp) | CAM-1, CAM-2, V-1 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Clicks | applied | whole class (src/presentation/camera/overview.cpp) | CAM-1, CAM-2, V-1, V-1a | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Distance | applied | whole class (src/presentation/camera/overview.cpp) | CAM-3, V-2 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Distance2 | applied | whole class (src/presentation/camera/overview.cpp) | CAM-1, CAM-4, V-3 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_FOV | applied | whole class (src/presentation/camera/overview.cpp) | CAM-3, V-2 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_FOV2 | applied | whole class (src/presentation/camera/overview.cpp) | CAM-1, CAM-4, V-3 | — | src/presentation/camera/overview.cpp | basis: auto |
| Top_Bounds_Buffer | todo | application not recorded | — | 650 | — | — |
| Use_Splines | applied | whole class (src/presentation/camera/camera.cpp) | SHA-256 | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Armor_Type | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+12 more). |
| Can_Participate_In_Space_Battle | todo | application not recorded | — | 650 | — | — |
| CategoryMask | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Collidable_By_Projectile_Living | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Energy_Refresh_Rate | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Exit_Door_Angle_Degrees | todo | application not recorded | — | 650 | — | — |
| Exit_Door_Distance | todo | application not recorded | — | 650 | — | — |
| Fire_Category_Restrictions | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Fire_Cone_Height | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Fire_Cone_Width | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Is_Valid_Target | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Max_Thrust | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger (+6 more). |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger (+6 more). |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more). |
| Projectile_Types | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Property_Flags | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Shield_Points | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Shield_Refresh_Rate | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Tactical_Health | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Targeting_Max_Attack_Distance | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Use_Special_Submit_Rules | todo | application not recorded | — | 650 | — | — |
| Variant_Of_Existing_Type | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |
| Victory_Relevant | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| User_Bound_Max | todo | application not recorded | — | 650 | — | — |
| User_Bound_Min | todo | application not recorded | — | 650 | — | — |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Blast_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); missing: concrete nested-handler effects | WHE-09, WHE-10 | 936 | src/sim/tactical/abilities.cpp | WHE-09/10/12/14: typed handler infrastructure applies gates, exact-type filter precedence, scheduling and cleanup. Concrete effects and unknown default activation policies remain separate; no hero ability is enabled by metadata alone.; basis: reviewed |
| Abilities/Blast_Ability/Charge_Up_Seconds | todo | application not recorded | — | 760 | — | — |
| Abilities/Blast_Ability/Damage_Multiplier | todo | application not recorded | — | 760 | — | — |
| Abilities/Concentrate_Fire_Attack_Ability/Activation_Style | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Categories | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Types | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Target_Damage_Increase_Percent | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Max_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Min_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-09, WHE-10; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Damage_Per_Frame | partial | ship (src/sim/tactical/session_step_commands.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_step_commands.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Shield_Flare_Ability/Activation_Style | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Charge_Up_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Damage_Radius | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Abilities/Shield_Flare_Ability/Max_Damage_Per_Victim | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Auto_Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Converted_To_Enemy_Die_Time_Seconds | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Create_Team | partial | loaded local space battle (apps/viewer/src/battle_scoring.cpp); craft (src/skirmish/economy.cpp); missing: land companies and non-scoring team creation | WBF-46; WHE-49 | 650 | apps/viewer/src/battle_scoring.cpp; src/skirmish/economy.cpp | WHE-49 team conversion is applied to the selected unique space ship; results scoring remains independently scoped to loaded local space objects.; basis: reviewed |
| Deploys | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Fire_Category_Restrictions | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Hero_Ability | todo | application not recorded | — | 650 | — | — |
| Is_Affected_By_Gravity_Control_Field | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone) |
| Is_Decoration | applied | whole class (src/skirmish/start.cpp); UniqueUnit (src/units/unit_tables_profiles.cpp) | U-04; WSU-15, WSU-19, WSU-21 | — | src/skirmish/start.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| Is_Supercrusher | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Minimum_Follow_Distance | applied | craft (src/units/unit_motion.cpp) | EWSQ-29, FM-03, FM-04, WSQ-33 | — | src/units/unit_motion.cpp | basis: auto |
| MinimumPushReturnDistance | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Out_Of_Combat_Defense_Adjustment | applied | craft (src/units/unit_motion.cpp) | CF-04, CF-05, DG-05, FA-07, S-28 | — | src/units/unit_motion.cpp | basis: auto |
| Overall_Length | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Overall_Width | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Projectile_Damage | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Redirect_Damage_To_Teammates | partial | craft (src/sim/tactical/session_step_combat.cpp); missing: U-08: deleted-member/service races; mixed redirecting leaders are skipped by the recursion guard | WHE-64 | 942 | src/sim/tactical/session_step_combat.cpp | WHE-64: raw shares enter each recipient mode/armor route in roster order; ordinary leader damage is consumed, LuaDebugDamage additionally reaches the leader. Sparse ordered environmental routing preserves worker isolation.; basis: reviewed |
| Respawn_Whole_Team_When_Killed | todo | application not recorded | — | 650 | — | — |
| Turret_Targets_Anything_Else | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Unit_Abilities_Data/Unit_Ability/Bomb_Countdown_Seconds | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_step_commands.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_step_commands.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Flag | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Walk_Transition | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |
| Weather_Category | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Gargantuan_Battle_Platform, Gargantuan_Battle_Platform_Death_Clone, Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Disable_Reinforcement_Vulnerability | todo | application not recorded | — | 760 | — | — |
| Abilities/Battlefield_Modifier_Ability/Enable_Special_Weapons | todo | application not recorded | — | 760 | — | — |
| Abilities/Battlefield_Modifier_Ability/Reinforcement_Deploy_Time_Multiplier | todo | application not recorded | — | 760 | — | — |
| Abilities/Battlefield_Modifier_Ability/Stealth_Duration_Multiplier | todo | application not recorded | — | 760 | — | — |
| Abilities/Combat_Bonus_Ability/Ability_Recharge_Bonus_Percentage | todo | application not recorded | — | 760 | — | — |
| Abilities/Combat_Bonus_Ability/Activation_Style | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-09, WHE-10; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-14; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-14; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Fire_Range_Bonus_Percentage | todo | application not recorded | — | 760 | — | — |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Enable_Ability/Affects_All_Allies | todo | application not recorded | — | 760 | — | — |
| Abilities/Enable_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | — |
| Abilities/Enable_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | — |
| Abilities/Garrison_Upgrade_Ability/Additional_Garrison_Units | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND |
| Abilities/Weatherproof_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Weatherproof_Ability/Apply_To_All_Allies | todo | application not recorded | — | 760 | — | — |
| Is_Skirmish_Tactical_Super_Weapon | applied | whole class (src/skirmish/economy.cpp); whole class (src/skirmish/inputs.cpp) | WSS-30; WSS-30 | — | src/skirmish/economy.cpp; src/skirmish/inputs.cpp | Inherited authored flag gates space listing and authoritative human/AI purchase admission; it does not remove already placed objects or disable abilities.; basis: reviewed |
