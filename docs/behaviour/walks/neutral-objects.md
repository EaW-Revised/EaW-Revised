# Walk: space skirmish neutral and environmental objects

## Scope and evidence

This walk inventories stock space-skirmish map objects and records their ownership,
combat, capture, destruction and presentation contracts. Here “neutral map object”
means an object initially controlled by no lobby player. It does **not** imply
that its owner has the faction flag `Is_Neutral`. Coruscant's orange resource
containers belong to Hutts, a nonplayable, **nonneutral** player; the grey merchant
dock belongs to Neutral. This distinction explains why ordinary attacks can
reach the containers while an AI plan can choose a dock it cannot attack.

Sources are the **debug build**, **XML/TED data**, existing **owner observations**,
and explicitly **unverified** details. NO-E01..06 identify reused debug-build
interfaces; NO-E07..12 cover this walk's ownership and radar batch. Their private
source maps and raw text remain outside committed files.
NO-D01 is an independent effective-registry/variant and map-record audit of all
24 stock space-skirmish maps, with **zero unresolved type CRCs**. No retail assets
or screenshots are part of this document.

The following completed walks own their internals. This walk records their
interfaces, without repeating their call trees:

| Owner | Contract consumed here |
|---|---|
| [Skirmish setup](skirmish-setup.md), WSS-47/48/62; [skirmish start](../../skirmish-start.md) | Runtime player creation, TED-owner remapping and team relationships. |
| [Frame order](frame-order.md), WFO-08/12..19/25/26/31 | Fog-grid decay, object turns, authored behavior order, deferred creation/deletion and player AI placement. |
| [Capital combat](capital-combat.md), WCC-12/13/20/25/70..72; [targeting](../space-targeting.md), R-06..10 | Ordered versus automatic fire, suitability, projectile admission and ordinary death. |
| [Tactical AI](tactical-ai.md), WTA-01/08/29; [AI note](../foc-tactical-ai.md), GS-11/FH-20 | Goal kinds, evaluator admission, nearest-object masks and damage retaliation. |
| [Hazards](hazards.md), WHZ-01..33/40..53/70..72 | Environmental contact, capture-point service, neutral projectile relationships and environmental presentation. |
| [Build pads](build-pads.md), WBP-04/27..34 | Construction children, capture/ownership, mining income, selling, producer menus and neutral respawn. |
| [Area damage](area-damage.md), WAD-01/04..09/14..26 | Detonation, immune factions, area recipients, routing and delayed damage. |
| [Sensors/UI](sensors-ui.md), WSU-01..08/13/16/17/21/50/63; [minimap](../foc-minimap.md), MM-06/07/12 | Sensor contact versus drawn models, selection, clicks, bars and owner-coloured radar. |

Comparison base is `8edca4e8771abc721397da3ab79de200e3c40e76`. Earlier walks'
implementation tables describe their own snapshots; their old “missing” verdicts
are not copied over capture, producer, respawn or fog features now on this base.
The pending resource-container admission fix is tracked separately (legacy EAWR-1728,
EAWR-1737); its proposed behavior is not treated as merged code.

## Complete starting-map census

All maps below place Neutral-owned environmental objects as well as the listed
structures. Counts describe **authored placements before skirmish remapping**.
Laser and mining pads start empty; completed satellites and mines are construction
results, not initially placed neutral fleets. The census finds no starting Pirate,
Hostile or Sarlacc combat fleet and no placed destroyable-asteroid variant. The
only Hutt-owned placements are Coruscant's eight resource containers. A station
with “Hutt” in its type name need not have a Hutt owner.

Legend: L = `Defense_Satellite_Laser_Pad`, M = `Mineral_Extractor_Pad`,
D = `Skirmish_Merchant_Dock`, A = `Skirmish_Hutt_Asteroid_Base`,
G = `N_Gravity_Well_Station`, S = `N_Remote_Sensor_Pod`,
P = `N_Orbital_Construction_Pod`, C = `Orbital_Resource_Container`.
Environment counts follow effective flags: fields, impassable solids, nebulas;
the storm count is a subset of nebulas. Field-bearing junk counts as a field.

| Map | Authored structures | Fields / solids / nebulas (storms) |
|---|---|---|
| Alderaan | L 4, M 8, G 1 | 27 / 4 / 4 (4) |
| Bespin | L 4, M 5, D 2 | 5 / 0 / 0 (0) |
| Bothawui | L 10, M 6, A 1 | 20 / 2 / 2 (0) |
| Coruscant | L 7, M 6, D 1, G 1, C 8 (Hutts) | 11 / 0 / 7 (2) |
| Dagobah | L 12, M 6 | 13 / 0 / 6 (4) |
| Dathomir | L 10, M 6, S 1 | 13 / 0 / 0 (0) |
| Endor | L 4, M 5 | 5 / 0 / 3 (3) |
| Felucia | L 6, M 6, A 1, S 1 | 8 / 0 / 1 (0) |
| Geonosis | M 5, D 1 | 15 / 3 / 1 (0) |
| Hoth | L 11, M 5, A 1, S 1 | 23 / 7 / 1 (0) |
| Hypori | L 10, M 4, A 1 | 11 / 0 / 0 (0) |
| Kamino | L 6, M 7, G 1 | 12 / 0 / 0 (0) |
| Kashyyyk | L 8, M 6 | 27 / 3 / 9 (5) |
| Kessel | L 5, M 4, D 1 | 7 / 0 / 0 (0) |
| Kuat | L 11, M 5, D 1, S 1 | 13 / 4 / 0 (0) |
| Naboo | L 6, M 5, A 1, S 1 (Rebel; removed) | 11 / 0 / 3 (3) |
| Polus | L 11, M 5 | 9 / 13 / 0 (0) |
| Ryloth | L 10, M 6, A 1, G 2 | 16 / 10 / 0 (0) |
| Saleucami | L 20, M 6, S 1 | 10 / 0 / 0 (0) |
| Shola | L 7, M 7, D 2 | 11 / 0 / 0 (0) |
| Tatooine | L 10, M 6, A 1 | 6 / 0 / 2 (0) |
| The Maw | L 4, M 5, S 2 | 7 / 0 / 0 (0) |
| Utapau | L 8, M 4, P 1 | 19 / 0 / 0 (0) |
| Yavin IV | L 4, M 6 | 6 / 0 / 0 (0) |

Naboo's sensor pod is authored to the **Rebel** editor owner, rather than Neutral;
the playable-faction map-object removal rule removes it before battle. There are
**351 authored capture-point placements, 350 retained**. Utapau does place the
capturable construction-pod variant; the five-map build-pad census did not cover
that map. Dathomir/Felucia/Hoth/Kuat/Saleucami/Maw add remote sensor pods to that
walk's narrower corpus.

Other placed families are backdrop/planet/moon props, idle lightning, ordinary
junk, dock/girder props, the Maw black-hole prop, the special-weapon source marker
and Geonosis's two ship-shaped death-clone props. Their names or shapes establish
neither an independent damage service nor salvage/capture. TED records that are
actual setup markers remain with skirmish setup. `SpaceProp` presentation is
separate from live combat admission.

## Data profiles and update entry points

| Stock object | Hull / shields | Capture radius / seconds / sticky | Reveal range / neutral respawn seconds | Living projectile contact / active weapon interface |
|---|---|---|---|---|
| Resource container | 20 / 0 | No capture behavior | 100 / no positive respawn authored | Yes / no gun or weapon hardpoints authored |
| Merchant dock | 600 / 500 | 800 / 15 / Yes | 850 / 80 | Yes / ordinary producer, no gun authored |
| Asteroid base | 700 / 300 | 800 / 20 / Yes | 1000 / 120 | Yes / nine inherited hardpoints, including shield generator and guns |
| Gravity station | 600 / no positive shields authored | 350 / 13 / Yes | 800 / 50 | Yes / no gun authored; interdiction is a reinforcement interface |
| Remote sensor pod | 600 / no positive shields authored | 350 / 10 / Yes | **18000** / 30 | Yes / no gun authored |
| Capturable construction pod | 10000 / 0 | 800 / 5 / Yes | 850 / no positive respawn authored | **No** / producer menu, no gun authored |
| Empty laser pad | 1200 / no positive shields authored | 350 / 8 / default No | 200 / 45 | **No** / built child supplies weapon |
| Empty mining pad | 1700 / no positive shields authored | 550 / 10 / default No | 275 / 38 | **No** / built child supplies income |
| Destroyable asteroid, small/medium/large/huge; not placed here | 125/250/500/1000 / no positive shields authored | No capture behavior | 700 on small; remaining values from each type / no positive respawn authored | Yes / no stock gun |

These are XML values, not code constants. Pads, construction pod, gravity station
and sensor pod explicitly disable `Influences_Capture_Point`; merchant and
asteroid base use the true default. Community selection is authored on the
merchant, asteroid base, gravity station, sensor pod and construction pod. It
does not let a neutral owner become an ally or enable a player's purchases before
capture. Merchant and asteroid-base shield refresh is 10; their energy
capacity/refresh pairs are 5000/1000 and 2000/1000. Absence of `TARGETING` or a
locomotor is not permission to invent aggression or movement.

Logical battle FPS is **30**. Commands enter before the object-manager traversal.
For each admitted object turn, pending delayed damage precedes enabled, due
behavior services; general `Behavior` entries attach before `SpaceBehavior`
entries, preserving each list's order. Capture has interval **4 logical frames**;
unit AI, shield/power/reveal and environmental handlers run at their owning
walk's service cadence. A neutral object has no additional global neutral-object
sweep. The resource container's general list is selectable, unit AI, idle,
obstacle, reveal, hide-when-fogged. Merchant/asteroid-base general capture precedes
their space obstacle, power, shield, hide, reveal, unit-AI and ion-state handlers.
The construction pod's general reveal/hide precedes capture; it also repeats
obstacle in its space list, whose duplicate-attachment behavior remains UFO-04.

Death can occur inside damage delivery, rather than at one universal end-frame
phase. Eligible respawns are created after existing object services and before
queued deletion; new objects from that tail begin ordinary service on a later
traversal. Player AI runs after mode/object work, in player order. Selection runs
on input and minimap rebuilding on rendered frames. These are WFO interfaces;
the order of sections below is not one replacement global frame loop.

## Rule list, ordered within each entry point

“Same” means the identified interface is present at the comparison base.
It does not claim that a missing resource-container profile can exercise it.
“Differs” and “missing” identify executable or data-path gaps; “unverified” does
not authorize implementing a guessed original rule.

### Loading and owner assignment

| ID | Rule and branch conditions | Source / existing-note comparison | Remake verdict and code |
|---|---|---|---|
| WNO-01 | Resolve effective registry winners, inherited variants and every placed type CRC before interpreting behaviors. Preserve TED position, facing and editor owner. Apply the all-map census above rather than admitting by model/name. | XML/TED NO-D01; same WHZ-01/WBP-01, extends their map list | **same**; `src/skirmish/inputs.cpp`, `src/skirmish/start.cpp`, selected-map unit loading |
| WNO-02 | Type `Affiliation`, category/property masks and behavior lists describe capabilities. Current TED/runtime owner is a separate input. Empire affiliation on a container does not turn its Hutt placement into an Empire object; the asteroid-base type name does not override its Neutral placement. | XML/TED NO-D01; same setup ownership interface | **same**; `start.cpp::add_map_objects`, resolved unit profiles |
| WNO-03 | After lobby players, create nonplayable factions requesting `Create_Player_In_Multiplayer_Games`, in faction order. Stock order is Rebel, Empire, Pirates, Neutral, Hostile, Sarlacc, Underworld, Hutts; only the five nonplayable entries add map-owner players. Editor Neutral index **3**, Hutts **7**, are not fixed runtime player IDs. | WSS-47/48; faction XML; same | **same stock interface**; `start.cpp::add_map_objects`; arbitrary lobby/team details remain WSS |
| WNO-04 | A playable-faction map object is removed. A nonplayable object transfers to its faction's real player; without one, only nondiscardable decoration falls back to Neutral, otherwise remove it. An editor index beyond the faction list resolves to Neutral. Naboo's Rebel sensor pod is the concrete removal case. | Setup ownership interface; NO-D01; same, new corpus case | **same**; `start.cpp::add_map_objects`, `removed` records |
| WNO-05 | Ordinary relationship checks treat either neutral side as nonhostile. Self/assigned allies remain allied; different unassigned nonneutral map players can be enemies. Hutts and Pirates author `Is_Neutral=No`; Neutral authors Yes. Lobby relationships, rather than differing faction names alone, decide skirmish enemies. | WHZ-51, WSS-48/62; faction XML; same | **same**; `src/sim/tactical/combat_targeting.cpp::players_hostile`, `players_hostile` |
| WNO-06 | Live map profiles retain health, armor, masks, projectile contact, reveal and authored capture/producer/weapon capabilities. A positive-health, living-collidable resource container is a damageable map object despite lacking capture behavior. | WHZ-50; XML NO-D01; differs from the earlier footprint-only scope | **differs G1**; `src/units/unit_tables.cpp::Loader::run` admits capture points, leaves this container as an obstacle; attack-admission work remains pending (legacy EAWR-1728, EAWR-1737) |

### Commands, automatic fire and AI queries

| ID | Rule and branch conditions | Source / existing-note comparison | Remake verdict and code |
|---|---|---|---|
| WNO-07 | An ordinary object attack checks the target owner's hostility before assigning it. Nonhostile returns failure; target dead/limbo/deleting and stealth gates remain ordinary orders/targeting interfaces. Hutt containers pass the owner test; a Neutral merchant does not. | debug build NO-E01; same WHZ-51 | **same**; `src/sim/tactical/session_step_commands.cpp`, `players_hostile`; firing still needs WNO-06 |
| WNO-08 | The attack interface has a separate forced-friendly option that bypasses this hostility rejection. It does not follow automatically from projectile collidability, a neutral type name or player attack mode. Remaining suitability/collision behavior of such forced attacks is unverified. | debug build NO-E01; same WHZ-51's explicit exception | **missing G5**; no corresponding replay/player command option; generic/mod boundary |
| WNO-09 | Ordinary automatic weapon search skips neutral and nonhostile players, then consumes type validity, living-projectile contact, visibility, weapon categories and priority-set exclusions. `NotOpportunityTarget` is a property interpreted by the firing unit's priority set; it is not a universal manual-attack prohibition. Exact-type priority entries and exclusion precedence remain R-09. | R-06..10; XML; same | **same interface**; `combat_targeting.cpp::service_opportunity`, `combat_algorithms.hpp::Opportunity`, loaded priority sets |
| WNO-10 | Container `Property_Flags=NotOpportunityTarget` excludes it from stock priority sets that exclude that property. Explicit object orders can still fire on it. Neither `Is_Valid_Target` nor living collision should be inferred from that property. There is no active stock XML/Lua occurrence of an extra opportunity-target tag in this corpus; the requested opportunity-candidate check remains the linked targeting interface, not another authored scalar. | XML scan NO-D01; R-09; NO-E01; same interfaces, concrete distinction missing in older notes | **same priority interface**, with WNO-06's missing live data |
| WNO-11 | `Find_Nearest` uses the complete per-player object list, excludes Neutral players, tests requested ally/nonally relation, category/property/type masks, living state and requesting-player fog, and chooses strict nearest 3D distance. It does not require `Has_Space_Evaluator` or an opportunity priority. Thus a Hutt Structure can beat an enemy station despite `NotOpportunityTarget`; Neutral scenery cannot. | debug build NO-E03; FH-20 interface; differs from its blanket AI-fog exception | **differs G6**; `src/script/foc/tactical_ai_bindings.cpp::find_nearest` bypasses raw fog for every AI requester; other neutral/mask/nearest gates agree |
| WNO-12 | AI goal object admission is separately driven by `Has_Space_Evaluator`. Stock merchant, capture pads, gravity/sensor stations, asteroid base and capturable pod author true. The container does not. The AI goal matcher then treats nonallied capture structures as enemy-structure candidates, including Neutral objects. | debug build NO-E02; WTA-08/GS-11; XML; same | **same**; `src/script/foc/ai_goals.cpp::target_matches`, AI type/evaluator loading |
| WNO-13 | For the AI goal matcher, authored initial/last fog-state visibility can bypass its ordinary target-fog rejection. This proposal admission is separate from nearest-query forced fog and weapon fog. It does not guarantee a later attack succeeds. | debug build NO-E02; sensors/AI interface; extends the dock case | **differs G6**; `ai_goals.cpp::target_matches` has no target-fog admission test or initial/last-state exception; evaluator enumeration alone does not supply it |
| WNO-14 | A bombing plan can therefore select a Neutral merchant and have its attack rejected by WNO-07. This mismatch is original behavior, not evidence for removing every neutral goal candidate or relaxing ordinary hostility. Reuse the closed dock investigation. | debug build NO-E01/02; AI plan data; existing dock finding (legacy EAWR-1723) | **same interface**; `ai_goals.cpp`, `ai_taskforces.cpp`, session attack admission; no new AI-classification bug |
| WNO-15 | A stationary object fires only through actual weapon/targeting capabilities. Containers, merchants, sensor/gravity stations and empty pads have no stock gun; a captured asteroid base and completed satellites use ordinary weapons with their current owner. No capture or `UNIT_AI` flag grants an unarmed object a gun. | XML NO-D01; WCC-20/WBP interfaces; same | **same interface**; loaded hardpoint/object weapons, `combat_fire.cpp`; inherited spawner/script behavior remains with station/Lua owners |
| WNO-16 | On damage, a unit with `UNIT_AI` can request non-direct retaliation through WTA-29: normalize craft attacker to its team, respect weapon/category/suitability/direct-order and formation-divert gates, replace no target or a worse-priority target. These gates apply to map structures too; neutral ownership does not introduce its own retaliation override. | WTA-29, WCC-25; same rule | **missing G3** for generic unit-AI retaliation; craft/squadron retaliation alone is not this interface (legacy EAWR-730) |
| WNO-17 | Ship-level suitability must reject a target whose type is invalid or cannot be hit by a living projectile, before scanning/retaining it; other WCC-25 gates remain that walk's responsibility. A captured empty mining pad is a concrete noncollidable case, despite hostile ownership and positive hull. | WCC-25, R-08; same; existing mining-pad findings | **differs G4**; `combat_targeting.cpp::UnitCombat::ship_suitable`, existing suitability gap (legacy EAWR-1618); weapon opportunity admission already rejects living-contact false |

### Capture, ownership and reclamation

| ID | Rule and branch conditions | Source / existing-note comparison | Remake verdict and code |
|---|---|---|---|
| WNO-18 | Only objects with capture behavior service capture, every **4 logical frames**, when enabled/due. Multiplayer has no opening delay. Single-player space uses `Space_Capture_Allowed_Countdown_Seconds=30`, with the neutral/no-owner exception. Containers and ordinary scenery are not made capturable by proximity. | WHZ-40/WBP-04; XML; same | **same stock interface**; `src/sim/tactical/pads.cpp`, session capture jobs |
| WNO-19 | Query raw `Capture_Point_Radius`; accept nonself, alive, not deleting/limbo, influential candidates with an eligible faction matching the point's `Affiliation`, at **3D distance squared <= radius squared**. Influence defaults true; stock explicitly false types cannot capture each other. | WHZ-41/WBP-02/04; same | **same**; capture profiles/influence loading, `service_capture` |
| WNO-20 | A Neutral point chooses the first eligible owner; additional allies retain that choice, a nonally contests to Neutral and ends the scan. An owned point plus hostile presence also targets Neutral. There is no majority vote and no direct enemy-to-enemy transfer in one service. | WHZ-42/WBP-04; same | **same**; `pads.cpp::service_capture` |
| WNO-21 | With no eligible influence, sticky/reinforcement points retain their owner; otherwise request Neutral. Empty laser/mining pads use the false sticky default; merchant, asteroid base, gravity/sensor station and construction pod author sticky true. | WHZ-43/WBP-02/04; NO-D01; same, extends type list | **same**; capture profiles, `service_capture` |
| WNO-22 | Same wanted/current owner rolls progress back by `4/(transition_seconds*FPS)` to zero. A different owner advances by `4/(transition_seconds*modifier*FPS)` to one; changing target preserves progress. Modifier is one plus applicable positive capture-time adjustments; nonpositive effective modifier falls back to one, zero modified duration advances immediately. Stock capture durations are in the profile table. | WHZ-44/45/WBP-04; same; zero-duration rollback remains their unverified boundary | **same stock interface**; `pads.cpp::step_of/service_capture`, capture-time modifier stage |
| WNO-23 | At completed progress, notify control/story consumers, change current owner and reset progress; no same-service second claim follows neutralization. Same-owner change is a no-op. A real transfer removes old control-group/selection membership, saves hull/shield fractions, migrates owner-list/collision membership, unregisters attached special-weapon interfaces and, on the normal player-list path, clears unit-AI threats before writing the new owner. Re-register, notify attached behaviors in stored order, then publish the owner-change signal. Restore fractions, hand an AI owner's object to its free-store interface, update colour/remembered ownership, then reset targeting and weapon-hardpoint targets. Population/team propagation follows through their existing interfaces. | debug build NO-E07/08; WHZ-46/WBP-04; same capture interface, generic ordering missing there | **same capture/current-owner interface**; `session_step_systems.cpp::commit_survivors`, `pad_captured`, current-owner economy/visibility consumers; bounded transfer differences are WNO-42/G7 |
| WNO-42 | Ownership change restores pre-transfer hull and shield **percentages** against post-notification maxima, then clears targeting/weapon-hardpoint attack targets. This differs from ordinary stat-bonus gain/loss adjustment. Old-owner selection/control-group removal and pre-write unit-AI threat cleanup are transfer interfaces, even when an existing target remains hostile to the new owner. Individual behavior callback effects remain their owning walks/UN-02. | debug build NO-E07/08; generic transfer boundary missing in the old capture notes; ordinary WHE-18/19 bonus rules remain unchanged | **differs G7**; staged capture writes `unit.state.owner` without combat cleanup; `src/sim/tactical/session_economy.cpp::apply_bonuses` adds/clamps maximum deltas instead of ownership-specific fraction restoration |
| WNO-24 | Capture/pad construction and ordinary production remain separate. A laser pad builds a weapon child, a mining pad builds an income child; captured merchants/construction pods and asteroid-base multiplayer menus supply ordinary produced units. The asteroid's surrounding field does not own or grant mining income. | WBP-03/05/22/33/34; XML NO-D01; same, extends all-map reach | **same interface**; `src/skirmish/economy.cpp`, pad children, ordinary producer queue; producer retention work already tracked (legacy EAWR-927) |
| WNO-25 | Selling a completed child clears its link before death, preserves the empty pad/owner and does not run killed-child reclamation. Later ordinary capture may neutralize a nonsticky empty pad. Destruction is separately WBP-27..29. | WBP-27..32; same | **same**; shared pad/economy sale and destruction paths |
| WNO-26 | Ordinary destruction of a nonclone with positive `Tactical_Respawn_Time_In_Secs` schedules the same type/position/facing at death frame plus nearest-rounded seconds*FPS. Capture points return to Neutral; generic noncapture types retain owner. Nonpositive means no schedule. Stock durations are 30/38/45/50/80/120 seconds as typed above; the container has none. | debug build NO-E05; WHZ-52/WBP-29; same, generic-owner branch explicit | **same**; `pads.cpp::respawn_after_death`, `session_step_economy.cpp`; no old capture progress or constructed child copied |
| WNO-27 | Due creation occurs on the first playing object-manager tail with current frame >= deadline, after existing object services and before deletion. A replacement starts ordinary service on a subsequent traversal. This does not add four frames to the respawn deadline. | WFO-25/26, WBP-29; same | **same interface**; ordered deferred respawn in `TacticalSession::step`; exceptional child/visual races remain build-pad captures |

### Damage, death and explosive payloads

| ID | Rule and branch conditions | Source / existing-note comparison | Remake verdict and code |
|---|---|---|---|
| WNO-28 | Ordinary projectile contact visits hostile owners and applies living/dead projectile collision and geometry. A Neutral scenery collision flag alone is not a projectile blocker. Armor, shields, energy, hardpoints and hull damage are ordinary WCC/WWP interfaces using current ownership. | WHZ-51, WWP-66, WCC; same | **same interface**; owner spatial trees, `projectiles.cpp`, shared damage service; container still needs G1 |
| WNO-29 | At ordinary death, the WCC-70/72 interface draws one declared `Death_Projectiles` entry with synchronized random choice, creates it at the dead object's height-adjusted target position and facing, preserves the **dead object's owner**, and associates the source and creation frame. The killer's player is not substituted as payload owner. No entries means no payload. | debug build NO-E04; same WCC death interface, owner detail missing there | **missing G2**; unit tables/durability carry no generic death-projectile list or spawn handoff |
| WNO-30 | Coruscant container `Death_Projectiles` resolves to `Proj_Construction_Pod_Detonation`: direct damage **0**, area damage **3000**, radius **400**, `Damage_Bomb`, flight distance/lifetime **0/0**, speed **6**, shield/hull damage Yes, energy No, no authored dropoff override, maximum area-distance delay **0**, per-delivery projectile delay **0.5 s**, immune faction **Rebel**. Container `Ranged_Target_Z_Adjust` is **30**. Its named huge death explosion is separate from this damaging projectile. | XML NO-D01; NO-E04; WAD-01/04/08/09/26 interfaces; concrete payload missing in earlier notes | **missing G2**; existing blast loader/service can consume area/immunity data once this payload is admitted/spawned; positive delay remains WAD G8 (legacy EAWR-650) |
| WNO-31 | The payload's zero flight/lifetime hands terminal detonation to projectile service; generic projectile shutdown is not a second explosion. Immune-faction area mode visits every player then excludes Rebel-faction recipients, independently of allies/neutrality. It can therefore damage Empire/Underworld ships and other Hutt containers; Rebel allies/enemies share Rebel immunity. Living-projectile contact, radius geometry, armor and hardpoint routing still gate actual damage. | WAD-04/06/09/14..25; XML; same shared blast rules | **same blast interface**, unreachable through container death until G2; `src/sim/tactical/blast.cpp`; actual chain timings UN-01 |
| WNO-32 | Positive `Projectile_Damage_Delay_Secs` replaces area-distance delay with synchronized uniform delay **[0.25*value,value] per delivery**: here **[0.125,0.5] s**. It is not one fixed half-second pause for the whole explosion. Delayed delivery must preserve the damage input after the source/container disappears. Exact retained-source policy remains area-damage U-04. | WAD-26, XML; same rule, stock neutral-payload applicability extends that walk | **missing G2**; current projectile/blast profiles have no positive-delay plumbing; reuse combat tag coverage rather than changing area formulas |
| WNO-33 | `Death_Explosions` alone is not a proven gameplay blast. Merchant/asteroid-base/gravity/sensor deaths name visual explosion types, with no container-like `Death_Projectiles`. The construction-pod variant unusually names the damaging projectile in `Death_Explosions`; whether that creation route services a synchronized payload is UN-03. | XML NO-D01; BP-14, WCC-70; same visual interface, exception unverified | **unverified** for Utapau pod gameplay blast; do not implement it by treating every death-particle type as damage |
| WNO-34 | Neutral map objects are not automatically contenders or victory-relevant stations. Container/merchant/asteroid base/gravity/sensor/pod data all author `Victory_Relevant=No`; nonplayable map-owner factions do not become a winning lobby player merely because an object has a base/category interface. | XML; VT-03/07 and setup interface; same | **same**; `src/sim/tactical/victory.cpp`, skirmish contender registration |

### Sensors, selection and radar

| ID | Rule and branch conditions | Source / existing-note comparison | Remake verdict and code |
|---|---|---|---|
| WNO-35 | A `REVEAL` object supplies its current owner's allied players with its typed `Space_FOW_Reveal_Range`; it does not reveal to every lobby player because it started neutral. Capture changes which team receives that source. Raw fog contact stays independent of a visible ghost or environmental prop. | V-01/10..19, WSU-01; XML NO-D01; same | **same interface**; `src/sim/tactical/fog_cells.cpp`, owner-bound reveal sources; profile reach depends on G1 for containers |
| WNO-36 | Hide-when-fogged samples local fog every **30 logical frames**, faster while fading; drawn model and radar fade use WSU-03/04/07 gates. Initial/last-state ghosts do not grant combat visibility or selectable live targets. Asteroid base and capturable pod author initial+last state true; gravity/sensor stations last true/initial false. | WSU-01..08; XML; same; ghost entry/removal capture remains their boundary | **same stock presentation interface**; loaded flags, `apps/viewer/src/live_session_frame.cpp`, fog-model support |
| WNO-37 | On selection input, require selectable state and reject `Not_Really_Selectable`; accept own objects or, in multiplayer tactical play, an **allied** object's `Is_Community_Property`. Neutral is not allied. Captured merchant/base/pod can be selected by teammates without changing the owner's identity. | debug build NO-E06; WSU-16; same | **same**; `apps/viewer/src/battle_input.cpp`, loaded selection/community state |
| WNO-38 | Right-click attacks a visible, living, nonstealthed enemy whose type admits projectiles; otherwise it becomes positional movement. Neutral merchant does not become an attack cursor simply because it has health. Noncollidable empty pads and construction pod retain their capture/build interactions rather than ordinary projectile targeting. | WSU-13/17/21, WHZ-51; same | **same interface**; battle pick/cursor and command routing; ship-level retention still G4 |
| WNO-39 | Suppressing Neutral-owner health/shield bars, reticles and selection is **owner presentation policy**, WSU-63. FoC bar admission itself has no blanket neutral-owner exclusion. Apply current owner after capture; this policy does not suppress Hutt containers as Neutral-faction objects. | WSU-50/63; owner decision; same, explicit policy distinction | **same project policy**; `battle_input.cpp`, current neutral metadata; never cite suppression as a FoC damage rule |
| WNO-40 | Ordinary radar colour is current player/faction colour: Neutral **(100,100,100,255)**, Hutts/Pirates **(255,128,0,255)**; lobby owners use their chosen colour. Optional enemy colour precedes community colour; selected colour follows, and capture/reinforcement handlers can override it last. Stock enemy recolour is No; selected recolour is Yes, **(209,255,209,255)**. Field/storm/nebula types are environmental layers, not ordinary blips. Icons/sizes remain MM-06 interfaces. | debug build NO-E09; faction XML, MM-05/06/07/12, WHZ-70; same ordinary colour, overrides missing there | **same ordinary owner/selected interface**; `src/presentation/ui/minimap.cpp`, `apps/viewer/src/map_mode_hud.cpp`; community/capture exceptions are WNO-43 |
| WNO-41 | Live radar rejects out-of-playable-bounds noncapital-layer objects, limbo, radar-disabled types, enemies lacking enemy-radar permission or jammed/stealthed, hazards and dead objects. When enemy display is disabled, capture behavior bypasses that particular enemy-display rejection. `Visible_On_Radar_When_Fogged=True` bypasses hidden-model and radar-fade rejection, independent of Neutral ownership; replay also bypasses those two presentation checks. Enemy nebula exclusion still applies. Ordinary live icons/blips later require local visibility or active interdiction; obstacle/background routing is separate. The flag is therefore not unconditional live visibility. | debug build NO-E09; XML asteroid-base flag; supplements MM-12's earlier blanket fog wording | **typed admission applied**; `src/presentation/ui/minimap.cpp`, `apps/viewer/src/map_mode_hud.cpp`; jam/stealth/interdiction contracts use copied inputs pending published viewer states; stock runtime branch witness stays UN-04 |
| WNO-43 | A capture-point radar colour override first checks forced local raw fog: fogged means Neutral grey, regardless of live owner/progress. Otherwise use current-owner colour when there is no different pending owner, or linearly interpolate old/new colours by capture progress. In multiplayer, nonneutral community property uses local player's colour for its own team, another team's representative colour otherwise; Neutral capture endpoints retain their faction colour; nonneutral unassigned community colour falls back to nonzero type then faction no-colourisation data. Capture overrides selected recolouring. | debug build NO-E09/10/12; same WBP-37 progress interface, supplements MM-07 | **capture radar override applied**; `map_mode_hud.cpp`, `src/presentation/ui/minimap.cpp`; world material tint remains a separate consumer |
| WNO-44 | After live radar objects, submit remembered fog models only when previously revealed and a retained model exists. Use stored type/owner and model transform within playable bounds, enemy-radar permission/enemy-display gating and exclude field/storm/nebula types. This path does not repeat the ordinary live radar-type visibility switch. Capture-point memories use Neutral faction colour regardless of stored owner; other multiplayer memories use stored player's colour. Remembered icon admission is separate from live fogged-radar flag/local-visibility gates and never creates a selectable target. | debug build NO-E11; same WSU remembered-model interface, supplements MM-12 | **separate radar memory applied**; `live_session_frame.cpp` retains observed identities/transforms, `map_mode_hud.cpp` hands them to `minimap_blips`; selection and attack cursor still read live visibility |

## Gaps, ownership and acceptance

Tracking issue: neutral/environmental object walk (legacy EAWR-1773). This snapshot
contains **44 rule rows: 30 same, 7 differs, 6 missing, 1 unverified**; the
43 sourced rows include inherited interfaces. Eight connected gap scopes reuse
three existing issues and add five focused sub-issues.

The first M2 priorities are resource-container live admission, death-payload
creation, positive-delay delivery, generic unit-AI return fire, and ship-level
noncollidable-target rejection. The first is already being fixed; the middle two
form one connected death-payload scope. Preserve the original merchant AI stall
unless a separate owner policy deliberately changes it.

| Gap | Rules / size | Exact work and observable acceptance | Existing ownership |
|---|---|---|---|
| G1 Resource-container combat profile | WNO-06; **S/M**, M2 bug | Admit the eight Hutt containers with authored hull, armor, masks, collision and reveal; keep stationary behavior, manual attacks and automatic exclusions. Installed Coruscant contract must observe shots, hits and damage, with 1/2/4/8-worker equality. | Existing attack/firing gap (legacy EAWR-1728), pending data-path fix (legacy EAWR-1737); no duplicate issue |
| G2 Container death-payload handoff and delay | WNO-29/30/32; **M/L**, M2 bug | Carry/resolve typed death-projectile entries, spawn exactly once with dead owner's identity and source transform, then use shared flight/blast services. Wire positive delay through the existing damage scheduler without guessing retained-source semantics. Pin empty/list-choice cases, source death, Rebel immunity including allied cases, non-Rebel damage, nearby Hutt-container chain, range boundaries and per-delivery delay draws. Preserve visual explosion and no duplicate blast. | Focused death-payload gap (legacy EAWR-1774); shared combat-tag/delay ownership (legacy EAWR-650), WAD G8; UN-01 and area U-04 provide runtime witness |
| G3 Generic unit-AI retaliation | WNO-16; **S/M**, M2 behavior gap | Reuse WTA-29 for captured armed structures and units; no retaliation gun on an unarmed container. Pin direct-order, formation, suitability and priority gates, distinguish craft/container notification. | Existing unit-AI retaliation (legacy EAWR-730); no duplicate issue |
| G4 Ship-level unsuitable neutral-derived targets | WNO-17; **S**, M2 bug | Apply WCC-25 before retaining/scanning a captured noncollidable mining pad; preserve ordinary weapon rejection and later valid candidate selection. | Existing suitability gap (legacy EAWR-1618); no duplicate issue |
| G6 Forced fog at nearest/goal boundaries | WNO-11/13; **S/M**, M2 bug | Nearest queries with a player filter must use that player's raw object fog even for AI. Object-goal admission may bypass forced fog only for the authored initial/last-state exception. Preserve Neutral-player skipping, masks and the original merchant nonally classification. Pin human/AI visible versus hidden Hutt containers and enemy structures, with/without the persistent-state flags; leave unforced AI metrics alone. | Forced-fog interface bug (legacy EAWR-1776); debug build NO-E02/03 and V-09/AU-41; correct FH-20's blanket AI-fog wording |
| G5 Forced-friendly attack option | WNO-08; **M**, nice-to-have | If generic/mod attack APIs are extended, carry an explicit force option rather than weakening normal neutral relationships. Verify its downstream projectile/damage behavior first; distinguish acceptance from a shot actually damaging a nonhostile recipient. | Bounded API/evidence gap (legacy EAWR-1775); UN-05; no stock neutral-dock workaround |
| G7 Ownership-transfer cleanup and fractions | WNO-23/42; **M**, M2 bug | Preserve pre-transfer hull/shield fractions against new maxima and clear threat/object/hardpoint targets at the ownership boundary. Reconcile old-owner selection/control groups and owner-index consumers. Keep ordinary bonus arithmetic unchanged. Pin wounded/boosted structure neutralization and claim, same-owner no-op, target still hostile after transfer and four-frame completion; 1/2/4/8-worker equality. | Focused transfer gap (legacy EAWR-1779); broader capture/profile interface (legacy EAWR-927); consumer-specific callbacks stay UN-02/USL-05 |
| G8 Fogged and remembered radar structures | WNO-41/43/44; **M**, M2 bug | Carry the typed fogged-radar exceptions without weakening other live gates; feed capture raw-fog/progress/community colour and separately submit previously revealed retained fog models with Neutral capture-point colour. Pin stock asteroid-base, merchant/sensor controls, team colours, transition/destruction and no ghost selection. | Focused radar gap (legacy EAWR-1780); presentation coverage (legacy EAWR-653); existing mark-size (legacy EAWR-1369) and world-tint (legacy EAWR-1495) have different scope; UN-04 narrows remaining runtime branch witness |

Capture, construction, income, producer queues and neutral respawn reuse the
build-pad/station ownership above. The earlier merchant profile issue remains
open (legacy EAWR-927), but it is not evidence that all current merchant capture and
ordinary menus are missing. Generic per-object Lua is separately tracked
(legacy EAWR-1711); no new station-script rule is inferred from a type name.
Nearest/object-goal interfaces explicitly request fog checking, including for AI.
The older FH-20 sentence saying AI sees everything differs here: V-09/AU-41
settles that the no-AI-fog preference relaxes only **unforced** queries, while
NO-E02/03 call the forced interface. Keep ordinary planning-force metrics and
weapon fog as separate consumers.

The special radar flag and remembered/capture colouring are now debug-build
rules, with runtime drawing-branch observation still bounded by UN-04. The unusual
construction-pod explosion route remains unverified evidence work.

## XML tags and registry snapshot

This docs-only walk changes no tag application or shared registry row. Actual
types consumed here span `SpaceStructure` (the container), `SpaceBuildable`
(pads/pod), `SecondaryStructure` (merchant/base/gravity/sensor), `SpaceProp`,
`Faction` and `Projectile`. Status is per class: “loaded elsewhere” does not
upgrade the container's row to applied.

| Consumed input / class | Status at base | Rule / consequence |
|---|---|---|
| `SpaceStructure/Death_Projectiles` | **todo**, combat tag coverage (legacy EAWR-650) | WNO-29/30; new death-payload handoff |
| `SpaceStructure/CategoryMask`, `Property_Flags` | **todo**, combat tag coverage (legacy EAWR-650) | WNO-06/09/10; live container admission must preserve actual Structure/NotOpportunityTarget values |
| `SecondaryStructure/Property_Flags` | **todo**, combat tag coverage (legacy EAWR-650) | Priority interface; not every category/property declaration has a demonstrated live consumer |
| `SpaceStructure/Collidable_By_Projectile_Living` | **todo**, movement/contact coverage (legacy EAWR-649) | WNO-06/28; G1 needs the living collision profile |
| `SecondaryStructure`, `SpaceBuildable` and ordinary combat classes / `Collidable_By_Projectile_Living` | **partial**, movement/contact coverage (legacy EAWR-649) | WNO-17/28; per-type contact/application coverage differs |
| `SpaceStructure/Has_Space_Evaluator` | **todo**, AI tag coverage (legacy EAWR-652) | WNO-11/12; omission on stock container is false/default admission, not a nearest-query exclusion |
| `SpaceStructure/Space_FOW_Reveal_Range` | **todo**, presentation tag coverage (legacy EAWR-653) | WNO-06/35; container's 100 range is not supplied by footprint alone |
| `Visible_On_Radar_When_Fogged` / SpaceProp, SpaceStructure, SpaceUnit, StarBase, UniqueUnit | **todo**, presentation tag coverage (legacy EAWR-653) | WNO-41/G8; sourced early-gate exceptions, separate final live admission |
| `Projectile/Projectile_Damage_Delay_Secs` | **todo**, combat tag coverage (legacy EAWR-650) | WNO-32; payload authors 0.5, ordinary zero-delay blast support is insufficient |
| `Is_Valid_Target` / Marker, SpaceProp, SpaceUnit, UniqueUnit | **partial**, combat tag coverage (legacy EAWR-650) | WNO-09/17; default and application must remain separate from the opportunity property |
| `No_Colorization_Color` / Faction, GenericHeroUnit, SpaceStructure, SpaceUnit, UniqueUnit | **todo**, presentation tag coverage (legacy EAWR-653) | WNO-43; unassigned/neutral community colour fallback |
| `Radar_Colorize_Multiplayer_Enemy`, `Radar_Multiplayer_Enemy_Color` / GameConstants | **todo**, presentation tag coverage (legacy EAWR-653) | WNO-40; optional enemy recolour, stock switch No |
| `Radar_Clip_To_Visible_Region` / SpaceUnit | **todo**, presentation tag coverage (legacy EAWR-653) | Radar obstacle/background interface; authored classes differ from the neutral capture structures |

Additional active inputs, already owned by the linked walks: registry ordering
and `Variant_Of_Existing_Type`; `Behavior`, `SpaceBehavior`, `Affiliation`,
`Is_Neutral`, `Is_Playable`, `Create_Player_In_Multiplayer_Games`, `Allies`,
`Enemies`, `Color`; `Tactical_Health`, `Shield_Points`, `Shield_Refresh_Rate`,
`Energy_Capacity`, `Energy_Refresh_Rate`, `Armor_Type`, `Shield_Armor_Type`,
`HardPoints`, `Damage_Type`; `Targeting_Priority_Set`, category/property/unit
exclusions, `Is_Valid_Target`; `Capture_Point_Radius`,
`Capture_Point_Transition_Time_Seconds`, `Capture_Point_Time_Multiplier`,
`Influences_Capture_Point`, `Ownership_Sticks`, `Is_Community_Property`,
`Tactical_Buildable_Objects_Multiplayer`, `Tactical_Buildable_Constructed`,
`Tactical_Respawn_Time_In_Secs`, `Victory_Relevant`;
`Collidable_By_Projectile_Dead`, `Death_Explosions`, `Death_Clone`,
`Ranged_Target_Z_Adjust`, `Remove_Upon_Death`; projectile flight/lifetime,
damage channels and all six `Projectile_Blast_Area_*` inputs; radar visibility,
icon/size/facing/color tags; `Radar_Colorize_Multiplayer_Enemy`,
`Radar_Multiplayer_Enemy_Color`, `Radar_Colorize_Selected_Units`,
`Radar_Selected_Units_Color`, `Radar_Clip_To_Visible_Region`,
`No_Colorization_Color`; initial/last fog-state
tags and `Multisample_FOW_Check`;
environment flags, space layer, footprint/radius/offset and scale.

The asteroid base's inherited spawn lists/delay and hardpoints are data interfaces
to station lifecycle, not proof of unconditional Neutral-owner fleet spawning.
`Damage` on that structure is not a tactical weapon merely because it is numeric.
`Reinforcement_Prevention_Radius`, `Is_Interdictor` and reveal-modifier fields
remain with reinforcement/ability interfaces. Capture menus use multiplayer
entries here; campaign lists, land damage alternates and galactic acquisition
requirements do not introduce additional M2 mechanics. Environmental damage,
storm shield suppression and nebula targeting remain WHZ/WAD interfaces.

## Unverified details and focused observations

| ID | What remains unknown | Observation that settles it |
|---|---|---|
| UN-01 | Exact container death-to-payload service frame, positive-delay delivery/source retention and recursive chain ordering | Coruscant, suspended AI, fog state stated: destroy one container with one controlled shot; stage isolated Rebel/Empire/Underworld craft and another container at 399/400/401 planar distance and varied height. Record death, payload birth/expiry, each damage frame and source/owner identity. Repeat chain case with both orderings. Reuse area-damage U-04 for delayed-source retention. |
| UN-02 | Consumer-specific owner-change callback effects on reveal registration, hangars, queued purchases and producer permissions; generic wrapper order/fraction/target cleanup is now sourced WNO-23/42 **Sweep:** Still unverified: Generic owner-change cleanup does not enumerate reveal, hangar, queue and producer callbacks. The spawner selector/detach receipt resolves neither all owner callbacks nor producer permission reconstruction. | Capture a Neutral merchant then an armed asteroid base with own/allied/enemy observers; inspect before/after ownership, sensor cells, queued menu entries and spawned-owner IDs on the four-frame completion boundary. Reuse station USL-05; do not infer each virtual callback's internals from the wrapper. Retained sweep boundary: EUS-35. |
| UN-03 | Utapau construction pod's projectile-valued `Death_Explosions` creation route **Sweep:** Still unverified: The requested death-explosion creator entry point was not resolved by the targeted query. Ordinary type creation does not settle a projectile-valued death payload’s source/aim/owner parameters. | Private staged lethal damage to the noncollidable pod, then observe whether a synchronized projectile is born and damages non-Rebel recipients. Compare a visual-only explosion type and a `Death_Projectiles` container. This does not authorize making the pod an ordinary attack target. Retained sweep boundary: EUS-37. |
| UN-04 | Which sourced live-versus-remembered radar branch draws the stock Neutral/captured asteroid base during hide/fade transitions; exact runtime overlap count **Sweep:** Live radar admission uses scene-admission visibility, not raw fog or the model-hidden flag. The fogged-radar tag bypasses early model-hidden/radar-fade gates; it still needs scene admission or interdiction. A revealed memory also submits through its separate route, using Neutral colour for capture points. Stock asteroid-base initial/last-state flags establish memory eligibility. Still unverified: Final scene-admission visibility getter is traced; the exact hide/reveal live-versus-memory overlap count still requires observer-frame captures. | Fog-on Bothawui/Hoth: before discovery, after discovery, after capture, after leaving sensor range and after destruction. Record world model, ghost, radar blip, owner colour and attack cursor separately. Repeat remote sensor/merchant and a Hutt container control. Check WNO-41's final local-visibility gate and WNO-44's revealed-memory path rather than treating the flag as unconditional visibility. Retained sweep boundary: EUS-03. |
| UN-05 | Forced-friendly attack downstream contact/damage permissions and public API reachability **Sweep:** Still unverified: Stored hostility is settled, but a forced-friendly attack bypasses a different admission interface. Public wrapper reachability and downstream projectile contact/damage gates need the forced-order caller; do not infer friendly damage from enemy filtering. | Trace or stage the explicit force option against allied and Neutral collidable structures, plus noncollidable pad. Record command result, projectile creation/contact and damage independently. A bypassed order gate alone proves neither friendly projectile contact nor area-damage admission. Retained sweep boundary: EUS-28. |

Existing build-pad direct-parent-death/child-cleanup and due-frame visibility
observations remain U-BP-5; asteroid-base generic hangar ownership remains USL-05;
station/neutral ghost presentation retains the sensor walk's bounded questions.
None is expanded into a second walk of those internals. No builds or GPU suites
are needed for this documentation-only change; validation is clean-room,
public-reference and whitespace checking.
