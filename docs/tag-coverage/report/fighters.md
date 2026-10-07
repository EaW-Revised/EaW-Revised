# fighters tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 15 | 15 |
| partial | 5 | 5 |
| todo | 27 | 27 |
| presentation-later | 0 | 2 |
| foc-ignores | 12 | 12 |
| deferred | 4 | 4 |
| land-or-galactic | 0 | 26 |
| multiplayer | 0 | 0 |
| **Total** | **63** | **91** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## CIN_SpaceUnit, Container, MOV_Cinematic, Props_Story, SpaceUnit, Squadron, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Bomber | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## CIN_SpaceUnit, MOV_Cinematic, Props_Story, SpaceUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Number_per_Squadron | foc-ignores | application not recorded | — | — | — | DB-NOTAG |

## Container

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Squadron_Formation_Error_Tolerance | deferred | application not recorded | — | 651 | — | Hero-team Container dispatch/application needs qualification; existing Squadron formation and craft-summed AI consumers do not prove this class applies the tag.; TCFA-04; basis: reviewed |
| Squadron_Offsets | deferred | application not recorded | — | 651 | — | Hero-team Container dispatch/application needs qualification; existing Squadron formation and craft-summed AI consumers do not prove this class applies the tag.; TCFA-04; basis: reviewed |

## Container, TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Escort | todo | application not recorded | — | 651 | — | — |

## Faction

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Blob_Size | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| Bombing_Run_Shadow_Blob_Material_Name | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| Fighter_Icon_Name | presentation-later | application not recorded | — | — | — | Faction icon presentation is missing; Fighter_Icon_Name has a verified galactic UI consumer, Squadron_Icon_Name exact reader remains unverified.; TCFA-02; basis: reviewed |
| Reinforcements_Pick_Landing_Zone_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-11 | 1502 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; basis: reviewed |
| SFXEvent_Bombing_Run_Ally_Available | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_Bombing_Run_Available | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_Bombing_Run_Begin_Crosstalk | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_Bombing_Run_Cancelled | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_Bombing_Run_Enemy_Available | todo | application not recorded | — | 651 | — | — |
| SFXEvent_HUD_Landing_Zone_Captured | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_HUD_Landing_Zone_Lost | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_HUD_Last_Landing_Zone_Lost | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| Squadron_Icon_Name | presentation-later | application not recorded | — | — | — | Faction icon presentation is missing; Fighter_Icon_Name has a verified galactic UI consumer, Squadron_Icon_Name exact reader remains unverified.; TCFA-02; basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Reduction_Per_Squadron_Percent | todo | application not recorded | — | 844 | — | loaded into the combat constants, never read; 0 in the shipped data |
| Indigenous_Spawn_Destruction_Reward | land-or-galactic | application not recorded | — | — | — | Credit reward for an indigenous-spawner destruction story event, not space carrier spawning.; TCFA-03; basis: reviewed |
| Max_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |
| Min_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Remote_Bomb_Ability/Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Always_Spawn_In_Orbit | todo | application not recorded | — | 651 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+11 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fighter_Bay_Flyout_Distance | applied | whole class (src/units/unit_motion.cpp) | E75-06, E75-08, FL-06 | — | src/units/unit_motion.cpp | basis: auto |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Infection_Ability/Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Always_Spawn_In_Orbit | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+15 more)) |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Han_Solo, IG-88, Mara_Jade, Obi_Wan_Kenobi (+2 more)) |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Map_Load_Spawn_Table | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bonus_Spawn_Point, Random_Secondary_Structure_E, Random_Secondary_Structure_P, Random_Secondary_Structure_R) |

## MiscObject, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_step_commands.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | src/sim/tactical/session_step_commands.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Cause_Invulnerability_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Convert_Enemy_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Emperor_Instant_Dark_Side_Corrupt_Blast, Proj_Special_Mara_Corrupt_Blast) |
| Projectile_Instant_Heal_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Stun_Spawn_Effect | todo | application not recorded | — | 651 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Weaken_Enemy_Spawn_Effect | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (apps/viewer/src/battle_effects_projectiles.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 941 | apps/viewer/src/battle_effects_projectiles.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Delay_Seconds | todo | application not recorded | — | 651 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Starting_Spawned_Units_Tech_0 | todo | application not recorded | — | 651 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |

## SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Reserve_Spawned_Units_Tech_0 | todo | application not recorded | — | 651 | — | read by a loader, but nothing applies the value |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Location_Bones | todo | application not recorded | — | 651 | — | — |
| Spawned_Squadron_Location_Flyout_Distances | todo | application not recorded | — | 651 | — | — |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Escort | deferred | application not recorded | — | 651 | — | Debug-build combatant classification reads this flag; auto-resolve/classification implementation is missing and the complete mode boundary is unverified.; TCFA-05; basis: reviewed |
| Space_Escort_Unit_Types | todo | application not recorded | — | 651 | — | — |
| Squadron_Capacity | land-or-galactic | application not recorded | — | — | — | Galactic fleet packing holds (WGM-24/87), not tactical garrison squadron capacity.; TCFA-03; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | todo | application not recorded | — | 941 | — | The scene's TIE_Scout FOW_REVEAL_PING field is outside the loader's HARMONIC_BOMB/WEAKEN_ENEMY/SELF_DESTRUCT spawned-object branches and remains unread; space reveal-ping support is still pending. SELF_DESTRUCT's blast object (Vengeance, Krayt) is read and applied by src/units/unit_abilities.cpp&#35;ability_table (WSD-01, WSD-05, WSD-06), but neither ship is in the M2 scene, so the scene trace cannot show it. |

## SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Delay_Seconds | applied | whole class (src/units/unit_motion.cpp) | E75-04, FL-05 | — | src/units/unit_motion.cpp | basis: auto |
| Starting_Spawned_Units_Tech_0 | applied | whole class (src/units/unit_motion.cpp) | E75-02, FL-02 | — | src/units/unit_motion.cpp | basis: auto |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Prevention_Radius | todo | application not recorded | — | 651 | — | — |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | whole class (src/skirmish/start.cpp) | none recorded | — | src/skirmish/start.cpp | basis: auto |
| Is_Escort | deferred | application not recorded | — | 651 | — | Debug-build combatant classification reads this flag; auto-resolve/classification implementation is missing and the complete mode boundary is unverified.; TCFA-05; basis: reviewed |
| Is_Homogeneous | applied | loaded space squadrons (apps/viewer/src/battle_input_cards.cpp); loaded space squadrons (src/presentation/ui/unit_cards.cpp) | L-2, WHE-SQ-01; L-2, WHE-SQ-01 | — | apps/viewer/src/battle_input_cards.cpp; src/presentation/ui/unit_cards.cpp | basis: reviewed |
| Max_Squad_Size | partial | loaded local space battle (apps/viewer/src/battle_scoring.cpp); missing: land companies and non-scoring team creation | WBF-46 | 651 | apps/viewer/src/battle_scoring.cpp | Results scoring applies the guarded first-member team size clamp; authoritative team creation is separate.; TCFA-06; basis: reviewed |
| Squadron_Formation_Error_Tolerance | applied | whole class (src/units/unit_motion.cpp) | E75-18, FM-12, S-28, WSQ-52 | — | src/units/unit_motion.cpp | basis: auto |
| Squadron_Offsets | applied | whole class (src/units/unit_motion.cpp) | AU-82, C-18, E597-02, FM-10, FM-14 | — | src/units/unit_motion.cpp | basis: reviewed |
| Squadron_Units | applied | whole class (src/skirmish/ai.cpp) | AU-82, E597-02, E597-05, E597-07, E75-06 | — | src/skirmish/ai.cpp | matched by the field's name; the loader is table-driven; basis: auto |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | todo | application not recorded | — | 941 | — | The scene's TIE_Scout_Squadron FOW_REVEAL_PING field is outside the loader's HARMONIC_BOMB/WEAKEN_ENEMY spawned-object branches (WHE-61/62) and remains unread; space reveal-ping support is still pending. |
| Variant_Of_Existing_Type | applied | whole class (src/data/xml_merge.cpp) | R-08, R-11 | — | src/data/xml_merge.cpp | basis: reviewed |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Use_Transported_Object_Bounds_For_Landing_Z | todo | application not recorded | — | 651 | — | — |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Landing_Transport_Variant | todo | application not recorded | — | 651 | — | — |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Squadron_Capacity | todo | application not recorded | — | 651 | — | — |
| Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Enable_Bombing_Runs | todo | application not recorded | — | 760 | — | — |
