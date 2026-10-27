# combat tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 573 | 573 |
| partial | 544 | 544 |
| todo | 2017 | 2017 |
| presentation-later | 0 | 0 |
| foc-ignores | 45 | 45 |
| deferred | 0 | 0 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 4 |
| multiplayer | 0 | 1 |
| **Total** | **3179** | **3184** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## AIPlayerType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LandFreeStoreScript | todo | application not recorded | — | 3776 | — | Land AI type selection missing; tactical_ai_loading fixes the mounted selection to space; basis: reviewed |
| Personality/Aggressiveness | todo | application not recorded | — | 737 | — | &#91;tactical-ai&#93; No AIPlayerType Personality reader under src/ or apps/.; basis: reviewed |
| Personality/Focus | todo | application not recorded | — | 737 | — | &#91;tactical-ai&#93; No AIPlayerType Personality reader under src/ or apps/.; basis: reviewed |
| Templates/Land | partial | land battle objects (src/script/foc/ai_data.hpp); missing: land battle consumers (see note) | none recorded | 3776 | src/script/foc/ai_data.hpp | Templates/Land read into land_templates and selected for land domain; land AI use not verified; basis: reviewed |
| Templates/Space | applied | whole class (src/script/foc/ai_data.cpp) | DG-24, DG-26, V-03, W-10 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## Audio

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Audio_Land_3D_Listener_Z_Pullback_Dist | todo | application not recorded | — | 3776 | — | no code reads Audio_Land_3D tags on main; space twin read in src/presentation/audio/sfx.cpp:431; basis: reviewed |
| Audio_Land_3D_Rolloff_Distance_Mod | todo | application not recorded | — | 3776 | — | no code reads Audio_Land_3D tags on main; space twin read in src/presentation/audio/sfx.cpp:431; basis: reviewed |
| Audio_Land_3D_Saturation_Distance_Mod | todo | application not recorded | — | 3776 | — | no code reads Audio_Land_3D tags on main; space twin read in src/presentation/audio/sfx.cpp:431; basis: reviewed |
| Default_MSS_DIG_DS_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |
| Default_MSS_DIG_DS_MIX_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | — |
| Delay_Between_Land_Base_Attack_Announcement_Seconds | todo | application not recorded | — | 3776 | — | no reader found; announcement timing not implemented; basis: reviewed |
| Music_Land_Battle_To_Ambient_Peace_Seconds | todo | application not recorded | — | 3776 | — | space twin read in apps/viewer/src/battle_audio_prepare.cpp:210; no land reader; basis: reviewed |
| Sustained_IO_MSS_DIG_DS_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | audio: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Sustained_IO_MSS_DIG_DS_FRAGMENT_CNT behavior remains unwired.; basis: reviewed |
| Sustained_IO_MSS_DIG_DS_MIX_FRAGMENT_CNT | todo | application not recorded | — | 650 | — | audio: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Sustained_IO_MSS_DIG_DS_MIX_FRAGMENT_CNT behavior remains unwired.; basis: reviewed |

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
| File | applied | whole class (src/data/xml.cpp) | C-01, L-03b, R-07 | — | src/data/xml.cpp | a registry file entry, read by the catalog's file lists; basis: reviewed |

## CommandBarComponent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blink_Duration | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the text and style structs (presentation/ui/hud_shell.cpp) but no renderer under apps/viewer or presentation/godot consumes it.; basis: reviewed |
| Blink_Rate | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the text and style structs (presentation/ui/hud_shell.cpp) but no renderer under apps/viewer or presentation/godot consumes it.; basis: reviewed |
| Default_Offset | partial | g_planet_fleet (apps/viewer/src/galactic/galactic_fleet_hud.cpp); g_ground_level_pips, g_planet_name, g_political_control, g_space_level_pips (apps/viewer/src/galactic/galactic_hud.cpp); missing: other command-bar components | WGUI-28, WGUI-39; WGUI-38, WGUI-39 | 653 | apps/viewer/src/galactic/galactic_fleet_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp | a data loader reads it; where the value goes is not traced; basis: reviewed |
| Default_Offset_Widescreen | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Only the ground HUD (apps/viewer/src/galactic/ground_hud.cpp) reads it; the space battle HUD does not.; basis: reviewed |
| Dialog_Scene | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the command-bar field table (data/ui/command_bar.cpp) but no HUD code reads this field (no Field:: use under src/ or apps/).; basis: reviewed |
| Disable_Darken | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the command-bar field table (data/ui/command_bar.cpp) but no HUD code reads this field (no Field:: use under src/ or apps/).; basis: reviewed |
| Disabled | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the command-bar field table (data/ui/command_bar.cpp) but no HUD code reads this field (no Field:: use under src/ or apps/).; basis: reviewed |
| Disabled_Darken | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Disabled_Offset | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the command-bar field table (data/ui/command_bar.cpp) but no HUD code reads this field (no Field:: use under src/ or apps/).; basis: reviewed |
| Group | partial | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); missing: other authored component groups | WGEP-141, WGEP-143 | 650 | apps/viewer/src/galactic/black_market_hud.cpp | The black-market widget derives its bounded choice capacity from authored BlackMarket group membership; other groups remain untraced.; basis: reviewed |
| Hidden | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Parsed into the command-bar field table (data/ui/command_bar.cpp) but no HUD code reads this field (no Field:: use under src/ or apps/).; basis: reviewed |
| Offset | partial | authored black-market menu and encyclopedia (apps/viewer/src/galactic/black_market_hud.cpp); g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); encyclopedia_back, encyclopedia_header_text, encyclopedia_text (apps/viewer/src/galactic/galactic_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_value (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); missing: other command-bar components | WGEP-141, WGEP-143; WGCH-151, WGCH-154; WGUI-70, WGUI-81; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-39; WGUI-96 | 650 | apps/viewer/src/galactic/black_market_hud.cpp; apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp | Applied to the overview planet-name template; other command-bar uses remain pending.; basis: reviewed |
| Outlined_Bar | todo | application not recorded | — | 650 | — | hud-radar: command_bar.cpp stores the flag, but no outlined_bar field consumer draws the authored outline. Previous context: a data loader reads it; where the value goes is not traced; basis: reviewed |
| Overlay2_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Overlay_Offset | applied | whole class (src/presentation/ui/hud_shell.cpp) | AB-08 | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Pixel_Align | partial | squadron identity icons (apps/viewer/src/world_ui_groups.cpp); missing: other command-bar component geometry | WSU-60 | 650 | apps/viewer/src/world_ui_groups.cpp | hud-radar: world_ui_prepare reads the component flag and draw_identity uses it for icon rectangles; other component geometry does not consume this field.; basis: reviewed |
| Right_Justified | partial | g_special_ability (apps/viewer/src/galactic/destruction_hud.cpp); GHUD-S01 command-bar components (apps/viewer/src/galactic/galactic_hud.cpp); g_planet_name (apps/viewer/src/galactic/galactic_hud.cpp); generic_flytext (src/presentation/godot/ui/hud_icon_text.hpp); missing: other command-bar components | WGCH-151, WGCH-154; WGUI-33, WGUI-34, WGUI-35, WGUI-80, WGUI-94; WGUI-96; HCB-64 | 650 | apps/viewer/src/galactic/destruction_hud.cpp; apps/viewer/src/galactic/galactic_hud.cpp; src/presentation/godot/ui/hud_icon_text.hpp | Applied to the overview planet-name template; other command-bar uses remain pending. a data loader reads it; where the value goes is not traced HCB-64 applies the shared XML readout style to caller-prepared generic_flytext; bounty accounting remains outside presentation.; basis: reviewed |
| Smooth_Bar | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Snap_Location | todo | application not recorded | — | 650 | — | hud-radar: command_bar.cpp stores the flag, but no snap_location field consumer applies authored snapping. Previous context: a data loader reads it; where the value goes is not traced; basis: reviewed |
| Tab | todo | application not recorded | — | 650 | — | hud-radar: command_bar.cpp stores the flag, but no tab field consumer implements the component tab behavior. Previous context: a data loader reads it; where the value goes is not traced; basis: reviewed |
| Toggle | todo | application not recorded | — | 650 | — | hud-radar: command_bar.cpp stores the flag, but no toggle field consumer applies the component toggle behavior. Previous context: a data loader reads it; where the value goes is not traced; basis: reviewed |
| Type | partial | shell and text-button HUD components (src/presentation/ui/hud_shell.cpp); missing: other command-bar component kinds | none recorded | 650 | src/presentation/ui/hud_shell.cpp | hud-radar: the parsed component type selects shell and text-button handling; remaining component kinds lack general type-driven presentation. No current behavior rule specifically covers the general Type dispatcher.; basis: reviewed |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Hack_Ability/Activation_Max_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Hack_Ability/Activation_Min_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Hack_Ability/Activation_Style | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Hack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Hack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Hack_Ability/Team_Member_With_Ability | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Activation_Style | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Initially_Enabled | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Weapon_Index | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Time_In_Seconds | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Mine_Type | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Number_Of_Mines | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Trigger_Radius | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Repair_Ability/Activation_Max_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Repair_Ability/Activation_Min_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Repair_Ability/Activation_Style | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Repair_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Repair_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Repair_Ability/Team_Member_With_Ability | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| CategoryMask | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/script/foc/tactical_ai_bindings.cpp); missing: tactical scene objects of this class | WAR-21, WAR-26; WAR-21, WAR-26 | 650 | src/campaign/auto_resolve_strength.cpp; src/script/foc/tactical_ai_bindings.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| ContainerArrangement | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Is_Squashable | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Pre_Lit | todo | application not recorded | — | 650 | — | vfx-models: The only literal reader is the galactic fleet scene adapter; tactical Container rendering does not read or apply this flag.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Damage_Percent_When_Activated | todo | application not recorded | — | 3776 | — | No reader anywhere on main; parse gap; todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |

## Container, GenericHeroUnit, GroundInfantry, GroundVehicle, HeroUnit, Indigenous_Unit, Mobile_Defense_Unit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Category | todo | application not recorded | — | 3776 | — | Land compatibility gate missing; only campaign AI selection (ai_selection_content.cpp:182) reads it; basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | applied | whole class (src/data/xml_merge.cpp); galactic current-type membership (src/data/xml_merge.cpp) | R-08, R-11; WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |

## Container, GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_FOW_Reveal_Range | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |

## Container, GenericHeroUnit, HeroUnit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 760 | — | abilities: Nested ability loaders iterate child elements but do not read or apply the SubObjectList attribute; list-selection semantics remain unwired. Previous context: Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+12 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+50 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Home_One, Houndstooth (+14 more). Mixed ground/space, space carriers on Container: Hutt_VWing_Squadron_Container, Red_Squadron_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar. Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+50 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Eclipse_Super_Star_Destroyer, Executor_Super_Star_Destroyer, Executor_Super_Star_Destroyer_No_Tractor_Beam, Home_One, Houndstooth (+14 more).; basis: reviewed |

## Container, GenericHeroUnit, MiscObject, Mobile_Defense_Unit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Type | applied | ROCKET_ATTACK space bodies (src/sim/tactical/abilities.cpp); UniqueUnit ENERGY_WEAPON and TRACTOR_BEAM (src/sim/tactical/session_cmd_abilities.cpp); REPLENISH_WINGMEN (src/sim/tactical/session_cmd_abilities.cpp); HUNT craft and lone locomotor units (src/sim/tactical/session_cmd_lanes.cpp); space MISSILE_SHIELD and SENSOR_JAMMING (src/units/unit_abilities.cpp); whole class (src/units/unit_abilities.cpp); space ships with BARRAGE (src/units/unit_abilities.cpp) | WRA-01; WHE-26, WHE-27, WHE-57, WHE-59, WHE-60; WHE-63; WAB-04, WAB-50, WAB-51, WAB-55, WAB-56; WHE-31, WHE-32, WPJ-17; DG-05, DG-06, FO-09, WSU-38; WAD-38 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp; src/sim/tactical/session_cmd_lanes.cpp; src/units/unit_abilities.cpp | G7 binds named beam handlers and services tracked targets.; basis: reviewed |

## Container, GenericHeroUnit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | applied | space MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/abilities.cpp); space ships with BARRAGE (src/sim/tactical/session_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAB-03, WAB-05, WHE-31; WAD-38; AB-01, AB-60, BP-23, BP-24, WAB-11 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_abilities.cpp; src/units/unit_abilities.cpp | basis: reviewed |

## Container, GenericHeroUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | applied | whole class (src/sim/tactical/abilities.cpp); whole class (src/sim/tactical/session_cmd_abilities.cpp) | AB-45, WAB-35; AB-01, AB-40, AB-60, AB-68, WAB-30 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | matched by the field's name; the loader is table-driven; basis: auto |

## Container, HeroUnit, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | todo | application not recorded | — | 650 | — | Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Captain_Piet, Chewbacca, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune (+15 more). Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, E_Ground_Turbolaser_Tower, Ground_Empire_Hypervelocity_Gun, Ground_Magnepulse_Cannon, R_Ground_Turbolaser_Tower, Template_Ground_Turbolaser_Tower, U_Ground_Turbolaser_Tower. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker. Mixed ground/space, space carriers on SpecialStructure: Defense_Satellite, E_Ground_Turbolaser_Tower, Ground_Empire_Hypervelocity_Gun, Ground_Magnepulse_Cannon, R_Ground_Turbolaser_Tower, Template_Ground_Turbolaser_Tower, U_Ground_Turbolaser_Tower. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+50 more). |

## Container, HeroUnit, SpaceBuildable, SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 760 | — | abilities: Nested ability loaders iterate child elements but do not read or apply the SubObjectList attribute; list-selection semantics remain unwired. Previous context: Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+10 more). Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+41 more). Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on UniqueUnit: Accuser_Star_Destroyer, Admonitor_Star_Destroyer, Admonitor_Star_Destroyer_No_Engine_Hardpoint, Arc_Hammer, Biggs_XWing, Eclipse_Super_Star_Destroyer, Escort_TIE_Fighter, Executor_Super_Star_Destroyer (+41 more).; basis: reviewed |

## Container, Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Max_Attack_Distance | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container.; basis: reviewed |

## Container, Marker, MultiplayerStructureMarker, SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 650 | src/campaign/live_setup.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Marker: AI Hint Zone Center, Attacker Entry Position, Attacker Setup Phase Reveal, Base Shield Structure Position, Captureable_Turret, Defender Setup Phase Reveal, Defending Forces Position, DEMO_CONTROLLER (+85 more). Mixed ground/space, space carriers on Marker: AI Hint Zone Center, Attacker Entry Position, Attacker Setup Phase Reveal, Base Shield Structure Position, Captureable_Turret, Defender Setup Phase Reveal, Defending Forces Position, DEMO_CONTROLLER (+85 more).; basis: reviewed |

## Container, Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Property_Flags | partial | live space bodies (src/units/unit_priority.cpp); missing: map-only or projectile instances outside live-body loading | G-03, PL-13 | 650 | src/units/unit_priority.cpp | tactical-ai: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## Container, SecondaryStructure, SpaceBuildable, SpecialStructure, Squadron, StarBase, TechBuilding

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Dummy | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |

## Container, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 760 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. Mixed ground/space, space carriers on Container: Red_Squadron_Container, Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. |

## Container, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Buzz_Droids_Ability/Activation_Max_Range | partial | squadron (src/sim/tactical/session_cmd_abilities.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-70 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | Planar ordinary attack-range test against the point; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activation_Min_Range | partial | squadron (src/sim/tactical/session_cmd_abilities.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-70 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | Exclusive planar minimum; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; load_buzz_droids_ability reads range, radius, damage and object fields but never Activation_Style; activation is always a command.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Activation_Time_In_Seconds | partial | squadron (src/units/unit_abilities.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-71 | 4532 | src/units/unit_abilities.cpp | Arming delay of the created droids, truncated to frames; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Damage_Radius | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-73 | 4532 | src/sim/tactical/session_step_combat.cpp | Strict reach to the victim's nearest standing hardpoint; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Enemy_Damage_Per_Second | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-72 | 4532 | src/sim/tactical/session_step_combat.cpp | Per-frame miscellaneous damage to every victim in reach; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Object_Type | partial | squadron (src/sim/tactical/session_cmd_abilities.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-71 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | The created droid object type; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Buzz_Droids_Ability/Own_Damage_Per_Second | partial | squadron (src/sim/tactical/session_step_combat.cpp); missing: craft (a script addressing one StarViper craft, WAB-70) | WAB-72 | 4532 | src/sim/tactical/session_step_combat.cpp | Per-frame self wear before the victims; the squadron team container's nested ability (AB-60).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Container, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Ion_Cannon_Shot_Attack_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The nested Abilities block is not in the kind table; ION_CANNON_SHOT runs from Unit_Abilities_Data (AB-61), so this block's style and filter never gate it.; basis: reviewed |
| Abilities/Ion_Cannon_Shot_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The nested Abilities block is not in the kind table; ION_CANNON_SHOT runs from Unit_Abilities_Data (AB-61), so this block's style and filter never gate it.; basis: reviewed |
| Abilities/Ion_Cannon_Shot_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The nested Abilities block is not in the kind table; ION_CANNON_SHOT runs from Unit_Abilities_Data (AB-61), so this block's style and filter never gate it.; basis: reviewed |

## Container, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Projectile_Types_Override | applied | space ships with BARRAGE (src/units/unit_combat.cpp); whole class (src/units/unit_tables_abilities_nested.cpp) | WAD-38; AB-60 | — | src/units/unit_combat.cpp; src/units/unit_tables_abilities_nested.cpp | Team ion shots and BARRAGE weapon overrides retain the normal projectile profile separately.; basis: reviewed |

## Container, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radar_Icon_Scale_Land | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Radar_Icon_Scale_Land behavior remains unwired. Previous context: scope label removed (&#35;4002): what a tactical battle does with this tag is not audited; basis: reviewed |

## Container, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | todo | application not recorded | — | 760 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. Mixed ground/space, space carriers on Container: Rogue_Squadron_Space_Container, TIE_Defender_Squadron_Container. |

## Container, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Lucky_Shot_Attack_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Lucky_Shot_Attack_Ability is not in the kind table and the sim has no LUCKY_SHOT handler.; basis: reviewed |
| Abilities/Lucky_Shot_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Lucky_Shot_Attack_Ability is not in the kind table and the sim has no LUCKY_SHOT handler.; basis: reviewed |
| Abilities/Lucky_Shot_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Lucky_Shot_Attack_Ability is not in the kind table and the sim has no LUCKY_SHOT handler.; basis: reviewed |

## Decal

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Category | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Decal objects are not loaded; no code under src/ or apps/ reads the Decal class.; basis: reviewed |
| Permanent | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Permanent behavior remains unwired.; basis: reviewed |
| UV_Slot | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored UV_Slot behavior remains unwired.; basis: reviewed |
| Z_Angle | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Z_Angle behavior remains unwired.; basis: reviewed |

## Decal, LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Intensity | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Decal objects are not loaded; no code under src/ or apps/ reads the Decal class.; basis: reviewed |

## Difficulty_Adjustment

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage_Multiplier | applied | whole class (src/sim/tactical/session_lifecycle.cpp) | WTA-46 | — | src/sim/tactical/session_lifecycle.cpp | The shooter's owner's level scales projectile, area and beam damage with the cause-damage mode; delayed blast damage keeps WAD-26's no-multiplier route.; basis: reviewed |
| Health_Multiplier | applied | whole class (src/sim/tactical/durability.cpp); whole class (src/sim/tactical/session_lifecycle.cpp) | WTA-46; WTA-46 | — | src/sim/tactical/durability.cpp; src/sim/tactical/session_lifecycle.cpp | The AI owner's level scales the maximum hull and every destroyable hardpoint's maximum health, at creation and on an ownership transfer (percentages restored).; basis: reviewed |
| Land_AI_Contrast_Multiplier | todo | application not recorded | — | 3776 | — | Parsed into the difficulty struct (ai_data.cpp:404); no land AI consumer; basis: reviewed |
| Land_AI_Goal_Cycle_Sleep_Duration | todo | application not recorded | — | 3776 | — | Parsed (ai_data.cpp:405); no land goal loop reads it; basis: reviewed |
| Land_Build_Time_Multiplier | todo | application not recorded | — | 3776 | — | Parsed (ai_data.cpp:406) only; no land build consumer; basis: reviewed |
| Shield_Multiplier | applied | whole class (src/sim/tactical/durability.cpp); whole class (src/sim/tactical/session_lifecycle.cpp) | WTA-46; WTA-46 | — | src/sim/tactical/durability.cpp; src/sim/tactical/session_lifecycle.cpp | The AI owner's level scales the maximum shield; the refresh rate is unscaled.; basis: reviewed |

## EnumDefinition

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | applied | whole class (src/units/unit_support.cpp) | G-03, U-02 | — | src/units/unit_support.cpp | basis: reviewed |

## Equations

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | applied | whole class (src/script/foc/ai_perception.cpp) | none recorded | 737 | src/script/foc/ai_perception.cpp | EquationSet::parse_new retains authored equation names/expressions and Engine::evaluate executes them through the tactical perception context; no current behavior rule names the wildcard equation tag.; basis: reviewed |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Allies | partial | fresh galactic players (src/campaign/starting_forces.cpp); missing: later relation changes and tactical team services | WGS-89 | 650 | src/campaign/starting_forces.cpp | Directional initial galactic relations apply; later relation changes and tactical team services remain separate.; basis: reviewed |
| Bombardment_Initial_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Number_Of_Salvos | todo | application not recorded | — | 650 | — | — |
| Bombardment_Projectile | todo | application not recorded | — | 650 | — | — |
| Bombardment_Salvo_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Shot_Delay | todo | application not recorded | — | 650 | — | — |
| Bombardment_Shots_Per_Salvo | todo | application not recorded | — | 650 | — | — |
| Create_Player_In_Multiplayer_Games | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |
| Debug_Ground_Structures | todo | application not recorded | — | 3776 | — | no reader on origin/main; faction table tag (engine-tags ET-T039); no rule found that retail ignores it on land; basis: reviewed |
| Enemies | partial | fresh galactic players (src/campaign/starting_forces.cpp); missing: later relation changes and tactical team services | WGS-89 | 650 | src/campaign/starting_forces.cpp | Directional initial galactic relations apply with enemy precedence; later relation changes and tactical team services remain separate.; basis: reviewed |
| Faction_Leader | partial | galactic victory (src/campaign/setup.cpp); missing: hero lifecycle | WGS-79 | 650 | src/campaign/setup.cpp | Victory metadata and copied-input predicates apply these fields; live scheduled returns and active-mode leader objects remain with the hero provider.; basis: reviewed |
| Faction_Leader_Company | partial | galactic victory (src/campaign/setup.cpp); missing: hero lifecycle | WGS-23, WGS-24 | 650 | src/campaign/setup.cpp | Victory metadata and copied-input predicates apply these fields; live scheduled returns and active-mode leader objects remain with the hero provider.; basis: reviewed |
| Faction_Super_Weapon | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Garrison_Reinforcement_Delay_Seconds | applied | whole class (src/skirmish/inputs.cpp) | FL-14 | — | src/skirmish/inputs.cpp | basis: reviewed |
| Ground_Base_Icon_Name | todo | application not recorded | — | 3776 | — | WIRING; no reader of the icon name on origin/main; basis: reviewed |
| Ground_Transport_Icon_Name | todo | application not recorded | — | 3776 | — | WIRING; no reader of the icon name on origin/main; basis: reviewed |
| Infantry_Icon_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Is_Neutral | applied | whole class (src/campaign/setup.cpp); whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/sim/tactical/session_lifecycle.cpp); whole class (src/skirmish/ai.cpp) | WGEP-03; WHZ-51, WWP-66; WHZ-51; none recorded | — | src/campaign/setup.cpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_lifecycle.cpp; src/skirmish/ai.cpp | Every combat session applies authored neutral factions to ordinary attack commands, ship and opportunity targeting, cursor classification and projectile contact; captured objects use the current owner relationship.; basis: reviewed |
| Is_Playable | applied | active human live campaign seats (src/campaign/live_setup.cpp); whole class (src/campaign/setup.cpp); whole class (src/skirmish/inputs.cpp); whole class (src/skirmish/start.cpp) | CGS-04; WGEP-03; SC-01; SSOWN-01, WSS-47 | — | src/campaign/live_setup.cpp; src/campaign/setup.cpp; src/skirmish/inputs.cpp; src/skirmish/start.cpp | basis: reviewed |
| Land_Ability_Targeting_Range_Overlay_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Ability_Targeting_Range_Overlay_RGBA | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Ability_Targeting_Range_Overlay_Scale_Factor | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Advisor_Hints | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited yet; basis: reviewed |
| Land_Area_Effect_Range_Overlay_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Area_Effect_Range_Overlay_RGBA | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Area_Effect_Range_Overlay_Scale_Factor | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Mode_Garrison_Selection_Blob_Material_Name | todo | application not recorded | — | 3776 | — | No reader on main; presentation only; basis: reviewed |
| Land_Mode_Selection_Blob_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Retreat_Begin_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Cancel_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Countdown_Color_RGBA | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Countdown_Seconds | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Countdown_Text_ID | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Enemy_Begin_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Not_Allowed_Reason_1_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Not_Allowed_Reason_2_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Not_Allowed_Reason_3_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Not_Allowed_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Pursue_Max_Speed_Mod_Factor | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Units_Damaged_Mod_Factor | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Skirmish_AI_Default_Forces | applied | whole class (src/skirmish/land_start.cpp); whole class (src/skirmish/land_start.cpp) | none recorded; none recorded | — | src/skirmish/land_start.cpp | No walk rule currently names this tag; reviewed L3 application creates the authored companies, with an explicit note when the optional tag is absent.; basis: reviewed |
| Land_Skirmish_Unit_Buy_Credits | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Skirmish_Unit_Cap_By_Player_Count | partial | land start output rules (src/skirmish/land_start.cpp); missing: live land economy | LC-46 | 1458 | src/skirmish/land_start.cpp | Land start emits the multiplayer cap rules; the movement-only land host does not yet run economic ledgers.; basis: reviewed |
| Land_Surrender_SFXEvent | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Music_Event_Land_Ambient_Super_Weapon | todo | application not recorded | — | 3776 | — | no land music event reader on main; basis: reviewed |
| Music_Event_Land_Battle_Super_Weapon | todo | application not recorded | — | 3776 | — | no land music event reader on main; basis: reviewed |
| Music_Event_Tactical_Land_Battle_Pending | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-26 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Primary_Enemy | partial | galactic victory (src/campaign/setup.cpp); missing: hero lifecycle | WGS-23, WGS-24 | 650 | src/campaign/setup.cpp | Victory metadata and copied-input predicates apply these fields; live scheduled returns and active-mode leader objects remain with the hero provider.; basis: reviewed |
| SFX_Event_Tactical_Land_Battle_Pending | partial | galactic semantic cue adapter (apps/viewer/src/galactic/galactic_audio.cpp); missing: session producer integration for this galactic notification | WGP-26 | 4532 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic adapter resolves and applies the authored cue on a semantic notification; the owning gameplay service has not published this notification yet.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| SFXEvent_HUD_Lost_Land_Battle | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_HUD_Lost_Land_Battle_Enemy_TSW_Present | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_HUD_Won_Land_Battle | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_HUD_Won_Land_Battle_Enemy_TSW_Present | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_Land_Base_Under_Attack_Announcement | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_Land_Invasion_Commencing | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Skirmish_Land_Bomber | todo | application not recorded | — | 3776 | — | Land support initialisation missing (LC-58); basis: reviewed |
| Space_Mode_Garrison_Selection_Blob_Material_Name | todo | application not recorded | — | 3776 | — | Name says space mode but row is land; no reader. Owner may reclassify if space-only; basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Attacker | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Attacker_Conditional_Or | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Attacker_Last_Location | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Defender | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Defender_Conditional_Or | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Defender_Last_Location | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Raid_Attacker | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| SpeechEvent_Tactical_Intro_Land_Raid_Defender | todo | application not recorded | — | 3776 | — | only the space twin is read (src/campaign/space_entry_stock.cpp:387); basis: reviewed |
| Standalone_Space_Maps_Special_Weapon_A | applied | whole class (src/skirmish/start.cpp) | WSW-01, WSW-02 | — | src/skirmish/start.cpp | basis: reviewed |
| Standalone_Space_Maps_Special_Weapon_B | applied | whole class (src/skirmish/start.cpp) | WSW-01, WSW-02 | — | src/skirmish/start.cpp | basis: reviewed |

## FunctionSet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Function | applied | whole class (src/script/foc/ai_data.cpp) | FT-01, GS-02, L-10, L-20 | — | src/script/foc/ai_data.cpp | basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_LandAreaThreatScaleFactor | todo | application not recorded | — | 3776 | — | Documented only; no code reads it on main; basis: reviewed |
| AI_LandEvaluatorRegionSize | todo | application not recorded | — | 3776 | — | Copied into a Lua global only (ai_data.cpp:291); no land evaluator reads it; basis: reviewed |
| AI_LandThreatDistanceFactor | todo | application not recorded | — | 3776 | — | Documented only; no code reads it on main; basis: reviewed |
| AI_LandThreatLookAheadTime | todo | application not recorded | — | 3776 | — | Documented only; no code reads it on main; basis: reviewed |
| AI_LandThreatTurnRateFactor | todo | application not recorded | — | 3776 | — | Documented only; no code reads it on main; basis: reviewed |
| AIUsesFogOfWarLand | todo | application not recorded | — | 3776 | — | Retail reads it for land fog policy (WLAI-46); no land reader on main; basis: reviewed |
| Allow_Reinforcement_Percentage_Normalized | todo | application not recorded | — | 650 | — | &#91;production-tech&#93; Kept in the constants table (unit_support.cpp combat/economy scalars) but no consumer reads it; reinforcement percentages are not normalised.; basis: reviewed |
| ApproximationForwardDistance | todo | application not recorded | — | 650 | — | &#91;movement&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| ApproximationSmoothCosAngle | todo | application not recorded | — | 650 | — | &#91;movement&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Armor_Types | foc-ignores | application not recorded | — | — | — | The pair table comes from Damage_To_Armor_Mod; the list is not read (WCC-46, WCC-50, DG-12).; debug build: the Armor_Types and Damage_Types lists are not read; the damage-to-armor pair table is (rules WCC-46, WCC-50 in docs/behaviour/walks/capital-combat.md); basis: reviewed |
| Asteroid_Field_Damage | applied | eligible tactical frigates and capitals (src/sim/tactical/session_step_systems.cpp) | WHZ-11, WHZ-12 | — | src/sim/tactical/session_step_systems.cpp | basis: reviewed |
| Asteroid_Field_Damage_Rate | applied | eligible tactical frigates and capitals (src/sim/tactical/session_step_systems.cpp) | WHZ-11, WHZ-12 | — | src/sim/tactical/session_step_systems.cpp | basis: reviewed |
| Base_Land_Targeting_Arc_Angle_Coefficient | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Base_Shield_Delay_Time | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Base_Shield_Vulnerability_Modifier | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Battle_Pending_Timeout_Seconds | applied | multiplayer galactic battle vote (src/campaign/auto_resolve_live.cpp) | WGM-176, WGM-47 | — | src/campaign/auto_resolve_live.cpp | GCMP-06 converts mounted seconds to session ticks; the agreed conflict service defaults missing votes after the strict timeout.; basis: reviewed |
| Blockade_Run_Attrition_Factor | todo | application not recorded | — | 3776 | — | no reader in src/include/apps/tests; only FoC debug-build retreat coordinator per input note; basis: reviewed |
| Bombardment_Distribution | todo | application not recorded | — | 3776 | — | Synchronised strike scatter missing (LC-67); basis: reviewed |
| Bombardment_Offset | todo | application not recorded | — | 3776 | — | Firing offset missing (LC-67); basis: reviewed |
| Bribery_Fleet_Reveal_Range | todo | application not recorded | — | 4005 | — | Bribe tuning (reveal range or planet-state price modifier) is not read; the bribe path does not apply it.; basis: reviewed |
| Control_Point_Domination_Victory_Time_In_Secs | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/; no control-point domination victory exists.; basis: reviewed |
| Damage_To_Armor_Mod | applied | whole class (src/units/unit_support.cpp) | DG-06, DG-10, DG-12, DG-20 | — | src/units/unit_support.cpp | basis: reviewed |
| Damage_Types | foc-ignores | application not recorded | — | — | — | The pair table comes from Damage_To_Armor_Mod; the list is not read (WCC-46, WCC-50, DG-12).; debug build: the Armor_Types and Damage_Types lists are not read; the damage-to-armor pair table is (rules WCC-46, WCC-50 in docs/behaviour/walks/capital-combat.md); basis: reviewed |
| Default_Defense_Adjust | todo | application not recorded | — | 2006 | — | The auto-resolve fire steps do not read the default defence adjustment.; basis: reviewed |
| Default_Hero_Respawn_Time | partial | live galactic ability consumption (src/campaign/live_setup.cpp); campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WGCH-70, WGM-79, WGM-80; WGM-78, WGM-80 | 1116 | src/campaign/live_setup.cpp; src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services. Live ability consumption also sources these values for the shared saved hero-return queue.; basis: reviewed |
| Depleted_Shield_Damage_Increment | applied | whole class (src/units/unit_durability.cpp) | BP-19, BP-21, DG-06, DG-08, DG-09 | — | src/units/unit_durability.cpp | basis: auto |
| Depleted_Shield_Disable_Time | applied | whole class (src/units/unit_durability.cpp) | AB-17, DG-08 | — | src/units/unit_durability.cpp | basis: auto |
| DesiredLandFOWCellSize | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Diminishing_Firepower | applied | whole class (src/units/unit_durability.cpp) | DG-05, DG-26 | — | src/units/unit_durability.cpp | basis: auto |
| DynamicLandQuotaResetInterval | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Elevated_Vulnerability_Duration | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Elevated_Vulnerability_Factor | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Energy_Beam_Frames | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| EnergyRechargeIntervalInSecs | applied | whole class (src/sim/tactical/damage.cpp) | DG-13, EN-02 | — | src/sim/tactical/damage.cpp | basis: auto |
| EnergyToShieldExchangeRate | applied | whole class (src/units/unit_durability.cpp) | DG-13, DG-15, EN-04 | — | src/units/unit_durability.cpp | basis: auto |
| First_Strike_Extra_Damage_Percent | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Fleeing_Infantry_Speed_Bonus | todo | application not recorded | — | 3776 | — | GameConstants tag with no land-path reader.; basis: reviewed |
| Fleet_Maintenance_Update_Delay_Seconds | foc-ignores | application not recorded | — | — | — | no getter reference in the debug build; galactic fleet maintenance service (docs/behaviour/walks/gc-movement-control.md); basis: reviewed |
| Force_Ability_Disable_Time | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| FramesPerPositionApproximationRebuild | todo | application not recorded | — | 650 | — | &#91;movement&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Good_Ground_Color_Tint | todo | application not recorded | — | 3228 | — | WLEN-36 tint; no loader or land consumer; basis: reviewed |
| GUI_Move_Acknowledge_Scale_Land | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Hardpoint_Recharge_Cutoff_For_Opportunity_Fire | applied | whole class (src/sim/tactical/combat_fire.cpp); whole class (src/units/unit_combat.cpp) | R-01; R-01 | — | src/sim/tactical/combat_fire.cpp; src/units/unit_combat.cpp | basis: reviewed |
| Health_Critical_Percent_Threshold | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp) | SND-33, WCC-54; SND-33, WCC-54 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp | basis: reviewed |
| Health_Low_Percent_Threshold | applied | whole class (apps/viewer/src/battle_audio_events.cpp); whole class (apps/viewer/src/battle_audio_prepare.cpp); whole class (src/sim/tactical/durability.cpp) | SND-33, WCC-54; SND-33, WCC-54; AB-42, T-07 | — | apps/viewer/src/battle_audio_events.cpp; apps/viewer/src/battle_audio_prepare.cpp; src/sim/tactical/durability.cpp | basis: reviewed |
| High_Ground_Color_Tint | todo | application not recorded | — | 3228 | — | WLEN-36 tint; no loader or land consumer; basis: reviewed |
| Hull_Vs_Hard_Points_Health_Constraint | applied | whole class (src/sim/tactical/durability.cpp) | HD-21, HS-02, S-45 | — | src/sim/tactical/durability.cpp | basis: auto |
| Infantry_Ground_Color_Tint | todo | application not recorded | — | 3228 | — | WLEN-36 tint; no loader or land consumer; basis: reviewed |
| InfantryFormationRecruitmentDistance | todo | application not recorded | — | 3776 | — | GameConstants tag with no land-path reader.; basis: reviewed |
| InfantryTurnBlendTime | todo | application not recorded | — | 3776 | — | GameConstants tag with no land-path reader.; basis: reviewed |
| Ion_Storm_Shield_Disable_Time | applied | shield-bearing tactical combatants (src/sim/tactical/damage.cpp); mesh collision targets (src/sim/tactical/projectiles.cpp); shield-bearing tactical combatants (src/sim/tactical/session_step_systems.cpp); GameConstants (src/units/unit_durability.cpp) | WHZ-32; WHZ-33; WHZ-30, WHZ-31; WHZ-31 | — | src/sim/tactical/damage.cpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_systems.cpp; src/units/unit_durability.cpp | basis: reviewed |
| Land_Auto_Resolve_Delay_Seconds | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Base_Destruction_Forces_Retreat | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Capture_Allowed_Countdown_Seconds | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Collidable_Grid_Cull_Size | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Land_Guard_Range | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited yet; basis: reviewed |
| Land_Health_Bar_Scale | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Land_Retreat_Allowed_Countdown_Seconds | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Retreat_Attrition_Factor | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Landbase_Damage_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Landbase_Health_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| LandDestinationProximity | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| LandFOWColor | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| LandFOWRegrowTime | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| LandObjectTrackingInterval | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| LandObjectTrackingTreeCount | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| LandPredictionTimeInterval | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| LandTemporaryDestinationProximity | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| LandWaitOperatorSpeedCoefficient | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Lava_Ground_Color_Tint | todo | application not recorded | — | 3228 | — | WLEN-36 tint; no loader or land consumer; basis: reviewed |
| Max_Bombard_Interval_Seconds | todo | application not recorded | — | 2007 | — | Planetary bombardment timing is not modelled in the galactic layer.; basis: reviewed |
| Max_Ground_Forces_On_Planet | partial | explicit galactic control inputs (src/campaign/control.cpp); uncontested galactic landing (src/sim/galactic/control.cpp); missing: defended invasion admission, other ground capacity consumers | WGM-39, WGM-60; WGM-39 | 4532 | src/campaign/control.cpp; src/sim/galactic/control.cpp | src/data/galactic/galactic_world.cpp&#35;load_constants parses the authored quantity; src/campaign/control.cpp&#35;bind_control_data binds it and src/sim/galactic/control.cpp&#35;execute_landing applies surface capacity. Defended invasion and other ground consumers remain explicit seams.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Max_Remote_Bombs_Per_Player | todo | application not recorded | — | 3776 | — | Remote bomb cap missing (WLAB-39, G6); no reader on main; basis: reviewed |
| MaximumSpecialStructuresLand | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| MaxLandFormationFormupFrames | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MaxObstacleCostLand | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Melee_Cutoff_Range | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_combat.cpp) | WWP-27; WWP-27 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_combat.cpp | The inclusive type attack-distance cutoff suppresses the target craft container radius in projectile travel; it is read in the space hardpoint launch path too.; basis: reviewed |
| Min_Bombard_Interval_Seconds | todo | application not recorded | — | 2007 | — | Planetary bombardment timing is not modelled in the galactic layer.; basis: reviewed |
| MinLandPredictionDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MinObstacleCostLand | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MP_Default_Land_Tactical_Win_Condition | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Nebula_Ability_Disable_Time | applied | loaded tactical space units, craft and static obstacles (src/sim/tactical/session_step_systems.cpp); GameConstants (src/units/unit_motion.cpp) | WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-21 | — | src/sim/tactical/session_step_systems.cpp; src/units/unit_motion.cpp | basis: reviewed |
| Object_Max_Health_Multiplier_Land | applied | whole class (src/units/unit_durability.cpp) | none recorded | — | src/units/unit_durability.cpp | BF-2 (&#35;3779): a land table reads this spelling through the domain selector (src/units/domain_tags.cpp&#35;domain_tag and domain_variant); no land battle runs yet, so no rule mentions it.; basis: reviewed |
| Object_Max_Health_Multiplier_Space | applied | M2 squadron containers (apps/sim_headless/scenario_run.cpp); whole class (src/units/unit_durability.cpp) | WSQ-60; G-07 | — | apps/sim_headless/scenario_run.cpp; src/units/unit_durability.cpp | Container Tactical_Health and Shield_Points are absent from the loaded stock XML; WSQ-60 applies the native hull default to traces with the space multiplier.; basis: auto |
| Object_Max_Speed_Multiplier_Land | partial | land company members (include/eawr/skirmish/land_content.hpp); whole class (src/units/unit_motion.cpp); missing: GameConstants objects outside adapted land companies | MV-01; none recorded | 1450 | include/eawr/skirmish/land_content.hpp; src/units/unit_motion.cpp | BF-2 (&#35;3779): a land table reads this spelling through the domain selector (src/units/domain_tags.cpp&#35;domain_tag and domain_variant); no land battle runs yet, so no rule mentions it. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Occlusion_Silhouettes_Enabled | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Occlusion_Silhouettes_Enabled behavior remains unwired.; basis: reviewed |
| Override_Death_Persistence_Duration | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Override_Death_Persistence_Duration behavior remains unwired.; basis: reviewed |
| Quickmatch_Map_Exclusion_List | multiplayer | application not recorded | — | — | — | debug build: read only by the quickmatch map-name lookup (online matchmaking) |
| Raid_Force_Free_Object_Category_Mask | applied | whole class (src/campaign/fleets.cpp) | WGM-37 | — | src/campaign/fleets.cpp | Effective raid policy binds faction identities, hero and limited caps, and authored category membership; the shared pure predicate consumes current transport payloads.; basis: reviewed |
| Raid_Force_Limited_Object_Category_Mask | applied | whole class (src/campaign/fleets.cpp) | WGM-37 | — | src/campaign/fleets.cpp | Effective raid policy binds faction identities, hero and limited caps, and authored category membership; the shared pure predicate consumes current transport payloads.; basis: reviewed |
| Raid_Force_Max_Heros | applied | whole class (src/campaign/fleets.cpp) | WGM-37 | — | src/campaign/fleets.cpp | Effective raid policy binds faction identities, hero and limited caps, and authored category membership; the shared pure predicate consumes current transport payloads.; basis: reviewed |
| Raid_Force_Max_Limited_Objects | applied | whole class (src/campaign/fleets.cpp) | WGM-37 | — | src/campaign/fleets.cpp | Effective raid policy binds faction identities, hero and limited caps, and authored category membership; the shared pure predicate consumes current transport payloads.; basis: reviewed |
| Sensor_Jamming_Time | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Sensor_Jamming_Time behavior remains unwired.; basis: reviewed |
| SetupPhaseCountdownSeconds | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored SetupPhaseCountdownSeconds behavior remains unwired.; basis: reviewed |
| SetupPhaseEnabled | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored SetupPhaseEnabled behavior remains unwired.; basis: reviewed |
| ShieldRechargeIntervalInSecs | applied | whole class (src/units/unit_durability.cpp) | DG-13 | — | src/units/unit_durability.cpp | basis: auto |
| ShouldInfantryTeamsSplitAcrossFormations | todo | application not recorded | — | 3776 | — | GameConstants tag with no land-path reader.; basis: reviewed |
| ShouldSkipLandFormup | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Skirmish_Reinforcement_Delay_Frames | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Skirmish_Reinforcement_Delay_Frames behavior remains unwired.; basis: reviewed |
| Slow_Ground_Color_Tint | todo | application not recorded | — | 3228 | — | WLEN-36 tint; no loader or land consumer; basis: reviewed |
| Solo_Attack_Range | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Space_Elevated_Vulnerability_Duration | applied | whole class (src/sim/tactical/session_step_combat.cpp) | WR-41 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| Space_Elevated_Vulnerability_Factor | applied | whole class (src/sim/tactical/session_step_combat.cpp) | WR-41 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| SpecialAlignedOperatorBonus | todo | application not recorded | — | 650 | — | other: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored SpecialAlignedOperatorBonus behavior remains unwired.; basis: reviewed |
| Starbase_Damage_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Starbase_Health_Quadratic | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Strategic_Edge_Scroll_Region | todo | application not recorded | — | 1157 | — | Galactic map camera scroll tuning is not read; the map camera uses its own speeds.; basis: reviewed |
| Strategic_Offscreen_Scroll_Region | todo | application not recorded | — | 1157 | — | Galactic map camera scroll tuning is not read; the map camera uses its own speeds.; basis: reviewed |
| SyncedFrameInterval | todo | application not recorded | — | 650 | — | other: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored SyncedFrameInterval behavior remains unwired.; basis: reviewed |
| Tactical_Edge_Scroll_Region | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Tactical_Offscreen_Scroll_Region | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| TacticalEdgeScrollRegion | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| TacticalOffscreenScrollRegion | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Terrain_Resurface_Rand | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Terrain_Resurface_Rand behavior remains unwired.; basis: reviewed |
| Terrain_Resurface_Tolerance | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Terrain_Resurface_Tolerance behavior remains unwired.; basis: reviewed |
| Under_Construction_Damage_Multiplier | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Under_Construction_Damage_Multiplier behavior remains unwired.; basis: reviewed |
| Use_Reinforcement_Points | todo | application not recorded | — | 3776 | — | Land placement enable gate missing (LC-44); basis: reviewed |
| Value | todo | application not recorded | — | 650 | — | other: No GameConstants Value reader exists; generic local variables or XML attributes named value are unrelated.; basis: reviewed |
| XYExpansionDistanceLand | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |

## GameConstants, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Grenade_Gravity | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Grenade_Gravity behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb.; basis: reviewed |
| Projectile_Grenade_Gravity_Lob_Mod | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Grenade_Gravity_Lob_Mod behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb.; basis: reviewed |

## GameXMLFiles

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| File/@filename | todo | application not recorded | — | 650 | — | other: No GameXMLFiles File filename-attribute reader exists; catalog loading does not implement this manifest attribute.; basis: reviewed |
| File/@metafile | todo | application not recorded | — | 650 | — | other: No GameXMLFiles File metafile-attribute reader exists; catalog loading does not implement this manifest attribute.; basis: reviewed |
| File/@type | todo | application not recorded | — | 650 | — | other: No GameXMLFiles File type-attribute reader exists; catalog loading does not implement this manifest attribute.; basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Initially_Enabled | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Enhance_Defense_Ability/Activation_Style | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Enhance_Defense_Ability/Apply_To_All_Allies | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Enhance_Defense_Ability/Enhancement_Percentage | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC space scene (src/campaign/space_rules.cpp) | WAR-26; WGT-49, WGT-50, WGT-51 | — | src/campaign/auto_resolve_strength.cpp; src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Enhance_Defense_Ability/Unit_Strength_Category | todo | application not recorded | — | 650 | — | abilities: Only galactic_types.cpp reads this field; tactical CombatBonus/BattlefieldBonus construction does not retain or apply the strength-category selector. Previous context: applied only by the galactic auto-resolve (src/campaign/auto_resolve_*); no tactical code reads it, and whether FoC's tactical battle does is unverified (&#35;4002); basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Major_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Galactic bounty-hunter (GenericHeroUnit) neutralization: the GC prepare path accepts a minor or major target only when the actor's CAN_NEUTRALIZE flag is set (ability_neutralize.cpp, valid = named ? can_major : can_minor), with the flags parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none).; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Minor_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Galactic bounty-hunter (GenericHeroUnit) neutralization: the GC prepare path accepts a minor or major target only when the actor's CAN_NEUTRALIZE flag is set (ability_neutralize.cpp, valid = named ? can_major : can_minor), with the flags parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none).; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Owner_Respawn_Time_In_Secs | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-21 | — | src/sim/galactic/ability_neutralize.cpp | GC neutralization samples the actor and target absence times from the authored OWNER/TARGET_RESPAWN_TIME_IN_SECS ranges in prepare_neutralization (ability_neutralize.cpp absence_sample), parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none).; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Target_Respawn_Time_In_Secs | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-21 | — | src/sim/galactic/ability_neutralize.cpp | GC neutralization samples the actor and target absence times from the authored OWNER/TARGET_RESPAWN_TIME_IN_SECS ranges in prepare_neutralization (ability_neutralize.cpp absence_sample), parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Remote bomb placement and detonation absent on land; no handler code.; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Remote bomb placement and detonation absent on land; no handler code.; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Bomb_Type | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/System_Spy_Ability/Activation_Style | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Causes_Despawn | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Duration_In_Secs | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Initially_Enabled | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Fleet_Contents | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Num_Fleets | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Damage | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Execute_Script_On_Type | todo | application not recorded | — | 650 | — | &#91;tactical-ai&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Is_Generic_Hero | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); craft, ship, station (src/presentation/ui/world_ui.cpp); ship (src/skirmish/economy.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects outside the simulated unit closure, other objects and consumers outside the live galactic closure | WGEP-28, WGEP-32, WGUI-18; WSU-50; WHE-01, WHE-49; WSU-50 | 650 | src/campaign/live_setup.cpp; src/presentation/ui/world_ui.cpp; src/skirmish/economy.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces.; basis: reviewed |
| Is_Squashable | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| MaxJiggleDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Occlusion_Silhouette_Enabled | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Ranged_Target_Z_Adjust | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Targeting_Stickiness_Time_Threshold | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GenericHeroUnit, GroundInfantry, GroundVehicle, HeroUnit, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ground_Infantry_Turret_Target | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Collidable_By_Projectile_Living | partial | craft, ship, station (src/presentation/ui/selection.cpp); craft, ship, station (src/sim/tactical/blast.cpp); loaded tactical units (src/sim/tactical/combat_algorithms.hpp); loaded non-team combat candidates (src/sim/tactical/combat_targeting.cpp); loaded tactical units (src/sim/tactical/pads.cpp); loaded tactical units (src/sim/tactical/projectiles.cpp); loaded tactical units (src/sim/tactical/session_step_targets.cpp); missing: objects outside the simulated unit closure | WSU-13; WAD-14; R-08; WCC-25; WBP-50, WBP-51; WPJ-23, WWP-66; WHZ-51, WWP-66 | 649 | src/presentation/ui/selection.cpp; src/sim/tactical/blast.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/pads.cpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_targets.cpp | Effective living-projectile admission defaults false for each loaded type. R-08 rejects noncollidable weapon opportunities, including hostile captured mining pads, before priority and aim. WBP-50/51 gates capture and palette candidates; WAD-14 gates blast recipients and WHZ-51 gates ordinary contact. Squadron records take the spawned team type flag, with independently admitted craft. WSU-13 mouse admission already uses false; objects outside the simulated closure remain deferred. WCC-25 independently rejects noncollidable ship-scan candidates before weapon/priority/range checks; team containers bypass that type check. WWP-66 admits only living-collidable profiles to persistent projectile contact collections and rechecks each contact candidate independently of collection membership; hostile-player admission excludes neutral and allied owners. WPJ-23 selects the candidate object's living permission. ESU-03 sources the false type default.; basis: reviewed |

## GenericHeroUnit, HeroUnit, Mobile_Defense_Unit, SpaceProp, SpaceUnit, SpecialStructure, StarBase, TechBuilding, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); whole class (src/skirmish/start.cpp) | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; none recorded | — | src/campaign/live_setup.cpp; src/skirmish/start.cpp | basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Priority_Set | applied | whole class (include/eawr/units/unit_tables.hpp) | FT-03, R-09, S-01, S-32 | — | include/eawr/units/unit_tables.hpp | matched by the field's name; the loader is table-driven; basis: auto |

## GenericHeroUnit, HeroUnit, SecondaryStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Persistence_Duration | applied | whole class (apps/viewer/src/live_session_death.cpp) | UA-01, UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team | partial | loaded local space battle (src/platform/battle_scoring.cpp); first authored space squadron member (src/skirmish/ai.cpp); missing: land companies and non-scoring team creation | WBF-46; PG-08 | 650 | src/platform/battle_scoring.cpp; src/skirmish/ai.cpp | WBF-46 result scoring and PG-08 tactical AI apply the first-member team-size guard; other non-scoring creation remains separate.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/units/unit_combat.cpp) | WAR-21, WAR-26; PL-13 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_combat.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Galactic_Stealth_Ability/Activation_Style | partial | fresh current galactic actors (src/campaign/starting_forces.cpp); missing: later ability locks and tactical lifecycle | WGM-160, WGM-161, WGS-88 | 4532 | src/campaign/starting_forces.cpp | Specific galactic stealth validation corrects other styles to automatic; fresh unlocked actors apply enabled effects and save live scalar state.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Galactic_Stealth_Ability/Evade_Detection_Chance | partial | fresh current galactic actors (src/campaign/starting_forces.cpp); live self-target galactic effect owners (src/sim/galactic/production_actions.cpp); missing: detector discovery and tactical lifecycle | WGM-160, WGM-161, WGM-169; WGM-161, WGM-162 | 4532 | src/campaign/starting_forces.cpp; src/sim/galactic/production_actions.cpp | Live actor chance is authored, clamped and saved independently of human intelligence and fleet visibility.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Galactic_Stealth_Ability/Initially_Enabled | partial | fresh current galactic actors (src/campaign/starting_forces.cpp); missing: later ability locks and tactical lifecycle | WGM-160, WGM-161, WGS-88 | 4532 | src/campaign/starting_forces.cpp | Specific galactic stealth validation corrects other styles to automatic; fresh unlocked actors apply enabled effects and save live scalar state.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GenericHeroUnit, HeroUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/See_Ground_Company_Contents | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Num_Ground_Companies | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |

## GenericHeroUnit, HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Can_Be_Neutralized_By_Major_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Campaign galactic scene only: the strategic neutralization action reads this on the hero types (WGCH-20..26); a tactical battle never does.; basis: reviewed |
| Can_Be_Neutralized_By_Minor_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Campaign galactic scene only: the strategic neutralization action reads this on the hero types (WGCH-20..26); a tactical battle never does.; basis: reviewed |
| Stay_In_Transport_During_Ground_Battle | partial | transport-only land member admission refusal (include/eawr/skirmish/land_content.hpp); galactic direct transport payload (src/sim/galactic/control.cpp); missing: land battle transport retention | LC-84; WGM-106, WGS-91 | 1149 | include/eawr/skirmish/land_content.hpp; src/sim/galactic/control.cpp | GC power consumer remains; land admission preserves the transport-only role and refuses ordinary movable formation creation until transport/unload is bound.; basis: reviewed |

## GenericHeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Health | applied | whole class (src/skirmish/start.cpp) | HD-01, U-04 | — | src/skirmish/start.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | applied | whole class (src/skirmish/content.cpp) | VT-03 | — | src/skirmish/content.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WCC-46, WCC-50 | — | src/units/unit_durability.cpp | basis: auto |
| Shield_Points | applied | whole class (src/units/unit_durability.cpp) | DG-06, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |
| Shield_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## GenericHeroUnit, Mobile_Defense_Unit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Max_Attack_Distance | applied | whole class (src/units/unit_combat.cpp) | A-06, DG-24, S-26, S-27, WWP-48 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Recharge_Seconds | applied | planetary source objects (src/sim/tactical/session_step_combat.cpp); whole class (src/units/unit_combat.cpp) | WSW-19, WSW-20, WSW-27; EN-05, W-06, W-06a, W-10 | — | src/sim/tactical/session_step_combat.cpp; src/units/unit_combat.cpp | basis: reviewed |
| Projectile_Types | applied | whole class (src/sim/tactical/damage.cpp) | EN-05, EN-06 | — | src/sim/tactical/damage.cpp | basis: auto |

## GenericHeroUnit, SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Galactic_Stealth_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Jabba_The_Hutt. |

## GenericHeroUnit, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | partial | space ships with BARRAGE (src/sim/tactical/combat_fire.cpp); Millennium_Falcon (src/sim/tactical/damage.cpp); Admonitor_Star_Destroyer, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing, Rogue_7_XWing, Wedge_XWing_Rogue (src/sim/tactical/session_step_combat.cpp); Admonitor_Star_Destroyer, Rogue_10_XWing, Rogue_11_XWing, Rogue_2_XWing, Rogue_4_XWing, Rogue_7_XWing, Wedge_XWing_Rogue (src/units/unit_abilities.cpp); Millennium_Falcon (src/units/unit_abilities.cpp); whole class (src/units/unit_abilities.cpp); missing: space abilities authoring TARGET_SPEED_MULTIPLIER or TARGET_TAKE_DAMAGE_MULTIPLIER | WAD-38; WHE-51; WHE-51; WHE-22, WHE-23; WHE-22, WHE-52; AB-01, WAB-03, WAB-11, WAB-30 | 760 | src/sim/tactical/combat_fire.cpp; src/sim/tactical/damage.cpp; src/sim/tactical/session_step_combat.cpp; src/units/unit_abilities.cpp | AB-26: applied to the ten existing supported names on space abilities. All twelve known names are retained; TARGET_SPEED_MULTIPLIER and TARGET_TAKE_DAMAGE_MULTIPLIER have no victim consumer and produce a load diagnostic. Unknown names are ignored without discarding the ability.; basis: reviewed |

## GenericHeroUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Activation_Style | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Abilities/Battlefield_Modifier_Ability/Apply_To_All_Allies | partial | GC space scene (src/campaign/space_rules.cpp); missing: other modes and persistent story/corruption producer | WGT-49, WGT-50, WGT-51 | 760 | src/campaign/space_rules.cpp | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Targeting_Fire_Inaccuracy | applied | whole class (src/units/unit_combat.cpp) | DG-24 | — | src/units/unit_combat.cpp | basis: auto |

## GenericHeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Type | applied | whole class (src/units/unit_abilities.cpp) | DG-05, DG-06, FO-09, WSU-38 | — | src/units/unit_abilities.cpp | basis: auto |

## Goals

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */Category | applied | whole class (src/script/foc/ai_goals.cpp) | AI-05, AI-14, FH-20, GS-30, R-09 | — | src/script/foc/ai_goals.cpp | basis: auto |
| */GameMode | applied | whole class (src/script/foc/ai_service.cpp) | none recorded | — | src/script/foc/ai_service.cpp | basis: auto |
| */Global_Exclusions | applied | whole class (src/script/foc/ai_goals.cpp) | WGA-57 | 737 | src/script/foc/ai_goals.cpp | Reference-counted proposal exclusions by goal name, read in ai_data.cpp and applied when goals are proposed.; basis: reviewed |
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
| Base_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |
| Build_Tab_Outpost | partial | observer-local galactic production choices (apps/viewer/src/galactic/planet_hud.cpp); missing: tactical build menus and unavailable galactic production providers | WGUI-91 | 654 | apps/viewer/src/galactic/planet_hud.cpp | basis: reviewed |
| Next_Level_Base | todo | application not recorded | — | 1458 | — | base levels are read only by the space economy (session_step_economy replacement_id); basis: reviewed |
| Prev_Level_Base | todo | application not recorded | — | 1458 | — | previous_base_levels is space AI only (ai_perception); no land reader; basis: reviewed |
| Tech_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/sim/galactic/production.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-147; WGEP-130, WGEP-147, WGEP-42 | 1149 | src/campaign/live_setup.cpp; src/sim/galactic/production.cpp | basis: reviewed |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Variant_Of_Existing_Type | applied | galactic current-type membership (src/data/xml_merge.cpp) | WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Icon_Name | partial | observer-local ground production palette (apps/viewer/src/galactic/galactic_hud.cpp); missing: tactical/other GUI consumers and unavailable production providers | WGUI-34, WGUI-91 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | basis: reviewed |
| Text_ID | partial | observer-local ground production palette (apps/viewer/src/galactic/galactic_hud.cpp); missing: tactical/other GUI consumers and unavailable production providers | WGUI-34, WGUI-91 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | basis: reviewed |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Victory_Relevant | todo | application not recorded | — | 3776 | — | read in load_body/content.cpp for the space win condition; land_start has no reader (victory_condition fixture only); basis: reviewed |

## GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Build_Cancelled | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:108); no land reader; basis: reviewed |
| SFXEvent_Build_Complete | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:107); no land reader; basis: reviewed |
| SFXEvent_Build_Started | todo | application not recorded | — | 3776 | — | galactic build cue reads it (src/presentation/audio/galactic.cpp:106); no land reader; basis: reviewed |

## GroundBase, GroundBuildable, GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Dummy | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |

## GroundBase, GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |

## GroundBase, GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-LAND |

## GroundBase, GroundBuildable, GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Size_Value | todo | application not recorded | — | 3776 | — | tag is not read anywhere on origin/main; land placement uses its own radius and padding; basis: reviewed |

## GroundBase, GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Distance | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| GUI_Model_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| GUI_Velocity | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |

## GroundBase, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GroundBase, GroundCompany, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Row | partial | observer-local ground production palette (apps/viewer/src/galactic/galactic_hud.cpp); missing: tactical/other GUI consumers and unavailable production providers | WGUI-34, WGUI-91 | 653 | apps/viewer/src/galactic/galactic_hud.cpp | basis: reviewed |

## GroundBase, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |

## GroundBase, GroundInfantry, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Cost_Credits | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |
| Build_Time_Seconds | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |

## GroundBase, GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | todo | application not recorded | — | 3776 | — | Only galactic battle dialog and space threat read the tag; no land AI consumer; basis: reviewed |

## GroundBase, GroundStructure, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Ground_Base_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |

## GroundBase, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Star_Base_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 1149 | src/campaign/live_setup.cpp | basis: reviewed |

## GroundBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Income_Stream_Ability/@Name | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Income_Stream_Ability/Base_Income_Value | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Income_Stream_Ability/Base_Interval_In_Secs | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Income_Stream_Ability/Full_Amount_To_Everyone | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Income_Stream_Ability/Split_Favors_Owner | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Income_Stream_Ability/Split_Income_With_Allies | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Base_Position | todo | application not recorded | — | 1458 | — | no land code on origin/main reads Base_Position; land P1 has no pads (session_land and land_start); registry partial is for space SecondaryStructure; basis: reviewed |
| Capture_Point_Radius | todo | application not recorded | — | 1458 | — | unit_tables_decode reads capture_radius but no land consumer; land has no capture pads; basis: reviewed |
| Capture_Point_Transition_Time_Seconds | todo | application not recorded | — | 1458 | — | decoded into capture_seconds only; no land consumer; registry applied is for space pads.cpp; basis: reviewed |
| Fine_Tune_Occupied_Passability | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Hides_When_Built_On | todo | application not recorded | — | 3776 | — | Parsed into a unit flag (unit_tables_profiles.cpp:473) but no pad visibility reader; basis: reviewed |
| Immune_To_Damage | partial | stock galactic lethal roster closure (src/campaign/auto_resolve_live.cpp); missing: other tactical consumers | WAR-85, WAR-86, WAR-88 | 1965 | src/campaign/auto_resolve_live.cpp |  Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter.; basis: reviewed |
| Include_In_UI_Map_Header | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Is_Community_Property | todo | application not recorded | — | 1458 | — | read by unit_tables_decode.cpp&#35;Loader::flag for viewer pads/selection (battle_input.cpp); land view and land_start do not read it; basis: reviewed |
| Minimum_Time_Before_Pad_Can_Build_Again | todo | application not recorded | — | 1458 | — | pad_rebuild_seconds decoded; no land consumer; basis: reviewed |
| MP_Encyclopedia_Text | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | MP_Encyclopedia_Text read only in multiplayer branch of hud_shell; basis: reviewed |
| Ownership_Sticks | todo | application not recorded | — | 3776 | — | decoded after the land early return in unit_tables_decode.cpp; consumer is space build pads (pads.cpp), no land reader; basis: reviewed |
| Political_Control | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-23, WGEP-50, WGEP-69, WGM-69, WGS-90, WGS-94 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Radar_Icon_Name | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Radar_Show_Facing | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Respects_Skirmish_Pre_Build_Bases_Option | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Reveal_During_Setup_Phase_Only | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| SFXEvent_Unit_Under_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Tactical_Additional_Structure_Type | todo | application not recorded | — | 1458 | — | read only by space_entry_stock (space partial); no land reader; basis: reviewed |
| Tactical_Build_Cost_Campaign | todo | application not recorded | — | 1458 | — | read by the space economy only (skirmish economy and unit_tables_profiles); land economy holds credits and pop cap only (land_start.cpp 334-338); basis: reviewed |
| Tactical_Build_Start_Lower_Z | todo | application not recorded | — | 1458 | — | no reader on origin/main; basis: reviewed |
| Tactical_Buildable_Constructed | todo | application not recorded | — | 1458 | — | constructed_type decoded only; no land consumer; basis: reviewed |
| Tactical_Buildable_Objects_Campaign | todo | application not recorded | — | 1458 | — | space economy and campaign only; no land reader; basis: reviewed |
| Tactical_Buildable_Objects_Multiplayer | todo | application not recorded | — | 1458 | — | skirmish economy buildable (space); no land reader; basis: reviewed |
| Terrain_Texture_Modifier_Join_Distance | todo | application not recorded | — | 3228 | — | no code reads the tag on main; basis: reviewed |
| Terrain_Texture_Modifier_Material | todo | application not recorded | — | 3228 | — | no code reads the tag on main; basis: reviewed |
| Terrain_Texture_Modifier_Square | todo | application not recorded | — | 3228 | — | no code reads the tag on main; basis: reviewed |
| Visible_To_Enemies_When_Empty | todo | application not recorded | — | 3776 | — | Parsed (unit_tables_profiles.cpp:474) but no pad visibility reader; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Text | partial | admitted galactic production types (apps/viewer/src/galactic/scene_content.cpp); missing: objects outside admitted galactic production | WGUI-18, WGUI-81 | 4532 | apps/viewer/src/galactic/scene_content.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; SCOPE-LAND; basis: reviewed |
| Encyclopedia_Unit_Class | partial | galactic reference pages (apps/viewer/src/galactic/reference_catalog.cpp); missing: UI consumers outside galactic reference pages | GREF-06 | 1952 | apps/viewer/src/galactic/reference_catalog.cpp | SCOPE-LAND; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Score_Cost_Credits | partial | live galactic automatic battle summary (apps/viewer/src/galactic/galactic_battle_dialog.cpp); live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); missing: consumers outside loaded space and live galactic battle summaries | WAR-88, WBF-45; WBF-45, WGP-178 | 4532 | apps/viewer/src/galactic/galactic_battle_dialog.cpp; src/platform/galactic/campaign_scoring.cpp |  Live GC summary filters confirmed original-type losses using the resolved Score_Cost_Credits threshold.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Scale_Factor | partial | land company members (include/eawr/skirmish/land_content.hpp); ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-06; WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66 | 1450 | include/eawr/skirmish/land_content.hpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; basis: reviewed |
| Select_Box_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-02 | — | apps/viewer/src/world_ui_prepare.cpp | Select_Box_Scale read in world UI prepare (WU-02 blob); basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit, SpacePrimarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Model_Name | partial | land company members (include/eawr/skirmish/land_content.hpp); ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-06; WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66 | 1450 | include/eawr/skirmish/land_content.hpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Area_Effect_Decal_Distance | todo | application not recorded | — | 3776 | — | Parser branch only for CORRUPT_SYSTEMS (galactic ability type); basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/GUI_Activated_Ability_Name | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Type | todo | application not recorded | — | 3776 | — | ability_kind() used by viewer audio (battle_audio_prepare.cpp:485); no land apply found; basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LOD_Bias | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |

## GroundBuildable, GroundCompany, GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Cost_Multiplayer | todo | application not recorded | — | 1458 | — | build_cost_multiplayer is space-only (skirmish economy); no land build path; basis: reviewed |
| Tactical_Build_Time_Seconds | todo | application not recorded | — | 1458 | — | build_time_seconds is read by space economy only; no land reader; basis: reviewed |

## GroundBuildable, GroundCompany, GroundStructure, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| GUI_Bounds_Scale | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-11 | — | apps/viewer/src/world_ui_prepare.cpp | GUI_Bounds_Scale read in world UI prepare (shield/bar sizing); basis: reviewed |

## GroundBuildable, GroundCompany, HeroCompany, HeroUnit, SecondaryStructure, SlaveCompany, SpaceBuildable, SpaceUnit, SpecialStructure, StarBase, TechBuilding, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Ground_Base_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-22, WGEP-35 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundBuildable, GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Can_Be_Unlocked_By_Slicer | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/sim/galactic/production.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-147; WGEP-130, WGEP-147, WGEP-42 | 1149 | src/campaign/live_setup.cpp; src/sim/galactic/production.cpp | basis: reviewed |
| Build_Cost_Credits | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-29, WGEP-30 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Initially_Locked | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); all registered galactic types: automatic technology unlocks (src/sim/galactic/production.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-147; WGEP-19; WGEP-130, WGEP-147, WGEP-42 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp; src/sim/galactic/production.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Tech_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); all registered galactic types: automatic technology unlocks (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); all registered galactic types: automatic technology unlocks (src/sim/galactic/production.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-147; WGEP-19, WGEP-22; WGEP-130, WGEP-147, WGEP-42 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp; src/sim/galactic/production.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Death_Explosions | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Property_Flags | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Shield_Points | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Shield_Refresh_Rate | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Capacity | todo | application not recorded | — | 3776 | — | energy pool read only by space unit_durability and unit_tables_profiles; land unit table has no energy field; basis: reviewed |
| Energy_Refresh_Rate | todo | application not recorded | — | 3776 | — | same as Energy_Capacity: no land consumer; basis: reviewed |
| No_Reflection_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| No_Refraction_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Occlusion_Silhouette_Enabled | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Marker, Projectile, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_FOW_Reveal_Range | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Collidable_By_Projectile_Living | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Custom_Hard_XExtent | applied | whole class (include/eawr/skirmish/land_content.hpp); whole class (src/units/unit_tables_land.cpp) | none recorded; none recorded | — | include/eawr/skirmish/land_content.hpp; src/units/unit_tables_land.cpp | read in land_locomotion, scaled by Scale_Factor into the hard footprint of the land binding; malformed value refuses the binding; basis: reviewed |
| Custom_Hard_YExtent | applied | whole class (include/eawr/skirmish/land_content.hpp); whole class (src/units/unit_tables_land.cpp) | none recorded; none recorded | — | include/eawr/skirmish/land_content.hpp; src/units/unit_tables_land.cpp | read in land_locomotion, scaled by Scale_Factor into the hard footprint of the land binding; malformed value refuses the binding; basis: reviewed |
| Death_SFXEvent_Start_Die | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Dense_FOW_Reveal_Range_Multiplier | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| GUI_Bracket_Size | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-11 | — | apps/viewer/src/world_ui_prepare.cpp | GUI_Bracket_Size read in world UI prepare; WU-11 bar set; basis: reviewed |
| Influences_Capture_Point | todo | application not recorded | — | 1458 | — | influences_capture decoded (unit_tables_decode) but no land consumer; land has no capture points; basis: reviewed |
| Is_Visible_On_Radar | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| LandBehavior | partial | ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: ungated defaults, refined/combined objects, live behavior dispatch | WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; SCOPE-LAND |
| Loop_Idle_Anim_00 | applied | whole class (src/scene/idle_tags.cpp) | none recorded | — | src/scene/idle_tags.cpp | Loop_Idle_Anim_00 parsed in idle_tags, used for idle playback; basis: reviewed |
| Mass | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Projectile_Types | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Ranged_Target_Z_Adjust | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Select_Box_Z_Adjust | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| SFXEvent_Select | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Space_Layer | todo | application not recorded | — | 3776 | — | Read only by space motion/domain tags (domain_tags.cpp, unit_motion.cpp); land code does not read it; basis: reviewed |
| Tactical_Health | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Targeting_Max_Attack_Distance | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| UnitCollisionClass | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-35 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## GroundBuildable, GroundInfantry, GroundStructure, GroundVehicle, Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Damage_Alternates | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Land_Damage_SFX | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Land_Damage_Thresholds | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GroundBuildable, GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Custom_Soft_Footprint_Radius | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-06, WLPA-14 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## GroundBuildable, GroundInfantry, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Movie_Object | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |

## GroundBuildable, GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/@Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Percent | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Pulse_Count | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Light_Effect_Type | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Owner_Particle_Bone_Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/SFXEvent_Target_Affected | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Single_Target_Heal | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Base_Shield_Always_Off | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Exclude_From_Distance_Fade | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Obstacle_Height | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_Proxy_Type | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_Width | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_X_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Obstacle_Y_Offset | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Radar_Icon_Size | todo | application not recorded | — | 3776 | — | minimap/radar tag read only by presentation/ui/minimap.cpp; land live (ready_land_live) creates no minimap, so not applied on land; basis: reviewed |
| Reveal_During_Setup_Phase | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |

## GroundBuildable, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Ambient_Loop | todo | application not recorded | — | 3776 | — | no land SFX dispatch; battle_audio per-unit readers not used by land mode (map_mode_land_live.cpp); basis: reviewed |

## GroundBuildable, GroundStructure, GroundVehicle, Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Land_Evaluator | todo | application not recorded | — | 3776 | — | Retail enrols land targets by this tag (WLAI-04); main has no land enrolment; basis: reviewed |

## GroundBuildable, GroundStructure, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Initial_State_Visible_Under_FOW | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| Last_State_Visible_Under_FOW | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |

## GroundBuildable, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Unit_Lost | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |

## GroundCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Company_Units | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); galactic current-type membership (src/data/xml_merge.cpp); galactic current build limits (src/sim/galactic/production.cpp); land company profile (src/units/unit_tables_land.cpp) | WAR-20, WAR-25, WAR-26; WGA-363, WGA-364; WGEP-78; LC-01, LC-04 | — | src/campaign/auto_resolve_strength.cpp; src/data/xml_merge.cpp; src/sim/galactic/production.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer. L2a (&#35;1450): a land load keeps the ordered member list with per-member kind, locomotor and hero identity.; basis: reviewed |
| Ignore_For_Reoptimization | todo | application not recorded | — | 3776 | — | Retail zeroes ignored members (WR-03); no land reinforcement reader on main; basis: reviewed |
| Piracy_Value_Credits | partial | galactic fleet departure (src/sim/galactic/movement.cpp); missing: units carried inside a transport are not flattened by the departure sum | WGEP-113, WGM-150 | 4905 | src/sim/galactic/movement.cpp | A ground company that is itself a departing fleet member contributes its value; carried-unit flattening follows the transport rule and is not wired.; basis: reviewed |
| Required_Orbiting_Units | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundCompany, GroundInfantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Permanent_Weapon_Swap_Ability/@Name | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Activation_Style | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Initially_Enabled | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Must_Be_Bought_On_Black_Market | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Permanent_Weapon_Swap_Ability/Weapon_Index | todo | application not recorded | — | 3239 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Proximity_Mines_Ability/@Name | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activate_SFX | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Activation_Time_In_Seconds | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Mine_Type | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Must_Be_Bought_On_Black_Market | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Number_Of_Mines | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Abilities/Proximity_Mines_Ability/Trigger_Radius | todo | application not recorded | — | 3243 | — | Mine placement and trigger absent on land; no Proximity_Mines code on main outside walk docs and UI kind tables.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Bomb_Countdown_Seconds | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Damage_Percent_When_Activated | todo | application not recorded | — | 3776 | — | No reader anywhere on main; parse gap; todo; basis: reviewed |

## GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Encyclopedia_Good_Against | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | Good_Against read by hud_shell icons; shared encyclopedia, land screen reads it too; basis: reviewed |
| Encyclopedia_Vulnerable_To | applied | whole class (src/presentation/ui/hud_shell.cpp) | none recorded | — | src/presentation/ui/hud_shell.cpp | Vulnerable_To read by hud_shell icons; shared encyclopedia; basis: reviewed |

## GroundCompany, GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Stealth_Ability/@Name | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Stealth_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Stealth_Ability/Must_Be_Bought_On_Black_Market | todo | application not recorded | — | 1130 | — | Black-market purchase gate is not applied to this carrier; kept galactic (PR &#35;4993). Overlaps PR &#35;4965 and branch Thom-Ernst/gc-tag-corruption-blackmarket: not claimed done here.; basis: reviewed |
| Abilities/Stealth_Ability/SFXEvent_Activate | todo | application not recorded | — | 3246 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Color | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Transition_Time | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| SFXEvent_Tactical_Build_Cancelled | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; land has no audio routing; basis: reviewed |
| SFXEvent_Tactical_Build_Complete | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; no land reader; basis: reviewed |
| SFXEvent_Tactical_Build_Started | todo | application not recorded | — | 3776 | — | only space battle_audio_prepare reads the build cue; no land reader; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Must_Be_Bought_On_Black_Market | todo | application not recorded | — | 1130 | — | Black-market purchase gate is not applied to this carrier; kept galactic (PR &#35;4993). Overlaps PR &#35;4965 and branch Thom-Ernst/gc-tag-corruption-blackmarket: not claimed done here.; basis: reviewed |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Fleet_Move | todo | application not recorded | — | 653 | — | Fleet-move audio plays from the moving fleet member types (apps/viewer/src/galactic/galactic_audio.cpp&#35;GalacticAudio::fleet_move); this carrier class is not a fleet member, so no consumer for it is verified.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_Target_Ability | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Activated | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |

## GroundCompany, GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Required_Timeline | todo | application not recorded | — | 1457 | — | no reader on origin/main; walk gc-ai calls it an authored type tag not a producer trigger; basis: reviewed |

## GroundCompany, GroundInfantry, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Alternate_Description_Text | todo | application not recorded | — | 3776 | — | Reader in hud_shell.cpp:918-920 (HUD title/description/icon); land HUD use not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Alternate_Name_Text | todo | application not recorded | — | 3776 | — | Reader in hud_shell.cpp:918-920 (HUD title/description/icon); land HUD use not verified; basis: reviewed |

## GroundCompany, GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Build_Prerequisites | todo | application not recorded | — | 1458 | — | production_allowed and build_option_state are space-only; no land reader; basis: reviewed |
| Tactical_Production_Queue | todo | application not recorded | — | 1458 | — | land_start.cpp:327 sets a production_queue player flag but no land code reads the tag or flag found in this pass; basis: reviewed |

## GroundCompany, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Charging_Time_In_Seconds | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |

## GroundCompany, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/@Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Damage_Per_Second | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Effect_Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Radius | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Source_Bone_Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Duration_In_Seconds | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Max_Drain_Victims | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/SFXEvent_Activate | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Should_Heal | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| No_Colorization_Color | partial | land battle objects (apps/viewer/src/live_session_prepare.cpp); missing: land battle consumers (see note) | none recorded | 3776 | apps/viewer/src/live_session_prepare.cpp | No_Colorization_Color applied for skirmish battles (&#35;3076); campaign land battles off per live_session_view.hpp:756; basis: reviewed |

## GroundCompany, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Activation_Max_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/@Name | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Activation_Style | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Block_Chance | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/SFXEvent_Activate | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Turn_To_Face_Unblockable_Shots | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Is_Pulsing | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Max_Number_Of_Pulses | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Pulse_Frequency_Secs | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Stop_When_Activated | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Types | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Targeting_Max_Attack_Distance | todo | application not recorded | — | 3776 | — | Shared loader retains this ability range and the space runtime applies WRA-03; these stock classes still await the land targeting consumer.; basis: reviewed |

## GroundCompany, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Owner_Attachment_Bone | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); WIRING todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Particle_Effect | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GroundCompany, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Alternate_Icon_Name | todo | application not recorded | — | 3776 | — | Reader in hud_shell.cpp:918-920 (HUD title/description/icon); land HUD use not verified; basis: reviewed |

## GroundCompany, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Behavior | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC map placements with MARKER behavior (src/campaign/conflict_entry.cpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: other galactic behavior dispatch, ungated defaults, refined/combined objects, live behavior dispatch | WAR-24; WGT-121; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | src/campaign/auto_resolve_strength.cpp; src/campaign/conflict_entry.cpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. Static land initialization applies only authored LAND_OBSTACLE candidates with explicit geometry/passability/refinement gates; see land-content-adapter.md.; basis: reviewed |
| Cheap_Bribe_Cost | partial | galactic fleet members (src/campaign/fleets.cpp); galactic fleet movement (src/sim/galactic/movement.cpp); galactic fleet movement (src/sim/galactic/movement.cpp); missing: units carried inside a transport are not flattened by the admission sum | WGM-85; WGM-84, WGM-85; WGM-84 | 4005 | src/campaign/fleets.cpp; src/sim/galactic/movement.cpp | A ground company that is itself a departing fleet member contributes its cost; carried-unit flattening follows the transport rule and is not wired. Movement admission only: a corruption-benefiting mover pays, before departure, the summed original-type cost of the flattened departing members for each intermediate path planet its owner holds, unless the mover holds that orbit; AI difficulty multiplier and the AI bribe request stay with &#35;3981.; basis: reviewed |
| Expensive_Bribe_Cost | partial | galactic fleet members (src/campaign/fleets.cpp); galactic fleet movement (src/sim/galactic/movement.cpp); galactic fleet movement (src/sim/galactic/movement.cpp); missing: units carried inside a transport are not flattened by the admission sum | WGM-85; WGM-84, WGM-85; WGM-84 | 4005 | src/campaign/fleets.cpp; src/sim/galactic/movement.cpp | A ground company that is itself a departing fleet member contributes its cost; carried-unit flattening follows the transport rule and is not wired. Movement admission only: a corruption-benefiting mover pays, before departure, the summed original-type cost of the flattened departing members for each intermediate path planet its owner holds, unless the mover holds that orbit; AI difficulty multiplier and the AI bribe request stay with &#35;3981.; basis: reviewed |
| Is_Homogeneous | todo | application not recorded | — | 3776 | — | Only read by load_squadron (space); load_land_company never reads it.; basis: reviewed |
| Required_Planets | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundCompany, HeroCompany, Indigenous_Unit, Slave_Unit, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team_Type | partial | land company profile (src/units/unit_tables_land.cpp); missing: tactical team creation | LC-03, LC-06 | 1450 | src/units/unit_tables_land.cpp | WHE-05: the HeroCompany occurrences are ground teams. L2a (&#35;1450): the company's team type (else the first member's) is resolved with its Max_Speed as the team speed source; unauthored means the generic team fallback.; basis: reviewed |

## GroundCompany, HeroCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Time_Reduced_By_Multiple_Factories | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Time_Seconds | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-32 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Formation_Priority | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Max_Squad_Size | partial | land company profile (src/units/unit_tables_land.cpp); missing: tactical squad grouping at creation | LC-03, LC-04 | 1450 | src/units/unit_tables_land.cpp | L2a (&#35;1450): decoded as a whole size of at least 1 and refused otherwise; the expansion plan groups created members by it. Absent stays absent (the plan then holds one squad). Tactical creation consumes the plan later.; basis: reviewed |
| Population_Value | partial | opt-in galactic production offers (src/campaign/setup.cpp); opt-in global galactic owned-list census (src/sim/galactic/production.cpp); missing: Live owned-list lifecycle/usability, modifier/filter providers and AI task budget payment remain with their owning services. | WGEP-50; WGEP-48, WGEP-49, WGEP-50, WGEP-62, WGEP-68, WGEP-69 | 1129 | src/campaign/setup.cpp; src/sim/galactic/production.cpp | Original-type GC population and capacity feed partitioned census contributions; both actual queues reserve population.; basis: reviewed |
| Required_Special_Structures | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | src/campaign/live_setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Required_Star_Base_Level | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); opt-in galactic production offers (src/campaign/setup.cpp); missing: Live modifier/filter providers, AI task budgets and nested result placement remain owning-service seams., other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WGEP-22, WGEP-35 | 4532 | src/campaign/live_setup.cpp; src/campaign/setup.cpp | Parsed into the independent galactic type table (src/units/galactic_types.cpp, docs/galactic-types.md); application belongs to the consuming GC ticket. Galactic queues consume explicitly prepared validated inputs; missing data is never defaulted by this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## GroundCompany, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Indigenous_Unit_Corruptible | todo | application not recorded | — | 3776 | — | WLEN-52 marks it missing in code.; basis: reviewed |

## GroundCompany, SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Build_Tab_Land_Units | partial | observer-local galactic production choices (apps/viewer/src/galactic/planet_hud.cpp); missing: tactical build menus and unavailable galactic production providers | WGUI-91 | 654 | apps/viewer/src/galactic/planet_hud.cpp | basis: reviewed |
| Company_Transport_Unit | partial | galactic current build limits (src/sim/galactic/production.cpp); missing: other ground company consumers | WGEP-78 | 4532 | src/sim/galactic/production.cpp |  GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Is_Escort | todo | application not recorded | — | 1455 | — | no reader on origin/main; TCFA-05 covers only the space debug-build escort classification; basis: reviewed |
| Ship_Class | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |

## GroundInfantry

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Grenade_Attack_Ability/@Name | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Explode_Timer_In_Secs | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Bone | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Frame | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Toss_Anim | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Type | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Requires_Direct_Player_Command | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Spawn_Ability/@Name | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Activation_Max_Range | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Activation_Min_Range | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Creation_Delay_In_Seconds | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Explosion_Type | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Object_Type | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Recharge_Time_In_Seconds | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Self_Destruct_Radius | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Spawn_Ability/Spawn_Bone_Name | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Cinematic_Anim_Blend_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Cinematic_Anim_Index | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Cinematic_Anim_Speed | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Conversion_Ability_Changes_To_Enemy | todo | application not recorded | — | 3244 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Formation_Formup_Wait_Style | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Land_Terrain_Model_Mapping | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| LateralAcceleration | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Pause_During_Cinematic_Anim | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Presence_Induced_Animations | todo | application not recorded | — | 3776 | — | LR13-07 describes it; no land-path code reads it.; basis: reviewed |
| SFXEvent_Cough_Override | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| SFXEvent_Health_Critical_Warning | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp health warnings are space-battle only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Health_Low_Warning | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:414 health warnings are space-battle only; land mode does not use BattleAudio; basis: reviewed |

## GroundInfantry, GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Squashable_By_Supercrusher | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Projectile_Damage | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| SFXEvent_Engine_Cinematic_Focus_Loop | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| Target_Stealth_Units | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Targeting_Stickiness_Time_Threshold | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |

## GroundInfantry, GroundStructure, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Lua_Script | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |

## GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Min_Attack_Distance | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Turret_Elevate_Extent_Degrees | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Turret_Rotate_Extent_Degrees | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |

## GroundInfantry, GroundStructure, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Blob_Shadow_Below_Detail_Level | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Material_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Blob_Shadow_Scale | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Death_Clone | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| SFXEvent_Fire | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:408 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| Targeting_Fire_Inaccuracy | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Targeting_Priority_Set | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |

## GroundInfantry, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Autonomous_Move_Extension_Vs_Attacker | todo | application not recorded | — | 3776 | — | Read only by space combat step (session_step_combat.cpp) and profile load; land combat step not on main; basis: reviewed |
| Cheap_Bribe_Cost | todo | application not recorded | — | 4005 | — | Galactic fleet bribe reads it (src/units/galactic_types.cpp); land bribe reads Tactical_Bribe_Cost. Overlaps PR &#35;4965 (bribe): not claimed done here.; basis: reviewed |
| Destruction_Survivors | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Is_Affected_By_Gravity_Control_Field | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Selt_Destruct_SFXEvent_Start_Die | todo | application not recorded | — | 3776 | — | battle_audio_events.cpp self-destruct reader is space-battle only; no land reader; basis: reviewed |
| Surface_Type_Cover_Damage_Shield | todo | application not recorded | — | 3228 | — | LC-21: missing terrain-material cover lookup (G3); no code reads the tag; basis: reviewed |
| Tactical_Bribe_Cost | todo | application not recorded | — | 3244 | — | LR11-30 says the land tactical bribe starts from this tag; no reader in src on origin/main; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/SFXEvent_GUI_Unit_Ability_Deactivated | todo | application not recorded | — | 3776 | — | Read in viewer battle_audio_prepare.cpp only; land audio apply not verified; basis: reviewed |

## GroundInfantry, GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GroundInfantry, GroundVehicle, HeroCompany, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Move_Response_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Guard_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Idle_Chase_Range | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| SFXEvent_Move | todo | application not recorded | — | 3776 | — | galactic.cpp:189 reads it for galactic fleets only; no land reader; basis: reviewed |

## GroundInfantry, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bank_Turn_Angle | todo | application not recorded | — | 3776 | — | Read only by ship/craft motion (unit_motion.cpp ship and craft loops); no land reader; basis: reviewed |
| Base_Shield_Penetration_Particle | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Close_Enough_Angle_For_Move_Start | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Crouch_Animation_Speed | todo | application not recorded | — | 3776 | — | LR13-14 multiplier is not in load_land_body or land_movement.; basis: reviewed |
| Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Category_Restrictions | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Is_Stationary_When_Attacking | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Max_Rate_Of_Roll | todo | application not recorded | — | 3776 | — | Read only by ship and craft motion (unit_motion.cpp); no land reader; basis: reviewed |
| MaxJiggleDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Min_Speed_Fraction_For_Turn | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Political_Control | todo | application not recorded | — | 3776 | — | only campaign control.cpp and starting_forces.cpp reference the tag; no land reader found; basis: reviewed |
| Specific_Death_Anim_Index | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Specific_Death_Anim_Type | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Type | partial | land battle objects (src/units/unit_tables_land.cpp); missing: land battle consumers (see note) | none recorded | 3776 | src/units/unit_tables_land.cpp | land_kind reads the xml type for infantry/vehicle; Indigenous_Unit falls to kind other.; basis: reviewed |
| Walk_Transition | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-31 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Wind_Disturbance_Radius | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Sphere_Alpha | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |
| Wind_Disturbance_Strength | todo | application not recorded | — | 3227 | — | WLEN-44: missing wind disturbance; no code reads the tag; basis: reviewed |

## GroundInfantry, GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| AI_Combat_Power | partial | live galactic campaign end (src/platform/galactic/campaign_scoring.cpp); galactic direct transport payload (src/sim/galactic/control.cpp); missing: land tactical AI and combat consumers | WBF-45, WGP-178; WGM-106, WGS-91 | 1149 | src/platform/galactic/campaign_scoring.cpp; src/sim/galactic/control.cpp | GC current-type power qualifies direct transport payloads; land tactical consumers remain separate.; basis: reviewed |
| Blob_Shadow_Bone_Name | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| CanCellStack | todo | application not recorded | — | 1458 | — | CanCellStack is read nowhere in src include apps tests on origin/main; basis: reviewed |
| Death_Fade_Time | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Death_Persistence_Duration | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| FormationGrouping | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| FormationOrder | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| FormationRaggedness | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| FormationSpacing | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-79, LC-80 | 1450 | include/eawr/skirmish/land_content.hpp | Land member spacing defaults to zero additional spacing; hard geometry remains. Team-type and member spacing are retained separately (LC-79/80).; basis: reviewed |
| Is_Squashable | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Land_Model_Anim_Override_Name | applied | whole class (src/scene/scene_build.cpp) | none recorded | — | src/scene/scene_build.cpp | Land_Model_Anim_Override_Name read by scene build and land death clips (options_.land branch); basis: reviewed |
| Max_Rate_Of_Turn | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Max_Speed | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | LC-06, MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Min_Speed | todo | application not recorded | — | 3776 | — | Read only by craft motion and fighter formation (unit_motion.cpp, fighters_formation.cpp); no land reader; basis: reviewed |
| MinimumPushReturnDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Mouse_Collide_Override_Sphere_Radius | todo | application not recorded | — | 3776 | — | Mouse_Collide_Override_Sphere_Radius parsed in unit tables; battle_input.cpp:102 builds pick shape; basis: reviewed |
| Movement_Animation_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MovementClass | partial | land company members (include/eawr/skirmish/land_content.hpp); live galactic auto-resolve (src/campaign/auto_resolve_live.cpp); missing: other tactical consumers | WLPA-01; WAR-35 | 1965 | include/eawr/skirmish/land_content.hpp; src/campaign/auto_resolve_live.cpp |  Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter. Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| OccupationStyle | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| OverrideAcceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| OverrideDeceleration | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | MV-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Projectile_Appearance_Delay_Frames | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Rotation_Animation_Speed | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| SFXEvent_Ambient_Moving | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:419 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Ambient_Moving_Max_Delay_Seconds | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:435 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Ambient_Moving_Min_Delay_Seconds | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:435 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Assist_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Assist_Move | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SFXEvent_Attack | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Engine_Moving_Loop | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:421 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |
| SFXEvent_Guard | todo | application not recorded | — | 3776 | — | no land SFX dispatch on main; basis: reviewed |
| SurfaceFX_Name | todo | application not recorded | — | 3228 | — | LC-16: missing surface-state component (G3); no code reads the tag; basis: reviewed |
| Walk_Animation_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Weather_Category | todo | application not recorded | — | 3225 | — | WLEN-15: no loader or sim field for Weather_Category on land; basis: reviewed |

## GroundInfantry, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Force_Sensitive | todo | application not recorded | — | 3776 | — | Read only in the space/galactic decode path after the land early return; no land reader.; basis: reviewed |

## GroundInfantry, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Can_Indigenous_Unit_Stop | todo | application not recorded | — | 3776 | — | WLEN-46 describes it; indigenous behaviour is Missing.; basis: reviewed |

## GroundInfantry, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Burning_Damage_Per_Second | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Create_Team | partial | land company member (src/units/unit_tables_land.cpp); missing: tactical team creation | LC-02 | 1450 | src/units/unit_tables_land.cpp | L2a (&#35;1450): kept per company member; the expansion plan lets the first created member decide whether squads form.; basis: reviewed |
| Is_Combustible | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| On_Fire_Speed_Modifier | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Targeting_Allowed_When_Burning | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GroundStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range_Blob_Material | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Color2 | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Duration | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Pulse_Count | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Light_Effect_Type | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Bone_Name | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Healing_Ability/Target_Particle_Effect | todo | application not recorded | — | 3245 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/@Name | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Blob_Color | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Blob_Material_Name | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Duration_In_Secs | todo | application not recorded | — | 3776 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Base_Shield_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Disable_Force_Abilities_Range | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Friendly_Spawn_Text | todo | application not recorded | — | 3776 | — | no code reads spawn text tags on main; basis: reviewed |
| GUI_Hide_Health_Bar | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-11, WU-17 | — | apps/viewer/src/world_ui_prepare.cpp | GUI_Hide_Health_Bar read in world UI prepare; hides bars; basis: reviewed |
| Is_Branched_Map_Discardable | partial | GC simulated map objects (src/campaign/conflict_entry.cpp); missing: land tactical and presentation-only placements | WGT-121 | 4532 | src/campaign/conflict_entry.cpp | WGT-121 reads authored flags and their debug-build defaults through skirmish inputs, then applies them only to GC simulated map-object admission. Land entry and inert presentation scenery retain their separate scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Is_Decoration | partial | land battle objects (src/skirmish/start.cpp); missing: land battle consumers (see note) | WLEN-40, WLEN-53 | 3230 | src/skirmish/start.cpp | Is_Decoration/Is_Discardable read in skirmish/inputs.cpp and applied by the shared owner remap (start.cpp:633, used by land_start.cpp); ground-prop variants not verified per WLEN-276; basis: reviewed |
| Loads_When_Faction_Present | partial | GC simulated map objects (src/campaign/conflict_entry.cpp); missing: land tactical and presentation-only placements | WGT-121 | 4532 | src/campaign/conflict_entry.cpp | WGT-121 reads authored flags and their debug-build defaults through skirmish inputs, then applies them only to GC simulated map-object admission. Land entry and inert presentation scenery retain their separate scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Max_Distance_From_Spawner | todo | application not recorded | — | 3776 | — | WLEN-47 marks it missing in code.; basis: reviewed |
| Modifies_Reveal_Range | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Multisample_FOW_Check | todo | application not recorded | — | 3776 | — | FOW tag read only in space load_body/profiles path and skirmish/start.cpp; load_land_body never reads it; land fog is not implemented (live_session_land.cpp: fog belongs to L6 P2); basis: reviewed |
| OverridePassability | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66 | 4822 | include/eawr/skirmish/land_content.hpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit.; SCOPE-LAND |
| Passive_Missile_Shield_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Radar_Range_Icon_Name | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Reinforcement_Prevention_Radius | todo | application not recorded | — | 3776 | — | Space applies it (WGT-18); land prevention query missing (LC-45); basis: reviewed |
| Reveal_Range_Modifier | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Shield_Normal_Color | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Shield_Off_Anim | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Shield_On_Anim | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Shield_Requires_Base_Power | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Space_Model_Name | todo | application not recorded | — | 3776 | — | land_kind classifies a structure before Space_Model_Name is consulted, so the tag is unread for this carrier.; basis: reviewed |
| Space_Obstacle_Offset | todo | application not recorded | — | 3776 | — | Decoded in unit_tables_decode.cpp; read only by space hazard code (content.cpp, inputs.cpp); basis: reviewed |
| Spawn_Indigenous_Units_Chance | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawn_Indigenous_Units_In_Packs | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawn_Indigenous_Units_Radius | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Pack_Type | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Delay_Seconds | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Quantity | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Spawned_Indigenous_Units_Type | todo | application not recorded | — | 3776 | — | SPAWN_INDIGENOUS_UNITS data; WLEN-50 says missing in code.; basis: reviewed |
| Unit_Abilities_Data | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## GroundStructure, GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Apply_Y_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Apply_Z_Turret_Rotate_To_Axis | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Attack_Category_Restrictions | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Damage_Type | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Remove_Upon_Death | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| SFXEvent_Turret_Rotating_Loop | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GroundStructure, GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Drain_Life_Ability/Drain_Target_Bone_Name | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Barrel_Bone_Name | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Death_Clone_Is_Obstacle | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Turret_Bone_Name | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |
| Turret_Rotate_Speed | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |

## GroundStructure, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Enemy_Spawn_Text | todo | application not recorded | — | 3776 | — | no code reads spawn text tags on main; basis: reviewed |
| Is_Discardable | partial | land battle objects (src/skirmish/start.cpp); missing: land battle consumers (see note) | WLEN-40, WLEN-53 | 3230 | src/skirmish/start.cpp | Is_Decoration/Is_Discardable read in skirmish/inputs.cpp and applied by the shared owner remap (start.cpp:633, used by land_start.cpp); ground-prop variants not verified per WLEN-276; basis: reviewed |
| Is_Valid_Target | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Required_Special_Structures | todo | application not recorded | — | 1458 | — | only campaign live_setup reads it; no land production gate; basis: reviewed |

## GroundVehicle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Cable_Ability/@Name | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Activation_Style | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_End_Speed_Fraction | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_Fadeout_Time | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_Length_Fraction | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_Render_Mode | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_Texture_Name | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Cable_Width | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Drop_Phase_Duration | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Paralyze_Phase_Duration | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Pull_Phase_Duration | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Cable_Ability/Terminate_Effect_On_Move_Command | todo | application not recorded | — | 3242 | — | No main code reads Cable_Ability element; Cable_Ability tow service (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Laser_Defense_Ability/@Name | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Bone_Names | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Defense_Duration_In_Secs | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Frames | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Texture | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Beam_Width | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Laser_Color | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Lightning_Effect_Name | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Dist_Travelled | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Types_Targeted | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Protection_Radius | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Recharge_Time_In_Secs | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Abilities/Laser_Defense_Ability/Zap_SFXEvent | todo | application not recorded | — | 3240 | — | Only space tactical session runs service_laser_defence; step_land never calls it, so the land Juggernaut never zaps.; basis: reviewed |
| Alternate_Max_Rate_Of_Turn | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Alternate_Max_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Asteroid_Damage_Hit_Particles | todo | application not recorded | — | 3776 | — | read only by battle_effects_prepare.cpp (space impact looks); no land view reads it; basis: reviewed |
| Attack_Animation_Is_Overlay | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Auto_Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Cache_Crusher_Boxes | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Converted_To_Enemy_Die_Time_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Custom_Hard_YExtent_Deployed | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Custom_Hard_YExtent_Offset | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Damaged_Smoke_Asset_Name | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Death_Explosions_End | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Death_SFXEvent_End_Die | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Deployed_Max_Attack_Distance_Multiplier | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Deployed_Turret_Elevate_Extent_Degrees | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Deployed_Turret_Rotate_Extent_Degrees | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Deployed_Walk_Anim_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Deployed_Walk_Speed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Deployment_Anim_Rate | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Destroy_When_Stunned_Over_Water | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Exit_Door_Angle_Degrees | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; basis: reviewed |
| Exit_Door_Distance | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; basis: reviewed |
| Fire_Inaccuracy_Distance | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Fire_Weapon_When_Deployed | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Weapon_When_In_Normal_Attack_Mode | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Weapon_When_In_Rocket_Attack_Mode | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Weapon_When_Undeployed | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fires_Forward | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Glory_Cinematics | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| HardPoints | todo | application not recorded | — | 3776 | — | space twin applied but no land reader of HardPoints on origin/main; basis: reviewed |
| Hover_Height | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Is_Supercrusher | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Locomotor_Has_Animation_Priority | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Max_Dist_Walk_While_Deployed | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MaxSecondaryTurnROTCoefficient | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MinSecondaryTurnROTCoefficient | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MovementBoxExpansionFactor | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MovementPredictionInterval | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Must_Face_Target_To_Attack_When_Deployed | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Overall_Length | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Overall_Width | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| PathFrameDelay | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Presence_Range | todo | application not recorded | — | 3776 | — | No land-path reader.; basis: reviewed |
| Primary_Locomotor_Name | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Ranking_In_Category | todo | application not recorded | — | 3776 | — | category ranking read only by space and galactic audio paths; basis: reviewed |
| Secondary_Locomotor_Name | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| SecondaryTurnAngle | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| SecondaryTurnInPlaceROTCoefficient | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| SecondaryTurnLookaheadDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Shield_Armor_Type | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Shield_Sub_Mesh | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Shielded_Only_When_Deployed | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Should_Cause_Limited_Turrets_To_Turn | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Should_Reinforcements_Move | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; basis: reviewed |
| Space_Full_Stop_Command | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Spin_Away_On_Death | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Spin_Away_On_Death_Chance | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Spin_Away_On_Death_Explosion | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Spin_Away_On_Death_SFXEvent_Start_Die | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Spin_Away_On_Death_Time | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Stationary_Space_Layer | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Stopped_Rate_Of_Turn | todo | application not recorded | — | 3776 | — | Loaded in unit_tables_land.cpp:183 and validated in land_content.hpp; no land motion reader; basis: reviewed |
| Target_Bones | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Targeting_Fire_Inaccuracy_Fixed_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Targeting_Land_Model_Stay_Horiz_Flat | todo | application not recorded | — | 3776 | — | No code reads it.; basis: reviewed |
| Tread_Scroll_Rate | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Turret_Deployed_Rest_Angle | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Turret_Rest_Angle | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Disable_Movement | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Max_Secs_For_AE_Delayed_Damage | todo | application not recorded | — | 3776 | — | Read into projectile blast max_delay (unit_tables_decode.cpp:547); land combat apply not verified; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Uses_Multiple_Locomotors | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| UseSecondaryFacing | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Vehicle_Thief_Inside_Clone | todo | application not recorded | — | 3776 | — | Vehicle transmutation and kill credit missing (WLAB-34); basis: reviewed |

## GroundVehicle, HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Projectile_Types_Override | todo | application not recorded | — | 3776 | — | Parsed (unit_tables_abilities_nested.cpp:96) and used by viewer effects; land apply not verified; basis: reviewed |

## GroundVehicle, Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Arc_Sweep_Attack_Ability/@Name | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Apply_Damage_Right_To_Left | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Width_In_Degrees | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Speedup | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Attack_Animation_Subindex | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Begin_Applying_Damage_Frame | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Sweep_Time_In_Secs | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Type | todo | application not recorded | — | 3242 | — | No ability reader; registry HeroUnit 'applied' is weapon/projectile Damage_Type, not this ability; Arc_Sweep creature sweep (G5) missing; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Drain_Life_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Eat_Attack_Ability/@Name | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Attachment_Frame | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Attack_Animation | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Attack_Animation_Speedup | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Damage_Type | todo | application not recorded | — | 3242 | — | No ability reader; registry HeroUnit 'applied' is weapon/projectile Damage_Type, not this ability; Eat_Attack attach/destroy (G5) missing; basis: reviewed |
| Abilities/Eat_Attack_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Health_Per_Victim | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Owner_Attachment_Bone | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Target_Destruction_Frame | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Target_Grabbed_Animation | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/@Name | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Attack_Animation | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Attack_Animation_Speedup | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Frame_Number | todo | application not recorded | — | 3242 | — | Creature pound handler (Generic_Attack_Ability) is not in the land sim; no grep hit in src for the handler.; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3242 | — | Creature pound handler (Generic_Attack_Ability) is not in the land sim; no grep hit in src for the handler.; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Type | todo | application not recorded | — | 3242 | — | Creature pound handler (Generic_Attack_Ability) is not in the land sim; no grep hit in src for the handler.; basis: reviewed |
| Air_Vehicle_Turret_Target | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Collidable_By_Projectile_Dead | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Custom_Hard_XExtent_Offset | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Has_Pre_Turn_Anim | todo | application not recorded | — | 3776 | — | Loaded in unit_tables_land.cpp:196 and listed in land_content.hpp; no land motion/anim reader (identity hash only); basis: reviewed |
| Hover_Offset | todo | application not recorded | — | 3776 | — | Read only by campaign space entry (space_entry_stock.cpp, space_entry.cpp); basis: reviewed |
| IsDeathCloneObstacle | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Layer_Z_Adjust | todo | application not recorded | — | 3776 | — | Read by space placement (skirmish/start.cpp, space_entry.cpp, session_squadrons.cpp); land placement does not read it; basis: reviewed |
| Max_Lift | todo | application not recorded | — | 3776 | — | Read only by craft motion (unit_motion.cpp craft loop, fighters.hpp); no land reader; basis: reviewed |
| Max_Thrust | todo | application not recorded | — | 3776 | — | Read only by craft motion and UI icon (unit_motion.cpp, world_ui_prepare.cpp); no land reader; basis: reviewed |
| MaxFacingLookAheadFrames | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Prepare_Strafe_Height | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Turret_Targets_Air_Vehicles | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Turret_Targets_Anything_Else | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |

## GroundVehicle, Indigenous_Unit, Marker, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Ground_Infantry | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Turret_Targets_Ground_Vehicles | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |

## GroundVehicle, Indigenous_Unit, Slave_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| SFXEvent_Engine_Idle_Loop | todo | application not recorded | — | 3776 | — | battle_audio_prepare.cpp:420 reads it for space battles only; land mode does not use BattleAudio; basis: reviewed |

## GroundVehicle, Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Value | todo | application not recorded | — | 3776 | — | Entry slot check needs it; no land containment on main; basis: reviewed |
| Ground_Vehicle_Turret_Target | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |

## GroundVehicle, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Garrison_Bone_Names | todo | application not recorded | — | 3776 | — | Garrison bones drive debug-build entry and exit; no land bone lookup on main; basis: reviewed |
| Garrison_Enter_Dist | todo | application not recorded | — | 3776 | — | Entrance distance missing from land garrison movement (P6); basis: reviewed |
| Garrison_Exit_Dist | todo | application not recorded | — | 3776 | — | Exit geometry missing from land garrison movement (P6); basis: reviewed |
| Garrison_Radius | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; only a registry scope test names it; basis: reviewed |
| Num_Garrison_Slots | todo | application not recorded | — | 3776 | — | Only the HUD doc names it (garrison slot dots); no slot reader on main; basis: reviewed |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Allow_Opportunity_Fire_When_Targeting | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | A-02, DG-35, R-01, W-04, WCC-17; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Allows_Special_Weapon_Use | applied | whole class (src/sim/tactical/session_step_finish.cpp); whole class (src/units/unit_tables_profiles.cpp) | WSW-48; WSW-48 | — | src/sim/tactical/session_step_finish.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| Blast_Ability_Fire_Projectile_Type | partial | The_Peacebringer (src/units/unit_combat.cpp); missing: other SPECIAL carriers | WAD-31, WAD-35, WAD-36 | 650 | src/units/unit_combat.cpp | Peacebringer SPECIAL shots select the charged projectile and retain its flight, area and presentation profile; other carriers remain gated.; basis: reviewed |
| Death_Breakoff_Prop | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp) | BA-17, BP-30, DG-38, WCC-62 | — | apps/viewer/src/battle_audio_prepare.cpp | basis: auto |
| Engine_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Fire_Cone_Height | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | WAD-32, WAD-33, WAD-37; P-04, W-07, W-09 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Cone_Width | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | WAD-32, WAD-33, WAD-37; A-06, P-04, W-07 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Inaccuracy_Distance | applied | whole class (src/units/unit_tables_decode.cpp) | DG-24 | — | src/units/unit_tables_decode.cpp | basis: reviewed |
| Fire_Max_Recharge_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-21, S-01, W-06, W-06a; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Min_Recharge_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-21, W-06, W-06a, W-10; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Projectile_Type | applied | whole class (src/sim/tactical/combat_fire.cpp); whole class (src/sim/tactical/damage.cpp) | WAD-31, WAD-32; WWP-18 | — | src/sim/tactical/combat_fire.cpp; src/sim/tactical/damage.cpp | SPECIAL uses the selected authored projectile through ordinary weapon service; a projectile-free attachment has no firing slot.; basis: auto |
| Fire_Pulse_Count | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | W-06, W-06a, W-10, W-11; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Pulse_Delay_Seconds | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | W-06, W-06a, W-10, W-11; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_Range_Distance | applied | whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp) | AB-66, DG-23, P-04; WAD-32, WAD-33, WAD-37 | — | src/units/unit_combat.cpp | basis: auto |
| Fire_When_Deployed | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; hardpoint firing is not gated by a mode.; basis: reviewed |
| Fire_When_In_Normal_Attack_Mode | partial | space hardpoint weapons (src/sim/tactical/combat_fire.cpp); missing: land hardpoint firing | WRA-02 | 650 | src/sim/tactical/combat_fire.cpp | Space weapon profiles retain both flags (default Yes) and each firing attempt reads the current rocket mode; land firing is a separate integration.; basis: reviewed |
| Fire_When_In_Power_To_Weapons_Mode | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; hardpoint firing is not gated by a mode.; basis: reviewed |
| Fire_When_In_Rocket_Attack_Mode | partial | space hardpoint weapons (src/sim/tactical/combat_fire.cpp); missing: land hardpoint firing | WRA-02 | 650 | src/sim/tactical/combat_fire.cpp | Space weapon profiles retain both flags (default Yes) and each firing attempt reads the current rocket mode; land firing is a separate integration.; basis: reviewed |
| Fire_When_Undeployed | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; hardpoint firing is not gated by a mode.; basis: reviewed |
| Full_Salvo_Weapon_Delay_Multiplier | applied | loaded space hardpoint weapons (src/sim/tactical/combat_fire.cpp) | WAB-80 | — | src/sim/tactical/combat_fire.cpp | — |
| Health | applied | whole class (src/units/unit_durability.cpp) | DG-38, HD-02, HD-21, WU-22 | — | src/units/unit_durability.cpp | basis: auto |
| Is_Destroyable | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/units/unit_durability.cpp) | WAD-14, WAD-19; none recorded | — | src/sim/tactical/blast.cpp; src/units/unit_durability.cpp | basis: auto |
| Is_Targetable | applied | whole class (src/units/unit_combat.cpp) | none recorded | — | src/units/unit_combat.cpp | basis: auto |
| Is_Turret | partial | ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: land hardpoints | HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; MC-06, WAD-39 | 4532 | src/sim/tactical/combat_aim.cpp; src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |
| Manual_Hardpoint_Firing_Cooldown_Secs | partial | one-pulse manual space hardpoints (src/units/unit_combat.cpp); missing: nonmanual or other manual burst hardpoints | MC-04, MC-05, WAD-40 | 4532 | src/units/unit_combat.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Power_To_Weapons_Mode_Weapon_Delay_Multiplier | todo | application not recorded | — | 650 | — | weapons-shields: The POWER_TO_WEAPONS ability kind exists, but no reader binds this hardpoint delay multiplier into weapon cadence.; basis: reviewed |
| Repair_Amount_Per_Frame | applied | whole class (src/sim/tactical/durability.cpp); whole class (src/units/unit_durability.cpp) | WSL-41, WSL-42; HR-01 | — | src/sim/tactical/durability.cpp; src/units/unit_durability.cpp | Live station command and partitioned repair service apply the authored values; zero amount still pays without gain.; CHECK-842; basis: reviewed |
| Requires_Manual_Target_Assignment | partial | one-pulse manual space hardpoints (src/sim/tactical/combat_aim.cpp); station automatic-fire guard (src/sim/tactical/combat_fire.cpp); missing: nonmanual or other manual burst hardpoints | MC-01, MC-02, WAD-39; WAD-39 | 4532 | src/sim/tactical/combat_aim.cpp; src/sim/tactical/combat_fire.cpp | Manual assignment, player clock and mechanical fire frames use the authored value; visual mesh articulation remains gated.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Shield_Generator_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Special_Ability_Name | partial | space skirmish station radar (src/sim/tactical/session_step_finish.cpp); skirmish station supply docks (src/skirmish/economy.cpp); missing: non-radar handler lock consumers and ground structures | WHD-05, WRAD-04; PU-04, WHD-05 | 715 | src/sim/tactical/session_step_finish.cpp; src/skirmish/economy.cpp | Named owner/type lock state persists; station radar consumes it. Supply dock income retains its existing local predicate.; basis: reviewed |
| Type | applied | whole class (apps/viewer/src/battle_audio_prepare.cpp); BLAST, LEECH_SHIELDS and TRACTOR_BEAM carriers (src/units/unit_abilities.cpp); whole class (src/units/unit_combat.cpp); whole class (src/units/unit_combat.cpp); whole class (src/units/unit_durability.cpp) | DG-05, DG-06, FO-09, WSU-38; WHD-01; MD-01; WAD-31, WAD-32; WAD-31 | — | apps/viewer/src/battle_audio_prepare.cpp; src/units/unit_abilities.cpp; src/units/unit_combat.cpp; src/units/unit_durability.cpp | basis: reviewed |
| Weapon_Hardpoint | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## HardPoint, HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Category_Restrictions | applied | whole class (src/units/unit_combat.cpp) | ML-53, R-09, R-10 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, Projectile, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-25, G-02, G-3, WCC-22, WWP-26 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, Projectile, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25 | — | src/units/unit_combat.cpp | basis: auto |

## HardPoint, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Rest_Angle | partial | one-pulse manual space hardpoints (src/sim/tactical/combat.cpp); ordinary space hardpoint turrets (src/sim/tactical/combat_aim.cpp); space objects declaring TURRET (src/sim/tactical/combat_aim.cpp); missing: land hardpoints and remaining non-turret object interfaces | MC-06, WAD-40; HPT-01, HPT-02, HPT-04, HPT-05, HPT-06; UA-05, WWP-17 | 4532 | src/sim/tactical/combat.cpp; src/sim/tactical/combat_aim.cpp | Manual assignment, player clock and mechanical fire frames retain their existing rules; UA-05 ordinary TURRET space objects also apply mechanical readiness and viewer articulation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. HPT-01..08 apply ordinary space hardpoint turret mechanics and committed parent/attached presentation; manual command eligibility stays separate.; basis: reviewed |

## HardPoint, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Appearance_Delay_Frames | partial | loaded space hardpoint weapons (src/sim/tactical/projectiles.cpp); missing: non-space objects | MC-07, WAD-37, WAD-40 | 4532 | src/sim/tactical/projectiles.cpp | Positive delay controls projectile visibility and first movement independently of manual assignment.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Hero_Clash

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Clash_Actions/@SubObjectList | todo | application not recorded | — | 650 | — | — |
| Clash_Actions/Attack_Action/Action_Distance_From_Target_Curve | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Action_Health_Percentage_Curve | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Action_Object | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Action_Relevance_Type | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Action_Target | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Accuracy_Modifier | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Number_Of_Shots | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Attack_Action/Attack_Projectile_Type | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Ability_Cooldown_Time_In_Secs | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Action_Distance_From_Target_Curve | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Action_Health_Percentage_Curve | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Action_Object | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Action_Relevance_Type | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Actions/Special_Ability_Action/Action_Target | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Range | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Clash_Type | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Combat_Distance | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Damage_Amount | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Damage_Percentage | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| First_Hero_Damage_Multiplier | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| First_Hero_Type | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Involved_Hero_Types | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Hero clash definitions are not loaded: no Hero_Clash reader exists under src/ or apps/.; basis: reviewed |
| Play_Conversation_Events | todo | application not recorded | — | 650 | — | — |
| Second_Hero_Damage_Multiplier | todo | application not recorded | — | 650 | — | — |
| Second_Hero_Type | todo | application not recorded | — | 650 | — | — |

## HeroCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Remote_Bomb_Ability/@Name | todo | application not recorded | — | 3243 | — | Remote bomb placement and detonation absent on land; no handler code.; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Remote bomb placement and detonation absent on land; no handler code.; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Bomb_Type | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Spawn_Bone | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Spawn_Frame | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Toss_Anim | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Always_Spawn_In_Orbit | partial | authored company members (src/sim/galactic/production.cpp); missing: the HeroCompany type itself (flag read only from its members) | WGS-80 | 651 | src/sim/galactic/production.cpp | Parsed by the galactic type table (src/units/galactic_types.cpp). The placement path (src/sim/galactic/production.cpp&#35;place_nested_force, WGS-80) reads the flag from the authored company MEMBERS, not from the company type itself; the one HeroCompany that authors the tag has no retail evidence of a reader on the company type, so it stays unproven there.; basis: reviewed |
| Available_In_Skirmish | todo | application not recorded | — | 3776 | — | no code on origin/main reads this tag; land hero company gating not found; basis: reviewed |
| Build_Limit_Current_Per_Player | partial | ship (include/eawr/presentation/ui/production.hpp); reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); ship (src/sim/tactical/session_economy.cpp); missing: land and campaign companies, other objects and consumers outside the live galactic closure | WPR-61, WPR-62, WPR-63; WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WHE-40, WPR-33 | 4532 | include/eawr/presentation/ui/production.hpp; src/campaign/live_setup.cpp; src/sim/tactical/session_economy.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Limit_Lifetime_For_All_Allies | partial | ship (include/eawr/presentation/ui/production.hpp); ship (include/eawr/sim/tactical/economy.hpp); missing: land and campaign companies | WPR-61, WPR-62, WPR-63; WHE-05, WHE-40, WPR-33 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp | Space companies retain the authored purchase type through queue, pool and deployment; negative lifetime limits remain unbounded. Ground and campaign consumers are separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Limit_Lifetime_Per_Player | partial | ship (include/eawr/presentation/ui/production.hpp); ship (include/eawr/sim/tactical/economy.hpp); reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: land and campaign companies, other objects and consumers outside the live galactic closure | WPR-61, WPR-62, WPR-63; WHE-05, WHE-40, WPR-33; WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 4532 | include/eawr/presentation/ui/production.hpp; include/eawr/sim/tactical/economy.hpp; src/campaign/live_setup.cpp | Space companies retain the authored purchase type through queue, pool and deployment; negative lifetime limits remain unbounded. Ground and campaign consumers are separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Build_Tab_Heroes | partial | observer-local galactic production choices (apps/viewer/src/galactic/planet_hud.cpp); missing: tactical build menus and unavailable galactic production providers | WGUI-91 | 654 | apps/viewer/src/galactic/planet_hud.cpp | basis: reviewed |
| Build_Tab_Space_Units | partial | observer-local galactic production choices (apps/viewer/src/galactic/planet_hud.cpp); missing: tactical build menus and unavailable galactic production providers | WGUI-91 | 654 | apps/viewer/src/galactic/planet_hud.cpp | basis: reviewed |
| Can_Be_Only_One | todo | application not recorded | — | 3776 | — | no reader found on origin/main; basis: reviewed |
| CategoryMask | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp) | WAR-21, WAR-26 | — | src/campaign/auto_resolve_strength.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Combat_Power_Value | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Company_Transport_Unit | partial | galactic current build limits (src/sim/galactic/production.cpp); ship (src/skirmish/economy.cpp); missing: land and campaign companies | WGEP-78; WHE-05, WHE-49 | 4532 | src/sim/galactic/production.cpp; src/skirmish/economy.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate. GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Company_Units | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); galactic current-type membership (src/data/xml_merge.cpp); galactic current build limits (src/sim/galactic/production.cpp); ship (src/skirmish/economy.cpp); land company profile (src/units/unit_tables_land.cpp); missing: land and campaign companies | WAR-20, WAR-25, WAR-26; WGA-363, WGA-364; WGEP-78; WHE-05, WHE-49; LC-01, LC-04 | 4532 | src/campaign/auto_resolve_strength.cpp; src/data/xml_merge.cpp; src/sim/galactic/production.cpp; src/skirmish/economy.cpp; src/units/unit_tables_land.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. L2a (&#35;1450): a land load keeps the ordered member list with per-member kind, locomotor and hero identity.; basis: reviewed |
| Is_Generic_Hero | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-28, WGEP-32, WGUI-18 | 4532 | src/campaign/live_setup.cpp | WHE-01/05: hero identity has independent named and generic flags in space. Company generic-identity consumers remain unimplemented; parsing alone is not application.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Is_Named_Hero | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); ship (src/skirmish/inputs.cpp); missing: land and campaign companies | WAR-24; WHE-01, WHE-04 | 4532 | src/campaign/auto_resolve_strength.cpp; src/skirmish/inputs.cpp | Space skirmish company identity and deployment preserve logical purchases; ground and campaign consumers remain separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Is_Stealth_Company | todo | application not recorded | — | 4532 | — | GC loader retains inherited metadata; live stealth object/hero predicates require the action provider, not a company-flag inference.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |
| Land_Override_Population_Value | todo | application not recorded | — | 3776 | — | Retail has a land population getter; no land override reader on main; basis: reviewed |
| Ranking_In_Category | partial | locally owned named galactic heroes (apps/viewer/src/galactic/hero_map_bar.hpp); missing: hero ranking consumers outside the local named-hero galaxy map bar | WGUI-89 | 654 | apps/viewer/src/galactic/hero_map_bar.hpp | Inherited authored rank orders the local named-hero map bar; non-bar representative consumers remain outside this application.; basis: reviewed |
| SFXEvent_Hero_Respawned | todo | application not recorded | — | 3776 | — | no reader found for this tag on main; basis: reviewed |
| Show_Hero_Head | applied | whole class (apps/viewer/src/world_ui_prepare.cpp) | WU-47 | — | apps/viewer/src/world_ui_prepare.cpp | Show_Hero_Head read in world UI prepare (WU-47); basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Flag | todo | application not recorded | — | 3776 | — | AB-26 note: HeroCompany ability block skipped on load; todo; basis: reviewed |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Absorb_Blaster_Ability/Absorb_Chance | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Damage_Absorb_Amount | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Absorb_Blaster_Ability/Damage_Absorb_Percentage | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Apply_Damage_Right_To_Left | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Arc_Width_In_Degrees | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Begin_Applying_Damage_Frame | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Sweep_Time_In_Secs | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Arc_Sweep_Attack_Ability/Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25 | — | src/units/unit_combat.cpp | basis: auto |
| Abilities/Arc_Sweep_Attack_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Arc_Sweep_Attack_Ability element; Arc_Sweep creature sweep (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Berserker_Ability/Activation_Max_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Activation_Min_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Berserker_Damage | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Berserker_Damage_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Berserker_Ability/Berserker_Duration_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Activation_Style | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Drain_Life_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Damage_Per_Second | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Drain_Radius | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Duration_In_Seconds | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Drain_Life_Ability/Should_Heal | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Find_Weakness_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The block parses into a special-ability profile (load_special_ability) but no find_weakness handler exists in the sim; nothing applies the weakness bonus.; basis: reviewed |
| Abilities/Find_Weakness_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The block parses into a special-ability profile (load_special_ability) but no find_weakness handler exists in the sim; nothing applies the weakness bonus.; basis: reviewed |
| Abilities/Find_Weakness_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The block parses into a special-ability profile (load_special_ability) but no find_weakness handler exists in the sim; nothing applies the weakness bonus.; basis: reviewed |
| Abilities/Find_Weakness_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The block parses into a special-ability profile (load_special_ability) but no find_weakness handler exists in the sim; nothing applies the weakness bonus.; basis: reviewed |
| Abilities/Find_Weakness_Ability/Damage_Bonus_Percentage | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Not read for Find_Weakness_Ability (only Combat_Bonus_Ability reads Damage_Bonus_Percentage); no find_weakness handler exists in the sim.; basis: reviewed |
| Abilities/Find_Weakness_Ability/Initially_Enabled | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The block parses into a special-ability profile (load_special_ability) but no find_weakness handler exists in the sim; nothing applies the weakness bonus.; basis: reviewed |
| Abilities/Force_Cloak_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Cloak_Ability/Force_Cloak_Transition_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Activation_Max_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Activation_Min_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Confuse_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Confuse_Time_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Confuse_Ability/Confuse_Travel_Time_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Excluded_Unit_Types | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Percent | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Single_Target_Heal | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Sight_Ability/Activation_Style | todo | application not recorded | — | 3238 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Sight_Ability/Force_Sight_Duration_In_Secs | todo | application not recorded | — | 3238 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Activation_Chance | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Activation_Max_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Activation_Min_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Activation_Style | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Damage_Amount | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Damage_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Force_Whirlwind_Ability/Travel_Time_In_Secs | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Galactic_Stealth_Ability/Causes_Despawn | todo | application not recorded | — | 760 | — | Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Jabba_The_Hutt.; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Amount | todo | application not recorded | — | 3242 | — | No main code reads Generic_Attack_Ability element; Generic_Attack pound (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Frame_Number | todo | application not recorded | — | 3242 | — | Creature pound handler (Generic_Attack_Ability) is not in the land sim; no grep hit in src for the handler.; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Percentage | todo | application not recorded | — | 3242 | — | Creature pound handler (Generic_Attack_Ability) is not in the land sim; no grep hit in src for the handler.; basis: reviewed |
| Abilities/Generic_Attack_Ability/Damage_Type | applied | whole class (src/units/unit_combat.cpp) | AB-66, DG-12, DG-23, DG-25 | — | src/units/unit_combat.cpp | basis: auto |
| Abilities/Grenade_Attack_Ability/Activation_Chance | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Explode_Timer_In_Secs | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Type | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Requires_Direct_Player_Command | todo | application not recorded | — | 3243 | — | Grenade handler not in land sim; no Grenade_Attack code on main outside walk docs.; basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/Activation_Style | partial | galactic ground-activated abilities (src/sim/galactic/ability_host.cpp); missing: activation groups outside galactic ground deployment | WGCH-157, WGM-34 | 760 | src/sim/galactic/ability_host.cpp | Authored ground activation membership is bound by the campaign loader and checked against the live service on final arrival.; basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/Causes_Despawn | todo | application not recorded | — | 3244 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Hack_Super_Weapon_Ability/Initially_Enabled | applied | live galactic hack ability service default (src/campaign/live_setup.cpp) | WGCH-139, WGCH-180, WGCH-57 | — | src/campaign/live_setup.cpp | basis: reviewed |
| Abilities/Infection_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Damage_Per_Second | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Distance_To_Infect | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Duration_Of_Infection | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Infection_CategoryMask | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Projectile_Type | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Infection_Ability/Seconds_To_Infect | todo | application not recorded | — | 3243 | — | Infection state and throw machine absent on land (walk: missing G6); no handler code.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Major_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Galactic bounty-hunter (GenericHeroUnit) neutralization: the GC prepare path accepts a minor or major target only when the actor's CAN_NEUTRALIZE flag is set (ability_neutralize.cpp, valid = named ? can_major : can_minor), with the flags parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none). The ability host is keyed by authored name and profile, so the same reader serves the HeroUnit carrier.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Can_Neutralize_Minor_Heroes | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-52 | — | src/sim/galactic/ability_neutralize.cpp | Galactic bounty-hunter (GenericHeroUnit) neutralization: the GC prepare path accepts a minor or major target only when the actor's CAN_NEUTRALIZE flag is set (ability_neutralize.cpp, valid = named ? can_major : can_minor), with the flags parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none). The ability host is keyed by authored name and profile, so the same reader serves the HeroUnit carrier.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Owner_Respawn_Time_In_Secs | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-21 | — | src/sim/galactic/ability_neutralize.cpp | GC neutralization samples the actor and target absence times from the authored OWNER/TARGET_RESPAWN_TIME_IN_SECS ranges in prepare_neutralization (ability_neutralize.cpp absence_sample), parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none). The ability host is keyed by authored name and profile, so the same reader serves the HeroUnit carrier.; basis: reviewed |
| Abilities/Neutralize_Hero_Ability/Target_Respawn_Time_In_Secs | applied | whole class (src/sim/galactic/ability_neutralize.cpp) | WGCH-21 | — | src/sim/galactic/ability_neutralize.cpp | GC neutralization samples the actor and target absence times from the authored OWNER/TARGET_RESPAWN_TIME_IN_SECS ranges in prepare_neutralization (ability_neutralize.cpp absence_sample), parsed in live_setup.cpp install_live_ability_services. Basis: reviewed; no ticket (applied rows carry none). The ability host is keyed by authored name and profile, so the same reader serves the HeroUnit carrier.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Amount | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Arc_In_Degrees | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Damage_Delay_In_Secs | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Personal_Flame_Thrower_Ability/Fire_Time_In_Secs | todo | application not recorded | — | 3243 | — | Flame thrower handler not read on land; FLAME_THROWER only in UI kind tables.; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Contamination handler absent on land; walk marks it missing.; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Contamination handler absent on land; walk marks it missing.; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Contamination handler absent on land; walk marks it missing.; basis: reviewed |
| Abilities/Radioactive_Contaminate_Ability/Spray_Time_In_Secs | todo | application not recorded | — | 3243 | — | Contamination handler absent on land; walk marks it missing.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Activation_Style | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Block_Chance | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Saber_Throw_Ability/Activation_Max_Range | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Saber_Throw_Ability/Activation_Min_Range | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Saber_Throw_Ability/Activation_Style | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Saber_Throw_Ability/Catch_Dist | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Stealth_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Stealth_Ability/Stealth_Transition_Time | todo | application not recorded | — | 3776 | — | Land bodies return in load_land_body before load_abilities; no land-path code reads this tag (space-side abilities.cpp/session_*.cpp are SpaceUnit only); basis: reviewed |
| Abilities/Stun_Ability/Activation_Max_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Activation_Min_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Activation_Style | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Stun_Range | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Stun_Time_In_Secs | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Stun_Ability/Stun_Travel_Time_In_Secs | todo | application not recorded | — | 3241 | — | land bodies return in load_land_body before load_abilities; no land-path code (load_land_body, land_companies, session_land, land_movement, land_start, live_session_land) reads this tag; space handlers do not count; basis: reviewed |
| Abilities/Summon_Ability/Activation_Max_Range | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Summon_Ability/Activation_Min_Range | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Summon_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Summon_Ability/Creation_Delay_In_Seconds | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Summon_Ability/Object_Type | todo | application not recorded | — | 3240 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/System_Spy_Ability/Activation_Style | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Causes_Despawn | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Duration_In_Secs | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Initially_Enabled | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Fleet_Contents | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Force_Sensitive_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Most_Powerful_Ship | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Num_Fleets | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Super_Weapons | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/Tactical_Bribe_Ability/Activation_Max_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/Activation_Min_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Tactical_Bribe_Ability/Activation_Style | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Activation_Chance | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Activation_Max_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Activation_Min_Range | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Activation_Style | todo | application not recorded | — | 3244 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3244 | — | no code reads Vehicle_Thief_Ability on origin/main; land vehicle thief not built; basis: reviewed |
| Abilities/Vehicle_Thief_Ability/Applicable_Unit_Types | todo | application not recorded | — | 3244 | — | no code reads Vehicle_Thief_Ability on origin/main; land vehicle thief not built; basis: reviewed |
| Armor_Type | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Damage_Type | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36). |
| Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Dynamic_Transform_Delay_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Dynamic_Transform_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Energy_Refresh_Rate | todo | application not recorded | — | 3776 | — | no land consumer of energy refresh; basis: reviewed |
| Holster_Disable_Engine_Loops | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Holster_Transition_Time_In_Secs | todo | application not recorded | — | 3776 | — | Only listed in engine-tags.json; no land-path reader or walk rule applies it.; basis: reviewed |
| Is_Force_Sensitive | applied | whole class (src/sim/tactical/session_cmd_lanes.cpp) | WAB-54 | 650 | src/sim/tactical/session_cmd_lanes.cpp | Hunt preference and offset exemption of buzz droids (WAB-54); loaded for any object type that authors it.; basis: reviewed |
| Is_Squashable | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Is_Stationary_When_Attacking | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| MaxJiggleDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MinimumPushReturnDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Occlusion_Silhouette_Enabled | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Presence_Range | todo | application not recorded | — | 3776 | — | No land-path reader.; basis: reviewed |
| Projectile_Appearance_Delay_Frames | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Damage | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit metadata in space; the hero object does not bind its own object-weapon damage. Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Projectile_Types | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Ranged_Target_Z_Adjust | todo | application not recorded | — | 3776 | — | Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Share_Damage_With_Teammates | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Shield_Points | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Shield_Refresh_Rate | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Tactical_Health | partial | standalone space heroes with authored space locomotors (src/units/unit_durability.cpp); standalone space heroes with authored space locomotors (src/units/unit_tables_decode.cpp); missing: carried hero identities and ground-only heroes outside the live space-body path | HD-01; WHE-SQ-02, WPR-33 | 3776 | src/units/unit_durability.cpp; src/units/unit_tables_decode.cpp | A HeroUnit with effective SIMPLE_SPACE_LOCOMOTOR or FIGHTER_LOCOMOTOR loads its own authored body and Tactical_Health feeds live space durability. Carried identities remain metadata with no independent space hull; land combat has its separate binding.; basis: reviewed |
| Target_Stealth_Units | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Targeting_Fire_Inaccuracy | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Targeting_Max_Attack_Distance | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Targeting_Stickiness_Time_Threshold | todo | application not recorded | — | 3776 | — | weapons-shields: Loader::load_unit skips load_body for carried HeroUnit/GenericHeroUnit metadata in space; the ordinary weapon/durability consumer does not apply this authored field to carried heroes. Previous context: Read into the land unit tables by shoot PR 1 (load_land_body, src/units/unit_tables_land.cpp) and bound into the land start content; the land step applies it in shoot PR 3 (docs/behaviour/walks/land-combat.md LC-26..36).; basis: reviewed |
| Type | todo | application not recorded | — | 650 | — | other: The top-level HeroUnit Type has no consumer; hardpoint Type and nested Unit_Ability Type are separate contexts. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Captain_Piet, Commander_Akbar, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+2 more). Mixed ground/space, space carriers on HeroUnit: Captain_Piet, Commander_Akbar, Luke_Skywalker.; basis: reviewed |
| Unique_Ground_Unit | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); craft, ship, squadron (src/units/unit_tables_abilities_bonuses.cpp); missing: ground deployment | WAR-20; WHE-13, WHE-56 | 4532 | src/campaign/auto_resolve_strength.cpp; src/units/unit_tables_abilities_bonuses.cpp | The authored ground container is excluded from a carried hero's space command recipients; ground deployment remains separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Unique_Space_Unit | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); prepared campaign space entry (src/campaign/space_entry.cpp); craft, ship, squadron (src/skirmish/economy.cpp); craft, ship, squadron (src/units/unit_tables_abilities_bonuses.cpp); missing: campaign and ground conversions | WAR-20; WGT-21; WHE-49; WHE-13, WHE-56 | 4532 | src/campaign/auto_resolve_strength.cpp; src/campaign/space_entry.cpp; src/skirmish/economy.cpp; src/units/unit_tables_abilities_bonuses.cpp | The space company resolver selects the authored unique ship and its creation team; command recipients exclude the unique container. Other modes remain separate. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue, Tyber_Zann_Prologue_Cin, UM06_Tyber_Zann, Urai_Fen (+1 more). |
| Unit_Abilities_Data/Unit_Ability/Friendly_Ability | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo. |
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 650 | — | abilities: load_unit_abilities parses the entry and ability_table supports modeled kinds; carried hero or scene-unloaded class instances still lack the corresponding runtime activation/cooldown binding. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+8 more).; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Type | partial | standalone space heroes with their own locomotor (src/units/unit_abilities.cpp); missing: carried hero metadata and land ability runtime | WRA-01 | 650 | src/units/unit_abilities.cpp | Standalone space heroes retain ordinary ability profiles, including ROCKET_ATTACK; carried hero metadata has no corresponding live activation state.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 3776 | — | read in load_body/content.cpp for the space win condition; land_start has no reader (victory_condition fixture only); basis: reviewed |
| Walk_Transition | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-31 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Weather_Category | todo | application not recorded | — | 3225 | — | WLEN-15: no loader or sim field for Weather_Category on land; basis: reviewed |

## HeroUnit, Mobile_Defense_Unit, SecondaryStructure, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Has_Land_Evaluator | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |

## HeroUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WAR-26; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/campaign/auto_resolve_strength.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | partial | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WAR-26; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/campaign/auto_resolve_strength.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | partial | automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## HeroUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Unit_Strength_Category | todo | application not recorded | — | 650 | — | abilities: Only galactic_types.cpp reads this field; tactical CombatBonus/BattlefieldBonus construction does not retain or apply the strength-category selector. Previous context: applied only by the galactic auto-resolve (src/campaign/auto_resolve_*); no tactical code reads it, and whether FoC's tactical battle does is unverified (&#35;4002); basis: reviewed |

## HeroUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Super_Weapon_Killer | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); galactic tactical return (src/campaign/game_session_lifecycle.cpp) | WAR-25; WGS-28 | — | src/campaign/auto_resolve_strength.cpp; src/campaign/game_session_lifecycle.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Respawn_Whole_Team_When_Killed | partial | live galactic ability consumption (src/campaign/live_setup.cpp); campaign tactical return (src/campaign/tactical_return_live.cpp); missing: cinematic flight cadence and hero placement | WGCH-70, WGM-79, WGM-80; WGM-79 | 1116 | src/campaign/live_setup.cpp; src/campaign/tactical_return_live.cpp | Validated return bindings feed the native countdown, attrition and scheduled hero records; the remaining cinematic cadence and placement owners are explicit services. Live ability consumption also sources these values for the shared saved hero-return queue.; basis: reviewed |

## HintSets

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| */* | todo | application not recorded | — | 650 | — | — |

## Indigenous_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Eat_Attack_Ability/Needs_To_Rotate | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Abilities/Eat_Attack_Ability/Rotation_Base_Bone_Name | todo | application not recorded | — | 3242 | — | No main code reads Eat_Attack_Ability element; Eat_Attack attach/destroy (G5) missing per land-abilities walk; basis: reviewed |
| Allow_Idle_When_Moving | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Avoidance_Disabled | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Begin_Turn_Towards_Distance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Harass_Enemy_Exclusion_Range | todo | application not recorded | — | 3776 | — | Only the space harass service (FC-34) uses it; no land indigenous reader; basis: reviewed |
| Required_Planets | todo | application not recorded | — | 3776 | — | Only read by galactic corruption setup and galactic types; no land-side indigenous reader.; basis: reviewed |
| Squash_Damage_Type | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Turret_XY_Only | todo | application not recorded | — | 3776 | — | Only the space load_body TURRET path reads it; land bodies return earlier.; basis: reviewed |

## LensFlares

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| LensFlare/Flare/Diffuse | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; LensFlares definitions are not loaded; no code under src/ or apps/ reads the class.; basis: reviewed |
| LensFlare/Flare/Fraction | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; LensFlares definitions are not loaded; no code under src/ or apps/ reads the class.; basis: reviewed |
| LensFlare/Flare/Num_Slots | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Num_Slots behavior remains unwired.; basis: reviewed |
| LensFlare/Flare/Radius | todo | application not recorded | — | 653 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Radius behavior remains unwired. Previous context: no loader reads it (the scan's match is an unrelated particle property); basis: reviewed |
| LensFlare/Flare/Slot_X | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Slot_X behavior remains unwired.; basis: reviewed |
| LensFlare/Flare/Slot_Y | todo | application not recorded | — | 650 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Slot_Y behavior remains unwired.; basis: reviewed |

## LightningEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bolt_Creation_Interval_Max | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Bolt_Creation_Interval_Min | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Cycles_Per_Distance | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 650 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; basis: reviewed |
| Detail | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Displace | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Number_Bolts | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Update_All | partial | tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | TBF-06, TBF-07, TBF-08 | 650 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; basis: reviewed |
| Width_Max | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Width_Min | partial | repair lightning (apps/viewer/src/battle_effects_projectiles.cpp); tractor and shield-leech lightning (apps/viewer/src/battle_effects_projectiles.cpp); missing: other lightning effects | WSH-08; TBF-06, TBF-07, TBF-08 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | Tractor and shield-leech authored lightning; other consumers remain incomplete.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## LightningEffect, LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Radius | todo | application not recorded | — | 653 | — | vfx-models: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Radius behavior remains unwired. Previous context: no loader reads it (the scan's match is an unrelated particle property); basis: reviewed |

## LightSource

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Auto_Destruct_Time | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Not read anywhere under src/ or apps/; light sources never expire.; basis: reviewed |
| Diffuse | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; LightSource objects are not loaded; space lighting comes from the map environment, not these definitions.; basis: reviewed |
| Falloff_Start | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; LightSource objects are not loaded; space lighting comes from the map environment, not these definitions.; basis: reviewed |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Cash_Point_Radius | todo | application not recorded | — | 650 | — | &#91;production-tech&#93; Not read anywhere under src/ or apps/; no cash-point mechanic exists in the skirmish economy.; basis: reviewed |
| Cash_Point_Transition_Time_Seconds | todo | application not recorded | — | 650 | — | &#91;production-tech&#93; Not read anywhere under src/ or apps/; no cash-point mechanic exists in the skirmish economy.; basis: reviewed |
| CategoryMask | todo | application not recorded | — | 650 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. Mixed ground/space, space carriers on Container: Darth_Vader_TIE_Fighter_Container, Hutt_VWing_Squadron_Container, Red_Squadron_Container, Rogue_Squadron_Space_Container, StarViper_Team, TIE_Defender_Squadron_Container, Y_Wing_Squadron_Container. |
| Control_Point_Domination_Condition_Relevant | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/; no control-point domination victory exists.; basis: reviewed |
| Influences_Cash_Point | todo | application not recorded | — | 650 | — | &#91;production-tech&#93; Not read anywhere under src/ or apps/; no cash-point mechanic exists in the skirmish economy.; basis: reviewed |
| Reinforcement_Enabling_Radius | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Reinforcement_Enabling_Radius behavior remains unwired.; basis: reviewed |
| Reveal_For_Attacker | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Reveal_For_Attacker behavior remains unwired.; basis: reviewed |
| Reveal_For_Defender | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Reveal_For_Defender behavior remains unwired.; basis: reviewed |
| Shield_Points | todo | application not recorded | — | 650 | — | weapons-shields: The generic live-body loader supports this field, but the scene map-only instances of these classes bypass live weapon/durability binding; that class-specific application remains missing. Previous context: the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 650 | — | other: The live-body loader and start rules support the flag, but metadata/map-only instances of this class do not enter that live victory binding. Previous context: the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## Marker, MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Immune_To_Damage | partial | stock galactic lethal roster closure (src/campaign/auto_resolve_live.cpp); Dummy_Barrage_Target (src/units/unit_tables_decode.cpp); missing: other markers and misc objects | WAR-85, WAR-86, WAR-88; WAD-38 | 4532 | src/campaign/auto_resolve_live.cpp; src/units/unit_tables_decode.cpp | The canonical BARRAGE target must be immune and receives no durability profile; its model collision remains available to projectiles. Live galactic auto-resolve reads the effective authored value; other tactical consumers remain outside this adapter.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Marker, MiscObject, MultiplayerStructureMarker, SpaceProp, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Discardable | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |

## Marker, MiscObject, Projectile, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | partial | loaded tactical AI types (src/script/foc/tactical_ai_bindings.cpp); loaded tactical AI types (src/skirmish/ai.cpp); missing: non-AI decoration consumers and loader exclusions | L-32; L-32 | 650 | src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp | L-32: loaded tactical AI types exclude decoration candidates. Other decoration consumers remain incomplete; Dummy_Barrage_Target still returns before load_selection.; basis: reviewed |

## Marker, MultiplayerStructureMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Include_In_UI_Map_Header | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Marker_For_Specific_Object_Type | applied | whole class (src/skirmish/inputs.cpp) | none recorded | — | src/skirmish/inputs.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## Marker, MultiplayerStructureMarker, ScriptMarker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Marker | applied | whole class (src/scene/space_population.cpp) | none recorded | — | src/scene/space_population.cpp | basis: auto |

## Marker, Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Tactical_Health | partial | live space bodies (src/units/unit_durability.cpp); missing: map-only or projectile instances outside live-body loading | none recorded | 650 | src/units/unit_durability.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## Marker, SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## Marker, SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Energy_Refresh_Rate | applied | whole class (src/sim/tactical/damage.cpp) | EN-02 | 650 | src/sim/tactical/damage.cpp | Loaded for every space object type (unit_tables_profiles.cpp) and applied by the energy recharge; a Marker has no energy pool, so only the structure classes use it.; basis: reviewed |
| Projectile_Fire_Pulse_Count | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Projectile_Fire_Recharge_Seconds | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Projectile_Types | partial | live space bodies (src/units/unit_combat.cpp); missing: map-only or projectile instances outside live-body loading | W-06 | 650 | src/units/unit_combat.cpp | weapons-shields: Generic live-body loading binds this field into its tactical profile; map-only or projectile instances outside that loading path remain unbound. No current rule establishes full class-wide coverage. the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## Marker, SecondaryStructure, SpaceStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Reveal_During_Setup_Phase | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Reveal_During_Setup_Phase behavior remains unwired. Previous context: Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, E_Ground_Advanced_Vehicle_Factory, E_Ground_Barracks, E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Heavy_Vehicle_Factory (+58 more).; basis: reviewed |

## Marker, SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Turret_Targets_Anything_Else | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Turret_Targets_Anything_Else behavior remains unwired.; basis: reviewed |

## Marker, SecondaryStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ranged_Target_Z_Adjust | partial | external redirect targets (src/sim/tactical/projectiles.cpp); projectile defence sources (src/sim/tactical/session_world.cpp); missing: ordinary weapon and missile aiming | WPJ-42; WPJ-17 | 4532 | src/sim/tactical/projectiles.cpp; src/sim/tactical/session_world.cpp | Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. |

## Marker, SpaceProp, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | todo | application not recorded | — | 4532 | — | BP-75: the tactical viewer clears tags.background before classification; live arrivals only count backdrop_ships_. No tactical background pass, depth clear or widened clip range applies this flag.; basis: reviewed |
| Is_Valid_Target | partial | craft, ship, station (src/presentation/ui/selection.cpp); loaded hardpoint opportunity candidates (src/sim/tactical/combat_algorithms.hpp); loaded non-team combat candidates (src/sim/tactical/combat_targeting.cpp); loaded direct attack targets after team promotion (src/sim/tactical/session_step_commands.cpp); loaded map props (src/units/unit_tables_decode.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: objects outside the simulated unit closure | WSU-13; R-08; WCC-25; WCC-25; WSU-13; WSU-13 | 650 | src/presentation/ui/selection.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_step_commands.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp | Verified mouse/bar admission for loaded tactical units only; hero gameplay and other combat-valid-target consumers remain separate interfaces. Loaded map props apply the same mouse admission before picking: neutral noncollidable fields pass a right click through to the move point; impassable asteroids retain contacts. WCC-25 applies effective valid-target admission (default true) to ship scan acquisition and non-direct retention/replacement. R-08 also rejects invalid effective types before turret pointing, priority and aim. WCC-25 direct assignment refuses invalid effective types without changing the previous combat target; outer command acceptance and movement remain separate.; basis: reviewed |

## MiscObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Affiliation | applied | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); whole class (src/skirmish/start.cpp) | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; none recorded | — | src/campaign/live_setup.cpp; src/skirmish/start.cpp |  HCB-12a..12f: Multiplayer_Beacon_Type objects author MULTIPLAYER_BEACON in Behavior. Their agreed setup consumes only the resolved beacon type and Beacon_Lifetime_In_Secs; Affiliation, Behavior and Tactical_Health are not beacon inputs. Other MiscObject consumers retain their application and trace requirements.; basis: reviewed |
| Tactical_Health | applied | whole class (src/skirmish/start.cpp) | HD-01, U-04 | — | src/skirmish/start.cpp |  HCB-12a..12f: Multiplayer_Beacon_Type objects author MULTIPLAYER_BEACON in Behavior. Their agreed setup consumes only the resolved beacon type and Beacon_Lifetime_In_Secs; Affiliation, Behavior and Tactical_Health are not beacon inputs. Other MiscObject consumers retain their application and trace requirements.; basis: reviewed |
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Remove_Upon_Death | applied | whole class (apps/viewer/src/live_session_death.cpp) | BP-34, SP-01, SP-03, UA-08 | — | apps/viewer/src/live_session_death.cpp | basis: auto |

## MiscObject, Mobile_Defense_Unit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | applied | whole class (src/units/unit_combat.cpp) | PL-13 | — | src/units/unit_combat.cpp | basis: auto |

## MiscObject, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Expiration_Seconds | applied | space MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/abilities.cpp); space ships with BARRAGE (src/sim/tactical/session_abilities.cpp); whole class (src/units/unit_abilities.cpp) | WAB-03, WAB-05, WHE-31; WAD-38; AB-01, AB-04, BP-23, BP-24, WAB-03 | — | src/sim/tactical/abilities.cpp; src/sim/tactical/session_abilities.cpp; src/units/unit_abilities.cpp | basis: reviewed |

## Mobile_Defense_Unit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Damage | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Weapon_When_Deployed | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Fire_Weapon_When_Undeployed | todo | application not recorded | — | 3239 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Is_Affected_By_Gravity_Control_Field | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Is_Squashable_By_Supercrusher | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| MaxJiggleDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| MinimumPushReturnDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Occlusion_Silhouette_Enabled | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Overall_Length | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Overall_Width | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Tactically_Built_Child_Object_Persists | todo | application not recorded | — | 1458 | — | child_persists decoded only; no land consumer; basis: reviewed |
| Unit_Abilities_Data/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Weather_Category | todo | application not recorded | — | 3225 | — | WLEN-15: no loader or sim field for Weather_Category on land; basis: reviewed |

## Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpaceStructure, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Damage_Alternates | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |
| Land_Damage_SFX | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |
| Land_Damage_Thresholds | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |

## MovementClassType

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| * | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-01 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |

## MusicEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Files | applied | whole class (src/presentation/audio/sfx.cpp) | AI-01, AI-02, R-09 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Loop | applied | whole class (src/presentation/audio/sfx.cpp) | A-01, AI-05, AI-50, U-01 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| MSS_Internal_Loop | partial | galactic music stream output (apps/viewer/src/galactic/galactic_audio.cpp); missing: tactical music stream output | WGP-26 | 650 | apps/viewer/src/galactic/galactic_audio.cpp | The galactic output sets backend stream looping so loading can starve presentation service; tactical output remains deferred.; basis: reviewed |
| Volume_Percent | applied | whole class (src/presentation/audio/sfx.cpp) | BA-40 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |

## Particle

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/@SubObjectList | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | todo | application not recorded | — | 3776 | — | Land ground bodies return in unit_tables_decode.cpp (land_ground_object -&gt; load_land_body) before load_abilities; load_land_body/land_member/land HUD never read this ability tag, space-side readers are not a land anchor; basis: reviewed |
| Dynamic_Transform_Delay_Seconds | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Not read anywhere under src/ or apps/; particles have no dynamic transform.; basis: reviewed |
| Dynamic_Transform_Seconds | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Not read anywhere under src/ or apps/; particles have no dynamic transform.; basis: reviewed |
| In_Background | partial | galactic particle scene layering (apps/viewer/src/galactic/scene_content.cpp); missing: tactical background scene pass, depth clear and widened clip range | WGP-01b | 4532 | apps/viewer/src/galactic/scene_content.cpp | Galactic particle insertion uses the authored scene layer (WGP-01b). Tactical compose clears tags.background and live arrivals only report backdrop_ships_; the BP-75 background pass is not implemented.; basis: reviewed |
| Is_Editor_Placed | todo | application not recorded | — | 3230 | — | no code reads the tag on main; basis: reviewed |

## Particle, SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | applied | Particle (apps/viewer/src/battle_effects_prepare.cpp); whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp); whole class (src/skirmish/start.cpp) | WPJ-40; L-32; L-32; U-04 | — | apps/viewer/src/battle_effects_prepare.cpp; src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp; src/skirmish/start.cpp | basis: reviewed |

## Planet

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Max_Ground_Base | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Priority_Set

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Attack_Priorities | applied | whole class (src/units/unit_priority.cpp) | PI-1, R-09 | — | src/units/unit_priority.cpp | basis: reviewed |
| Category_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| Hard_Point_Priorities | applied | whole class (src/sim/tactical/combat_aim.cpp); whole class (src/units/unit_combat.cpp) | WCC-18, WCC-19; WCC-18 | — | src/sim/tactical/combat_aim.cpp; src/units/unit_combat.cpp | ranks the attacker's best target hardpoint for the object weapon, the A-04 aim and the attack approach; hardpoint weapons keep WWP-16 until the hand-off WCC-17; basis: reviewed |
| Property_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |
| Unit_Exclusions | applied | whole class (src/units/unit_priority.cpp) | none recorded | — | src/units/unit_priority.cpp | no behaviour-note rule mentions this tag yet; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Description | todo | application not recorded | — | 650 | — | other: No Projectile description-attribute reader or runtime consumer exists in src/, apps/ or include/.; basis: reviewed |
| CategoryMask | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; The projectile loader does not read it; no projectile category mask is applied.; basis: reviewed |
| Causes_Infection | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Explode_When_Reached_Target_Radius | partial | DEFAULT space projectiles, space ROCKET projectiles (src/sim/tactical/projectiles.cpp); missing: LIGHTSABER and MPTL_ROCKET | RFL-05, RFL-08, WAD-04 | 4532 | src/sim/tactical/projectiles.cpp | RFL-05/08: ordinary space ROCKET construction stops at retained aim or extends/trims according to this flag; land and returning-saber interfaces remain separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Friendly_Damage_Amount | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; projectiles apply no separate friendly damage.; basis: reviewed |
| Internal_Damage_Type | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read from the XML; combat.hpp only names it in a comment (DG-05: a hard-coded type separate from the XML armor damage types).; basis: reviewed |
| Max_Secs_For_AE_Delayed_Damage | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/blast.cpp) | WAD-17, WAD-21; WAD-17 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Damage | applied | ship, station (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/blast.cpp) | WAD-18, WAD-21; WAD-01, WAD-02 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Dropoff | applied | whole class (src/sim/tactical/blast.cpp) | WAD-15, WAD-16 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Dropoff_Tiers | applied | whole class (src/sim/tactical/blast.cpp) | WAD-16 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Immune_Faction | applied | whole class (src/sim/tactical/blast.cpp) | WAD-09 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Blast_Area_Range | applied | whole class (src/sim/tactical/blast.cpp); ship, station (src/sim/tactical/blast.cpp) | WAD-01, WAD-10, WAD-12; WAD-19 | — | src/sim/tactical/blast.cpp | WAD blast service; secondary hull or authored hardpoint-share routing. Positive area distance delay uses unverified U-04 project policy.; basis: reviewed |
| Projectile_Block_Chance_Modifier | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Category | applied | whole class (src/units/unit_combat.cpp); ordinary space projectiles, space ROCKET projectiles (src/units/unit_combat.cpp) | DG-22, MS-01; RFL-01, RFL-08, WAD-04 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Cause_Invulnerability_Duration_Frames | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Cause_Invulnerability_Max_Targets | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Cause_Invulnerability_On_Detonation | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Cause_Invulnerability_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Cause_Invulnerability_Targets_Category_Mask | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Combat_Mod | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Combat_Mod_Category_Mask | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Combat_Mod_Duration | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Combat_Mod_Value | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Convert_Enemy_On_Detonation | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Convert_Enemy_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Convert_Enemy_Targets_Category_Mask | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Damage_Delay_Secs | applied | whole class (src/sim/tactical/blast.cpp); whole class (src/sim/tactical/session_step_combat.cpp) | WAD-26, WNO-32; WAD-26, WFO-14 | — | src/sim/tactical/blast.cpp; src/sim/tactical/session_step_combat.cpp | Positive explicit delay overrides area-distance delay with a separate synchronized draw per delivery.; basis: reviewed |
| Projectile_Damages_Random_Hard_Points | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Damages_Random_Hard_Points behavior remains unwired.; basis: reviewed |
| Projectile_Disable_Engines_Duration | applied | whole class (src/units/unit_combat.cpp) | EN-08, EN-09 | — | src/units/unit_combat.cpp | Absent duration defaults to zero; negative durations disable indefinitely. Nonnegative durations truncate to logical frames without an authored one-hour limit; finite durations must fit frame storage below the no-expiry sentinel.; basis: reviewed |
| Projectile_Disables_Engines_When_Power_Drained | applied | whole class (src/sim/tactical/session_step_combat.cpp) | EN-07, EN-08, EN-09 | — | src/sim/tactical/session_step_combat.cpp | basis: reviewed |
| Projectile_Does_Energy_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-06, DG-07, DG-09, EN-07 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Does_Hitpoint_Damage | applied | whole class (src/units/unit_combat.cpp) | DG-10, WCC-50 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Does_Shield_Damage | applied | whole class (src/units/unit_combat.cpp) | BP-19, DG-06, DG-09, DG-38, S-45 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Energy_Per_Shot | applied | whole class (src/units/unit_combat.cpp) | EN-05, EN-06 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Grenade_Can_Lob_Slower | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Grenade_Can_Lob_Slower behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Kyle_Katarn_Sticky_Bomb, Proj_Mara_Jade_Sticky_Bomb, Proj_Remote_Bomb, Proj_Sticky_Bomb.; basis: reviewed |
| Projectile_Ground_Detonation_Particle | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Ground_Detonation_SurfaceFX | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Instant_Heal_Duration_Frames | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Projectile_Instant_Heal_Health_Increase | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Instant_Heal_On_Detonation | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Instant_Heal_Radius | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Instant_Heal_Targets_Category_Mask | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Ion_Stun_Duration | applied | whole class (src/units/unit_combat.cpp) | DG-01, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Ion_Stun_On_Detonation | applied | whole class (src/units/unit_combat.cpp) | DG-01, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Ion_Stun_Radius | todo | application not recorded | — | 650 | — | weapons-shields: load_projectile reads the radius, but ShotProfileBuilder rejects nonzero values (IS-08); radial ion-stun delivery is missing. Previous context: the loader reads it for other classes; the M2 scene's objects of this class never have it read Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion.; basis: reviewed |
| Projectile_Ion_Stun_Shot_Rate_Reduction_Percent | applied | whole class (src/units/unit_combat.cpp) | DG-01, IS-01 | — | src/units/unit_combat.cpp | basis: auto |
| Projectile_Length | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-02, R-ROT-01 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |
| Projectile_Max_Flight_Distance | applied | space ROCKET projectiles (src/sim/tactical/projectiles.cpp); space ROCKET post-aim extensions (src/sim/tactical/rocket.cpp); whole class (src/units/unit_combat.cpp) | RFL-05, RFL-08; RFL-05; DG-23, G-03, P-04 | — | src/sim/tactical/projectiles.cpp; src/sim/tactical/rocket.cpp; src/units/unit_combat.cpp | basis: auto |
| Projectile_Max_Lifetime | partial | ordinary space projectiles (src/sim/tactical/projectiles.cpp); missing: grenades and land projectiles | RFL-08 | 4532 | src/sim/tactical/projectiles.cpp | G4 endpoint interface; stock zero-offset target-terminating space paths only. Nonzero offsets, post-aim extension and shield/jamming repaths remain gated; see docs/behaviour/rocket-flight.md.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Max_Scan_Range | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Max_Scan_Range behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Drunk_Missile, Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Ship_Concussion_Missile, Proj_Ship_Concussion_Missile_Bossk, Proj_Ship_Concussion_Missile_Dummy, Proj_Ship_Concussion_Missile_Satellite, Proj_Ship_Concussion_Missile_SSD (+6 more). Mixed ground/space, space carriers on Projectile: Proj_Drunk_Missile, Proj_Krayt_Bombardment_Damage, Proj_Krayt_Bombardment_Ion, Proj_Ship_Concussion_Missile, Proj_Ship_Concussion_Missile_Bossk, Proj_Ship_Concussion_Missile_Dummy, Proj_Ship_Concussion_Missile_Satellite, Proj_Ship_Concussion_Missile_SSD (+6 more).; basis: reviewed |
| Projectile_Redirect_Chance_Modifier | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Rocket_Curve_Distance | applied | space ROCKET projectiles (src/sim/tactical/rocket.cpp); space ROCKET projectiles (src/units/unit_combat.cpp) | RFL-03; ML-54 | — | src/sim/tactical/rocket.cpp; src/units/unit_combat.cpp | RFL-01..06: space ROCKET routes apply authored spacing, seeded spatial offsets and initial/main/post-aim scaling; land-aligned routes remain the land interface.; basis: reviewed |
| Projectile_Rocket_Curve_Offset | applied | space ROCKET projectiles (src/sim/tactical/rocket.cpp); space ROCKET projectiles (src/units/unit_combat.cpp) | RFL-03; ML-54 | — | src/sim/tactical/rocket.cpp; src/units/unit_combat.cpp | RFL-01..06: space ROCKET routes apply authored spacing, seeded spatial offsets and initial/main/post-aim scaling; land-aligned routes remain the land interface.; basis: reviewed |
| Projectile_Rocket_Straight_Distance | applied | space ROCKET projectiles (src/sim/tactical/rocket.cpp); space ROCKET projectiles (src/units/unit_combat.cpp) | RFL-02; ML-54 | — | src/sim/tactical/rocket.cpp; src/units/unit_combat.cpp | RFL-01..06: space ROCKET routes apply authored spacing, seeded spatial offsets and initial/main/post-aim scaling; land-aligned routes remain the land interface.; basis: reviewed |
| Projectile_Stun_Duration_Frames | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Stun_Duration_Frames behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb.; basis: reviewed |
| Projectile_Stun_On_Detonation | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Stun_On_Detonation behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb.; basis: reviewed |
| Projectile_Stun_Radius | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Stun_Radius behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb.; basis: reviewed |
| Projectile_Stun_Victims_Category_Mask | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Stun_Victims_Category_Mask behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb.; basis: reviewed |
| Projectile_Stun_Victims_Unit_Types | todo | application not recorded | — | 650 | — | weapons-shields: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Projectile_Stun_Victims_Unit_Types behavior remains unwired. Previous context: Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion. Mixed ground/space, space carriers on Projectile: Proj_Krayt_Bombardment_Ion.; basis: reviewed |
| Projectile_Target_Point_On_Terrain | todo | application not recorded | — | 3776 | — | no land-path reader: load_land_body does not decode this tag (space decoders/viewer presentation are not land anchors); basis: reviewed |
| Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Weaken_Enemy_Duration_Seconds | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Weaken_Enemy_On_Detonation | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Weaken_Enemy_Radius | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Weaken_Enemy_Take_Damage_Increase_Percent | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Weaken_Enemy_Targets_Category_Mask | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Projectile_Width | applied | whole class (apps/viewer/src/battle_effects_prepare.cpp) | BP-04, BP-07, G-08 | — | apps/viewer/src/battle_effects_prepare.cpp | basis: auto |

## RadarMapSettings

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Land_Backdrop_Texture_Name | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Land_FOW_Color | todo | application not recorded | — | 3776 | — | no reader found; basis: reviewed |
| Land_Is_Guide_Rectangle | todo | application not recorded | — | 3776 | — | no code reads the tag on main; basis: reviewed |
| Use_Event_System | applied | local space move, attack-move and double-click radar events (src/presentation/godot/ui/minimap_view.cpp) | OF-04 | — | src/presentation/godot/ui/minimap_view.cpp | basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/units/unit_combat.cpp) | WAR-21, WAR-26; R-09, WBP-01 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_combat.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Minimum_Destruction_Survivor_Count | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Secondary_Objective | todo | application not recorded | — | 650 | — | other: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Secondary_Objective behavior remains unwired.; basis: reviewed |
| Shield_Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## SecondaryStructure, SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); whole class (src/sim/tactical/pads.cpp) | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; WBP-04 | — | src/campaign/live_setup.cpp; src/sim/tactical/pads.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WBP-14 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Shield_Points | applied | whole class (src/units/unit_durability.cpp) | DG-06, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Shield_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-06, WBP-18 | — | src/units/unit_durability.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Tactical_Respawn_Time_In_Secs | applied | whole class (src/sim/tactical/pads.cpp) | WBP-29, WBP-49, WHZ-52 | — | src/sim/tactical/pads.cpp | basis: reviewed |
| Targeting_Max_Attack_Distance | applied | whole class (src/units/unit_combat.cpp) | W-09, WBP-18 | — | src/units/unit_combat.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| HardPoints | applied | whole class (src/scene/space_population.cpp) | DG-25, DG-39, HD-21, HS-02 | — | src/scene/space_population.cpp | basis: auto |

## SecondaryStructure, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ship_Class | applied | whole class (apps/viewer/src/world_ui_prepare.cpp); whole class (src/campaign/space_entry.cpp) | none recorded; WGT-45 | — | apps/viewer/src/world_ui_prepare.cpp; src/campaign/space_entry.cpp | basis: auto |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Base_Position | partial | prepared campaign space entry (src/campaign/space_entry.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-12 | 2006 | src/campaign/space_entry.cpp | Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station (+69 more). Mixed ground/space, space carriers on SpecialStructure: Civilian_Command_Center, Communications_Array_E, Communications_Array_R, Destroyable_Asteroid_Huge, Destroyable_Asteroid_Large, Destroyable_Asteroid_Medium, Destroyable_Asteroid_Small, E_Gravity_Well_Station (+69 more). Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| Destruction_Survivors | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/; destroyed structures spawn no survivors.; basis: reviewed |
| Modifies_Reveal_Range | todo | application not recorded | — | 650 | — | &#91;hud-radar&#93; Not read anywhere under src/ or apps/; structures change no reveal range.; basis: reviewed |
| Reveal_Range_Modifier | todo | application not recorded | — | 650 | — | hud-radar: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Reveal_Range_Modifier behavior remains unwired.; basis: reviewed |
| Tactical_Additional_Structure_Type | partial | prepared campaign space entry (src/campaign/space_entry.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-11 | 2006 | src/campaign/space_entry.cpp | Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Shutter_Shield, Ground_Gravity_Generator, Ground_Ion_Cannon, Ground_Magnepulse_Cannon, Hutt_Ground_Base_Shield (+8 more). Mixed ground/space, space carriers on SpecialStructure: E_Ground_Base_Shield, E_Ground_Base_Shield_Small, E_Ground_Turbolaser_Tower, Empire_Ground_Shutter_Shield, Ground_Gravity_Generator, Ground_Ion_Cannon, Ground_Magnepulse_Cannon, Hutt_Ground_Base_Shield (+8 more). Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SFXEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_2D | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Is_3D | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | basis: auto |
| Is_Preset | applied | whole class (src/presentation/audio/sfx.cpp) | none recorded | — | src/presentation/audio/sfx.cpp | basis: auto |
| Max_Instances | applied | whole class (src/presentation/audio/sfx.cpp) | BA-04, BA-05 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Max_Predelay | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Max_Volume | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Min_Predelay | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Min_Volume | applied | whole class (src/presentation/audio/sfx.cpp) | BA-08 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Overlap_Test | applied | whole class (include/eawr/presentation/audio/sfx.hpp) | BA-05, BA-23 | — | include/eawr/presentation/audio/sfx.hpp | matched by the field's name; the loader is table-driven; basis: auto |
| Play_Count | applied | whole class (src/presentation/audio/sfx.cpp) | SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Play_Sequentially | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Post_Samples | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05, SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Pre_Samples | applied | whole class (src/presentation/audio/sfx.cpp) | SND-05, SND-08 | — | src/presentation/audio/sfx.cpp | Runtime event lifecycle consumer; admission precedes backend sample allocation.; basis: reviewed |
| Priority | applied | whole class (src/presentation/audio/sfx.cpp) | R-08, R-09, S-01, WCC-18, WTA-21 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Probability | applied | whole class (src/presentation/audio/sfx.cpp) | AI-06, AI-10, AI-53 | — | src/presentation/audio/sfx.cpp | basis: auto |
| Samples | applied | whole class (src/presentation/audio/sfx.cpp) | BP-21, G-04, T-01, UA-08, V-03 | — | src/presentation/audio/sfx.cpp | a presentation parse site that uses the value where it reads it; basis: auto |
| Use_Preset | applied | whole class (src/presentation/audio/sfx.cpp) | BA-02 | — | src/presentation/audio/sfx.cpp | basis: reviewed |
| Volume_Saturation_Distance | applied | whole class (apps/viewer/src/battle_audio_events.cpp) | BA-11 | — | apps/viewer/src/battle_audio_events.cpp | basis: auto |

## SlaveCompany

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Company_Units | partial | galactic current-type membership (src/data/xml_merge.cpp); galactic current build limits (src/sim/galactic/production.cpp); land company profile (src/units/unit_tables_land.cpp); missing: other ground company consumers | WGA-363, WGA-364; WGEP-78; LC-01, LC-04 | 4532 | src/data/xml_merge.cpp; src/sim/galactic/production.cpp; src/units/unit_tables_land.cpp | GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. L2a (&#35;1450): a land load keeps the ordered member list with per-member kind, locomotor and hero identity.; basis: reviewed |

## SpaceBuildable

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Force_Healing_Ability/Activation_Style | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | partial | automatic space repair satellites (src/sim/tactical/durability.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-04 | 4532 | src/sim/tactical/durability.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Percent | partial | automatic space repair satellites (src/sim/tactical/durability.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-04 | 4532 | src/sim/tactical/durability.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Force_Healing_Ability/Single_Target_Heal | partial | automatic space repair satellites (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair; other activation modes | WSH-01, WSH-02, WSH-06 | 4532 | src/sim/tactical/session_abilities.cpp | Automatic space repair consumer; target tie, outside-centre bounds and frame-order seams are explicit in WSH-01..08.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; The nested block parses into a profile but its handler is the Unit_Abilities_Data SENSOR_JAMMING path (WHE-32); this style does not select it.; basis: reviewed |
| Abilities/Sensor_Jamming_Ability/Duration_In_Secs | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Duration_In_Secs is read only for Leech_Shields_Ability (load_special_leech); sensor jamming ignores the nested value.; basis: reviewed |
| CategoryMask | applied | whole class (src/units/unit_combat.cpp) | R-09, WBP-01 | — | src/units/unit_combat.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Destroy_When_Child_Dies | applied | whole class (src/sim/tactical/session_step_economy.cpp) | WBP-27, WBP-28 | — | src/sim/tactical/session_step_economy.cpp | basis: reviewed |
| Passive_Missile_Shield_Radius | applied | passive space sources (src/sim/tactical/session_world.cpp) | WPJ-17 | — | src/sim/tactical/session_world.cpp | — |
| Projectile_Damage | todo | application not recorded | — | 650 | — | weapons-shields: load_body calls read_object_weapon with object_damage=false; this class has no space object-weapon damage override consumer. Previous context: the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |
| Turret_Rest_Angle | partial | space objects declaring TURRET (src/sim/tactical/combat.cpp); missing: non-TURRET objects and manual hardpoint interfaces not covered by the ordinary object path | UA-05, WWP-17 | 4532 | src/sim/tactical/combat.cpp | UA-05: ordinary space objects declaring TURRET load mechanical aim, gate firing on readiness and pose turret/barrel bones; manual cannon behavior remains separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpaceBuildable, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Fire_Inaccuracy_Fixed_Radius | todo | application not recorded | — | 650 | — | weapons-shields: No object-level tag reader exists; the nested ability override is a different tag and does not bind this authored radius.; basis: reviewed |

## SpaceBuildable, SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Targeting_Min_Attack_Distance | partial | ordinary space objects declaring TURRET (src/sim/tactical/combat_targeting.cpp); ship beam targets (src/sim/tactical/session_abilities.cpp); tactical AI type query Get_Min_Range (src/skirmish/ai.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | FC-60, FS-33; WHE-58; WTA-34 | 940 | src/sim/tactical/combat_targeting.cpp; src/sim/tactical/session_abilities.cpp; src/skirmish/ai.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Turret_Rotate_To_Rest_Delay_Sec | applied | ordinary space objects declaring TURRET (src/units/unit_tables_profiles.cpp) | FC-61 | — | src/units/unit_tables_profiles.cpp | Engine-recognized tag absent from effective FoC data; engine default when absent: 2.0 synchronized seconds. Applied only to ordinary space objects declaring TURRET.; ET-T053; basis: reviewed |
| Turret_XY_Only | applied | ordinary space objects declaring TURRET (src/units/unit_tables_profiles.cpp) | FC-60, UA-05 | — | src/units/unit_tables_profiles.cpp | Engine-recognized tag absent from effective FoC data; engine default when absent: false. Applied only to ordinary space objects declaring TURRET.; ET-T053; basis: reviewed |

## SpaceBuildable, SpaceUnit, SpecialStructure, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Inaccuracy_Distance | foc-ignores | application not recorded | — | — | — | DG-24 |

## SpaceBuildable, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Target_Stealth_Units | applied | whole class (src/units/unit_combat.cpp) | WST-07, WST-08, WST-11 | — | src/units/unit_combat.cpp | Mixed ground/space, space carriers on HeroUnit: Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, IG-88, Silri, Urai_Fen, Urai_Fen_Prologue.; basis: reviewed |

## SpaceBuildable, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Recharge_Seconds | todo | application not recorded | — | 760 | — | abilities: load_unit_abilities parses the entry and ability_table supports modeled kinds; carried hero or scene-unloaded class instances still lack the corresponding runtime activation/cooldown binding. Previous context: the loader reads it for other classes; the M2 scene's objects of this class never have it read; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| In_Background | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; BP-75: the tactical viewer clears tags.background before classification; no background pass, depth clear or widened clip range applies this flag.; basis: reviewed |

## SpacePrimarySkydome, SpaceSecondarySkydome, SpecialEffect

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Decoration | applied | whole class (src/skirmish/start.cpp) | none recorded | 650 | src/skirmish/start.cpp | No docs/behaviour rule covers it (docs/skirmish-start.md, retail map-object ownership; SK-04 in the code comment). A non-discardable decoration without an owning faction goes to the Neutral player.; basis: reviewed |
| Is_Discardable | applied | whole class (src/skirmish/start.cpp) | none recorded | 650 | src/skirmish/start.cpp | No docs/behaviour rule covers it (docs/skirmish-start.md, retail map-object ownership; SK-04 in the code comment). A non-discardable decoration without an owning faction goes to the Neutral player.; basis: reviewed |

## SpaceProp

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| CategoryMask | todo | application not recorded | — | 650 | — | a data loader reads it; where the value goes is not traced |
| Death_By_TSW_Replacements | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Is_Asteroid_Field | partial | space tactical hazards (src/presentation/ui/minimap.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); selected-map asteroid fields (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp); map object (src/units/unit_tables_decode.cpp); missing: objects outside loaded tactical closure | WHZ-70, WHZ-72; WHZ-04, WHZ-08; WHZ-11, WHZ-12; WHZ-01; WHZ-01 | 4532 | src/presentation/ui/minimap.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp | Loaded fields participate in center queries and independent per-frame damage rolls; flags remain independent of tracking category. The field flag is read in selected-map profiles; these hazard types are outside the pinned M2 table closure and recorded trace.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed; trace: unrecorded |
| Is_Ion_Storm | applied | space tactical hazards (src/presentation/ui/minimap.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); shield-bearing tactical combatants (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp); map object (src/units/unit_tables_decode.cpp) | WHZ-70, WHZ-72; WHZ-04, WHZ-08; WHZ-30, WHZ-31; WHZ-01; WHZ-01 | — | src/presentation/ui/minimap.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp | Selected-map independent storm flags feed shield-service center XY contacts; cached disable predicates preserve shield pools while gating absorption and mesh collision.; basis: reviewed; trace: unrecorded |
| Is_Nebula | applied | space tactical hazards (src/presentation/ui/minimap.cpp); whole class (src/scene/space_population.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); loaded tactical space units, craft and static obstacles (src/sim/tactical/session_step_systems.cpp); map object (src/skirmish/inputs.cpp) | WHZ-70, WHZ-72; none recorded; WHZ-04, WHZ-08; WHZ-20, WHZ-21, WHZ-22, WHZ-25; WHZ-01 | — | src/presentation/ui/minimap.cpp; src/scene/space_population.cpp; src/sim/tactical/pathfind.cpp; src/sim/tactical/session_step_systems.cpp; src/skirmish/inputs.cpp | Selected-map independent flags feed circular affected-unit service and the hard-box public fallback; team queries union member state.; basis: reviewed |

## SpaceProp, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Impassable_Asteroid | partial | craft, ship, station (src/presentation/ui/selection.cpp); loaded tactical obstacles and ships (src/sim/tactical/pathfind.cpp); map object (src/skirmish/inputs.cpp); loaded map props (src/units/unit_tables_decode.cpp); craft, ship, station (src/units/unit_tables_profiles.cpp); missing: affected-unit environmental services | WSU-13; WHZ-04, WHZ-08; WHZ-01; WSU-13; WSU-13 | 4532 | src/presentation/ui/selection.cpp; src/sim/tactical/pathfind.cpp; src/skirmish/inputs.cpp; src/units/unit_tables_decode.cpp; src/units/unit_tables_profiles.cpp | Selected-map profiles retain independent flags; tracking applies one precedence category and fighter avoidance excludes field, storm and nebula categories. Loaded map props apply the same mouse admission before picking: neutral noncollidable fields pass a right click through to the move point; impassable asteroids retain contacts.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpaceStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Armor_Type | applied | ship (src/units/unit_durability.cpp) | DG-10, WCC-46, WCC-50, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| CategoryMask | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); ship (src/units/unit_combat.cpp) | WAR-21, WAR-26; PL-13, R-09, WHZ-50, WHZ-51 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_combat.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Collidable_By_Projectile_Living | applied | ship (src/sim/tactical/blast.cpp); ship (src/sim/tactical/combat_algorithms.hpp); loaded tactical units (src/sim/tactical/projectiles.cpp); loaded tactical units (src/sim/tactical/session_step_targets.cpp) | WAD-14, WHZ-50, WHZ-51; R-08, WHZ-50, WHZ-51; WPJ-23, WWP-66; WHZ-51, WWP-66 | — | src/sim/tactical/blast.cpp; src/sim/tactical/combat_algorithms.hpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_targets.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it. WWP-66 admits only living-collidable profiles to persistent projectile contact collections and rechecks each contact candidate independently of collection membership; hostile-player admission excludes neutral and allied owners. WPJ-23 selects the candidate object's living permission. ESU-03 sources the false type default.; basis: reviewed |
| Death_Projectiles | applied | whole class (src/sim/tactical/session_step_fighters.cpp) | WNO-29, WNO-30 | — | src/sim/tactical/session_step_fighters.cpp | basis: reviewed |
| Energy_Refresh_Rate | applied | ship (src/units/unit_durability.cpp) | DG-13, EN-02, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Property_Flags | applied | ship (src/units/unit_priority.cpp) | G-03, PL-13, R-09, WHZ-50, WHZ-51, WNO-09, WNO-10 | — | src/units/unit_priority.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it. WNO-10: the container's NotOpportunityTarget keeps it out of the automatic fire of every shooter with a priority set; a parent without a set scores it 1.0 (R-09), as in FoC.; basis: reviewed |
| Shield_Points | applied | ship (src/units/unit_durability.cpp) | DG-06, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |
| Shield_Refresh_Rate | applied | ship (src/units/unit_durability.cpp) | DG-13, WCC-46, WHZ-50, WHZ-51 | — | src/units/unit_durability.cpp | WHZ-50/51, RO-2: capture points and positive-hull, living-projectile-collidable map objects enter the live ship closure; other map-only structures remain outside it.; basis: reviewed |

## SpaceStructure, SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Ranged_Target_Z_Adjust | applied | whole class (src/sim/tactical/combat_aim.cpp); whole class (src/sim/tactical/combat_fire.cpp); whole class (src/sim/tactical/projectiles.cpp); external redirect targets (src/sim/tactical/projectiles.cpp); ordinary death payloads (src/sim/tactical/session_step_fighters.cpp); projectile defence sources (src/sim/tactical/session_world.cpp) | WWP-19, WWP-50, WWP-72; WWP-16, WWP-72; MS-07, WWP-64, WWP-72; WPJ-42; WNO-29, WNO-30; WPJ-17 | — | src/sim/tactical/combat_aim.cpp; src/sim/tactical/combat_fire.cpp; src/sim/tactical/projectiles.cpp; src/sim/tactical/session_step_fighters.cpp; src/sim/tactical/session_world.cpp | — |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Cluster_Bomb_Ability/Activation_Style | applied | MC30_Frigate CLUSTER_BOMB (src/sim/tactical/session_cmd_abilities.cpp) | WCB-01, WCB-02, WCB-03 | — | src/sim/tactical/session_cmd_abilities.cpp | Named self-targeted ring launcher binds count, type and countdown; object-script autofire cadence remains separate.; basis: reviewed |
| Abilities/Cluster_Bomb_Ability/Bomb_Type | applied | MC30_Frigate CLUSTER_BOMB (src/sim/tactical/session_cmd_abilities.cpp) | WCB-01, WCB-02, WCB-03 | — | src/sim/tactical/session_cmd_abilities.cpp | Named self-targeted ring launcher binds count, type and countdown; object-script autofire cadence remains separate.; basis: reviewed |
| Abilities/Cluster_Bomb_Ability/Detonation_Time_In_Secs | applied | MC30_Frigate CLUSTER_BOMB (src/sim/tactical/session_cmd_abilities.cpp) | WCB-01, WCB-02, WCB-03 | — | src/sim/tactical/session_cmd_abilities.cpp | Named self-targeted ring launcher binds count, type and countdown; object-script autofire cadence remains separate.; basis: reviewed |
| Abilities/Cluster_Bomb_Ability/Number_Of_Bombs | applied | MC30_Frigate CLUSTER_BOMB (src/sim/tactical/session_cmd_abilities.cpp) | WCB-01, WCB-02, WCB-03 | — | src/sim/tactical/session_cmd_abilities.cpp | Named self-targeted ring launcher binds count, type and countdown; object-script autofire cadence remains separate.; basis: reviewed |
| Abilities/Laser_Defense_Ability/@Name | applied | whole class (src/units/unit_tables_abilities_nested.cpp) | WLD-01 | — | src/units/unit_tables_abilities_nested.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Activation_Style | applied | whole class (src/units/unit_tables_abilities_nested.cpp) | WLD-02 | — | src/units/unit_tables_abilities_nested.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Defense_Duration_In_Secs | applied | whole class (src/units/unit_abilities.cpp) | WLD-02 | — | src/units/unit_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Dist_Travelled | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Projectile_Types_Targeted | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04, WLD-08 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Protection_Radius | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-04 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Laser_Defense_Ability/Recharge_Time_In_Secs | applied | whole class (src/sim/tactical/session_abilities.cpp) | WLD-03 | — | src/sim/tactical/session_abilities.cpp | basis: reviewed |
| Abilities/Leech_Shields_Ability/Activation_Max_Range | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp) | WLS-01 | — | src/sim/tactical/session_abilities.cpp |  WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality. |
| Abilities/Leech_Shields_Ability/Activation_Min_Range | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp) | WLS-01 | — | src/sim/tactical/session_abilities.cpp |  WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality. |
| Abilities/Leech_Shields_Ability/Activation_Style | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp); Kedalbe_Battleship (src/sim/tactical/session_cmd_lanes.cpp) | WLS-01; WAB-36 | — | src/sim/tactical/session_abilities.cpp; src/sim/tactical/session_cmd_lanes.cpp | — |
| Abilities/Leech_Shields_Ability/Applicable_Unit_Categories | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp) | WLS-01 | — | src/sim/tactical/session_abilities.cpp | — |
| Abilities/Leech_Shields_Ability/Applicable_Unit_Types | applied | Kedalbe_Battleship (src/sim/tactical/session_abilities.cpp) | WLS-01 | — | src/sim/tactical/session_abilities.cpp | — |
| Abilities/Leech_Shields_Ability/Beam_Frames | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Leech_Shields_Ability/Beam_Width | applied | Kedalbe_Battleship (apps/viewer/src/battle_effects_projectiles.cpp) | WLS-05 | — | apps/viewer/src/battle_effects_projectiles.cpp | — |
| Abilities/Leech_Shields_Ability/Damage_Multiplier | applied | Kedalbe_Battleship (src/sim/tactical/session_economy.cpp) | WLS-02 | — | src/sim/tactical/session_economy.cpp | — |
| Abilities/Leech_Shields_Ability/Duration_In_Secs | applied | Kedalbe_Battleship (src/sim/tactical/abilities.cpp) | WLS-03 | — | src/sim/tactical/abilities.cpp | — |
| Abilities/Leech_Shields_Ability/Shield_Damage_Per_Second | applied | Kedalbe_Battleship (src/sim/tactical/session_step_combat.cpp) | WLS-02 | — | src/sim/tactical/session_step_combat.cpp | — |
| Abilities/Super_Laser_Ability/Face_Target | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Fire_Continuation | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Fire_Delay | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Firing_Cone | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Damage_Type | applied | craft (src/units/unit_combat.cpp) | DG-12, WWP-48 | — | src/units/unit_combat.cpp | &#35;851 reassessment: the unit-authored tag supplies only its own Projectile_Types weapon (WWP-48); ship hardpoint shots use each hardpoint's Damage_Type, then its projectile's (DG-12). The checked ships do not author the unit tag; an absent baseline node is not proof of a failed read.; CHECK-842; basis: reviewed |
| Death_Leave_Hulk_Behind | todo | application not recorded | — | 650 | — | &#91;vfx-models&#93; Not read anywhere under src/ or apps/; a destroyed ship leaves no hulk.; basis: reviewed |
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, WCC-46 | — | src/units/unit_durability.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. This includes craft energy regeneration (EN-02, AB-23); no craft-specific gap was established.; CHECK-842; basis: reviewed |
| Land_Bomber_Type | todo | application not recorded | — | 650 | — | applied only by the galactic auto-resolve (src/campaign/auto_resolve_*); no tactical code reads it, and whether FoC's tactical battle does is unverified (&#35;4002) |
| Moniker | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Should_Attacker_Hold_Fire_For_Special_Ability | todo | application not recorded | — | 650 | — | abilities: No reader or special-ability fire suppression consumer binds this flag.; basis: reviewed |
| TSW_Attack_Distance_From_Target | partial | TACTICAL_SUPER_WEAPON objects (src/battle/start.cpp); missing: move into attack range, tactical super weapon attack stages | TSW-01 | 650 | src/battle/start.cpp | Applied at the start of a tactical battle for an effective TACTICAL_SUPER_WEAPON object: the nearest enemy-owned super weapon target anchors its starting position (TSW-01); the move into attack range, in-range state and attack stages are not modelled.; basis: reviewed |
| TSW_Power_Up_Countdown_Seconds | todo | application not recorded | — | 650 | — | abilities: load_body reads other TSW positioning fields, but no reader or firing countdown consumer binds this tag.; basis: reviewed |
| TSW_Start_Distance_From_Target | partial | TACTICAL_SUPER_WEAPON objects (src/battle/start.cpp); missing: move into attack range, tactical super weapon attack stages | TSW-01 | 650 | src/battle/start.cpp | Applied at the start of a tactical battle for an effective TACTICAL_SUPER_WEAPON object: the nearest enemy-owned super weapon target anchors its starting position (TSW-01); the move into attack range, in-range state and attack stages are not modelled.; basis: reviewed |
| TSW_Start_Pos_Match_Targets_Z | partial | TACTICAL_SUPER_WEAPON objects (src/battle/start.cpp); missing: move into attack range, tactical super weapon attack stages | TSW-01 | 650 | src/battle/start.cpp | Applied at the start of a tactical battle for an effective TACTICAL_SUPER_WEAPON object: the nearest enemy-owned super weapon target anchors its starting position (TSW-01); the move into attack range, in-range state and attack stages are not modelled.; basis: reviewed |
| TSW_Z_Adjust_Relative_To_Target_Pos | partial | TACTICAL_SUPER_WEAPON objects (src/battle/start.cpp); missing: move into attack range, tactical super weapon attack stages | TSW-01 | 650 | src/battle/start.cpp | Applied at the start of a tactical battle for an effective TACTICAL_SUPER_WEAPON object: the nearest enemy-owned super weapon target anchors its starting position (TSW-01); the move into attack range, in-range state and attack stages are not modelled.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Effective_Radius | partial | SpaceUnit LURE (src/sim/tactical/session_step_commands.cpp); SpaceUnit MISSILE_SHIELD and SENSOR_JAMMING (src/sim/tactical/session_world.cpp); missing: other space ability radii | WLU-04; WHE-32, WPJ-17 | 760 | src/sim/tactical/session_step_commands.cpp; src/sim/tactical/session_world.cpp | — |
| Unit_Abilities_Data/Unit_Ability/Is_Pulsing | foc-ignores | application not recorded | — | — | — | Parsed into the ability data, but the pulse cadence reads only the frequency and the maximum count and the data has no accessor for this flag; a runtime check may be wanted (Focumentation: Unit Abilities, LURE).; WLU-03 |
| Unit_Abilities_Data/Unit_Ability/Max_Number_Of_Pulses | applied | SpaceUnit LURE (the A-wing craft, the only space author) (src/sim/tactical/abilities.cpp) | WLU-03 | — | src/sim/tactical/abilities.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Pulse_Frequency_Secs | applied | SpaceUnit LURE (the A-wing craft, the only space author) (src/sim/tactical/abilities.cpp) | WLU-03 | — | src/sim/tactical/abilities.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Supports_Autofire | partial | whole class (src/sim/tactical/abilities.cpp); whole class (src/sim/tactical/session_cmd_abilities.cpp); Kedalbe_Battleship (src/sim/tactical/session_cmd_lanes.cpp); missing: SpaceUnit targeted abilities other than LEECH_SHIELDS (WAB-36 G-4) | AB-45, WAB-35; AB-01, AB-40, AB-60, AB-68, WAB-30; WAB-36 | 4532 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp; src/sim/tactical/session_cmd_lanes.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect. Ship ability profiles retain the flag for the ability service (AB-40).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; CHECK-842; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_cmd_abilities.cpp); space ships with BARRAGE (src/sim/tactical/session_cmd_targeted.cpp); missing: Other ability kinds; nonzero spawned placement offsets; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62; WAD-38 | 4532 | src/sim/tactical/session_cmd_abilities.cpp; src/sim/tactical/session_cmd_targeted.cpp | BARRAGE applies its authored point Z offset. G8 binds spawned projectile types, countdowns, timed category-0 modifiers and status particles; nonzero spawned placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Target_Types | applied | SpaceUnit LURE (the A-wing craft, the only space author) (src/sim/tactical/session_step_commands.cpp) | WLU-04, WLU-05 | — | src/sim/tactical/session_step_commands.cpp | basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override | partial | space ships with BARRAGE (src/sim/tactical/combat_aim.cpp); missing: other ability kinds | WAD-38 | 4532 | src/sim/tactical/combat_aim.cpp | The BARRAGE point interface applies this value; other point abilities remain gated.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Victory_Relevant | applied | whole class (src/skirmish/content.cpp) | WBF-35 | — | src/skirmish/content.cpp | The selected all-enemy-units condition applies ship/craft relevance; starbase-only retains its role filter.; basis: reviewed |

## SpaceUnit, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Redirect_Blaster_Ability/@Name | applied | whole class (src/units/unit_tables_abilities_nested.cpp) | WRB-01 | — | src/units/unit_tables_abilities_nested.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Activation_Style | applied | ship (src/units/unit_tables_abilities_nested.cpp) | WRB-01 | 760 | src/units/unit_tables_abilities_nested.cpp | Only SpaceUnit carriers are kept (WRB-01); the SpecialStructure authors are land structures.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Categories | applied | whole class (src/sim/tactical/session_abilities.cpp) | WRB-06 | — | src/sim/tactical/session_abilities.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Applicable_Unit_Types | applied | whole class (src/sim/tactical/session_abilities.cpp) | WRB-06 | — | src/sim/tactical/session_abilities.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Block_Chance | applied | whole class (src/sim/tactical/projectiles.cpp) | WRB-09 | — | src/sim/tactical/projectiles.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Initially_Enabled | applied | whole class (src/sim/tactical/session_abilities.cpp) | WRB-05 | — | src/sim/tactical/session_abilities.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Max_Projectile_Redirection_Angle_In_Degrees | applied | whole class (src/sim/tactical/projectiles.cpp) | WRB-10 | — | src/sim/tactical/projectiles.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Reaction_Arc_In_Degrees | applied | whole class (src/sim/tactical/projectiles.cpp) | WRB-07 | — | src/sim/tactical/projectiles.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Abilities/Redirect_Blaster_Ability/Redirect_Chance | applied | whole class (src/sim/tactical/projectiles.cpp) | WRB-09, WRB-10 | — | src/sim/tactical/projectiles.cpp | Space carriers only (Crusader_Gunship, Kedalbe_Battleship, Starviper_Fighter); ground carriers stay land-or-galactic by their own rows.; basis: reviewed |
| Special_Weapon_Valid_Targets | partial | ship (src/units/unit_abilities.cpp); missing: special-weapon structures and other spawned objects | WAB-73 | 650 | src/units/unit_abilities.cpp | Applied as the Buzz_Droids object's victim categories; other users stay with combat tag coverage.; basis: reviewed |

## SpaceUnit, SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Property_Flags | applied | whole class (src/units/unit_priority.cpp) | G-03, PL-13 | — | src/units/unit_priority.cpp | basis: auto |

## SpaceUnit, SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Fire_Pulse_Count | applied | planetary source objects (src/sim/tactical/session_step_combat.cpp); whole class (src/units/unit_combat.cpp) | WSW-19, WSW-20, WSW-27; W-06 | — | src/sim/tactical/session_step_combat.cpp; src/units/unit_combat.cpp | basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | applied | planetary source objects (src/sim/tactical/session_step_combat.cpp); whole class (src/units/unit_combat.cpp) | WSW-19, WSW-20, WSW-27; W-06 | — | src/sim/tactical/session_step_combat.cpp; src/units/unit_combat.cpp | basis: reviewed |

## SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Sensor_Jamming_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); missing: concrete nested-handler effects | WHE-09, WHE-10 | 4532 | src/sim/tactical/abilities.cpp | WHE-09/10/12/14: typed handler infrastructure applies gates, exact-type filter precedence, scheduling and cleanup. Concrete effects and unknown default activation policies remain separate; no hero ability is enabled by metadata alone.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Stealth_Ability/Activation_Style | applied | whole class (src/units/unit_abilities.cpp) | WST-02 | — | src/units/unit_abilities.cpp | basis: reviewed |

## SpaceUnit, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Shield_Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-06, DG-07, DG-09, DG-20, WCC-46 | — | src/units/unit_durability.cpp | basis: auto |

## SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Super_Laser_Ability/Activation_Max_Range | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Activation_Min_Range | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Applicable_Unit_Categories | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Applicable_Unit_Types | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Damage_Per_Frame | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Fire_Frame | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table (sim/tactical/abilities.cpp); no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Abilities/Super_Laser_Ability/Line_Width | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Super_Laser_Ability is not in the special-ability kind table; no loader reads the block and no SUPER_LASER handler exists, so the nested value never reaches the sim.; basis: reviewed |
| Defend_Mode_Energy_Regen_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Shield_Regen_Multiplier | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Defend_Mode_Weapon_Delay_Multiplier | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; no defend-mode weapon delay exists.; basis: reviewed |
| Fires_Forward | applied | whole class (src/units/unit_tables_profiles.cpp) | DG-24, G-08, OW-01, OW-02, OW-03, W-09, WWP-53 | — | src/units/unit_tables_profiles.cpp | basis: reviewed |
| Max_Thrust | partial | craft (apps/viewer/src/world_ui_groups.cpp); craft (src/units/unit_motion.cpp); missing: ship attack positioning with Keep_Moving_In_Battle; ship: foc-ignores | WSU-34; FM-01, FM-05 | 2674 | apps/viewer/src/world_ui_groups.cpp; src/units/unit_motion.cpp | WMV-40: ordinary ship flight ignores thrust; optional attack-positioning stopping distance remains unapplied. The flag defaults to false and no stock object authors it.; WMV-40; ship: debug build: ordinary layered ship path flight ignores Max_Thrust and Max_Lift; craft flight consumes them (docs/behaviour/walks/movement.md WMV-40). Max_Thrust also has an optional Keep_Moving_In_Battle attack-positioning consumer, retained as an explicit gap.; basis: reviewed |
| Minimum_Follow_Distance | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | WSQ-33 | — | src/units/unit_motion.cpp | WMV-43; ship: debug build: Minimum_Follow_Distance and Out_Of_Combat_Defense_Adjustment are consumed by craft following and flight states, never ordinary ship flight (docs/behaviour/walks/movement.md WMV-43, WSQ-33, DG-26).; basis: reviewed |
| Out_Of_Combat_Defense_Adjustment | applied | craft (src/units/unit_motion.cpp); ship: foc-ignores | DG-26, FA-07 | — | src/units/unit_motion.cpp | WMV-43; ship: debug build: Minimum_Follow_Distance and Out_Of_Combat_Defense_Adjustment are consumed by craft following and flight states, never ordinary ship flight (docs/behaviour/walks/movement.md WMV-43, WSQ-33, DG-26).; basis: reviewed |
| Spin_Away_On_Death | applied | craft (src/units/unit_motion.cpp) | UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Spin_Away_On_Death_Chance | applied | craft (src/units/unit_motion.cpp) | UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Spin_Away_On_Death_Time | applied | craft (src/units/unit_motion.cpp) | SP-05, UA-10 | — | src/units/unit_motion.cpp | basis: auto |
| Targeting_Stickiness_Time_Threshold | applied | whole class (src/sim/tactical/combat_targeting.cpp); whole class (src/units/unit_combat.cpp) | WCC-15; WCC-14, WCC-15 | — | src/sim/tactical/combat_targeting.cpp; src/units/unit_combat.cpp | a scanned target is kept while it will die within the scanner's threshold (a squadron craft: its leader's); 0 when not authored (debug build). The Lua override is not bound.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Base_Power_Ability/Activation_Style | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Base_Power_Ability is not in the special-ability kind table; no loader reads it and no base-power handler exists.; basis: reviewed |
| Abilities/Battlefield_Modifier_Ability/Reinforcement_Time_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only Communications_Array_E/R author it (ground structures); the debug build reads the player delay list only in the land transport reinforcement bar (90-frame Skirmish_Reinforcement_Delay_Frames base) |
| Abilities/Battlefield_Modifier_Ability/Reverse_Application_Logic | partial | special-weapon unlock modifiers (src/sim/tactical/session_economy.cpp); missing: other battlefield modifiers | WSW-11 | 760 | src/sim/tactical/session_economy.cpp | abilities: load_bonus_abilities retains reverse for special-weapon unlocks and special_weapons_enabled tests it; other BattlefieldBonus and cloak-duration modifiers do not apply reversal.; basis: reviewed |
| Abilities/Force_Healing_Ability/Activation_Style | applied | automatic space repair (src/units/unit_tables_abilities_nested.cpp) | WSH-01 | 760 | src/units/unit_tables_abilities_nested.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Categories | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-04 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Applicable_Unit_Types | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-04 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Amount | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-04 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Interval_In_Secs | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-06 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Percent | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-04 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Heal_Range | applied | automatic space repair (src/sim/tactical/session_abilities.cpp) | WSH-04 | 760 | src/sim/tactical/session_abilities.cpp | Class-agnostic loader: a Space_Automatic variant on a SpecialStructure is admitted like the SpaceBuildable repair satellites; Ground_Automatic variants are land.; basis: reviewed |
| Abilities/Force_Healing_Ability/Single_Target_Heal | partial | Space_Automatic healing (src/sim/tactical/session_abilities.cpp); missing: Skirmish_Automatic mine self-repair | WSH-01, WSH-02 | 760 | src/sim/tactical/session_abilities.cpp | abilities: load_special_healing binds the flag and the Space_Automatic healing service chooses one lowest-health eligible ally when set; the separate Skirmish_Automatic mine self-repair service remains missing (WSH-01).; basis: reviewed |
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WCC-46, WCC-50 | — | src/units/unit_durability.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| Base_Shield_Always_Off | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; no base-shield field exists.; basis: reviewed |
| Base_Shield_Radius | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/; no base-shield field exists.; basis: reviewed |
| Can_Contain_Heroes_During_Ground_Battle | todo | application not recorded | — | 3776 | — | Land hero-holder provider missing (WGT-82); basis: reviewed |
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, WCC-46 | — | src/units/unit_durability.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| Fine_Tune_Occupied_Passability | partial | ordinary static LAND_OBSTACLE instances at initialization (include/eawr/skirmish/land_content.hpp); ordinary static land obstacles with explicit content gates (src/skirmish/land_start.cpp); ordinary static land obstacles with explicit content gates (src/units/unit_tables_land.cpp); missing: non-obstacle roles, combined-object geometry and live invalidation | WLPA-61, WLPA-62, WLPA-63, WLPA-64, WLPA-65, WLPA-66; WLPA-07, WLPA-09; WLPA-03, WLPA-07, WLPA-08 | 1450 | include/eawr/skirmish/land_content.hpp; src/skirmish/land_start.cpp; src/units/unit_tables_land.cpp | Ordinary land static initialization uses model/proxy collision geometry, true refinement default and behavior passability; raw enum-bit qualification remains explicit. |
| Garrison_Category | partial | galactic orbital special structures and ground space weapons (src/campaign/ai_perception_content.cpp); missing: Other special-structure garrison/category consumers outside the galactic raw-space force provider | WGA-183 | 1967 | src/campaign/ai_perception_content.cpp | basis: reviewed |
| Gravity_Control_Field_Range | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Gravity_Control_Field_Slow_Fraction | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Include_In_UI_Map_Header | todo | application not recorded | — | 3776 | — | no code reads this tag on origin/main (exact and case-insensitive grep); basis: reviewed |
| Is_Special_Weapon_In_Space | partial | prepared galactic AI type facts (src/campaign/ai_selection_content.cpp); prepared campaign space entry (src/campaign/space_entry.cpp); tactical AI type query Is_Hero (src/skirmish/ai.cpp); missing: other special-weapon consumers (superweapon targeting and galactic modes), Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGA-253; WGT-13; WTA-36 | 2006 | src/campaign/ai_selection_content.cpp; src/campaign/space_entry.cpp; src/skirmish/ai.cpp | Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md. The tactical AI type query Is_Hero answers true for a type whose built hero authors this flag (debug build); other consumers remain separate.; basis: reviewed |
| Land_Victory_Relevant | todo | application not recorded | — | 650 | — | scope label removed (&#35;4002): what a tactical battle does with this tag is not audited |
| Lobbing_Superweapon_Chargeup_Frames | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Num_Garrison_Guns | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; basis: reviewed |
| Place_At_Every_Specific_Marker_Position | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Place_Other_Type_At_Every_Specific_Marker_Position | partial | prepared campaign space entry (src/campaign/space_entry.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-12 | 2006 | src/campaign/space_entry.cpp |  Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; SCOPE-LAND: only ground objects author it in the effective FoC XML (E_Galactic_Turbolaser_Tower_Defenses, R_Galactic_Turbolaser_Tower_Defenses); basis: reviewed |
| Prevents_Blockade_Run_Attrition | todo | application not recorded | — | 650 | — | — |
| Requires_Base_Power | todo | application not recorded | — | 650 | — | production-tech: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Requires_Base_Power behavior remains unwired.; basis: reviewed |
| Shield_Points | applied | whole class (src/units/unit_durability.cpp) | DG-06, WCC-46 | — | src/units/unit_durability.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| Shield_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, WCC-46 | — | src/units/unit_durability.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| Should_Be_Destroyed_When_All_Hardpoints_Destroyed | applied | whole class (src/units/unit_durability.cpp) | HD-21, IS-05, IS-06 | — | src/units/unit_durability.cpp | basis: auto |
| Space_Victory_Relevant | todo | application not recorded | — | 650 | — | other: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Space_Victory_Relevant behavior remains unwired.; basis: reviewed |
| Spawn_Garrison_On_Load | todo | application not recorded | — | 3776 | — | Read in the space station spawn path; land use unverified and no land reader; basis: reviewed |
| Special_Weapon_Index | applied | standalone planetary sources (src/sim/tactical/session_state.cpp) | WSW-01, WSW-14 | — | src/sim/tactical/session_state.cpp | basis: reviewed |
| Special_Weapon_Target_Action_Index | applied | standalone planetary sources (src/sim/tactical/session_state.cpp) | WSW-01, WSW-14 | — | src/sim/tactical/session_state.cpp | basis: reviewed |
| Tactical_Health | applied | whole class (src/skirmish/start.cpp) | HD-01, U-04 | — | src/skirmish/start.cpp | WSW-01/14/21: planetary SPECIAL_WEAPON sources do not use ordinary space-body inputs.; basis: auto |
| Weapon_Quantity | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Weapon_Type | todo | application not recorded | — | 650 | — | weapons-shields: No SpecialStructure Weapon_Type reader or weapon selection consumer exists.; basis: reviewed |

## SpecialStructure, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Combat_Bonus_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-09, WHE-10; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-14; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); automatic space command sources (src/sim/tactical/session_economy.cpp); missing: ground and nonautomatic combat bonuses | WHE-14; WHE-13, WHE-14, WHE-18, WHE-19, WHE-53, WHE-55 | 4532 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_economy.cpp | WHE-13..19/53..55: automatic space command effects, category aggregation and source cleanup; ground/nonautomatic handlers keep their own scope.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpecialStructure, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/System_Spy_Ability/Activation_Style | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Causes_Despawn | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Duration_In_Secs | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/Initially_Enabled | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Fleet_Contents | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Major_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Minor_Stealth_Heroes | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Most_Powerful_Ship | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Abilities/System_Spy_Ability/See_Num_Fleets | partial | ground-activated galactic spy (src/sim/galactic/ability_theft.cpp); missing: automatic unit/structure spy influence invocation | WGCH-108, WGCH-41, WGCH-56, WGCH-70, WGUI-101, WGUI-19, WGUI-20, WGUI-21, WGUI-23 | 1143 | src/sim/galactic/ability_theft.cpp | Ground-activated authored spy profiles and preview routing invoke current-owner source-keyed intelligence with success-only consumption; automatic unit/structure spy influence invocation remains deferred. |
| Unit_Abilities_Data/Unit_Ability/Active_By_Default | todo | application not recorded | — | 760 | — | &#91;abilities&#93; Not read anywhere under src/ or apps/.; basis: reviewed |

## SpeechEvent

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Files | partial | English space tactical production speech (apps/viewer/src/battle_audio_events.cpp); missing: scripted/campaign speech and other languages | BA-73, BA-74, SND-44 | 4532 | apps/viewer/src/battle_audio_events.cpp | Stream execution applies reachable speech files and volume; Text_ID/movie lifecycle is separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Volume_Percent | partial | English space tactical production speech (apps/viewer/src/battle_audio_events.cpp); missing: scripted/campaign speech and other languages | BA-73, BA-74, SND-44 | 4532 | apps/viewer/src/battle_audio_events.cpp | Stream execution applies reachable speech files and volume; Text_ID/movie lifecycle is separate.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Mod_Multiplier | foc-ignores | application not recorded | — | — | — | &#35;851 reassessment: the check changes structural tables without changing the battle. AB-15 establishes why: non-team abilities use each craft's Mod_Multiplier, not the squadron's; team ability data instead comes from Create_Team_Type (AB-60).; AB-15; basis: reviewed |

## Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Create_Team_Type | partial | Squadron (apps/viewer/src/map_mode_hud.cpp); craft (src/sim/tactical/session_economy.cpp); craft, ship, squadron (src/skirmish/economy.cpp); missing: campaign and ground conversions | MM-15; WHE-30, WHE-63; WHE-49 | 4532 | apps/viewer/src/map_mode_hud.cpp; src/sim/tactical/session_economy.cpp; src/skirmish/economy.cpp | The space company resolver selects the authored unique ship and its creation team; other modes remain separate. Loaded squadron radar independently applies the team type (MM-15).; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## StarBase

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Available_In_Skirmish | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/; the skirmish start does not filter stations by this flag.; basis: reviewed |
| Energy_Refresh_Rate | applied | whole class (src/units/unit_durability.cpp) | DG-13, EN-02, WCC-46 | — | src/units/unit_durability.cpp | &#35;851 reassessment: the 9000-tick M2 check changes structural tables but observes no battle change; the cited application remains, and this scenario does not prove its effect.; CHECK-842; basis: reviewed |
| Retreat_Self_Destruct_Explosion | applied | station (src/sim/tactical/session_step_commands.cpp); station (src/units/unit_tables_decode.cpp) | RP-09; RP-09 | — | src/sim/tactical/session_step_commands.cpp; src/units/unit_tables_decode.cpp | The station creates the authored projectile at the retreat's station boundary; its blast is gameplay.; basis: reviewed |
| Victory_Relevant | applied | station (src/skirmish/content.cpp) | VT-03 | — | src/skirmish/content.cpp | &#35;851 reassessment: the default enemy-starbase-destroyed condition builds relevant targets only from stations (VT-01 to VT-03); no altered victory result occurs in the check.; CHECK-842; basis: reviewed |

## TacticalCamera

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bottom_Bounds_Buffer | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/; camera bounds use no edge buffer.; basis: reviewed |
| Distance_Default | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Max | applied | whole class (src/presentation/camera/camera.cpp) | C-6, V-1 | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Min | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-01 | — | src/presentation/camera/camera.cpp | basis: reviewed |
| Distance_Smooth_Time | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Distance_Spline | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-02 | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: reviewed |
| Far_Clip | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Fov_Default | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-03 | — | src/presentation/camera/camera.cpp | basis: reviewed |
| Fov_Max | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-03 | — | src/presentation/camera/camera.cpp | basis: reviewed |
| Fov_Min | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-03 | — | src/presentation/camera/camera.cpp | basis: reviewed |
| Fov_Smooth_Time | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Location_Follows_Terrain | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Location_Height_Down_Smooth_Time | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Location_Height_Smooth_Time | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Location_Height_Up_Smooth_Time | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| Min_Height_Above_Terrain | applied | whole class (apps/viewer/src/map_camera.cpp) | none recorded | — | apps/viewer/src/map_camera.cpp | basis: auto |
| MinHeightAboveTerrain | foc-ignores | application not recorded | — | — | — | DB-NOTAG |
| Near_Clip | applied | whole class (src/presentation/camera/camera.cpp) | none recorded | — | src/presentation/camera/camera.cpp | basis: auto |
| Side_Bounds_Buffer | todo | application not recorded | — | 650 | — | movement: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Side_Bounds_Buffer behavior remains unwired.; basis: reviewed |
| Spline_Steps | todo | application not recorded | — | 650 | — | movement: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Spline_Steps behavior remains unwired.; basis: reviewed |
| Tactical_Overview_Click_Time | applied | whole class (src/presentation/camera/overview.cpp) | V-1 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Clicks | applied | whole class (src/presentation/camera/overview.cpp) | V-1, V-1a | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Distance | applied | whole class (src/presentation/camera/overview.cpp) | V-2 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_Distance2 | applied | whole class (src/presentation/camera/overview.cpp) | V-3 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_FOV | applied | whole class (src/presentation/camera/overview.cpp) | V-2 | — | src/presentation/camera/overview.cpp | basis: auto |
| Tactical_Overview_FOV2 | applied | whole class (src/presentation/camera/overview.cpp) | V-3 | — | src/presentation/camera/overview.cpp | basis: auto |
| Top_Bounds_Buffer | todo | application not recorded | — | 650 | — | movement: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Top_Bounds_Buffer behavior remains unwired.; basis: reviewed |
| Use_Splines | applied | whole class (src/presentation/camera/camera.cpp) | CAMXML-02 | — | src/presentation/camera/camera.cpp | matched by the field's name; the loader is table-driven; basis: reviewed |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | partial | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); missing: other objects and consumers outside the live galactic closure | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34 | 650 | src/campaign/live_setup.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Armor_Type | applied | whole class (src/units/unit_durability.cpp) | DG-10, WCC-46, WCC-50 | 650 | src/units/unit_durability.cpp | TransportUnit is a corvette-layer craft in the stock data (transportunits.xml); the class-agnostic space loader treats it like any other loaded ship.; basis: reviewed |
| Can_Participate_In_Space_Battle | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); explicit fleet pathfinder space eligibility (src/campaign/fleets.cpp); prepared campaign space entry (src/campaign/space_entry.cpp) | WAR-15; WGT-06, WGT-40; WGT-19 | — | src/campaign/auto_resolve_strength.cpp; src/campaign/fleets.cpp; src/campaign/space_entry.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md.; basis: reviewed |
| CategoryMask | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); whole class (src/script/foc/tactical_ai_bindings.cpp) | WAR-21, WAR-26; WAR-21, WAR-26 | — | src/campaign/auto_resolve_strength.cpp; src/script/foc/tactical_ai_bindings.cpp | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer.; basis: reviewed |
| Collidable_By_Projectile_Living | applied | loaded tactical units (src/sim/tactical/projectiles.cpp) | WPJ-23, WWP-66 | 650 | src/sim/tactical/projectiles.cpp | TransportUnit is a corvette-layer craft in the stock data (transportunits.xml); the class-agnostic space loader treats it like any other loaded ship.; basis: reviewed |
| Energy_Refresh_Rate | applied | whole class (src/sim/tactical/damage.cpp) | EN-02 | 650 | src/sim/tactical/damage.cpp | TransportUnit is a corvette-layer craft in the stock data (transportunits.xml); the class-agnostic space loader treats it like any other loaded ship.; basis: reviewed |
| Exit_Door_Angle_Degrees | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Exit_Door_Distance | todo | application not recorded | — | 650 | — | &#91;other&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Fire_Cone_Height | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Read only from hardpoint weapon objects (unit_tables_profiles.cpp); a value authored on the unit itself (transportunits.xml) is never read.; basis: reviewed |
| Fire_Cone_Width | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Read only from hardpoint weapon objects (unit_tables_profiles.cpp); a value authored on the unit itself (transportunits.xml) is never read.; basis: reviewed |
| Is_Valid_Target | applied | loaded tactical units (src/sim/tactical/combat_algorithms.hpp) | R-08, WCC-25 | 650 | src/sim/tactical/combat_algorithms.hpp | TransportUnit is a corvette-layer craft in the stock data (transportunits.xml); the class-agnostic space loader treats it like any other loaded ship.; basis: reviewed |
| Max_Thrust | todo | application not recorded | — | 650 | — | WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. |
| Projectile_Fire_Pulse_Count | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger (+6 more).; basis: reviewed |
| Projectile_Fire_Pulse_Delay_Seconds | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Chewbacca, Han_Solo, IG-88, Jabba_The_Hutt, Silri, Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Passenger (+6 more).; basis: reviewed |
| Projectile_Fire_Recharge_Seconds | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo, IG-88 (+11 more).; basis: reviewed |
| Projectile_Types | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Property_Flags | todo | application not recorded | — | 650 | — | tactical-ai: mask_flags and unit_priority apply property bits for live loaded bodies; map-only or metadata instances of these classes still bypass that combat/AI binding. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Shield_Points | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Shield_Refresh_Rate | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Tactical_Health | todo | application not recorded | — | 650 | — | weapons-shields: load_body binds durability for live bodies, but these metadata/map-only or projectile instances lack a corresponding space durability consumer. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Targeting_Max_Attack_Distance | todo | application not recorded | — | 650 | — | weapons-shields: Unselected transport metadata bypasses live body/weapon binding in the company deployment path; the generic consumer does not apply this field there. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Use_Special_Submit_Rules | todo | application not recorded | — | 650 | — | other: No reader or runtime consumer for this class/tag exists in src/, apps/ or include/; authored Use_Special_Submit_Rules behavior remains unwired.; basis: reviewed |
| Variant_Of_Existing_Type | applied | galactic current-type membership (src/data/xml_merge.cpp) | WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |
| Victory_Relevant | todo | application not recorded | — | 650 | — | other: The live-body loader and start rules support the flag, but metadata/map-only instances of this class do not enter that live victory binding. Previous context: WHE-07/49: the M2 company resolver touches carried hero or unselected transport metadata without deploying its body; the generic loader consumer for other classes is not applied to these scene objects.; basis: reviewed |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fire_Category_Restrictions | todo | application not recorded | — | 650 | — | &#91;weapons-shields&#93; Read only from hardpoint weapon objects (unit_tables_profiles.cpp); a value authored on the unit itself is never read.; basis: reviewed |
| User_Bound_Max | todo | application not recorded | — | 650 | — | vfx-models: No authored user-bound maximum reader or tactical selection/culling consumer exists.; basis: reviewed |
| User_Bound_Min | todo | application not recorded | — | 650 | — | vfx-models: No authored user-bound minimum reader or tactical selection/culling consumer exists.; basis: reviewed |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Blast_Ability/Activation_Style | partial | The_Peacebringer (src/sim/tactical/session_step_commands.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 4532 | src/sim/tactical/session_step_commands.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Abilities/Blast_Ability/Charge_Up_Seconds | partial | The_Peacebringer (src/sim/tactical/session_abilities.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 760 | src/sim/tactical/session_abilities.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; basis: reviewed |
| Abilities/Blast_Ability/Damage_Multiplier | partial | The_Peacebringer (src/sim/tactical/session_step_combat.cpp); missing: FD-56 broader inter-object firing order, UFS-30 exceptional callback policy | WAD-34, WAD-35, WAD-36, WHE-35, WHE-36 | 760 | src/sim/tactical/session_step_combat.cpp | Peacebringer BLAST applies the named user-input handler, admitted-service charge, shared successful SPECIAL shot budget and direct-only multiplier; authored presentation is retained. FD-56 broader inter-object order and UFS-30 exceptional callbacks remain deferred.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Activation_Style | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Categories | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Applicable_Unit_Types | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Concentrate_Fire_Attack_Ability/Target_Damage_Increase_Percent | applied | Home One concentrate fire (src/sim/tactical/session_abilities.cpp) | WHE-24, WHE-25 | — | src/sim/tactical/session_abilities.cpp | Target-centred same-owner recruitment, named handler gates and source-keyed target defense contribution.; basis: reviewed |
| Abilities/Corrupt_Systems_Ability/@Name | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp) | WHE-33 | — | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Activation_Max_Range | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp) | WHE-33 | — | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Activation_Min_Range | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp) | WHE-33 | — | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Activation_Style | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp) | WHE-33 | — | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Duration_In_Seconds | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_cmd_abilities.cpp) | WHE-33, WHE-34 | — | src/sim/tactical/session_cmd_abilities.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Inaccuracy_Radius | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/combat_aim.cpp) | WHE-66 | — | src/sim/tactical/combat_aim.cpp | basis: reviewed |
| Abilities/Corrupt_Systems_Ability/Shield_Drain_Per_Second | applied | UniqueUnit CORRUPT_SYSTEMS (src/sim/tactical/session_step_commands.cpp) | WHE-34 | — | src/sim/tactical/session_step_commands.cpp | basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Max_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate. WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Min_Range | partial | ship (src/sim/tactical/session_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate. WHE-58: authored distance has no 65536 ceiling; checked subtraction preserves extent-adjusted equality.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Activation_Style | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-09, WHE-10; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Categories | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Applicable_Unit_Types | partial | craft, ship, squadron (src/sim/tactical/abilities.cpp); ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-14; WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/abilities.cpp; src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Energy_Weapon_Attack_Ability/Damage_Per_Frame | partial | ship (src/sim/tactical/session_cmd_abilities.cpp); missing: U-09: excluded locomotor identity, exact timer scaling and AI/autofire policy | WHE-26, WHE-27, WHE-57, WHE-58, WHE-59, WHE-60 | 940 | src/sim/tactical/session_cmd_abilities.cpp | G7 applies traced beam services, target geometry, hardpoint lock/loss and source/category tractor speed. U-09 uses supported ship locomotors and conservative recharge; atlas animation remains separate.; basis: reviewed |
| Abilities/Shield_Flare_Ability/Activation_Style | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Charge_Up_Seconds | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Damage_Radius | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Shield_Flare_Ability/Max_Damage_Per_Victim | todo | application not recorded | — | 3240 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Auto_Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Converted_To_Enemy_Die_Time_Seconds | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Create_Team | partial | loaded local space battle (src/platform/battle_scoring.cpp); craft (src/skirmish/economy.cpp); missing: land companies and non-scoring team creation | WBF-46; WHE-49 | 650 | src/platform/battle_scoring.cpp; src/skirmish/economy.cpp | WHE-49 team conversion is applied to the selected unique space ship; results scoring remains independently scoped to loaded local space objects.; basis: reviewed |
| Deploys | todo | application not recorded | — | 1454 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| Hero_Ability | todo | application not recorded | — | 650 | — | &#91;abilities&#93; Not read anywhere under src/ or apps/.; basis: reviewed |
| Is_Affected_By_Gravity_Control_Field | todo | application not recorded | — | 3776 | — | no land reader of tag name on origin/main; basis: reviewed |
| Is_Decoration | applied | whole class (src/script/foc/tactical_ai_bindings.cpp); whole class (src/skirmish/ai.cpp); whole class (src/skirmish/start.cpp); UniqueUnit (src/units/unit_tables_profiles.cpp) | L-32; L-32; U-04; WSU-15, WSU-19, WSU-21 | — | src/script/foc/tactical_ai_bindings.cpp; src/skirmish/ai.cpp; src/skirmish/start.cpp; src/units/unit_tables_profiles.cpp | basis: reviewed |
| Is_Supercrusher | todo | application not recorded | — | 1453 | — | no land reader of tag name on origin/main (plan ticket; open state unchecked); basis: reviewed |
| MinimumPushReturnDistance | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Overall_Length | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Overall_Width | todo | application not recorded | — | 3776 | — | No reader on origin/main (grep src/include/apps/tests, literal and snake_case forms); basis: reviewed |
| Projectile_Damage | todo | application not recorded | — | 650 | — | weapons-shields: load_body calls read_object_weapon with object_damage=false; this class has no space object-weapon damage override consumer. Previous context: WHE-07/49: the M2 resolver touches carried heroes, unselected transports and unique creation templates as metadata; the generic body or ability consumer for other classes is not applied to these scene objects.; basis: reviewed |
| Redirect_Damage_To_Teammates | partial | craft (src/sim/tactical/session_step_combat.cpp); missing: U-08: deleted-member/service races; mixed redirecting leaders are skipped by the recursion guard | WHE-64 | 4532 | src/sim/tactical/session_step_combat.cpp | WHE-64: raw shares enter each recipient mode/armor route in roster order; ordinary leader damage is consumed, LuaDebugDamage additionally reaches the leader. Sparse ordered environmental routing preserves worker isolation.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Turret_Targets_Anything_Else | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Bomb_Countdown_Seconds | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_cmd_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Mod_Flag | todo | application not recorded | — | 3776 | — | AB-26 note: HeroCompany ability block skipped on load; todo; basis: reviewed |
| Walk_Transition | partial | land company members (include/eawr/skirmish/land_content.hpp); missing: objects outside adapted land companies | WLPA-31 | 1450 | include/eawr/skirmish/land_content.hpp |  Authored land-company adapter applies this value; raw passability bits, collision ordering and animation completion are explicit gates (docs/behaviour/land-content-adapter.md).; basis: reviewed |
| Weather_Category | todo | application not recorded | — | 3225 | — | WLEN-15: no loader or sim field for Weather_Category on land; basis: reviewed |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Activation_Style | applied | held special-weapon enabling upgrades (src/sim/tactical/session_economy.cpp); space upgrade objects (Stealth_Duration_Multiplier) (src/units/unit_tables_abilities_bonuses.cpp); held special-weapon enabling upgrades (src/units/unit_tables_abilities_bonuses.cpp) | WSW-07, WSW-11, WSW-46; WST-03; WSW-07, WSW-46 | 760 | src/sim/tactical/session_economy.cpp; src/units/unit_tables_abilities_bonuses.cpp | WSW-07/46 and WST-03: applied to automatic special-weapon enabling upgrades and cloak-duration upgrades; ally propagation follows Apply_To_All_Allies.; basis: reviewed |
| Abilities/Battlefield_Modifier_Ability/Apply_To_All_Allies | applied | held special-weapon enabling upgrades (src/sim/tactical/session_economy.cpp); space upgrade objects (Stealth_Duration_Multiplier) (src/units/unit_tables_abilities_bonuses.cpp); held special-weapon enabling upgrades (src/units/unit_tables_abilities_bonuses.cpp) | WSW-07, WSW-11, WSW-46; WST-03; WSW-07, WSW-46 | 760 | src/sim/tactical/session_economy.cpp; src/units/unit_tables_abilities_bonuses.cpp | WSW-07/46 and WST-03: applied to automatic special-weapon enabling upgrades and cloak-duration upgrades; ally propagation follows Apply_To_All_Allies.; basis: reviewed |
| Abilities/Battlefield_Modifier_Ability/Disable_Reinforcement_Vulnerability | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only Ground_Automatic upgrades author it (RL/EL/UL reinforcement upgrades); the debug build reads it only in the land transport landing spawn (elevated vulnerability of spawned reinforcements) |
| Abilities/Battlefield_Modifier_Ability/Enable_Special_Weapons | applied | held space skirmish upgrades (src/sim/tactical/session_economy.cpp); held space skirmish upgrades (src/units/unit_tables_abilities_bonuses.cpp) | WSW-07, WSW-11, WSW-46; WSW-07, WSW-46 | — | src/sim/tactical/session_economy.cpp; src/units/unit_tables_abilities_bonuses.cpp | basis: reviewed |
| Abilities/Battlefield_Modifier_Ability/Reinforcement_Deploy_Time_Multiplier | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only Ground_Automatic upgrades author it; the debug build reads it only as the land transport unload timer (2 s x (1 + total)) |
| Abilities/Battlefield_Modifier_Ability/Stealth_Duration_Multiplier | applied | space upgrade objects (src/sim/tactical/session_abilities.cpp) | WST-03, WST-04 | — | src/sim/tactical/session_abilities.cpp | WST-03: the upgrade adds (multiplier - 1) to its holder's owner (allies with Apply_To_All_Allies); the cloak's own count is trunc(Expiration_Seconds x (1 + category winners) x 30), sampled at switch-on.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Ability_Recharge_Bonus_Percentage | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only the Underworld Ground_Automatic recharge upgrades (UL_Recharge_Ability, UL_Ability_Recharge) author it |
| Abilities/Combat_Bonus_Ability/Activation_Style | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-09, WHE-10; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Categories | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-14; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Applicable_Unit_Types | applied | craft, ship, squadron (src/sim/tactical/abilities.cpp); whole class (src/skirmish/economy.cpp) | WHE-14; WPR-51 | — | src/sim/tactical/abilities.cpp; src/skirmish/economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Damage_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Defense_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Energy_Pool_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Fire_Range_Bonus_Percentage | applied | hardpoint weapons of Space_Automatic upgrade recipients (src/sim/tactical/combat_fire.cpp) | WFR-01 | 760 | src/sim/tactical/combat_fire.cpp | WFR-01: seventh combat-bonus slot scales hardpoint fire range; targeting distance and the object weapon are untouched.; basis: reviewed |
| Abilities/Combat_Bonus_Ability/Health_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Combat_Bonus_Ability/Shield_Bonus_Percentage | applied | whole class (src/sim/tactical/session_economy.cpp) | WPR-51 | — | src/sim/tactical/session_economy.cpp | basis: reviewed |
| Abilities/Enable_Ability/Affects_All_Allies | partial | redirect-blaster nested handlers (src/sim/tactical/session_abilities.cpp); redirect-blaster nested handlers (src/skirmish/economy.cpp); missing: nested abilities other than the redirect blaster | WRB-05; WRB-05 | 760 | src/sim/tactical/session_abilities.cpp; src/skirmish/economy.cpp | The debug build's enable-ability type switches any named nested ability; the remake applies it to the redirect-blaster handlers, the only stock target (WRB-05; Focumentation: XML Tags).; basis: reviewed |
| Abilities/Enable_Ability/Applicable_Unit_Categories | partial | redirect-blaster nested handlers (src/sim/tactical/session_abilities.cpp); redirect-blaster nested handlers (src/skirmish/economy.cpp); missing: nested abilities other than the redirect blaster | WRB-05; WRB-05 | 760 | src/sim/tactical/session_abilities.cpp; src/skirmish/economy.cpp | The debug build's enable-ability type switches any named nested ability; the remake applies it to the redirect-blaster handlers, the only stock target (WRB-05; Focumentation: XML Tags). WRB-05 names the type filter; the category filter is read and applied on the same path (loader enable categories, economy ability enables).; basis: reviewed |
| Abilities/Enable_Ability/Applicable_Unit_Types | partial | redirect-blaster nested handlers (src/sim/tactical/session_abilities.cpp); redirect-blaster nested handlers (src/skirmish/economy.cpp); missing: nested abilities other than the redirect blaster | WRB-05; WRB-05 | 760 | src/sim/tactical/session_abilities.cpp; src/skirmish/economy.cpp | The debug build's enable-ability type switches any named nested ability; the remake applies it to the redirect-blaster handlers, the only stock target (WRB-05; Focumentation: XML Tags).; basis: reviewed |
| Abilities/Garrison_Upgrade_Ability/Additional_Garrison_Units | todo | application not recorded | — | 1455 | — | No land-path code (load_land_body, land_content, land_start, session_land, land HUD) reads this ability tag; basis: reviewed |
| Abilities/Weatherproof_Ability/Activation_Style | todo | application not recorded | — | 760 | — | — |
| Abilities/Weatherproof_Ability/Apply_To_All_Allies | todo | application not recorded | — | 760 | — | — |
| Is_Skirmish_Tactical_Super_Weapon | applied | whole class (src/skirmish/economy.cpp); whole class (src/skirmish/inputs.cpp) | WSS-30; WSS-30 | — | src/skirmish/economy.cpp; src/skirmish/inputs.cpp | Inherited authored flag gates space listing and authoritative human/AI purchase admission; it does not remove already placed objects or disable abilities.; basis: reviewed |
