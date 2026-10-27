# fighters tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 18 | 18 |
| partial | 13 | 13 |
| todo | 27 | 27 |
| presentation-later | 0 | 2 |
| foc-ignores | 12 | 12 |
| deferred | 4 | 4 |
| by-design | 0 | 0 |
| land-or-galactic | 0 | 13 |
| multiplayer | 0 | 0 |
| **Total** | **74** | **89** |

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
| Is_Escort | todo | application not recorded | — | 651 | — | family: fighters-spawn; no loader or consumer.; basis: reviewed |

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
| SFXEvent_Bombing_Run_Enemy_Available | todo | application not recorded | — | 651 | — | family: fighters-spawn; no loader or consumer.; basis: reviewed |
| SFXEvent_HUD_Landing_Zone_Captured | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_HUD_Landing_Zone_Lost | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| SFXEvent_HUD_Last_Landing_Zone_Lost | land-or-galactic | application not recorded | — | — | — | Land support/landing-zone presentation; shared faction loading does not make this a space input.; TCFA-01; basis: reviewed |
| Squadron_Icon_Name | presentation-later | application not recorded | — | — | — | Faction icon presentation is missing; Fighter_Icon_Name has a verified galactic UI consumer, Squadron_Icon_Name exact reader remains unverified.; TCFA-02; basis: reviewed |

## GameConstants

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Reduction_Per_Squadron_Percent | land-or-galactic | application not recorded | — | — | — | LC-59: only the campaign land bombing-run cooldown reads it (attacker reduction per three bombers); no space skirmish consumer. 0 in the shipped data.; TCFA-01; basis: reviewed |
| Indigenous_Spawn_Destruction_Reward | todo | application not recorded | — | 2044 | — | The indigenous-spawn destruction credit reward story event is not wired.; basis: reviewed |
| Max_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |
| Min_Bombing_Run_Interval_Seconds | land-or-galactic | application not recorded | — | — | — | Land bombing-run cooldown (LC-59), outside the M2 space skirmish.; TCFA-01; basis: reviewed |

## GenericHeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Remote_Bomb_Ability/Spawn_Bone | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Remote_Bomb_Ability/Spawn_Frame | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Always_Spawn_In_Orbit | todo | application not recorded | — | 651 | — | Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Mixed ground/space, space carriers on HeroUnit: Boba_Fett, Boba_Fett_NoStealth, Bossk, Chewbacca, Darth_Vader, Darth_Vader_Bounty_Hunter_Immune, Darth_Vader_Expansion, Han_Solo (+11 more). Mixed ground/space, space carriers on GenericHeroUnit: Generic_Fleet_Commander_Empire, Generic_Fleet_Commander_Rebel. Initial placement authoring is retained by the validated galactic loaders (docs/galactic-types.md); parsing alone does not claim runtime application. |

## HardPoint

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Fighter_Bay_Flyout_Distance | applied | whole class (src/units/unit_motion.cpp) | FL-06 | — | src/units/unit_motion.cpp | basis: auto |

## HeroUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Bone | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Grenade_Attack_Ability/Grenade_Spawn_Frame | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Infection_Ability/Spawn_Bone | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Abilities/Infection_Ability/Spawn_Frame | todo | application not recorded | — | 3243 | — | Shared unit-table loader parses this field; no land session path applies this ability on land (no ability hook in session_land*.cpp).; basis: reviewed |
| Always_Spawn_In_Orbit | applied | whole class (src/sim/galactic/production.cpp) | WGS-80, WGS-92 | — | src/sim/galactic/production.cpp | Read per authored company member (live-&gt;always_spawn_in_orbit): forces the company transport into orbit when the accumulated stealth test succeeds or no nonstealth hostile orbital fleet exists; parsed in src/units/galactic_types.cpp.; basis: reviewed |
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | todo | application not recorded | — | 3776 | — | Parsed on main but no land sim reader found (land sim files have no ability use); todo until a land apply PR; basis: reviewed |

## Marker

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Map_Load_Spawn_Table | todo | application not recorded | — | 3776 | — | No reader on main and no retail rule cited; basis: reviewed |

## MiscObject, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (src/sim/tactical/session_cmd_abilities.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. WHE-61: an unresolved reference retains the ability, recharge and sound in both load modes; creation alone is skipped. SELF_DESTRUCT still counts down and kills its owner.; basis: reviewed |

## Projectile

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Projectile_Cause_Invulnerability_Spawn_Effect | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Projectile_Convert_Enemy_Spawn_Effect | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Projectile_Instant_Heal_Spawn_Effect | todo | application not recorded | — | 3776 | — | No code on the land path reads it (grep of src/apps finds no reader).; basis: reviewed |
| Projectile_Stun_Spawn_Effect | todo | application not recorded | — | 651 | — | family: vfx-models; no loader or consumer.; basis: reviewed |
| Projectile_Weaken_Enemy_Spawn_Effect | partial | HARMONIC_BOMB and WEAKEN_ENEMY space abilities (apps/viewer/src/battle_effects_projectiles.cpp); missing: Other spawned abilities; U-10 exact placement/cancellation/autofire policy | WHE-28, WHE-29, WHE-61, WHE-62 | 4532 | apps/viewer/src/battle_effects_projectiles.cpp | G8 binds spawned projectile types, countdowns, timed category-0 modifiers and attached status particles. Nonzero placement Z offsets remain unsupported.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice.; basis: reviewed |

## SecondaryStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Delay_Seconds | applied | whole class (src/units/unit_motion.cpp) | FL-05 | — | src/units/unit_motion.cpp | The spawner profile is read from any SPAWN_SQUADRON body regardless of class.; basis: reviewed |
| Starting_Spawned_Units_Tech_0 | applied | galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); GC space scene (src/units/unit_motion.cpp) | WAR-23; WGT-58, WGT-59 | — | src/campaign/auto_resolve_strength.cpp; src/units/unit_motion.cpp | the loader reads it for other classes; the M2 scene's objects of this class never have it read Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Reserve_Spawned_Units_Tech_0 | partial | prepared campaign space garrisons (src/units/unit_motion.cpp); GC space scene (src/units/unit_motion.cpp); missing: standalone skirmish deliberately ignores reserves | WGT-10; WGT-58, WGT-59 | 651 | src/units/unit_motion.cpp | read by a loader, but nothing applies the value Campaign authored-map space entry scope: docs/behaviour/galactic-space-entry.md. GC space scene consumer: docs/behaviour/galactic-space-entry.md.; basis: reviewed |

## SecondaryStructure, SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Spawned_Squadron_Location_Bones | todo | application not recorded | — | 651 | — | family: fighters-spawn; no loader or consumer.; basis: reviewed |
| Spawned_Squadron_Location_Flyout_Distances | todo | application not recorded | — | 651 | — | family: fighters-spawn; no loader or consumer.; basis: reviewed |

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
| Unit_Abilities_Data/Unit_Ability/Spawned_Object_Type | partial | FOW_REVEAL_PING owner projectile (src/sim/tactical/session_cmd_abilities.cpp); missing: FOW_REVEAL_PING fog-regrowth appearance (WFP-U1) | WFP-01, WFP-02, WFP-03 | 4532 | src/sim/tactical/session_cmd_abilities.cpp | Creates the owner reveal projectile; absent lifetime ends the stock zero-distance ping on its first flight step (WFP-03), followed by ordinary fog regrowth. WFP-U1 regrowth appearance remains capture-needed.; Remaining class/mode consumers are listed explicitly in the follow-up, beyond the completed implementation slice. WHE-61: an unresolved reference retains the ability, recharge and sound in both load modes; creation alone is skipped. SELF_DESTRUCT still counts down and kills its owner.; basis: reviewed |

## SpecialStructure

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Bombing_Run_Prevention_Radius | todo | application not recorded | — | 651 | — | family: fighters-spawn; no loader or consumer.; basis: reviewed |

## Squadron

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Affiliation | applied | reachable stock galactic startup/production objects (src/campaign/live_setup.cpp); whole class (src/skirmish/start.cpp) | WGEP-18, WGEP-19, WGEP-21, WGEP-22, WGEP-23, WGEP-25, WGEP-29, WGEP-32, WGEP-34; none recorded | — | src/campaign/live_setup.cpp; src/skirmish/start.cpp | basis: auto |
| Is_Homogeneous | applied | loaded space squadrons (apps/viewer/src/battle_input_cards.cpp); loaded space squadrons (src/presentation/ui/unit_cards.cpp) | L-2, WHE-SQ-01; L-2, WHE-SQ-01 | — | apps/viewer/src/battle_input_cards.cpp; src/presentation/ui/unit_cards.cpp | basis: reviewed |
| Max_Squad_Size | partial | loaded local space battle (src/platform/battle_scoring.cpp); purchased space teams (src/sim/tactical/session_economy.cpp); bay-born space teams (src/sim/tactical/session_squadrons.cpp); tactical AI squadron metric (src/skirmish/ai.cpp); placed space teams (src/skirmish/start.cpp); missing: land tactical team creation | WBF-46; FL-06; FL-06; PG-08; FL-06 | 4625 | src/platform/battle_scoring.cpp; src/sim/tactical/session_economy.cpp; src/sim/tactical/session_squadrons.cpp; src/skirmish/ai.cpp; src/skirmish/start.cpp | FL-06 applies authored chunking to space team creation; PG-08 guards tactical AI scaling by the first craft Create_Team. Invalid maxima skip the offending type with a diagnostic.; TCFA-06; basis: reviewed |
| Squadron_Formation_Error_Tolerance | applied | whole class (src/units/unit_motion.cpp) | FM-12, WSQ-52 | — | src/units/unit_motion.cpp | basis: auto |
| Squadron_Offsets | applied | whole class (src/units/unit_motion.cpp) | C-18, FM-10, FM-14 | — | src/units/unit_motion.cpp | basis: reviewed |
| Squadron_Units | applied | galactic squadron move responses (apps/viewer/src/galactic/galactic_audio.cpp); galactic auto-resolve (src/campaign/auto_resolve_strength.cpp); galactic current-type membership (src/data/xml_merge.cpp); whole class (src/presentation/ui/hud_shell.cpp); galactic current build limits (src/sim/galactic/production.cpp); whole class (src/skirmish/ai.cpp) | BA-24, WGP-181; WAR-16, WAR-25; WGA-363, WGA-364; CARD-16; WGEP-78; PG-08, PL-13 | — | apps/viewer/src/galactic/galactic_audio.cpp; src/campaign/auto_resolve_strength.cpp; src/data/xml_merge.cpp; src/presentation/ui/hud_shell.cpp; src/sim/galactic/production.cpp; src/skirmish/ai.cpp | matched by the field's name; the loader is table-driven Galactic auto-resolve gathering and strength use the validated original-type table (WAR-15..26); live roster binding remains with the conflict integration consumer. GC census resolves original company members, transport and squadron members through explicit type bindings and uses direct count plus recursive MAX for current build limits. WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |
| Variant_Of_Existing_Type | applied | whole class (src/data/xml_merge.cpp); galactic current-type membership (src/data/xml_merge.cpp) | R-08, R-11; WGA-363, WGA-364 | — | src/data/xml_merge.cpp | WGA-363/364: load-time roster marks and variant copy-time member bits qualify current types; runtime admission uses the freestore consumer.; basis: reviewed |

## TransportUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Use_Transported_Object_Bounds_For_Landing_Z | todo | application not recorded | — | 651 | — | family: other; no loader or consumer; hero transport hulls are not deployed as space bodies in a skirmish (WHE-49 keeps them as metadata).; basis: reviewed |

## TransportUnit, UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Landing_Transport_Variant | todo | application not recorded | — | 651 | — | family: other; no loader or consumer; hero transport hulls are not deployed as space bodies in a skirmish (WHE-49 keeps them as metadata).; basis: reviewed |

## UniqueUnit

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Unit_Abilities_Data/Unit_Ability/Max_Num_Spawned_Objects | todo | application not recorded | — | 3776 | — | No reader on main (0 hits); todo; basis: reviewed |

## UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| Abilities/Battlefield_Modifier_Ability/Enable_Bombing_Runs | land-or-galactic | application not recorded | — | — | — | family: fighters-spawn; no loader or consumer. &#124; scope walk (abilities PR2): SCOPE-LAND: only Ground_Automatic ground bombing-run upgrades author it; SCOPE-LAND: only Ground_Automatic ground bombing-run upgrades author it; basis: reviewed |
