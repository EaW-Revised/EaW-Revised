# Walk: skirmish space station lifecycle

## Scope and evidence

This walk covers the Empire and Rebel `Skirmish_*_Star_Base_1` through
`Skirmish_*_Star_Base_5` on Coruscant: inherited data, creation, hangar work,
player free garrisons, replacement, hardpoint repair and destruction. It audits
main at `6957fa608db84adee459ea2377f99d6b0849a8fb`. Pending garrison fixes are
listed separately from behavior present at that head.

Sources are **debug build**, **XML data**, **existing verified interface**, and
**unverified**. EWSL-01..15 identify retained read-only headless evidence;
EWSL-16 identifies the repair, replacement and countdown call-tree batch;
EWSL-17 identifies the ability initialization/deadline batch and retained integer-range evidence;
EWSL-18 identifies the behavior registration and station-role interface lookup. Their
private map and raw results stay outside the committed tree. Existing walks
retain ownership of their internals: [setup](skirmish-setup.md),
[production](production.md), [reinforcements](reinforcements.md),
[squadrons](squadrons.md), [capital combat](capital-combat.md),
[frame order](frame-order.md), [battle flow](battle-flow.md),
[sensors and UI](sensors-ui.md), and [tactical AI](tactical-ai.md).
This document records the station-facing interfaces instead of repeating those walks.

There are two different garrisons. The station inherits authored starting/reserve
lists, but marker-created multiplayer stations disable that route. The free
starting companies belong to the **player**, use the faction roster, and return
through an allied station only after the player's whole registered garrison is
depleted. More station levels do not grant more free starting companies.

## Concrete data cases

The skirmish variant of each level names the same-faction campaign station in
`Variant_Of_Existing_Type`. It replaces `HardPoints`, `Abilities`, the tactical
build menu and the space behavior list; inherited lists do not thereby become
enabled. The variant authors `Is_Community_Property=Yes`, `Next_Level_Base` and
`Prev_Level_Base`. Level 1 has no previous base; level 5 has no next base.

These are **raw XML values**, before difficulty and automatic bonuses. Each
level has its own `RB_STATION_0N.ALO` or `EB_STATION_0N.ALO` model.

| Level, both factions | `Tactical_Health` | `Shield_Points` | `Shield_Refresh_Rate` | Hardpoint slots | Next upgrade price / seconds |
|---|---:|---:|---:|---:|---|
| 1 | 1600 | 300 | 60 | 7 | 2000 / 80 |
| 2 | 2200 | 750 | 100 | 10 | 3200 / 100 |
| 3 | 3000 | 2000 | 100 | 13 | 4000 / 120 |
| 4 | 5000 | 4500 | 160 | 16 | 5500 / 140 |
| 5 | 7000 | 9000 | 160 | 20 | none |

Prices are `Tactical_Build_Cost_Multiplayer`; durations are
`Tactical_Build_Time_Seconds`, before WPR-33's modifiers. Level-up types use
`Tactical_Production_Queue=Tactical_Units`, not the research queue.

All ten stations inherit `Spawned_Squadron_Delay_Seconds=10`. The following
column-0 lists are **authored garrisons, disabled on marker-created skirmish
stations**, not the actual free starting fleet. Matching reserve rows are -1.

| Level | Rebel authored starting counts | Empire authored starting counts |
|---|---|---|
| 1 | X-wing 2, Y-wing 2 | TIE fighter 2, TIE bomber 2 |
| 2 | X-wing 2, Y-wing 2, Corellian corvette 1 | TIE fighter 2, TIE bomber 2, Tartan 1 |
| 3 | X-wing 3, Y-wing 3, Corellian corvette 1 | TIE interceptor 3, TIE bomber 3, Tartan 1 |
| 4 | X-wing 6, Y-wing 4, Corellian corvette 2, Nebulon-B 1 | TIE interceptor 3, TIE bomber 3, Tartan 2, Acclamator 1 |
| 5 | X-wing 10, Y-wing 6, Corellian corvette 2, Nebulon-B 2 | TIE interceptor 3, TIE bomber 3, Tartan 2, Acclamator 2 |

`Space_Skirmish_AI_Default_Forces` instead lists two
`Rebel_X-Wing_Squadron` entries for Rebel and two `TIE_Interceptor_Squadron`
entries for Empire. This list also supplies a human player's selected free
forces. Both factions author `Garrison_Reinforcement_Delay_Seconds=20.0`.
The free-force option and default roster selection are WSS-52/53 and WBF-07.

The Rebel shield generator at levels 1–3 has raw `Health=650`,
`Collision_Mesh=HP01_SHG_COLL`, attachment `HP01_SHG_BONE`, and attached model
`RB_Station_01_HP01_SHG.alo`. Levels 4–5 substitute the level-4 generator
(`Health=950`, `HP04_SHG_COLL`). The original bay is retained at every level:
raw `Health=1000`, `Collision_Mesh=SPAWN_00`. These values differ from the
difficulty-scaled 975/1500 examples in HD-05; they describe different inputs,
not an XML health change. Shield aiming, projectile collision and missile
arrival stay the weapon/projectile interfaces; XML alone cannot settle the
reported missed shots at the lower spire.

## Evaluation order

An admitted stock logical frame is 1/30 second. There is no independent global
station update: the object manager traverses its registered service list
(WFO-11/12). General `Behavior` precedes `SpaceBehavior`; enabled periodic
services run only when due (WFO-15/16). The skirmish space list places power and
shield work before fog hiding/revealing, unit AI, then squadron spawning and
ion-stun work. General behavior can supply additional earlier services.
Object abilities/Lua and hardpoint services follow the periodic prefix
(WFO-20..23). Destruction can happen inside another object's earlier projectile
service; it does not wait for this station's next hangar update.

After objects and deletion, income runs; after mode service, players run in
ascending player ID. Each player services AI, then its production queue, then
its free-garrison timer. Thus a timer that expires during the player pass cannot
feed a station hangar already serviced in that frame. Upgrade construction and
the replacement ability are also separate entry points. The rule groups below
follow this order and identify event-driven work explicitly.

### Loading and battle creation

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-01 | Resolve `Variant_Of_Existing_Type` before consuming station data. The skirmish child overrides its own hardpoint/ability/menu lists while retaining unauthored base properties. The model, hull, shields, spawn lists and delay are per type, not a level formula. | XML data; same mod-loading inheritance interface and WPR-50 |
| WSL-02 | Each team station marker is replaced once; the team's first runtime player owns it. Teammates have independent player records and spawn markers. Station count is independent of teammate count. | verified interface WSS-61; same |
| WSL-03 | `Is_Community_Property` enables allied station interaction; production tests the buyer's faction against the allied producer's menu. The station's owner/color does not become the teammate making a purchase. | XML data; WPR-33 and WSU selection interface; same |
| WSL-04 | Generic objects with squadron-spawning service start with authored-garrison spawning enabled. Marker-created multiplayer skirmish stations explicitly disable it. This is runtime state, distinct from having `SPAWN_SQUADRON` and nonempty XML lists. | debug build EWSL-01..03; missing from older FL-02/11 and WSS-61 |
| WSL-05 | Free starting-force templates are copied from the faction's space default-force list when selected. Placement creates companies independently of the station. Actual free craft/objects register in the player's garrison list; team containers are not the depletion counter. | debug build EWSL-06/08/09/10; same starting-placement interface, registration missing there |
| WSL-06 | Creation initializes the new station from its own type and current owner modifiers: model, `Tactical_Health`, shields, hardpoints and abilities. Raw values above are not difficulty-adjusted maxima. | XML data; WSS difficulty and WCC durability initialization interfaces; same |
| WSL-07 | Its periodic services attach in authored order and use their own due frames. Hangar first service has a synchronized 0..29-frame offset and repeats every 30 frames (code). | verified FL-01 and WFO-15/16; same |
| WSL-08 | Power/shield recharge is its normal station service, with `Shield_Points`, `Shield_Refresh_Rate` and the combat shield rules. The last lost shield-generator hardpoint disables shields; a type without generators has the separate no-generator branch. | verified DG shield and HD-12/WCC-61 interfaces; same |
| WSL-09 | Station fog snapshots, reveal, radar visibility, selection and command palette consume the current station state. The comm array controls its authored allied radar ability; the supply dock controls its +20 income modifier. | XML data; WSU radar/ghost and WPR-11 interfaces; same |

### Due hangar service

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-10 | The due service initializes empty authored spawn bookkeeping before handling its state. `Spawn_Garrison_On_Load` can request an instant-load route; that routine excludes multiplayer tactical battles. Idle is the normal stock space state; opening/deploying/closing animation states are the land interface. | debug build EWSL-06/07; FL-01/02 omit the instant-load exclusion |
| WSL-11 | At setup read the owner's tech, valid columns 0..5. Starting type/count lookup searches downward for the first nonempty starting column. Reserve lookup independently searches downward for the first nonempty reserve column, then matches the selected type; absent match/list gives 0. Finite total launches are starting count plus reserve; negative reserve is unlimited. | debug build EWSL-05/07; same FL-02/12, adds independent fallback and six-column bound |
| WSL-12 | Authored entries are considered only when the per-object spawning flag is enabled. A cap is simultaneous live companies, not surviving craft; a positive remaining budget decrements only after a successful launch, negative remains unlimited. These counters do not govern free player replacements. | debug build EWSL-05; differs from FL-11's former stock-station one-shot assumption |
| WSL-13 | Spawn eligibility requires idle state, a due launch deadline, no suspended locomotor/hyperspace and no active `DEFEND`. A prebattle space cinematic blocks spawning unless `Spawn_Garrison_On_Load` allows it. Campaign bribery/restricted-type checks are separate conflict-location branches, not ordinary Coruscant skirmish rules. | debug build EWSL-05; same FL-03/09/12; cinematic/campaign branches missing there |
| WSL-14 | Collect fighter-bay hardpoints. In space, remove a destroyed destroyable bay from the usable list; a nondestroyable bay remains. With none left, neither authored nor pending-player units can launch. The traced eligibility test checks destruction, not the bay's disabled flag. | debug build EWSL-05/07; same HD-13/FL-03; disabled distinction missing there |
| WSL-15 | For enabled authored spawning, test entries from a synchronized random starting index, cyclically. Selection performs its own random-start scan and skips full/exhausted/restricted entries. If no authored entry succeeds, consider the separate pending-player route. | debug build EWSL-05; same FL-04, adds eligibility/selection separation |
| WSL-16 | A successful launch chooses a usable bay at random, reads its world attachment point and `Fighter_Bay_Flyout_Distance` vector, creates `Squadron_Units` craft in roster order, then the container. A type with no squadron members uses the single-ship route. Movement/formation and targeting after launch are handed to the squadron/movement walks. | debug build EWSL-13/15; same FL-06/07 |
| WSL-17 | Launched craft and container are garrison units outside purchased tactical population accounting. Pending-player births additionally register the actual objects with that player; ordinary authored carrier launches do not join that free-force registry. Station launches move toward the flyout endpoint; mobile carriers use escort movement. | debug build EWSL-13/15; same FL-07/12, registration missing there |
| WSL-18 | After an eligible automatic space spawn attempt, set the next deadline to trunc(current frame + logical FPS × `Spawned_Squadron_Delay_Seconds`). This happens even when the pending-route selector returns false. Stock station delay is 10 seconds = 300 frames; next servicing quantizes admission to the 30-frame hangar cadence. | debug build EWSL-07; same FL-05, clarifies pending-route return |
| WSL-19 | Authored spawned-company detachment matches its type to the authored entry and decrements its live-company count. If every examined entry through the matched entry was full before removal, restart the full launch delay from that frame; otherwise keep the running deadline. | verified FL-08, WP-25; same |
| WSL-20 | Losing some craft keeps the authored company's live slot occupied until its final member/container is gone. No craft-by-craft refill is requested by this counter. Squad roster compaction and last-member death are the squadron interface. | verified FL-08 and WSQ roster/death interface; same |
| WSL-21 | Owner-change notification changes the hangar's next-spawn player index to the new owner's index. It does not itself rebuild entries, replenish reserves or change already launched squadron owners. Full ownership propagation is unverified and not an ordinary stock station capture mechanic. | debug build EWSL-06; missing there; interface only |

### Player free garrison and station handoff

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-22 | Initial placement and free replacement register the actual objects in the player's owned-garrison list. Detachment removes an object; only successful removal followed by an empty list invokes depletion. An unrelated carrier squadron loss cannot empty this registry. | debug build EWSL-08..11; missing from FL-02/08 |
| WSL-23 | Partial loss, even an entire first company while another registered company has survivors, starts no free-force timer. Full depletion calls the player timer route only in multiplayer tactical mode, with no timer already waiting and no pending replacements. | debug build EWSL-11/14; missing there |
| WSL-24 | Depletion initializes the countdown with faction `Garrison_Reinforcement_Delay_Seconds` (Rebel/Empire 20 seconds, 600 stock logical frames), sets waiting, and shows the local player's respawn indicator. This delay precedes hangar launch; it is not an authored reserve count. | debug build EWSL-14; XML data; missing there |
| WSL-25 | After that player's AI/build-queue service, an elapsed timer copies the stored starting-force templates into an empty pending list, retaining duplicates and order, then clears waiting. Space uses station launch; the separate land branch hands them to land reinforcements. It does not buy units or charge purchase credits. | debug build EWSL-12; WFO-31 timing interface; missing there |
| WSL-26 | `DUMMY_STAR_BASE` in `Behavior` or the active `SpaceBehavior` supplies the station role; `STARBASE` is not a registered behavior alias. Object kind alone does not grant this role. After authored eligibility fails, a multiplayer space object with that role scans runtime players in index order for an ally of the station owner with a nonempty pending list. The first qualifies; a generic carrier cannot service this fallback. Bay and deadline gates still apply. | debug build EWSL-05/18; stock XML data; missing there |
| WSL-27 | Spawn the first pending template for the selected **player**, preserving that player's ownership even through a teammate's station. Remove its pending entry before delegated space creation and register replacement craft/objects. This does not decrement the station's authored remaining budget or increment its authored live cap. Generic creation-failure recovery is unverified. | debug build EWSL-05/10/13/15; missing there |
| WSL-28 | Pending entries survive station replacement because the lists/timer belong to the player. Intact allied stations can service them; loss of all usable bays does not produce a ship elsewhere. A newly registered replacement participates in the next depletion cycle. Multi-station competition follows object servicing order, not a verified globally sorted station-ID rule. | debug build EWSL-04/05/11/12; WFO-12 interface; missing there |
| WSL-29 | While waiting, the local command bar creates its counter lazily, initializes progress at 0 and updates it from the timer's expired fraction. On timer expiry it hides the respawn indicator and plays faction `Reinforcements_Ready_SFXEvent`, before the station necessarily launches the first replacement. The indicator is distinct from each squadron's garrison flag. | debug build EWSL-12/14/16; missing from WSU garrison-flag interface |

### Upgrade completion and replacement

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-30 | Purchase/build admission uses the ordinary allied menu, positive cost/time, prerequisites, limits, queue and modifiers. Stock level-up costs/times are above. `Tech_Level` and `Required_Star_Base_Level` are not substitute tactical admission gates. | verified WPR-20/22/30/33/50; same |
| WSL-31 | Production completion creates the held level-up object. Its replacement handler has a **2-frame service interval** (code). An enabled periodic ability initializes its deadline with an inclusive synchronized draw from creation frame C through C+2. Its due/enabled gate services a deadline <= current frame. Queue completion has missed the current object traversal: draws C/C+1 therefore service at C+1, and draw C+2 services at C+2. That first eligible service attempts replacement once and removes the upgrade object. The remake keys the deadline draw by seed, creation frame and held-object ID, like FL-01's keyed scheduling policy. | debug build EWSL-16/17; verified WPR-22/52 and WFO-11/21/31; completion/service contracts |
| WSL-32 | Resolve the station itself or the upgrade's station container; read that station's `Next_Level_Base`. With none, no replacement is possible. Otherwise create the next type at the old station's owner, position and facing. New model, maxima, hardpoints and menus come from the replacement type. | debug build EWSL-04; same WPR-52 |
| WSL-33 | Transfer hardpoints **by index**, not bone/name/role. A repairing old slot transfers current health and every repairing player, disabled. Otherwise an old destroyed or disabled slot becomes disabled at **0.1 health** (code). An ordinary damaged but enabled slot is not assigned its old health by this transfer; added slots retain creation state. Special-weapon enabling follows the transferred disabled state. | debug build EWSL-16; verified WPR-52; same; adds ordinary-damage branch explicitly |
| WSL-34 | Transfer contained upgrades and every player's selection/control groups. Increase tech for the upgrade owner and every ally, capped by each player's maximum; redirect their existing queue references to the new station. This level-up ally test has no same-faction filter; research's tech-increment path is a different interface. | debug build EWSL-04; verified WPR-52/group transfer; same |
| WSL-35 | Copy the old authored-garrison enable flag to the replacement, then remove the old station without combat death. Free player templates/timer and living free squadrons are not recreated by leveling up. Thus inheriting larger campaign spawn lists does not grant their contents in skirmish. | debug build EWSL-04/05/12; missing from older WPR-52/56 |
| WSL-36 | There is no explicit old hull/shield percentage copy in the inspected replacement routine. Hardpoint transfer is the WSL-33 exception. Exact current/max hull/shield values after first cap/recharge/bonus service, and generic enabled-spawner finite reserves across replacement, require the focused capture below; do not present the remake's reserve reconciliation as retail fact. | debug build EWSL-04; same WPR-56's unverified distinction |
| WSL-37 | Local owner/ally/enemy upgrade announcements use faction `SFXEvent_Starbase_Upgraded`, `SFXEvent_Starbase_Ally_Upgraded` or `SFXEvent_Starbase_Enemy_Upgraded`. No kill credit, combat explosion or deciding station-death check is caused by this replacement. | debug build EWSL-04; same WPR-52; target/projectile reference transfer remains WPR-53 project policy |

### Hardpoints and repair

The live session now separates the completed level-up purchase from replacement:
`session_step_economy.cpp` services previously held level-up objects before the
late player queues. The queue-completion snapshot retains the old station type,
menu, tech and producer references; replacement later emits the existing
selection-transfer event and carries the allied queues. The retained initial
deadline gates that service, including both one-frame and two-frame waits. The
handler's repeat interval alone is not a fixed two-frame initial wait.
The station-upgrade contracts check completion/service separation for several
purchase durations and owners, destruction while held, a terminal replacement
attempt, and per-frame hash/snapshot equality at 1/2/4/8 workers. A replacement
station's hangar begins no earlier than a later traversal, following WFO-12.

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-38 | Hull/hardpoint cap and drag run after object Lua and before hardpoints. `Hull_Vs_Hard_Points_Health_Constraint=0.2` is a GameConstants scalar. Hardpoints then service in list order, skipping a deletion-pending parent or active setup. Enabled weapon work precedes enabled turret work, then active repair. Repair is per service/frame, distinct from 30-frame hangar work; the elapsed-frame argument does not multiply repair cost/amount. | debug build EWSL-16; verified WFO-23, WCC-03/30/31 and HR-01; same |
| WSL-39 | Directed hardpoint aim/damage uses the targeting/weapon/projectile interfaces. Destroyed hardpoints disable their corresponding shield, weapon, radar, income or special-weapon effect. A fighter bay gates future launch; already launched craft remain. | verified WCC-21/42/51/61, HD-12/13; same; shield-spire collision remains unverified |
| WSL-40 | Repair is a player command naming station, hardpoint index and repairing player. The UI requires a starbase hardpoint below full health; the event/service are not evidence of a general allied-ownership restriction. A destroyed hardpoint stops repair; disabled positive-health replacement slots can be repaired. Ordinary selection permits own objects and allied community property; select/add-select starts repair on an eligible damaged starbase hardpoint. The repair event has no additional owner/alliance check (EUS-01). | verified HR-01/02/06 and G-H4; same |
| WSL-41 | Each repairing player is registered at most once. Each frame: no payers clears repairing; a destroyed slot clears all payers; a missing player or credits below `Repair_Cost_Per_Frame` removes that payer without skipping the shifted next entry. Otherwise debit the authored cost and clamp health plus `Repair_Amount_Per_Frame` to 0..maximum. Multiple payers contribute separately, until a payer completes repair and returns immediately. No wall-clock repair timer or tactical purchase queue is used. | debug build EWSL-16; verified HR-01..03; same |
| WSL-42 | After each paid increment, with current hull H, hull fraction h and combined hardpoint fraction c, when h < c set hull to H × (1 + c − h). At full hardpoint health, clear all payers and re-enable the slot. Stock repair amount is 0.5 health/frame; common weapon cost 1.3, bay/level-1 shield 1.5, level-4 Rebel shield 1.7 credits/frame. The traced service has no positive-amount admission guard; zero can keep paying without progressing. UI admission is sourced by USL-03/EUS-01; the viewer nonowner restriction remains gap allied station repair UI gap (legacy EAWR-1816). | debug build EWSL-16; verified HR-04/05; XML data; same, extends higher-level cost |
| WSL-43 | Station hardpoint damage does not itself select `Prev_Level_Base` as a skirmish replacement. Combat death and upgrade are different routes; campaign base-level degradation is outside this walk. | verified SK-31/WCC-60 campaign boundary; same |

### Destruction and AI interfaces

| Rule | Behavior, branches and constants | Source / existing note comparison |
|---|---|---|
| WSL-44 | Actual combat death can follow lethal hull damage or loss of all required destroyable hardpoints. Mark death/detach, attribute the kill and run elimination/victory immediately through the combat/battle-flow interface. Pending victory gates later damage; replacement is excluded. | verified HD-20/21, WCC-70, WBF-31/33/37; same |
| WSL-45 | Station destruction ends its income source and invalidates production at the removed producer before later queue completion. The existing human/AI refund policy belongs to WPR-20. Living free garrison objects are not a station-owned reserve that dies with it. | verified WPR-14/20 and WFO-26/31; EWSL-11/12; same |
| WSL-46 | Contained research tries an eligible live allied holder using its upgrade owner's faction menu; if none exists, remove the upgrade/bonus and clamp affected maxima. Stock one-station-per-team skirmish reaches the no-holder branch. Tech already unlocked is distinct from a surviving bonus. | verified WPR-57; same; mod multi-holder rehoming remains its existing gap |
| WSL-47 | The victory condition tests remaining qualifying stations, not remaining free fighters. Countdown/results and hardpoint/death art/audio are battle-flow/combat/presentation interfaces. Destruction does not relocate pending free replacements to a non-starbase carrier. | verified WBF-33/37 and EWSL-05; same |
| WSL-48 | AI sees allied live station `Base_Level`, player tech, credits, shared producer/menu/limits, and remaining population. Station upgrade/research desires and attack hardpoint priorities are authored AI equations/plans; they submit ordinary production/attack orders. Missing player free refills must not be replaced with an AI-only periodic spawn. | verified SAE-01/02/03/06/12, WTA and WCC-18 interfaces; same |

## Gap audit against main

Each rule has one main status below. **Missing** includes a missing required
branch; **differs** includes deliberately scoped behavior that no longer matches
the full station contract. Interface matches describe the station-facing boundary,
not a new certification of the other subsystem. WSL-21 is excluded from totals
because complete ownership propagation remains unverified.

| Rules | Main consumer / finding | Status |
|---|---|---|
| WSL-01 | `src/data/xml_merge.cpp` variant resolution, `src/units/unit_tables_decode.cpp` | same |
| WSL-02/03/05/06 | `src/skirmish/start.cpp`, `src/units/unit_tables_decode.cpp`, `src/skirmish/economy.cpp`; WSL-05's placement matches, registration counted separately under WSL-22 | same |
| WSL-04/35 | `session_step_fighters.cpp` and `session_step_economy.cpp` enable/reinitialize station hangars without the skirmish per-object gate | differs G1 |
| WSL-07/08/09 | `fighters_spawner.cpp`, `durability.cpp`, `session_step_systems.cpp`, existing reveal/radar/income consumers | same interfaces |
| WSL-10 | `fighters_spawner.cpp` initializes its hangar once; general instant-load and animation states are outside stock station use | same stock boundary |
| WSL-11 | `unit_tables_profiles.cpp::load_spawner` reads only Tech_0; higher-column fallback is absent | missing G5; stock ten cases all fall back to 0 |
| WSL-12 | `src/units/unit_motion.cpp` sets every reserve to 0 under the old fixture policy | differs G1/G5; stock marker stations need the runtime gate, not authored reserves |
| WSL-13/15/16/17/18/19/20 | `fighters_spawner.cpp`, `session_step_fighters.cpp`, `session_squadrons.cpp`, `src/units/unit_motion.cpp`; stock eligibility, launch and authored-company loss interfaces | same stock boundaries |
| WSL-14 | `session_step_fighters.cpp::hangars` rejects both destroyed and disabled bays; the sourced space gate rejects only destroyed destroyable bays | differs G7; positive-health disabled bay after upgrade is reachable |
| WSL-21 | no live station owner-change operation; callback/complete propagation is outside stock scope | unverified interface |
| WSL-22..28 | no player garrison registry, full-depletion timer or pending station handoff on audited main | missing G2 |
| WSL-29 | viewer renders squadron garrison flags, but has no free-garrison countdown/ready notification | missing G4 |
| WSL-30 | `src/skirmish/economy.cpp`, `src/sim/tactical/session_economy.cpp`, `economy.cpp` | same |
| WSL-31 | `session_step_economy.cpp` completes production, then iterates the resulting held upgrades and replaces stations in that same player pass | differs G6 |
| WSL-32/33/34/36/37 | `session_step_economy.cpp`, `durability.cpp::carry_station_hardpoints`, replacement model/selection/audio consumers; WSL-36 retains its stated uncertainty | same verified boundary |
| WSL-38/39 | `durability.cpp`, `session_step_combat.cpp`, current hardpoint/shield/radar/income consumers; existing combat walk owns internal gaps | same interfaces |
| WSL-40/41/42 | `RepairHardpointPayload`, `session_step_commands.cpp`, `durability.cpp::reserve_hardpoint_repairs`, partitioned `session_step_systems.cpp` service and snapshot-driven repair UI | implemented; WSL-40/41/42 contracts |
| WSL-43/44/45/46/47/48 | no skirmish downgrade; durability/victory, queue cleanup, held-bonus removal and live allied AI station-level consumers | same interfaces |

Totals for 47 classified rules: **30 same, 5 differs, 12 missing**;
one additional ownership interface is unverified. These are rule counts, not
ticket counts. A listed uncertain detail does not become proven because its
station-facing boundary matches.

## Tracking and implementation gaps

Tracking issue: skirmish station lifecycle rule walk (legacy EAWR-1751).
Existing issues were searched before filing. Pending PRs are dependencies, not
main behavior. Sizes: S bounded service/UI change; M several session services.

| Gap | Rules | Work and observable acceptance | Size / ownership |
|---|---|---|---|
| G1 | WSL-04/12/35 | Preserve the disabled authored-spawn flag from marker creation through levels 1–5; upgrades create no inherited fleet and keep living free squads. Generic enabled carriers/map bases retain their separate contract. | M; existing station-upgrade spawn correction (legacy EAWR-1198), draft fix (legacy EAWR-1688) |
| G2 | WSL-22..28 | Player-owned actual-object registration, complete-depletion timer, ordered pending list and allied starbase handoff. Pin partial/full/repeated losses, teammate ownership, blocked/destroyed bays, carrier-loss isolation and upgrades while waiting. | M; existing free garrison replacement (legacy EAWR-1685), draft implementation (legacy EAWR-1718) |
| G3 | WSL-40..42 | Connect the station hardpoint repair command and each payer's per-frame service to the live session. Preserve transfer of repairing slots; full repair re-enables an upgraded 0.1 slot. Pin insufficient/exact credits, two payers, full stop and destroyed rejection with 1/2/4/8 equality. UI ownership admission is now sourced by EUS-01; the nonowner community-station UI restriction is tracked by allied station repair UI gap (legacy EAWR-1816). | M; live hardpoint repair integration (legacy EAWR-1755) |
| G4 | WSL-24/29 | Expose free-garrison waiting/progress and timer-ready feedback to the command bar, distinct from per-squadron flags. Hide on timer expiry, not first launch; read faction ready sound and preserve simulation hashes. | S; garrison countdown feedback (legacy EAWR-1756) |
| G5 | WSL-11/12 | Generic enabled-spawner data coverage: six tech columns with independent starting/reserve fallback, real reserve budgets, and non-squadron company rows. Keep marker-station authored spawning disabled. This is a mod/carrier boundary, not a reason to launch inherited Coruscant station fleets. | M; existing fighter tag coverage (legacy EAWR-651); nice-to-have beyond the stock station fix |
| G6 | WSL-31 | Stage a completed level-up until the next eligible object/ability service instead of replacing during queue completion. Pin producer type, tech/menu, selection and kill/victory behavior on completion and eligible-service frames; preserve source frame order without adding a second production pass. | S; station replacement completion timing (legacy EAWR-1757) |
| G7 | WSL-14 | Use destruction-only fighter-bay eligibility for authored and pending-player space launches. A disabled positive-health bay, including an upgraded 0.1 slot, remains usable in the traced path. Pin destroyed refusal versus disabled launch; do not enable its unrelated weapon/ability effects. | S; disabled-bay launch eligibility (legacy EAWR-1758) |

The five highest-impact follow-ups are G1 (extra inherited fleets), G2 (permanent
loss of free fighters), G3 (unrecoverable damaged/disabled station services),
G7 (lost refill eligibility after a bay's upgrade restoration), and the
shield-spire hit/collision comparison (legacy EAWR-1738). G6 replaces the station
before its first eligible ability service. The hit/collision report is an existing
unverified combat question, not a newly proved station rule difference. G4 is
player feedback; G5 is primarily wider mod fidelity.

## XML registry audit

This docs-only walk changes no registry rows. At the audited main head:

| Tag / class | Registry state | Actual station use / gap |
|---|---|---|
| `Faction/Garrison_Reinforcement_Delay_Seconds` | land-or-galactic | Incorrect for multiplayer space: WSL-24/25 directly use it. Free refill implementation must correct this row. |
| `Faction/Space_Skirmish_AI_Default_Forces` | applied | Start roster already applied; registration/timer are separate G2 consumers. |
| `Faction/Reinforcements_Ready_SFXEvent` | todo, presentation coverage (legacy EAWR-653) | WSL-29's timer-ready cue; the free-garrison countdown/feedback gap (legacy EAWR-1756) supplies this local space caller. |
| `StarBase/Starting_Spawned_Units_Tech_0` | applied | Loaded/authored hangar; runtime suppression still G1. No higher-column authored station rows in this stock audit. |
| `Reserve_Spawned_Units_Tech_0`, including StarBase/SpaceUnit | todo, fighter tag coverage (legacy EAWR-651) | G5; stock marker-station arrays are disabled, and free pending replacements are not these reserves. |
| `StarBase/Spawned_Squadron_Delay_Seconds` | applied | WSL-18; also read by the free pending launch path. |
| `HardPoint/Repair_Amount_Per_Frame`, `Repair_Cost_Per_Frame` | applied | Live station command and partitioned service consume the values; registry rows cite the applying repair and credit-reservation functions. |
| `StarBase/Next_Level_Base` | applied | WSL-32; not an automatic on-damage downgrade rule. |
| `Prev_Level_Base`, SecondaryStructure/StarBase | todo, economy coverage (legacy EAWR-654) | Reverse chain is data; skirmish hardpoint loss does not consume it as replacement. Campaign consumer belongs elsewhere. |
| `Base_Level`, SecondaryStructure/StarBase | partial, economy coverage (legacy EAWR-654) | Live allied AI station perception is applied; does not imply every campaign consumer. |
| `StarBase/Is_Community_Property` | applied | Shared station interaction; enabled generic authored spawns also rotate recipient among allies. Stock station gate prevents that authored route. |

`HardPoints`, hardpoint `Type`, `Health`, `Is_Destroyable`, `Is_Targetable`,
`Attachment_Bone`, `Model_To_Attach`, `Collision_Mesh`, `Fighter_Bay_Flyout_Distance`,
model/health/shield tags, and income/radar abilities are existing applying
interfaces. Weapon hardpoint targeting/collision tags remain the combat audit;
purchase cost/time/menu/limits and upgrade bonuses remain production's audit.
Land `Spawned_Squadron_Location_Bone_Name` and flyout-animation state are not the
stock space bay-selection path. Campaign corruption restrictions, planet base
degradation and `Required_Star_Base_Level` do not override WSL-30's skirmish
production contract.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| USL-03 | Ordinary repair is reached through select/add-select on a damaged station hardpoint. Selection admits the local owner and allied community property; the repair event itself has no owner/alliance check. Zero authored repair amount still charges each affordable service without progress. Remake gap: allied station repair UI gap (legacy EAWR-1816). | EUS-01 |

## Unverified details and focused captures

| ID | Unknown | Evidence that would settle it |
|---|---|---|
| USL-01 | Exact free refill launch frames after full depletion, including player-pass to next due hangar quantization | Existing retail refill capture (legacy EAWR-1739): Coruscant, two free Rebel squadrons, AI suspended and fog stated; partial loss at 60 seconds, no refill through 100, full loss at 100; record both replacement births. Repeat Empire to confirm the faction roster. |
| USL-02 | Numeric hull/shield result after replacement and subsequent cap/recharge/bonus service | Damage hull/shields and one enabled hardpoint, destroy a second and repair a third; step completion, first eligible replacement service and next service through levels 1–5. Compare raw data, difficulty and automatic bonuses separately. First-service scheduling is sourced by WSL-31/EWSL-17; the existing repair-state observation in WPR-52 establishes the disabled branch, not every numeric sample. |
| USL-04 | Small shield-generator hit accuracy, mesh/aim transform and torpedo circling | Existing spire collision/aim investigation (legacy EAWR-1738): matched level-3 Rebel station, Empire attack on `HP01_SHG_COLL`, per-shot target/mesh/aim/hit trace and lit retail comparison. Weapons/projectile walk owns the collision and homing internals. |
| USL-05 | Generic enabled-spawner reserves, existing spawned-unit references and ownership propagation across replacement **Sweep:** Still unverified: Replacement copies the garrison-enabled flag, but this does not transfer reserves or existing spawned-unit references. The generic ownership/reparent callback and reserve reconstruction were not resolved by the enable/selection/detach trace. | Controlled mod with enabled station spawning, finite reserves and owner change; track types, live companies, consumed budgets and detachment after replacement. WPR-53/56's reference/reserve reconciliation remains project policy. No stock marker-station dependency is inferred. Retained sweep boundary: EUS-35. |
| USL-06 | Competition between several pending players/stations and delegated creation failure **Sweep:** Still unverified: The selector traces synchronized roster-start choice and success-only allied-player advancement; pending creation is delegated. Failed creation cleanup and competition among several station/player passes require the delegated creator and reservation release, not this selector alone. | Controlled multi-station map, known service-registration order, blocked bay and missing/invalid creation template; observe pending-entry removal and recipient. Stock one-station-per-team success path is sourced; sorted stable-ID competition is a remake choice. Retained sweep boundary: EUS-35. |

No new retail capture, scenario ID, GPU run or build is part of this walk.
Future capture work stays in ignored output. Validation is clean-room scanning,
public-reference checking, Markdown link review and `git diff --check`.
