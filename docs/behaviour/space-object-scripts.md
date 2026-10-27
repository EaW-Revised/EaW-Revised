# Space object-script attachment survey

Survey of the effective FoC XML and all 72 effective `Data/Scripts/gameobject/*.lua`
members, compared with the Skirmish implementation at
`8e806e60ccb9120a37ccd2620a78185e0b09d63a` (2026-10-10).
This is an inventory and design recommendation, not an implementation contract
for unresolved lifecycle ties. Private receipts are retained outside tracked docs.

## Scope and result

| Rule | Finding | Evidence |
|---|---|---|
| SOS-01 | `Lua_Script` is the object attachment tag. Resolve `Variant_Of_Existing_Type` before deciding which script a type has; a blank or `TBD` value on a variant changes nothing and keeps the parent's script, like every other object-type tag (ML-24; the loader drops an empty value before the tag lookup, and `Lua_Script` has no exception). Stock: only `Droids_Team` (a hero company, variant of `Han_Solo_Team`) authors a blank value over a script, so it keeps `HanSoloPlan`; the other blank occurrences sit on types with no parent. `Execute_Script_On_Type` enables an additional type-level script consumer; it is not evidence that ordinary object attachment is disabled. | effective XML; debug build SOS-E01/02, ML-24 |
| SOS-02 | Space ships and hero containers have gameplay scripts: ability automation and AI decisions, rather than purely cosmetic work. General attachment is worthwhile as a bounded follow-up, but several effects already have native substitutes. | stock Lua; current source; tables below |
| SOS-03 | The proximity callback boundary is already supported for same-instance handlers. General attachment is still absent; adding another callback fix alone cannot make gameobject scripts run. Squadron candidate coverage is a separate seam (legacy EAWR-2706). | current authoritative host and tactical bindings; latest disposition (legacy EAWR-1850) |
| SOS-04 | Do not infer stock space use from a generic map marker or a land-only model. The existing complete space-map census identifies structures, environmental props and special-weapon markers, but no credit/tech power-up or test-trigger placement. No station or `SpaceProp` type in this XML inventory attaches a script. | effective XML; [neutral-object census](walks/neutral-objects.md#complete-starting-map-census) |

The inventory scans only object definitions in files named by `gameobjectfiles.xml`,
not story-plot `Lua_Script` declarations or commented-out tags. All 75 direct
nonempty declarations are in that index. There are 55 additional inherited
nonempty declarations, for 130 named types including cinematic, landing, captured
and scenario variants. These counts describe data declarations, not 130 runnable
space instances. Unparseable story/AI-template files are outside the object index.
The two `Lua_Script` entries on `Interdictor_Cruiser` require special care: the
later value is `ObjectScript_MissileShield`; the earlier `Interdictor` declaration
is not an additional simultaneously attached script. The remake's XML field lookup
also uses the later value.

## Complete direct attachment inventory

Each cell lists exact mod-facing type names. Space reachability is qualified by
actual domain and construction, not by whether a script file exists. Mixed rows
contain both space and land users; the per-script behaviour section identifies
the space consumers.

| Script | Direct types | SPACE skirmish applicability |
|---|---|---|
| `BobaFettPlan` | `Boba_Fett`, `Tyber_Zann`, `Tyber_Zann_Prisoner` | Land/strategic; no stock space combat consumer identified |
| `CreditPowerUp` | `Credit_Power_Up` | Map-authoring candidate; no stock space placement established |
| `DarthVader` | `Darth_Vader`, `Darth_Vader_Expansion` | Land/strategic; no stock space combat consumer identified |
| `Demo_Attract_Controller` | `DEMO_CONTROLLER` | Story/demo scenario, not ordinary space skirmish |
| `Demo_Land_Controller` | `DEMO_CONTROLLER_LAND` | Story/demo scenario, not ordinary space skirmish |
| `Demo_Land_Controller_E` | `DEMO_CONTROLLER_LAND_E` | Story/demo scenario, not ordinary space skirmish |
| `Demo_Land_Controller_R` | `DEMO_CONTROLLER_LAND_R` | Story/demo scenario, not ordinary space skirmish |
| `E3_Felucia_Zone` | `E3_Felucia_Zone` | Story/demo scenario, not ordinary space skirmish |
| `FieldCommander` | `Generic_Field_Commander_Rebel`, `Generic_Field_Commander_Empire`, `R_General_Rieekan_Commander`, `Hutt_Field_Commander` | Land/strategic; no stock space combat consumer identified |
| `FleetCommander` | `Generic_Fleet_Commander_Rebel`, `Generic_Fleet_Commander_Empire` | Flagship/hero metadata; no stock skirmish purchase established |
| `GenericBountyHunter` | `Bounty_Hunter_Team_E`, `Bounty_Hunter_Team_R` | Land/strategic; no stock space combat consumer identified |
| `HanSolo` | `Han_Solo` | Land/strategic; no stock space combat consumer identified |
| `HanSoloPlan` | `Han_Solo_Team` | Space company metadata (Han_Solo_Team_Space_MP); also land |
| `LukePlan` | `Luke_Skywalker` | Land/strategic; no stock space combat consumer identified |
| `MarkerTrigger` | `Test_Trigger_Marker` | Map-authoring candidate; no stock space placement established |
| `ObiWanPlan` | `Obi_Wan_Kenobi` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_Gargantuan` | `Gargantuan_Battle_Platform` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_IG2000` | `IG-2000` | Space combat; land users/variants separately noted |
| `ObjectScript_IG88` | `IG-88` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_Krayt` | `Krayt_Class_Destroyer` | Space combat; land users/variants separately noted |
| `ObjectScript_MaraJade` | `Mara_Jade` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_MC30` | `MC30_Frigate` | Space combat; land users/variants separately noted |
| `ObjectScript_MillenniumFalcon` | `Millennium_Falcon` | Space combat; land users/variants separately noted |
| `ObjectScript_MissileShield` | `AT_AA_Walker`, `Interdictor_Cruiser` | Space combat; land users/variants separately noted |
| `ObjectScript_PointDefense` | `HAV_Juggernaut`, `Crusader_Gunship` | Space combat; land users/variants separately noted |
| `ObjectScript_PowerToShields` | `Calamari_Cruiser`, `Nebulon_B_Frigate`, `Alliance_Assault_Frigate`, `Home_One` | Space combat; land users/variants separately noted |
| `ObjectScript_RemoteBomb` | `Proj_Remote_Bomb` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_SelfDestructLand` | `TIE_Crawler`, `MZ8_Pulse_Cannon_Tank` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_Silri` | `Silri`, `Silri_No_Abilities` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_Urai` | `Urai_Fen` | Land/strategic; no stock space combat consumer identified |
| `ObjectScript_VaderTIESquadron` | `Darth_Vader_TIE_Fighter_Container` | Space combat; land users/variants separately noted |
| `ObjectScript_Yoda` | `Yoda` | Land/strategic; no stock space combat consumer identified |
| `Palpatine` | `Emperor_Palpatine` | Land/strategic; no stock space combat consumer identified |
| `ProximitySpawnTrigger` | `Secondary_Pirate_Outpost_H` | Land/strategic; no stock space combat consumer identified |
| `ProximitySpawnTrigger_R` | `Secondary_Training_Facility_R` | Land/strategic; no stock space combat consumer identified |
| `SandCrawler` | `Jawa_Sandcrawler` | Land/strategic; no stock space combat consumer identified |
| `SecondaryHeavyFactory` | `Secondary_Heavy_Factory_E`, `Secondary_Heavy_Factory_R` | Land/strategic; no stock space combat consumer identified |
| `SecondaryMiningFacility` | `Secondary_Mining_Facility` | Land/strategic; no stock space combat consumer identified |
| `Slave_I` | `Slave_I` | Space combat; land users/variants separately noted |
| `SpeederBike` | `Scout_Trooper` | Land/strategic; no stock space combat consumer identified |
| `Sundered_Heart` | `Sundered_Heart` | Space combat; land users/variants separately noted |
| `Tarkin` | `Grand_Moff_Tarkin`, `Grand_Admiral_Thrawn` | Flagship/hero metadata; no stock skirmish purchase established |
| `TechPowerUp` | `Tech_Power_Up` | Map-authoring candidate; no stock space placement established |
| `Training_Facility` | `Rebel_Training_Script_Marker` | Land/strategic; no stock space combat consumer identified |
| `UM02_Destroyer_Spawn` | `UM02_Destroyer_Spawner` | Story/demo scenario, not ordinary space skirmish |
| `UM02_DroidCrate` | `UM02_Droid_Crate` | Story/demo scenario, not ordinary space skirmish |
| `UM02_Droids_Spawn` | `UM02_Droids_Spawner` | Story/demo scenario, not ordinary space skirmish |
| `UM02_MagnetDoors` | `UM02_Magnetically_Sealed_Doorway` | Story/demo scenario, not ordinary space skirmish |
| `UM02_Mission_Script` | `UM02_MissionScript_Temp` | Story/demo scenario, not ordinary space skirmish |
| `UM06_Mission_Script` | `UM06_MissionScript_Temp` | Story/demo scenario, not ordinary space skirmish |
| `UM11_ForceAdeptFour` | `UM05_ADEPTFOUR` | Story/demo scenario, not ordinary space skirmish |
| `UM11_ForceAdeptOne` | `UM05_ADEPTONE` | Story/demo scenario, not ordinary space skirmish |
| `UM11_ForceAdeptThree` | `UM05_ADEPTTHREE` | Story/demo scenario, not ordinary space skirmish |
| `UM11_ForceAdeptTwo` | `UM05_ADEPTTWO` | Story/demo scenario, not ordinary space skirmish |
| `UMP_Mission_Script` | `UMP_MissionScript_Temp` | Story/demo scenario, not ordinary space skirmish |
| `UMP_SpicePowerUp` | `UMP_Spice_Power_Up` | Story/demo scenario, not ordinary space skirmish |
| `underworld_hunt` | `Destroyer_Droid_Attack` | Land/strategic; no stock space combat consumer identified |
| `Veers` | `General_Veers` | Land/strategic; no stock space combat consumer identified |

## Complete inherited attachment inventory

These types inherit a nonempty attachment through `Variant_Of_Existing_Type`.
A derived type can retain a script while replacing its abilities, behaviours or
model; script inheritance does not prove that its guarded ability can fire.
For example, `The_Peacebringer` inherits the Krayt script but replaces the ability
list, and several fighter containers inherit the Vader script without being
Vader's team. Landing and ground props do not become space combatants because
of that inherited script.

| Inherited script | Derived types | SPACE qualification |
|---|---|---|
| `BobaFettPlan` | `UMP_TYBER_ZANN`, `Tyber_Zann_Passenger`, `Tyber_Zann_Prologue`, `Tyber_Zann_Prologue_Cin`, `UM06_Tyber_Zann`, `Underworld_Disruptor_Merc_Solo_Unit` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `DarthVader` | `Darth_Vader_Bounty_Hunter_Immune` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `FieldCommander` | `Squad_Generic_Field_Commander_Rebel`, `Squad_Generic_Field_Commander_Empire`, `MOV_FIELD_COMMANDER_EMPIRE`, `Spaceport_Director_Empire`, `Spaceport_Director_Rebel`, `Spaceport_Director_Hutts`, `Fleet_Commander_Empire`, `Fleet_Commander_Flankar_Empire`, `Fleet_Commander_Flankar_Rebel` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `HanSolo` | `EM6_Prison_Wookies`, `MOV_Mothma`, `MOV_R2D3`, `UMP_HanSolo` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `HanSoloPlan` | `Han_Solo_Team_Land_MP`, `Han_Solo_Team_Space_MP` | Space company metadata (Han_Solo_Team_Space_MP); also land; cinematic/landing/captured variants require their own admission |
| `ObjectScript_Gargantuan` | `Gargantuan_Battle_Platform_Death_Clone` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `ObjectScript_IG2000` | `IG2000_Landing`, `IG2000_Ground_Prop` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_Krayt` | `The_Peacebringer` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_MillenniumFalcon` | `Fin_Millennium_Falcon`, `Underworld_Vehicle_Transport_Landing_CIN`, `Millennium_Falcon_Landing`, `Millennium_Falcon_Space`, `Millennium_Falcon_Ground`, `Millennium_Falcon_Ground_CIN`, `Millenium_Falcon_Backwards` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_MissileShield` | `AT_AA_Walker_Captured` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_PointDefense` | `HAV_Juggernaut_Captured` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_PowerToShields` | `Fin_Calamari`, `Fin_Nebulon_B`, `Fin_A_Assault_Frigate`, `Fin_Home_One`, `Cin_Nebulon_B_Warp`, `Nebulon_B_Underworld` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `ObjectScript_SelfDestructLand` | `FIN_TIE_Crawler`, `TIE_Crawler_Captured`, `MZ8_Pulse_Cannon_Tank_Captured` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `ObjectScript_Urai` | `UMP_UraiFen`, `Urai_Fen_Prologue` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `ObjectScript_VaderTIESquadron` | `Y_Wing_Squadron_Container`, `Hutt_VWing_Squadron_Container`, `TIE_Defender_Squadron_Container` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `Slave_I` | `Slave_I_Landing`, `Slave_I_Ground_Prop`, `V_Wing_Ground_Prop` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |
| `SpeederBike` | `FIN_Scout_Trooper`, `Scout_Trooper_Converted_To_Enemy` | Land/strategic; no stock space combat consumer identified; cinematic/landing/captured variants require their own admission |
| `Sundered_Heart` | `Sundered_Heart_Cinematic_Clone` | Space combat; land users/variants separately noted; cinematic/landing/captured variants require their own admission |

## Space-reachable behaviour

These summaries describe stock Lua in our own words. The ordinary space state
machines author `ServiceRate` 1 second; that is a strict elapsed-time gate, not
an unconditional 30-tick callback. The engine calls the module's inherited
`Base_Definitions`, then gives its `main` coroutine the object context.
The generic state-machine default is 0.2 seconds; hero-plan metadata defaults
to 10 seconds. Read the authored value after initialization.

| Rule | Script and concrete space consumers | Player visibility | Current substitute |
|---|---|---|---|
| SOS-10 | `ObjectScript_PowerToShields`: Calamari_Cruiser, Nebulon_B_Frigate, Alliance_Assault_Frigate, Home_One; inherited Nebulon_B_Underworld and variants | owner-visible automation, AI too | native DEFEND consumer |
| SOS-11 | `ObjectScript_MissileShield`: Interdictor_Cruiser; AT_AA_Walker is land | owner-visible automation, AI too | ability payload exists; generic script absent |
| SOS-12 | `ObjectScript_PointDefense`: Crusader_Gunship; HAV_Juggernaut is land | AI-only automatic decision; human manual payload remains visible | native AI point-defence consumer |
| SOS-13 | `ObjectScript_MC30`: MC30_Frigate | owner-visible automation, AI too | native MC30 consumer |
| SOS-14 | `ObjectScript_Krayt`: Krayt_Class_Destroyer; The_Peacebringer inherits it with different abilities | owner-visible automation, AI too | native self-destruct consumer |
| SOS-15 | `ObjectScript_MillenniumFalcon`: Millennium_Falcon and space variants | owner-visible automation, AI too | ability payload exists; generic script absent |
| SOS-16 | `Slave_I`: Slave_I | owner-visible automation, AI too | ability payload exists; generic script absent |
| SOS-17 | `Sundered_Heart`: Sundered_Heart | AI-only decision | ability payload exists; generic script absent |
| SOS-18 | `ObjectScript_IG2000`: IG-2000 | AI-only decision | native AI corrupt-systems consumer |
| SOS-19 | `ObjectScript_VaderTIESquadron`: Darth_Vader_TIE_Fighter_Container; inherited Y_Wing_Squadron_Container, Hutt_VWing_Squadron_Container, TIE_Defender_Squadron_Container | owner-visible automation, AI too, only where the ability is authored | replenishment payload exists; script/count API absent |
| SOS-20 | `HanSoloPlan`: Han_Solo_Team and its space multiplayer variant; `FleetCommander`: generic fleet commanders; `Tarkin`: Tarkin/Thrawn flagship heroes | AI-only metadata; flagship heroes are conditional/scenario reachability | type weights partly bound; evaluator/attachment metadata consumer missing |

**SOS-10.** The shield script exits outside Space and initially chooses human
or AI behaviour. AI owners activate ready `DEFEND` when the reported incoming
damage rate exceeds 20. Human owners use the same trigger only while autofire
is armed; toggling autofire changes the state. Initialization chooses the owner
path once, so a generic host must preserve the script's own retained state.
The native DEFEND consumer already supplies this visible feature, with cadence
and rate-publication differences recorded by FD-15/FSC-16.

**SOS-11.** Missile shielding exits only in Galactic. On admitted updates it
looks for the object's deadly enemy and examines that enemy's projectile types;
a projectile affected by the shield prompts ready `MISSILE_SHIELD`. Human
owners require autofire, while AI owners automatically try. The AI state can
notice a transfer to a human owner. This is the later effective Interdictor
attachment, not the separate unattached retreat/interdiction script.

**SOS-12.** Point defence uses the same deadly-enemy/projectile-type pattern,
with `LASER_DEFENSE` and its affected-projectile predicate. Stock Crusader's
ability does not support autofire, so the human automatic path has no admitted
use; the AI choice has visible projectile-defense consequences. A native AI
consumer already implements that choice, with an entity-phased cadence.

**SOS-13.** MC30 counts distinct hostile parents in its 500-extent proximity
query, excluding promoted targets closer than 150. AI attempts cluster bombs
when accumulated combat rating reaches 1000; an armed human uses them for more
than two counted units. Each update clears the previous sample, and wrapper
service then collects the next one. FS-29, FC-57/58 and FD-47 establish the
previous-sample ordering and its retained seam; the native consumer already
covers the approved visible trigger.

**SOS-14.** Krayt collects hostile parents within extent 400 and accumulates
combat rating. AI below half hull attempts self-destruct when nearby threat
reaches a threshold that falls from 1500 toward zero as hull falls. An armed
human requires hull at most 0.2 and more than two units. Counts and identity
sets reset after updates. Its native substitute retains sampled state, so
running both consumers would risk duplicate attempts. Inheriting the script
onto a type without `SELF_DESTRUCT` does not grant that ability.

**SOS-15.** Falcon's script exits outside Space and requires a valid deadly
enemy. AI activates ready invulnerability below 0.2 shield. Human autofire
requires both shield below 0.05 and hull below 0.5. These are different trigger
thresholds; attaching the script would supply an owner-visible choice that is
not implied by implementing the manual ability alone.

**SOS-16.** Slave I samples hostile parents within extent 200, deduplicates
and sums their combat rating. AI attempts the harmonic bomb at threat at least
1000; an armed human requires more than two nearby units. It clears the sample
each update and refills it through proximity callbacks, giving the same
previous-sample structure as MC30. The bomb payload alone does not implement
this automatic selection.

**SOS-17.** Sundered Heart exits for human owners and outside Space. Its AI
samples enemies within extent 1000 and attempts `WEAKEN_ENEMY` at a selected
local threat centre of radius 300 after at least three distinct contacts and
threat above 1000, or at least ten contacts with a valid centre. It also
publishes hero attack/escort weighting metadata. The callback makes the ability
choice while collecting the current sample, unlike MC30's next-update use.

**SOS-18.** IG-2000 exits for humans and outside Space. It collects hostile
objects within extent 700, retaining shieldless objects only if they are the
deadly enemy. A later update finds a radius-300 local threat centre and activates
ready `CORRUPT_SYSTEMS` above threat 1000, then clears the collection. Its native
AI consumer already selects and commands this ability; generic Lua would need
the currently unbound threat-centre helper and the shared-RNG seam.

**SOS-19.** Vader's container script exits outside Space and activates ready
wingman replenishment when its contained-object count is below two. Humans
require autofire. Other inherited containers must still pass actual authored
ability readiness; the script name cannot identify Vader's leader or conjure
an ability. The missing contained-count binding is a direct blocker even though
the underlying replenishment command exists.

**SOS-20.** Han's company script and the fleet/flagship metadata scripts
publish attack and escort weights plus minimum plan-attachment costs. Their
`HeroService` functions are empty; pumping their `main` loops is not itself an
attack or ability decision. Their effect requires a consumer of those metadata
functions, using type-list evaluation. Fleet commanders also execute on
the type; Tarkin/Thrawn attach to flagships in campaign/scenario data. Keep the
type metadata state separate from any actual spawned object state, and do not
claim those heroes are ordinary space-skirmish purchases.

## Required API audit

B = already bound with a usable consumer; P = registered but incomplete/stubbed;
M = missing from this space host. A bound API does not imply that an unattached
script runs. Library functions are Lua code, not missing engine bindings.

| FoC API or library facility | Status at comparison base | Consumers / qualification |
|---|---|---|
| `Get_Game_Mode`, `Object.Get_Owner`, `Player.Is_Human`, `Player.Is_Enemy`, `Object.Is_Valid`, `Object.Get_Type`, `Object.Get_Position`, `Object.Get_Distance` | B | shared guards and object/owner queries |
| `Object.Has_Ability`, `Object.Is_Ability_Ready`, `Object.Is_Ability_Active`, `Object.Is_Ability_Autofire`, `Object.Activate_Ability` | B; activation produces a recorded use committed in the same object turn | every ability script; immutable staged rows replace per-object traversal (SOS-42) |
| `Object.Get_Hull`, `Object.Get_Shield` | B | Falcon, Krayt, IG-2000 |
| `FindDeadlyEnemy` | B with tactical AI goal host; otherwise diagnostic/nil | Falcon, missile shield, point defence, IG-2000 |
| `Object.Get_Rate_Of_Damage_Taken` | P: returns constant zero | shield script; native damage-rate consumer is not this Lua binding |
| `Object.Get_All_Projectile_Types`, `Type.Is_Affected_By_Missile_Shield`, `Type.Is_Affected_By_Laser_Defense` | B | missile shield / point defence |
| `Object.Event_Object_In_Range`, `Object.Cancel_Event_Object_In_Range`, `Object.Service_Wrapper` | B for same-instance functions; collection coverage remains bounded | MC30, Krayt, Slave I, Sundered Heart, IG-2000; squadron candidates have a separate issue |
| `Object.Get_Parent_Object` | P: registered unsupported diagnostic | MC30, Krayt, Slave I need actual parent promotion |
| `Type.Get_Combat_Rating` | M; an unsupported object-method namesake is not this API | MC30, Krayt, Slave I |
| `Object.Get_Contained_Object_Count` | M | Vader/container replenishment |
| `Find_Best_Local_Threat_Center` | M; native IG-2000 logic does not bind it | Sundered Heart, IG-2000 |
| `WeightedTypeList.Create`, list `Parse` | P: one placeholder handle, Parse validates but does not retain weights | hero metadata |
| `EvaluateTypeList` | M | `HeroPlanAttach.Get_Target_Weight` metadata evaluation |
| `Player.Get_Difficulty` | P: constant Normal | stock `Try_Ability` chance policy |
| `GameRandom` | P: deterministic per-instance stream rather than original shared stream | original initial service phasing uses synchronized host RNG; preserve that distinct host-phase seam |
| `GetCurrentTime`, `GetThreadID`, `_ScriptExit`, `ThreadValue`/`.Set`/`.Reset`, `GetEvent`/`.Params`/`.Reset`, `_ScriptMessage`, `Script.Debug_Should_Issue_Event_Alert` | B in existing host/bindings | state-machine base initialization and event/yield loop; debug alert is false |
| `Define_State`, `Set_Next_State`, `Register_Prox`, `PumpEvents`, `Try_Ability`, `TestValid`, `Register_Timer`, `Sleep`, `ScriptExit`, `DebugMessage`, `ScriptError` | Lua library facilities | provide PGStateMachine and existing PGCommands/PGBase dependencies through logical module loading; `Try_Ability` includes time-seeded chance and ready/inactive tests; stock chance is 100 for all difficulties |

The current native-substitute sites are
`src/sim/tactical/session_abilities.cpp` (DEFEND/self-destruct/MC30),
`src/script/foc/ai_taskforces.cpp` (point defence/corrupt systems), and
`src/skirmish/ai.cpp` (script-name flags).
The API audit reads `src/script/foc/tactical_ai_bindings.cpp`,
`src/script/foc/plan_bindings.cpp` and the authoritative host.
`src/script/foc/tactical_ai_loading.cpp` includes freestore/plan modules and
library directories, but no generic gameobject attachment/module path.

## Map and scenario boundaries

| Script family | Space relevance and behaviour | Required API status |
|---|---|---|
| `CreditPowerUp` | Conditional map-authoring use; stock type has only a land model. A human-owned nearby object causes a 1000-credit grant, callback cancellation and pickup despawn. Owner-visible if a space map deliberately authors it; stock placement not established. | owner/human, proximity/cancel and `Player.Give_Money` B; `Object.Despawn` M |
| `TechPowerUp` | Conditional map-authoring use; only land model. A human contact receives random sliceable tech, or 10000 credits if none is available, then the pickup cancels and despawns. Owner-visible, but no stock space placement established. | shared queries/proximity/money B; `Player.Give_Random_Sliceable_Tech`, `Object.Despawn` M |
| `MarkerTrigger` | Land test trigger: only a Plex soldier contact qualifies; reinforces a Rebel tank-buster squad at the marker and waits 60 seconds before rearming. It has no stock space-unit trigger. | proximity/cancel, owner/type/position, `Find_Object_Type` B; the marker's `Reinforce_Unit`/completion-wait workflow is not bound by the space plan host |
| `ProximitySpawnTrigger` / `_R` | Land secondary training/outpost users, not space stations. Do not replace space pad construction or reinforcement with these scripts. | no space attachment proposed; land workflow outside this API audit |
| `ObjectScript_SelfDestructLand`, `ObjectScript_Silri` | Land consumers, including proximity cancellation; do not arise from scripted space hero transports merely because their passengers have a land script. | land-only remainder, not part of this proposed space slice |
| `ObjectScript_RemoteBomb` | Projectile script exits outside Land and for humans; therefore no space decision even if a mod creates that projectile in space. | land-only remainder |
| story/demo/mission props | Explicit scenario orchestration, not ordinary skirmish; table retains all direct and inherited declarations. | scenario/story host remains separate |

No space-visible effect in this inventory is merely cosmetic. “AI-only” describes
who initiates a decision; the resulting bomb, defence or debuff remains visible
to opponents. Stock neutral space structures and props use native behaviour,
construction and special-weapon hosts; they do not justify attaching all land
marker scripts to every space map.

Fourteen effective gameobject members have no surviving object-type attachment:
`deathstarplan`, `demo_deathstar_controller`, `demo_felucia_controller`,
`heroplanattach`, `interdictor`, `ioncannon`, `jgs_functionlib`, `marajade`,
`mauler`, `objectscript_nightsister`, `um01_blackboxpowerup`,
`ump_enslaved_prisoners`, `ump_enslaved_wookies`, `wedgeplan`.
Some are required libraries or story-plot scripts; file presence is not an
attachment. In particular, the unattached `interdictor` script's retreat delay
and interdiction timer are not added to the effective missile-shield behaviour.

## Lifecycle and debug-build evidence

Read-only debug research completed successfully. SOS-E01 covers object attachment
and type-level initialization; SOS-E02 covers object creation, saved-state load
and destruction; SOS-E03 confirms the nonempty-name getter and rechecks the
existing object-service gates against the published schedule. The existing frame schedule is cited first; this survey does
not introduce a new service order or cadence trace.

| Rule | Observable lifecycle fact | Evidence |
|---|---|---|
| SOS-30 | Ordinary object initialization attaches the resolved script after authored behaviours are applied. Fresh attachment registers engine commands, runs `Base_Definitions` for a fresh script, creates `main` without a parameter, then assigns `Object`. That coroutine does not run during creation. Attachment also registers an otherwise unserviced object for service. | debug build SOS-E01/02/03; L-27 |
| SOS-31 | Object Lua is serviced after ordinary behaviours/model work and special abilities, before the hardpoint tail. Deletion-pending objects do not pump it. Admission reads current numeric `ServiceRate` and `LastService`; elapsed mode time must be strictly greater than the rate. Initial admission has a randomized negative reference phase; later continuing pumps stamp current mode time. A finished script detaches. | WFO-21..24; FS-29; FC-57; L-27/29, with the more specific FC-57 clock/initialization qualification |
| SOS-32 | Saved object-script data selects a load-specific attachment path, registers commands and restores saved Lua state. It does not run ordinary fresh initialization or create another fresh `main`. Exact identity fixup/owner-transfer producers are not established by this bounded reading. | debug build SOS-E01/02; UFS-27 |
| SOS-33 | Object destruction explicitly shuts down and releases an attached script. Pending deletion blocks later Lua service before memory destruction. Do not equate a hull death notification with immediate Lua state destruction. | debug build SOS-E02; WFO-24; L-04 |
| SOS-34 | Type-level script initialization clears an earlier type script, creates a script when its authored gate/name allows it and registers commands. This path does not bind an `Object` or create `main` as ordinary object attachment does. Generic commanders additionally use `Execute_Script_On_Type`; ordinary object initialization still uses the nonempty name. Metadata evaluation needs its own consumer. | debug build SOS-E01; effective XML |
| SOS-46 | A fresh object's authored Lua root failing to load leaves it without that attachment. Script creation cleans up and returns no script; attachment diagnoses and returns failure before definitions/main/Object setup. Ordinary object initialization ignores that attachment result and continues its other behavior, ability and radar setup. The type-level path likewise diagnoses a missing script without rejecting the type. In the remake, a missing object root produces one warning naming its referencing types; fresh and cached admission retain the object and omit only that attachment. Its native script decision implementations are also suppressed at fresh and restored binding; manual ability activation remains available. The sorted omission set enters the existing session Lua closure identity only when nonempty, leaving stock bytes unchanged. Invalid logical paths, other I/O failures and required AI/library roots keep their existing failures; present script compile/runtime behavior is unchanged. | debug build read-only missing-root caller chain; L-07; synthetic fresh/cache and 1/2/4/8-worker object-survival contracts |

Unresolved boundaries remain explicit: exceptional creation exclusions, transferred
hero/transport contexts, pooled-state ownership, load fixup ordering, callback
delivery against containers, same-frame commands and exact first service after
creation. FC-57/UFS-27 and the frame-service programme own any new order/cadence
question; the bounded initializer and destructor reading settles lifecycle
existence without claiming those wider ties.

## Smallest implementation recommendation

| Rule | Proposed design (project policy, not an original-game claim) | Acceptance |
|---|---|---|
| SOS-40 | Add an object-script registry to the authoritative session, keyed by stable object identity and generation, with a distinct script-instance ID namespace. Attach one isolated instance for an admitted resolved per-object `Lua_Script`; make the additional type-script declarations a separate metadata registry. Load gameobject/library modules through the existing logical manifest and include their bytes/dependencies in the content digest. | actual stock space units attach once at start, reinforcement and spawn; absent/empty tags attach none; explicit diagnostics identify unsupported modules/APIs |
| SOS-41 | On fresh start, load/initialize definitions, bind `Object`, and create `main` without running it until its first admitted pump. Retain service rate/reference time and Lua state. Preserve each script's actual ownership guards, not a blanket human-only or AI-only filter. | synthetic module checks globals at definitions versus main; AI/human, pause, completion and destruction cases; no duplicate main on restore |
| SOS-42 | Reuse partitioned ScriptScheduler instances with immutable copied world inputs and disjoint staging output. Specify deterministic instance/event/command order using stable IDs and existing canonical keys, then ordered commit. The object turn follows ability expiry/recharge, the damage-rate close and special-ability handlers, then precedes tracking and late impacts (WFO-22, FD-47). Due instances read one staged view; other objects observe their activations from the next tick, preserving the accepted staged-view seam. Due admission is prepared at the preceding barrier; initialized objects receive callbacks only in their next turn. Ability uses commit in the current tick; other verbs merge with barrier output by (issuer, sequence) and route next tick. Never add a serial per-object simulation loop to imitate original traversal. | identical effects and state hashes on 1/2/4/8 workers; work-count budget proportional to attached/due instances and indexed proximity candidates |
| SOS-43 | Reuse same-instance callback references and proximity state. Supply real parent/count/type-rating, damage-rate and threat-centre APIs only for the scripts admitted in a slice. Put callback lists, sample sets, coroutine slots, service reference and pending commands into existing canonical persistence/hash state. | interrupted versus uninterrupted save/load before due pump, after sample and before command application; stable callback identity and no extra event/use |
| SOS-44 | Retire a native substitute only in the same slice that makes its Lua replacement equivalent. Maintain one active consumer per effect. Object retirement stops service and drops callbacks/events before releasing its state; stale-generation handles cannot address a replacement object. | no double DEFEND/bomb/AI command; despawn/reinforcement identity reuse and pending deletion cases |
| SOS-45 | Start with Falcon's guarded invulnerability and Vader's replenishment (plus real contained-count API), then the shared proximity family after parent/type-rating and squadron collection are qualified. Migrate existing substitutes independently; defer hero metadata, land markers, campaign props and unplaced power-ups. | stock Falcon/Vader owner-visible automation contracts; later MC30/Slave I/Krayt previous-sample contracts; retain FS-29/FD-47/UFS-27 seams |

There is worthwhile space-visible work, but enabling every stock script now
would expose stubs and duplicate native consumers. Prepare the bounded
attachment follow-up now and schedule general phase/lifecycle migration after
M2 sign-off unless the owner separately approves a visible missing consumer.
The ignored implementation issue draft accompanies this survey; it is not filed
on GitHub by this docs-only task. Existing manual ability payloads and the
proximity fix remain usable while attachment is developed.

## Admitted attachment slice

Module admission follows every nonempty resolved `Lua_Script`, including
renamed mod modules. Falcon, Vader/container, Slave I, Sundered Heart and
missile-shield scripts currently run through the attachment lifecycle
(SOS-11/15..17/19/40..45). The
unit-table resolver supplies inherited `Lua_Script` values, including explicit
empty overrides. An unresolved `Create_Team_Type` stays an unresolved table row
and attaches no script; the purchase type's own value is not substituted. Ordinary attachment remains independent of type metadata.
Gameobject/library bytes enter the logical manifest and cached content digest.
DEFEND, Krayt, MC30, point defence and IG-2000 retain their native consumers
and existing same-turn timing. Attachment checks the existing native consumer
flags in the AI type or world ability profile and suppresses duplicate Lua
service; loading a module does not grant it a second active consumer. This is
independent of a stock script-name admission list. The same-turn object-script
barrier design remains a follow-up (legacy EAWR-3644). Separate authored plan-library
DEFEND and LASER_DEFENSE requests remain retail consumers (FH-28, WLD-10).

Campaign space entry freezes candidate attachment bytes alongside its resolved
unit tables, then selects the logical manifest from each battle's reachable
types (SOS-01/40/45). That closure includes carried companions, reinforcements,
spawned teams and buildable types even when no actor of that type exists at entry.
Only their admitted object roots and dependencies enter the child session; an
unrelated missing attachment module does not refuse another battle. A required
module that could not be prepared retains its logical-file diagnostic.

Attachment proposals and strict service admission run in partitioned phases over
copied world inputs. Ordered lifecycle commits allocate/retire Lua states; definitions,
main creation and Object assignment run in the existing partitioned scheduler.
Main waits for a subsequent admitted pump. Tactical allocation IDs never repeat,
so each allocation identity supplies its generation. Object instance IDs begin
at 2^36, separately from freestore/plan IDs. Save v4 retains attachment records,
queued initialization, callbacks, service globals and staged commands. Completed
or rejected attachments stay recorded while the object lives; destruction drops
callbacks, events and state.

A deletion-pending object is never pumped (SOS-31/33, FC-31). The world erases
an object in the tick of its lethal damage (FS-07), so the completed view never
holds one; absence from that view is the admission gate.

Strict due admission and initial negative phase consume FC-57. The post-world
snapshot/next-world command phase and per-instance RNG retain the existing
FS-29/FD-47/UFS-27 host seams. No new owner-transfer reset or retail load-fixup
claim is made. `Get_Contained_Object_Count` reads actual live members, including
the leader; inherited script names grant no absent ability. The journal counts
active object instances and due pumps independently of AI plans.

## Space-reachable classification

Every script with a space consumer in the inventory above, by current host
status. "Native substitute" means an existing native consumer already supplies
the visible decision; attaching the Lua as well would duplicate it (SOS-44), so
the script stays unattached until its migration replaces that consumer.

| Script | Space consumers (any faction) | Status | Remaining need |
|---|---|---|---|
| `ObjectScript_MillenniumFalcon` | Millennium_Falcon and space variants | attached, working (SOS-15) | none |
| `ObjectScript_VaderTIESquadron` | Darth_Vader_TIE_Fighter_Container; inherited Y_Wing, Hutt_VWing and TIE_Defender containers | attached, working (SOS-19); inherited containers act only with an authored ability | none |
| `Slave_I` | Slave_I | attached, working (SOS-16) | none |
| `Sundered_Heart` | Sundered_Heart | attached, working (SOS-17) | hero attack/escort metadata needs `EvaluateTypeList` and a consumer (SOS-20) |
| `ObjectScript_MissileShield` | Interdictor_Cruiser | attached, working (SOS-11) | none |
| `ObjectScript_PowerToShields` | Calamari_Cruiser, Nebulon_B_Frigate, Alliance_Assault_Frigate, Home_One, Nebulon_B_Underworld | native substitute (DEFEND consumer, SOS-10) | migration retires the native consumer |
| `ObjectScript_PointDefense` | Crusader_Gunship | native substitute (AI point defence, SOS-12) | migration |
| `ObjectScript_MC30` | MC30_Frigate | native substitute (SOS-13) | migration; previous-sample seam FS-29 |
| `ObjectScript_Krayt` | Krayt_Class_Destroyer, The_Peacebringer | native substitute (WSD-10, SOS-14) | migration |
| `ObjectScript_IG2000` | IG-2000 | native substitute (WHE-67/68, SOS-18) | migration; shared-RNG seam |
| `HanSoloPlan`, `FleetCommander`, `Tarkin` | Han_Solo_Team_Space_MP; fleet commanders; Tarkin/Thrawn flagships | not attached: metadata only (SOS-20) | `EvaluateTypeList` and a metadata consumer |
| all other declarations | land, strategic, story, demo and map-authoring types | not reachable in space skirmish (SOS-04, map boundaries above) | none |

`Get_Parent_Object` answers a craft's live team container and nil for any other
space object; `Type.Get_Combat_Rating` answers the type's combat power metric in
single precision (a squadron type's is its craft sum, PL-13). The object method
of that name stays an unsupported diagnostic.

`Find_Best_Local_Threat_Center(objects, radius)` takes the table's object values
(entries that are not objects warn, dead objects are skipped) in ascending ID
order and applies WHE-68 with the requested radius; the native IG-2000 consumer
shares the same computation. Its initial centers draw from the calling
instance's stream, the existing per-instance RNG seam. It answers a position
wrapper and the threat, or nil without positive threat. The wrapper answers
`Is_Valid` (true: only the unset value is invalid) and `Get_XYZ`, keeps planar
x and y bit-exact through save/load, and is accepted wherever a position is.
A non-sequential table of handles, such as an object-keyed sample, crosses the
binding boundary as its handles in ascending order. Lua `WEAKEN_ENEMY`
activation at a position switches on there (WHE-61); a boolean keeps the
owner-position path.

An empty object table passed to `Find_Best_Local_Threat_Center` warns and
returns nil (WHE-68, debug build). The nonfatal `script_warning` diagnostic
(EAWR-SCRIPT-0217) preserves continuation to later object updates; actual script
errors still fault the attachment. Installed-chunk contracts cover this first
empty sample followed by an IG-2000 request, and a renamed Krayt chunk attaches
and acts with the same effects and hashes on 1/2/4/8 workers. These are lifecycle
prerequisite witnesses, not retirement of the native consumers.

### Object-turn host boundary

The production host installs the provider when binding an engine, including a fresh live session,
a retained campaign battle and a rebuilt checkpoint owner. The due set is derived from restored
`ServiceRate`/`LastService`; it adds no continuation bytes. A failed bridge step is terminal for
both the world and provider: further steps, script saves and script hashes refuse continuation.

Both barrier publications and staged rows use the same unit-row projection. Turn proximity
queries use the post-movement collection boxes and the existing three-axis overlap filter
(FC-58/UFS-27); they read no fog. Ordinary squadron uses fan out to the actual ability holders,
with all current holders gated before any are changed (AB-15). Ion-team and droid-team
handlers remain unadmitted.
Replenishment allocates in the turn; sparse births join the ordered unit commit after the existing
indexed tracking inputs, before later impacts and commands, and first move the following tick.

Completed-world dynamic-read audit: `EvaluatePerception`, `Evaluate_In_Galactic_Context`,
`FindTarget` (including the reachable form), `Fire_Special_Weapon`, `Get_Build_Pad_Contents`,
`Get_Credits`, `Get_Tech_Level`, player-filtered `Find_Nearest` (raw fog), and the plan-only
`Withdraw_Units` fail with an object-turn
tripwire diagnostic before entering their completed-world readers. Static type/profile/player
configuration remains readable. The underlying tactical-world capability also guards every
dynamic accessor independently of API names; a rejected transitive read aborts the
provider before any effect commits. Position, health, damage rate, hardpoints, ability slots,
formation state and proximity queries use staged rows. A binding fault discards that instance's
entire turn output through the scheduler transaction. Engine ability kinds whose command
handlers have not been admitted at the turn seam report an unsupported-API diagnostic for
any module that calls them; no fixed module-name list hides that gap.

The SOS-15/19/16/17/11 scripts now request their effects at the in-tick seam rather than the
following command phase: Falcon invulnerability, Vader replenishment, Slave I bombs, Sundered
Heart weakening and the Interdictor shield respectively. Native DEFEND, self-destruct, cluster
bomb, point-defence and corrupt-systems ownership gates remain until their separate retirements
(SOS-44). The production witness pins the first activation at tick 1 instead of tick 2, and checks
1/2/4/8 plus reverse partitions, a checkpoint continued on a different worker count, deferred
non-ability commands and world-only replay of the recorded uses.
