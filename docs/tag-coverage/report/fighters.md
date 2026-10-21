# fighters tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 16 | 16 |
| partial | 13 | 13 |
| todo | 16 | 16 |
| presentation-later | 0 | 2 |
| foc-ignores | 12 | 12 |
| deferred | 4 | 4 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 26 |
| multiplayer | 0 | 0 |
| **Total** | **61** | **89** |

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
| Reinforcements_Pick_Landing_Zone_SFXEvent | partial | local space skirmish gestures (apps/viewer/src/battle_audio_events.cpp); missing: land and campaign callers | BA-84, WR-11 | 4532 | apps/viewer/src/battle_audio_events.cpp | Local pane opening, placement start and accepted scheduler submission use separate feedback cues.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |
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
| Bombing_Run_Reduction_Per_Squadron_Percent | land-or-galactic | application not recorded | — | — | — | LC-59: only the campaign land bombing-run cooldown reads it (attacker reduction per three bombers); no space skirmish consumer. 0 in the shipped data.; TCFA-01; basis: reviewed |
| Indigenous_Spawn_Destruction_Reward | land-or-galactic | application not recorded | — | — | — | Credit reward for an indigenous-spawner destruction story event, not space carrier spawning.; TCFA-03; basis: reviewed |
| Max_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |
| Min_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Remote_Bomb_Ability/Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Abilities/Remote_Bomb_Ability/Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Underworld_Saboteur, Underworld_Saboteur_Exec_Demo) |
| Always_Spawn_In_Orbit | todo | application not recorded | — | 651 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+11 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Initial placement authoring is retained by the validated galactic loaders (docs/galactic-types.md); parsing alone does not claim runtime application. |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fighter_Bay_Flyout_Distance | applied | whole class (src/units/unit_motion.cpp) | FL-06 | — | src/units/unit_motion.cpp | basis: auto |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Kyle_Katarn, Mara_Jade) |
| Abilities/Infection_Ability/Spawn_Bone | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Abilities/Infection_Ability/Spawn_Frame | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (IG-88) |
| Always_Spawn_In_Orbit | land-or-galactic | application not recorded | — | — | — |  Initial placement authoring is retained by the validated galactic loaders (docs/galactic-types.md); parsing alone does not claim runtime application.; SCOPE-LAND: only ground objects author it in the effective FoC XML (Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Droid_C3P0 (+15 more)) |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Emperor_Palpatine, Han_Solo, IG-88, Mara_Jade, Obi_Wan_Kenobi (+2 more)) |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Map_Load_Spawn_Table | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Bonus_Spawn_Point, Random_Secondary_Structure_E, Random_Secondary_Structure_P, Random_Secondary_Structure_R) |

## MiscObject, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_cmd_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Cause_Invulnerability_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Invulnerability_Blast) |
| Projectile_Convert_Enemy_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Emperor_Instant_Dark_Side_Corrupt_Blast, Proj_Special_Mara_Corrupt_Blast) |
| Projectile_Instant_Heal_Spawn_Effect | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Proj_Special_Obiwan_Instant_Heal_Blast) |
| Projectile_Stun_Spawn_Effect | todo | application not recorded | — | 651 | — | Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. Mixed ground/space, space carriers on Projectile: Proj_Ion_Cannon_Medium_Laser_Blue, Proj_Krayt_Bombardment_Ion, Proj_Speeder_Bomb. |
| Projectile_Weaken_Enemy_Spawn_Effect | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (apps/viewer/src/battle_effects_projectiles.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Delay_Seconds | todo | application not recorded | — | 651 | — | the loader reads it for other classes; the M2 scene's objects of this class never have it read |
| Starting_Spawned_Units_Tech_0 | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC space scene (src/units/unit_motion.cpp) | WAR-23; WGT-58, WGT-59 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_motion.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Reserve_Spawned_Units_Tech_0 | partial | prepared campaign space garrisons (src/units/unit_motion.cpp); GC space scene (src/units/unit_motion.cpp); missing: standalone skirmish deliberately ignores reserves | WGT-10; WGT-58, WGT-59 | 651 | src/units/unit_motion.cpp | read by a loader, but nothing applies the value Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Location_Bones | todo | application not recorded | — | 651 | — | — |
| Spawned_Squadron_Location_Flyout_Distances | todo | application not recorded | — | 651 | — | — |

## SpaceUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Space_Escort_Unit_Types | partial | prepared campaign space entry (src/campaign/space_entry.cpp); ordinary galactic targeted-plan proposals (src/script/foc/galactic_ai_selector_runtime.cpp); missing: Unsupported scene and qualification boundaries in docs/behaviour/galactic-space-entry.md | WGT-17; WGA-443, WGA-444 | 2006 | src/campaign/space_entry.cpp; src/script/foc/galactic_ai_selector_runtime.cpp |  Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md. Authored escorts also contribute to ordinary galactic targeted-plan proposal contrast.; basis: reviewed |

## SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Delay_Seconds | applied | whole class (src/units/unit_motion.cpp) | FL-05 | — | src/units/unit_motion.cpp | basis: auto |
| Starting_Spawned_Units_Tech_0 | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC space scene (src/units/unit_motion.cpp); whole class (src/units/unit_motion.cpp) | WAR-23; WGT-58, WGT-59; FL-02 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_motion.cpp |  Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SpaceUnit, Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Is_Escort | deferred | application not recorded | — | 651 | — | Debug-build combatant classification reads this flag; auto-resolve/classification implementation is missing and the complete mode boundary is unverified.; TCFA-05; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | partial | FOW_REVEAL_PING owner projectile (src/sim/tactical/session_cmd_abilities.cpp); missing: FOW_REVEAL_PING fog-regrowth appearance (WFP-U1) | WFP-01, WFP-02, WFP-03 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | Creates the owner reveal projectile; absent lifetime ends the stock zero-distance ping on its first flight step (WFP-03), followed by ordinary fog regrowth. WFP-U1 regrowth appearance remains capture-needed.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Prevention_Radius | todo | application not recorded | — | 651 | — | — |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); whole class (src/skirmish/start.cpp) | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; none recorded | — | src/campaign/live_setup.cpp; src/skirmish/start.cpp | basis: auto |
| Is_Homogeneous | applied | loaded space squadrons (apps/viewer/src/battle_input_cards.cpp); loaded space squadrons (src/presentation/ui/unit_cards.cpp) | L-2, WHE-SQ-01; L-2, WHE-SQ-01 | — | apps/viewer/src/battle_input_cards.cpp; src/presentation/ui/unit_cards.cpp | basis: reviewed |
| Max_Squad_Size | partial | loaded local space battle (src/platform/battle_scoring.cpp); missing: land companies and non-scoring team creation | WBF-46 | 651 | src/platform/battle_scoring.cpp | Results scoring applies the guarded first-member team size clamp; authoritative team creation is separate.; TCFA-06; basis: reviewed |
| Squadron_Formation_Error_Tolerance | applied | whole class (src/units/unit_motion.cpp) | FM-12, WSQ-52 | — | src/units/unit_motion.cpp | basis: auto |
| Squadron_Offsets | applied | whole class (src/units/unit_motion.cpp) | C-18, FM-10, FM-14 | — | src/units/unit_motion.cpp | basis: reviewed |
| Squadron_Units | applied | galactic squadron move responses (apps/viewer/src/galactic/galactic_audio.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); galactic current-type membership (src/data/xml_merge.cpp); whole class (src/presentation/ui/hud_shell.cpp); galactic current build limits (src/sim/galactic/production.cpp); whole class (src/skirmish/ai.cpp) | BA-24, WGP-181; WAR-16, WAR-25; WGA-363, WGA-364; CARD-16; WGEP-78; PG-08, PL-13 | — | apps/viewer/src/galactic/galactic_audio.cpp; src/campaign/auto_resolve_strength.cpp; src/data/xml_merge.cpp; src/presentation/ui/hud_shell.cpp; src/sim/galactic/production.cpp; src/skirmish/ai.cpp | matched by the field's name; the loader is table-driven Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |
| Variant_Of_Existing_Type | applied | whole class (src/data/xml_merge.cpp); galactic current-type membership (src/data/xml_merge.cpp) | R-08, R-11; WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |

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
| Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects | land-or-galactic | application not recorded | — | — | — | SCOPE-LAND: only ground objects author it in the effective FoC XML (Veers_AT_AT_Walker, Veers_AT_AT_Walker_Death_Clone, Veers_AT_AT_Walker_Deployed_Death_Clone) |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Enable_Bombing_Runs | todo | application not recorded | — | 760 | — | — |
