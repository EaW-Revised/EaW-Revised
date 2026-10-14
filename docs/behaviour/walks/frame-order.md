# Space tactical logical-frame order

Walk date: 2026-10-01. Remake audited at `21d997cf3b0e496c96d185965f1335d6e00dc3f0`.
This is a docs-only walk of the running space battle, from command execution to logical-frame
completion. Loading, subsystem internals and independent rendering remain interfaces.

Sources are fresh, read-only **debug build** queries, keyed by opaque EFO evidence IDs;
effective **XML data**; and the sourced rules in the linked subsystem walks. The evidence map
and raw queries stay in ignored `out/research/`. No recording was made for this walk.
An **unverified** result stays unverified even when the remake chooses a deterministic order.

Follow-up on 2026-10-08: the [frame service schedule](../frame-service-schedule.md)
narrows UFO-01..05 and the held-target-death question at remake `c0525d2d`.
Its FS/FC/FD rules use fresh debug-build EFS evidence; the historical comparison
and counts below remain the original walk's audit.

Squadron follow-up on the same date, compared at Skirmish `9de14f1d`:
FS-18..22/FC-38..50 in the schedule fixes the older squadron walk's
creation-order inference and distinguishes reverse member registration from
roster notification order. FD-22..31/FSC-23..32 supplies conditional same-frame
target, movement, pair and launch cases. No new recording or confirmed TTK
cause is claimed; UFS-14..20 carries exceptional order, lifecycle and runtime
normalization. The original audit counts below remain historical.
The priority-1 team Buzz follow-up is separate: FS-23/FC-51/FD-32/FSC-33
retains an admitted out-of-range request on its holder for later targeting
attempts, narrowing FS-08's earlier non-team boundary (UFS-21).

The original services objects in **most recent service-registration first** order, then each
object's periodic behaviours in **attachment order**. Ordinary construction attaches
`Behavior` before `SpaceBehavior`, preserving each list's authored order. Consequently there
is no universal movement/weapons/shields/hazards phase order shared by all types. Commands
precede mode service; fog decay precedes objects; income follows objects; each player's AI
precedes that player's build queue. These boundaries can be preserved by partitioned work
with ordered commits; they do not require a serial per-unit simulation.

## Ordered rules

Rows WFO-12 to WFO-24 are nested inside WFO-11 for **each** object, rather than global sweeps.
Attachment in WFO-15 explains that nested order; it is initialization work, not repeated each
frame. Cadences belong to the linked subsystem rules. Optional interfaces run only when
present. Campaign-only work, land bombardment and the map editor are outside this space case.

| ID | Ordered rule, branches and interface | Source |
|---|---|---|
| WFO-01 | Before logical admission, the outer loop services outgoing and scheduled events. An active standalone battle outside playback can execute queued events at the current frame here, even without a newly admitted logical frame. Input/UI work also occurs outside the logical gate. A paused world therefore does not establish that all command execution waits. | debug build EFO-15; WBF-12/13 |
| WFO-02 | An admitted logical frame begins synchronization, obtains its current frame and executes scheduled commands before tree maintenance or battle-mode service. Foreground/network/synchronization gates can prevent admission; a synchronization error separately prevents mode service. | debug build EFO-15, EFO-22; WBF-14 |
| WFO-03 | Normal queued execution scans players, then queued entries from head to next for that player. Scheduled events resolve an execution frame; multiplayer requires the resolved frame to be due, whereas standalone execution has a bypass. Executed entries are removed, optionally recorded, executed and released. Playback consumes its recorded event stream separately. FS-01 establishes tail admission and same-player executable FIFO ties; incoming sequence/remap correspondence and paused producer gates remain UFO-01. | debug build EFO-27; EFS-01/02; FS-01/02 |
| WFO-04 | Global collection-tree service precedes mode service. Object boxes can subsequently change during their owners' services; this is not a promise that all targeting sees one immutable set of final positions. | debug build EFO-15; CO-04/05/07/11 |
| WFO-05 | Mode management reevaluates a matured detail-change countdown, then, with an active mode and no synchronization error, services scoring, the active mode, game votes, voting progress and pending battles, in that order. | debug build EFO-22; WBF-15 |
| WFO-06 | Space mode services an active cinematic, battlefield modifiers and space pathfinding refresh, then attack/announcement counters and enemy-leader victory tests, before entering shared mode service. Battlefield modifiers are a system boundary distinct from per-object combat-modifier cleanup. | debug build EFO-01; WBF-16/17 |
| WFO-07 | Shared mode first refreshes speed/type dependencies and requested music/briefing/odds interfaces. Requested ambient reevaluation is suppressed while results are active. It then reaches fog. | debug build EFO-02; WBF-18 |
| WFO-08 | With fog support, grids present and setup inactive, service the grids for players whose ID modulo **16** equals the current frame modulo **16** (code). Service fogged models after those grids. This is decay/ghost maintenance; revealer circles are updated later by object behaviours, and may change coverage in the same frame. | debug build EFO-02; V-12/15/17, WSU-01 |
| WFO-09 | Object tracking runs next, when available, followed by scene pre-service flush. Tracking's predictions and targeting's collection trees are separate interfaces. | debug build EFO-02; WBF-19, AV-02/03 |
| WFO-10 | Before objects, each player's previous current-income accumulator becomes last-frame income, then current income and maintenance paid are cleared. This accounting reset does not pay income. | debug build EFO-02; WBF-20 |
| WFO-11 | Shared mode services the synchronized object manager completely, then the presentation object manager completely. A manager traverses its service list while gameplay is enabled. It does not scan the object hash table or sort by object ID. | debug build EFO-02/03 |
| WFO-12 | First service registration inserts an object at the **front** of the manager's service list. The iterator starts at the front and follows next links. Existing objects therefore run in reverse service-registration order. Registration normally occurs on the first periodic behaviour attachment, or when special abilities require service. Creation order is only equivalent when registration follows creation without later removal/re-registration; it is not a general ID contract. | debug build EFO-07/11/12/21/24/25/26/28/29 |
| WFO-13 | Within an ordinary object service, locomotor history/disabled-engine recovery precedes combat-modifier cleanup. Positive modifier duration uses current gameplay frame + trunc(seconds x logical FPS + **0.5**); duration **-1** uses unsigned final-frame sentinel **4294967295**, other nonpositive durations use final frame **0** (code constants). Cleanup removes a modifier only when its final frame is **strictly less** than the current gameplay frame; equality retains it. Cleanup covers defense, damage, health, shield, energy, movement, fire rate, reveal range, fire range, ability recharge and tactical build time. The traced total calculator does not perform another expiry sweep. | debug build EFO-04/09/18/23/30 |
| WFO-14 | Delayed damage follows modifier cleanup and precedes periodic behaviours. Positive remaining-frame counters decrement; damage becomes due when the decrement reaches zero and enters the ordinary damage interface immediately. A counter already below 1 is skipped by this service. Ordinary projectile impacts instead apply in the projectile object's service (WCC-01); hero direct damage applies in its handler. There is no single end-of-frame damage batch. | debug build EFO-04; WCC-01, WHE-26 |
| WFO-15 | Ordinary construction applies the general `Behavior` list first, then the list for the active mode (`SpaceBehavior` here), each from first authored entry to last. Periodic behaviours are inserted at the front of the serviced prefix; nonperiodic ones append outside it. Reverse traversal of that prefix thus preserves **attachment order**, not reverse XML order. Runtime attachment preserves its own insertion semantics; duplicate behaviour declarations are not resolved by this walk. | debug build EFO-07/12/16 |
| WFO-16 | For each behaviour in that serviced prefix, invoke it only when its next-service frame is **<= current frame** and its service is enabled. Its handler owns its interval and next deadline; this loop supplies no universal per-system cadence or catch-up loop. A cinematic animation whose type requests `Pause_During_Cinematic_Anim` skips the ordinary pre-animation services; the special-ability/Lua tail has its own gates. | debug build EFO-04/10/14; WHE-09 |
| WFO-17 | Generic ability countdown, locomotion and hyperspace arrival run at their respective positions in the object's attached periodic list. An ability activated by the pre-mode command execution can therefore reach its countdown in that same admitted frame. Arrival's reveal-disable/enable and unloaded notification occur inside locomotion, not in a global reinforcement sweep. The code arrival milestones **35**, **120**, **150** are WR-37/39/40; those do not identify global frame numbers. | debug build EFO-04/15; WAB-11/12, WR-35..40 |
| WFO-18 | Power/shield recharge, object targeting, reveal and hide-when-fogged likewise run at their authored periodic positions, when due/enabled. A type with power/shields in general `Behavior` can recharge before its space movement; a type listing them after locomotion in `SpaceBehavior` does the reverse. Object targeting can see changes made by earlier serviced objects. Weapon hardpoints run later under WFO-24. | debug build EFO-04/07/12/16; XML data; WCC-02/20, V-12, WSU-03 |
| WFO-19 | Asteroid and nebula services occupy their authored per-object positions and use code interval **1** logical frame; capture points use code interval **4**, with a per-behaviour due test rather than an established global absolute-frame-modulo sweep. Their contacts, ownership changes and damage/events occur at those positions; WHZ owns the conditions and constants inside. | debug build EFO-04/10; WHZ-02/10/20/40 |
| WFO-20 | After periodic behaviours, apply a dirty transform and selection-box refresh, then model/attachment visibility, animation and animation-triggered audio support. These calls are inside object service; the independent render frame still has its own services. | debug build EFO-04 |
| WFO-21 | The object's special-ability manager runs after that behaviour/model work and before object Lua. It visits declared handlers with the WHE-09 due/enabled/owner gates. A per-frame hero beam/status can therefore damage another object before or after that recipient's turn, depending on service-list position. Modifier expiry is WFO-13, not a post-hero global phase. | debug build EFO-04; WHE-09/26/34/35 |
| WFO-22 | An attached object Lua script is pumped only when the object is not deletion-pending and its `ServiceRate` is present. When `LastService` is present, elapsed gameplay seconds must be **strictly greater** than `ServiceRate`. Finished scripts detach; otherwise the service records the next reference time, with an initial randomized offset when no previous value exists. This service follows special abilities. | debug build EFO-04; WAB-31, WTA-02 |
| WFO-23 | After object Lua, a live object with eligible destroyable hardpoints applies its hull cap, then services hardpoints in their own list order. Hardpoint weapon/turret/repair work is WCC-03's interface. A projectile or direct handler can already have changed that hull/hardpoint state earlier in the frame. Dynamic importance decays afterward; converted-object attrition has a later object-tail placement. | debug build EFO-04; WCC-03/30/31 |
| WFO-24 | Damage/death notifications can occur inside these calls; queued memory deletion is later. This object service has no universal entry gate saying that every dead or deletion-pending object skips every behaviour. Lua and hardpoints have explicit gates, and each damage/behaviour handler owns its own eligibility. Do not confuse deferred deletion with permission to fire or take damage. | debug build EFO-04/19; WCC-03/40/61, WHE-08 |
| WFO-25 | After a manager's object traversal and camera-alignment refresh, scan its delayed-creation vector from **last entry to first**. Create entries whose due frame is **<= current synchronization frame**, apply requested fade-in support and erase them. New objects from this tail pass have missed that manager's object traversal and receive ordinary service on a subsequent traversal. FS-05 establishes append admission: equal deadlines already queued create in reverse admission order. Producer order, recursive callbacks and exceptional creation routes remain UFO-03. Immediate buzz-droid creation is a separate FS-08 boundary. | debug build EFO-03; EFS-04/09; FS-05/08 |
| WFO-26 | At the manager tail, process queued deletions from first entry onward. Objects allowed to delete detach from both synchronized and presentation managers; the loop rereads queue size as it proceeds, then clears the queue. Disallowed deletion is not performed here. This flush follows delayed creation and precedes return to shared mode; it is not a global end-frame flush. | debug build EFO-03/19 |
| WFO-27 | After both managers, service the timeline, collision system and movement coordinator, in that order. Object locomotion and projectile collision can already have run; these system interfaces must not be relabelled as all movement or all damage. | debug build EFO-02; WBF-22 |
| WFO-28 | Then service setup support, retreat outside the editor, and victory countdown outside the editor. Deciding destruction registers pending victory earlier, inside the destruction interface (WBF-31/37, WCC-40). Countdown placement does not authorize later same-frame damage while pending. | debug build EFO-02; WBF-22/31/37 |
| WFO-29 | Tactical income-stream service follows countdown, outside the editor. Multiplayer-style tactical credit balancing and pending domination follow it. Income therefore observes the object changes/deletions already made; it precedes the later player AI/build-queue pass. Human pre-mode purchases cannot spend income paid later in that same frame. | debug build EFO-02/05/15; WPR-10..13/30, WBF-23 |
| WFO-30 | Shared mode then handles matured victory/end transitions and presentation tails. Returning from mode management still leaves outer logical-frame work: the end callback is not proof of an immediate abort of the entire outer iteration. | debug build EFO-02/15; WBF-24 |
| WFO-31 | After mode management and eligible minicinematics (skipped while the load dialog is active), service players in **ascending player ID**. For each player, service a valid AI first, then that player's tactical build queue. Queue validity and completion see the prior object service; an AI decision precedes its own queue completion, while a later player's AI runs after earlier players' queues. The AI's internal perception/goal/plan cadence remains WTA-01. | debug build EFO-05/15/17; WPR-20..23, WTA-01 |
| WFO-32 | Global gameplay scripts follow the player pass; story elapsed/era hooks and tutorial follow them. The generic outer Lua engine/debug-server pump is a different, earlier outer-loop interface (WFO-01), not this global gameplay-script pass. | debug build EFO-15; WBF-25 |
| WFO-33 | Campaign-only win checks are gated separately. Mode frame advance then precedes end-frame synchronization, followed by quitting-player processing. Timers in these preceding calls read the frame being serviced, not a universally preincremented next frame. | debug build EFO-15; WBF-25 |
| WFO-34 | Independent render admission places space fighter-cell layout **after space camera setup and before shared mode render service**, including the active-cinematic camera branch. It is not a logical-frame service. The layout visits occupied cells and feeds icon positions to the command bar; later shared object rendering updates grippers and command-bar rendering rebuilds icons/bars. | debug build EFO-08/13/15; WBF-26, WSU-34/36 |

### Two stock attachment examples

The effective `Tartan_Patrol_Cruiser` general list places `ABILITY_COUNTDOWN`, `POWERED` and
`SHIELDED` before space locomotion and targeting. Its space list places `REVEAL` before
`HIDE_WHEN_FOGGED`. `Calamari_Cruiser` instead places countdown, locomotion, power, shields,
targeting, hide, reveal, unit AI, asteroid damage, damage tracking, ion effect and nebula in
that order in its space list. Nonperiodic entries still attach but are not periodic services.
These are data examples, not a prescribed phase schedule for every type or an inference that
two handlers due at different intervals run together. Sources: XML data in
`spaceunitscorvettes.xml` / `spaceunitscapital.xml`; debug build EFO-04/07/12/16.

## Deferral and same-frame visibility

| Trigger | Established boundary | Limit / source |
|---|---|---|
| Reinforcement created during pre-mode command execution | Registers before object traversal, so can receive ordinary service that frame; repeated requests observe population charged by earlier successful creations. | WFO-02/03/12/17; WR-02/05/30..32. Incoming equal-player queue ordering remains UFO-01. |
| Object immediately created while its manager is traversing | Front insertion puts it before the current iterator; that traversal's subsequent next links do not visit it. It waits for a subsequent traversal of that manager. | WFO-12; debug build EFO-24/25/26/29. A different manager whose pass has not begun is a separate case. |
| Object created by manager delayed-creation tail | Ordinary periodic service waits for the next traversal, even though creation notifications can run immediately. | WFO-25; creation notifications WHE-07/08. |
| Upgrade created by a player's completed build | Player pass is after both object managers; ordinary service waits for the next manager traversal. Normal unit completion adds to the pool rather than deploying. | WFO-31; WPR-22/51/52. |
| Arrival reaches local counter 120 | Reveal enables inside locomotion. Its later due reveal behaviour can mark cells that same frame; earlier objects' targeting has already run. Global fog decay/ghost service ran before the arrival. This does **not** imply a universal wait for the next 16-frame grid service before fresh cells are marked. | WFO-08/12/15/17/18; WR-39; V-11/12/15/17. |
| Arrival reaches local counter 150 | Position restore, movement release, unloaded signal and forced hide reevaluation happen in locomotion before later behaviours/special services on that object. Earlier objects already consumed their inputs. | WFO-17/18/21; WR-40. Arrival firing/order eligibility remains reinforcements U-1. |
| Station killed on its scheduled queue-completion frame | Object service/deletion precedes player-queue validity/completion. WPR-20's invalid-station sweep is reached before WPR-21 completion, so the due time alone cannot produce from the removed station. | WFO-26/31; WPR-20/21. This ordering already matches the remake's late production pass. |
| Timed combat modifier at its final frame | Cleanup retains equality and removes it on a later gameplay frame. Whether an earlier external damage caller reaches a still-uncleaned recipient depends on service order; compare each consumer, not generic ability expiration. | WFO-12/13/14/21; UFO-02. |
| Event raised during a service | Its immediate notification can affect later consumers; a newly scheduled command is constrained by scheduled-queue traversal and admission. There is no established universal rule deferring every event to the next frame. | WFO-03/17/24/32; UFO-01/02. |

## Amendments to the five subsystem questions

| Walk / U-item | Resolution | Remaining evidence |
|---|---|---|
| [Production](production.md) U-1 | **Settled schedule:** commands -> objects/deletions -> income -> per-player AI -> that player's build queue (WFO-02/26/29/31). Income is a mode system, not an income pass nested in each player service. Station death precedes queue completion. | Any particular AI purchase callback's queue insertion follows that AI walk's interface; UFO-01 applies only to input/event tie ordering. |
| [Heroes](heroes.md) U-01 | **Settled schedule:** service registration determines recipient/source turns; combat cleanup and delayed damage precede behaviours, special handlers follow them, Lua/hardpoints follow special handlers (WFO-12..24). There is no global post-damage modifier expiry. | Exact source-death/recipient-refresh/beam collision for a chosen handler remains UFO-02; schedule settlement does not invent the handler's callbacks. |
| [Hazards](hazards.md) U-01 | **Settled schedule and attachment:** general then space lists, authored order of periodic attachment, newest service registration first; fog first and each object's movement/shield/environment services at its own due positions (WFO-08/12/15..19). | Exceptional duplicate/runtime detach/reattach cases are UFO-04. Shield-mesh entry/exit remains hazards U-02. |
| [Reinforcements](reinforcements.md) U-4 | **Partly settled:** fog-grid decay precedes arrivals; arrival enabling can feed a later same-object revealer in the same frame; commands charge population before the object pass; deletion precedes late queues (WFO-02/08/12/17/18/26/31). | Same-player competing event insertion and the exact population-unregistration callback remain UFO-01/05. A kill later in object service is not available population for an earlier pre-mode command. |
| [Sensors/UI](sensors-ui.md) U-04 | **Settled with correction:** fighter-cell layout is in render service, after the camera and before shared object rendering; it is absent from the logical object-services sequence (WFO-34). | No capture is needed to establish this caller placement. Retail layout/slide comparisons remain that walk's separate questions. |

Four scheduling questions are settled; reinforcement U-4 retains explicitly bounded races.
The same findings correct capital-combat WCC-02's reverse-XML wording and settle the
pre-mode-command branch of abilities WAB-12: an ordinary admitted-frame activation can
count its first frame during that frame's later countdown service. Object-script activation
after the countdown, late AI execution, playback and paused input require their actual call
positions rather than that human-command shortcut.

## Comparison with the remake tick

All simulation rows below refer to `src/sim/tactical/session_step.cpp`, `TacticalSession::step`,
unless another file is named. Status is **same**, **differs**, or **missing** for the recorded
boundary; a difference in internal organization is not proof of a visible failure. The
comparison preserves [EnTT storage decision](../../architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids)'s copied inputs, disjoint staging and ordered commit contract.

| Rules | Remake phase / interface | Verdict and observability |
|---|---|---|
| WFO-01 | `src/platform/live_session.cpp`, pause/pacing and queued player input | **differs**: commands are held for a world step. Exact paused mutation is unverified; UFO-01, no new implementation ticket. |
| WFO-02 | commands at the late canonical `(tick, player, sequence)` pass | **differs, observable**: movement, targeting/fire and projectile damage precede commands. A newly activated fire mode or changed target cannot affect firing already emitted in that tick; original pre-mode commands can affect later eligible services. Reuse observable frame-order review (legacy EAWR-949). |
| WFO-03 | pending-command key order; `src/script/authoritative/tactical_bridge.cpp` queues scripts for the next world step | **differs**: explicit canonical keys versus original per-player queue traversal/playback. Whether equal-player ordering differs in a visible case is UFO-01. |
| WFO-04 | collection-box rebuild/service after movement, before targeting | **differs**: CO-11 deliberately uses one moved immutable view. Target ties/range/obstruction can differ at a crossing boundary; a concrete case requires UFO-04, reuse global review (legacy EAWR-949). |
| WFO-05 | world step has no original scoring/mode-vote/pending-battle pass | **missing**: lifecycle/scoring interface, owned by battle-flow/results work (legacy EAWR-953, EAWR-616). No frame-order-only ticket. |
| WFO-06 | motion/path planning and effects are separate interfaces | **differs**: no corresponding system prefix. Stock outcomes depend on active modifiers/cinematics; keep interiors with battle-flow/hero walks, no proven new visible mismatch here. |
| WFO-07 | host presentation and loaded tables | **differs**: dependency/music prefix has no matched world boundary; battle-flow owns these presentation gates. |
| WFO-08 | fog decay/circles/flashes in late visibility after economy/hangars | **differs**: per-player decay cadence agrees, but position and coverage consumption are reorganized. A revealer processed earlier in retail can expose a target before a later shooter's same-frame service, whereas remake targeting reads the previous snapshot. This is an observable contact/fire boundary; reuse global review (legacy EAWR-949). |
| WFO-09 | tracking/planning after movement/targeting/abilities, with copied inputs | **differs**: input age differs; no additional observable failure established beyond the crossing case above. |
| WFO-10 | late ledgers/income, no paired original income accumulator rollover | **missing**: the accounting interface, owned by production; missing counters alone do not prove a battle outcome difference. |
| WFO-11 | partitioned movement, targeting, projectiles, abilities, unit systems | **differs**: system sweeps replace nested object services. Implementation organization is allowed; the observable dependency cases below matter. |
| WFO-12 | ascending entity-ID staged/commit order; projectile IDs ascending | **differs; shooter side resolved**: reverse service registration differs from ascending ID. An existing later-registered projectile can kill/disable a ship before its retail weapon service; the remake emits all due targeting/fire before applying existing projectile hits. The hit commit now withdraws that frame's shots of a unit or weapon hardpoint those hits destroyed ([dependency review](#dependency-review)); a dead target can still draw that frame's shots. Pending-victory protection remains separate (legacy EAWR-779, EAWR-703). |
| WFO-13 | timed arrival vulnerability cleared in movement; ion expiry in abilities; generic hero modifiers absent | **differs**: no common per-recipient modifier-cleanup position. The traced final-frame predicate is inclusive; the arrival-specific endpoint comparison is UFO-02 until normalized. Generic modifier state is hero ownership work (legacy EAWR-937). |
| WFO-14 | partitioned projectile flight followed by ordered damage commit; queued blast damage delivered first in that commit | **differs; shooter side resolved**: projectile effects are batched after fire; a recipient that queued damage or an existing projectile destroys loses that frame's shots, as in WFO-12. |
| WFO-15 | typed tables/explicit phases, no retained periodic attachment scheduler | **differs**: universal phase placement loses general-versus-space order. Compare WFO-17/18's stat/expiry cases; do not introduce a serial per-entity loop to copy the container. |
| WFO-16 | per-feature deadlines/phase gates | **same interface**: due services/gates are represented by feature state. It does not reproduce every original behaviour type or exceptional cinematic gate; those are subsystem/lifecycle gaps. |
| WFO-17 | movement/arrival before targeting; abilities after targeting/projectile flight staging, before command execution | **differs, observable**: authored countdown-before-targeting types can stop a timed fire-rate mode before the final eligible weapon service; remake targeting reads its active slot before `expire_abilities` clears it. Similarly pre-mode activation and reveal hooks differ. Reuse global review (legacy EAWR-949); arrival permission itself remains its walk. |
| WFO-18 | energy/shield recharge in late unit-systems, after firing/hit commit | **differs, observable**: a due pre-weapon recharge can fund or absorb a same-frame shot in the original; the remake first fires/applies hits and recharges afterward. Tartan/MC80 attachment demonstrates which local order applies. Reuse global review (legacy EAWR-949). |
| WFO-19 | no asteroid/nebula/capture service in this audited tick | **missing**: already owned by hazard simulation/capture work (legacy EAWR-924, EAWR-925, EAWR-926, EAWR-927). Apply their sourced per-object interfaces when implemented; no new ordering bug inferred from absence. |
| WFO-20 | unit-system transforms and snapshot publication; host animation/audio | **differs**: presentation is detached from the logic object's tail. No extra visible fault established by this walk. |
| WFO-21 | generic special-handler service absent | **missing**: heroes handler work (legacy EAWR-936, EAWR-940, EAWR-945, EAWR-946); preserve WFO-13/17/23 consumer ordering in those implementations. |
| WFO-22 | native DEFEND stand-in in abilities; authoritative script host after world | **differs**: native script check follows early hits in abilities, before late impacts, while original object Lua follows its behaviours/special service. FC-16 now establishes randomized per-unit 30-frame tracking phases; FD-09/15 and FSC-10/16 distinguish rate publication before another ship's targeting from publication before object Lua. The global remake phase can differ by 0..29 frames, before script-service age; runtime Lua admission remains UFS-10. |
| WFO-23 | hull cap/drag in hull-service before targeting | **same dependency**: partitioned hull cap and drag now precede targeting and hardpoint fire (WCC-02/30/31). |
| WFO-24 | hit commit removes killed targets immediately; systems remove destroyed survivors | **differs**: logical removal and snapshot deletion are distinct from the original manager deletion tail. Immediate pending-victory gating is already tracked (legacy EAWR-779, EAWR-703); never copy deferred deletion as a damage permission. |
| WFO-25 | no generic manager delayed-creation/respawn vector | **missing**: respawn work already exists (legacy EAWR-928). Reverse due-vector traversal is sourced; producer insertion ties remain UFO-03. |
| WFO-26 | removals committed before late economy validity | **same boundary** for removed-station queue checks; memory/container organization differs but is not observable by itself. |
| WFO-27 | collision flight/commit and motion/plans embedded in preceding phases | **differs**: system calls do not map one-to-one. Object-service arbitration is covered by WFO-12/14, not a new global collision rule. |
| WFO-28 | victory after visibility, destruction events processed at end | **differs**: pending registration/countdown age and later same-frame damage are existing victory work (legacy EAWR-779, EAWR-703); this walk confirms their placement rather than duplicating them. |
| WFO-29 | late income followed by production, commands earlier | **same boundary** for object loss before income and human purchases before income. AI spending timing differs through WFO-31; income does not fund a pre-mode human purchase that already ran. |
| WFO-30 | host sees published outcome and opens results | **differs**: no original nested end callback. Battle-flow owns results/pause/teardown (legacy EAWR-616). |
| WFO-31 | economy queues finish inside world before `ScriptedTacticalSession::step` services AI/scripts | **differs**: AI sees a pool after this frame's completions and its generated commands wait for the next world step; original AI precedes its own queue. Purchase-capable AI is incomplete at this snapshot, so a new visible purchase race is unverified; reuse global review (legacy EAWR-949). |
| WFO-32 | script bridge services global scripts after world and AI, retains generated commands | **same broad boundary**: scripts follow world/AI. Individual script/command latency is WFO-03's separate question. |
| WFO-33 | completed tick advances during publication, then host/script processing | **differs**: clock convention differs; normalize creation/frame labels before declaring an endpoint bug (UFO-02). |
| WFO-34 | `apps/viewer/src/map_mode_hud.cpp`, icon layout/render from snapshots | **same boundary**: layout is presentation work outside logical simulation. Original layout/slide geometry still differs under sensors/UI's own gaps. |

Counts by WFO rule: **6 same**, **23 differs**, **5 missing**. The observable differences
form six dependency groups: command-before-consumers; revealer-before-later targeting;
existing-projectile-before-ship fire; countdown-before-weapon expiry; recharge-before-weapon
or damage; hull cap-before-hardpoint fire. They are executable/data consequences, not claimed
retail captures. All six already fall within the existing global frame-order review; reuse it
instead of filing duplicate implementation bugs. Tracking: Tactical frame order (legacy EAWR-1041).
The linked global review retains its existing battle-flow parent (legacy EAWR-953).

## Dependency review

The battle-flow G1 review (legacy EAWR-949) checked WBF-12..14, 16, 18..23 and 25..26 and the six
groups above against the [phase map](../../simulation.md#phase-map) on main at `78bee775`. One
shooter-side consequence was fixed first. The target-side and hull-service boundaries are now fixed too;
the earlier review recorded deterministic deviations of at most
one logical frame (1/30 s) on one boundary. The combat follow-up compares
Skirmish at `a7ae1c7592dde8d5b3f7c025428a16a0b45e2879`: FD-09/15 establishes
per-unit tracking phases and a target-retention consequence that is not bounded
to one frame. FD-10/11 also distinguishes a delayed event from an event whose
existence or hull/shield recipient changes. These findings narrow that earlier
bound, rather than treating all service organization as observable. The first frame-service pass adds the
armed-swarm case FD-07/FSC-09 separately; it can change whether a shot exists.
The accepted-deviation re-check uses Skirmish `487842eb052487f5aba6a98003e8fbbdac117ba3`.
FD-17..21 / FSC-18..22 finds a stock-visible static case for each of the five:
a one-frame input-age difference is not a one-frame bound on retained clocks,
shots, hull loss or production cancellation. They stay closed for M2, with
**implementation: post-M2-sign-off**; UFS-13 retains runtime normalization.
Implementation review keeps the partitioned phases with their ordered commits.

| Group / rules | Remake order | Verdict |
|---|---|---|
| Existing projectile before ship fire, shooter side (WBF-21; WFO-12/14/24; WCC-03/61) | queued damage and existing projectile hits commit before targeting | **Fixed for early hits.** Dead shooters and destroyed weapon hardpoints never service their weapons, spend pulses or launch shots in this frame. Test: `test_lost_shooter_withdraws_shots` (tactical damage contracts). Beam, buzz-droid, hazard, scripted and later-phase kills retain their shots in the remake. Buzz droids have their own object turns: FD-07/FSC-09 below bounds a later-registered armed swarm killing an older craft before its weapon turn. WFO-21 alone cannot place all such kills after firing. The remake still treats all in-flight projectiles as preceding every shooter; object registration order is not reproduced. |
| Armed swarm before older victim's weapon (FS-08; FD-07; WAB-72/73; WCC-03/70) | shots launch before droid damage in ability tracking | **Observable static difference.** FSC-09 gives a later-registered armed swarm, an older craft below one hit, and that craft's due weapon pulse. The original turn order kills it before firing; the remake already launched its shot. Offer this case to the existing buzz-droid work with partitioned staging and ordered commits; no runtime capture or universal serial object pass is implied. |
| Existing projectile before ship fire, target side (WCC-25) | targeting refreshes combat inputs after this frame's early hits | **Live-input boundary fixed; replacement one frame early.** A killed target is absent from the live combat view, so a due weapon pulse can retarget and fire in the same frame when its ordinary ship scan is due (WCC-12/13, T-05). FS-09/FC-06 find no deadline reset. In the original a holder that finds its target dead clears it and ends that turn, and detachment completes at the end of the death frame, so the first replacement scan and shot come at t+1 (FD-06). A hardpoint whose ordered target is dead does not opportunity-fire that frame (WWP-05). Weapon opportunity failure keeps its own immediate bypass (FS-10/FC-07). Test: `test_same_frame_retarget_after_kill` records the remake's same-frame replacement, equal on 1/2/4/8 workers; **implementation: open** (legacy EAWR-2092). |
| Commands before consumers (WBF-12/14; WFO-01..03, 17) | commands after movement, targeting and the hit commit | **Accepted for M2; static stock-visible re-check FD-17/FSC-18.** The recorded move acts from t+2 (MV-02; S-10/S-11/S-13). A Tartan weapon-mode activation is too late for its t pulse to write the shortened delay; the retained refire can still differ at t+2. One ordinary frame of input age does not bound downstream clocks or shots. Paused execution remains UFO-01. **implementation: post-M2-sign-off**. |
| Revealer before later targeting (WBF-18; WFO-08/18) | targeting reads the last published visibility; fog advances in `finalize` | **Accepted for M2; static stock-visible re-check FD-18/FSC-19.** Corellian reveal can reach a later due Tartan scan in the original; the remake reads the previous permission. Fresh permission at t+1 does not reset a scan spent at t, and fleeting shot eligibility can be lost entirely. FD-11/FSC-12 covers the decay inverse; 16-frame decay and display fade remain distinct. **implementation: post-M2-sign-off**. |
| Tracking before objects (WBF-19; WFO-09) | `tracking` samples after movement and targeting | **Deviation.** Input age differs inside a frame; no observable failure is established beyond the contact latency above (WFO-09 row). |
| Countdown before weapon expiry (WFO-17) | `expire_abilities` in the ability phase after targeting | **Accepted for M2; static stock-visible re-check FD-19/FSC-20.** Countdown precedes the Tartan hardpoint tail. A final burst pulse can reset to the still-active short recharge in the remake; both modes can be off at t+1 while their next shots remain different for many frames. FS-04/FC-03 also requires activation/first-service normalization; the isolated final-boundary case does not settle a combined start/end clock offset. **implementation: post-M2-sign-off**. |
| Recharge before weapon or damage (WFO-18) | energy and shield recharge in `unit-systems`, after the hit commit | **Accepted for M2; static stock-visible re-check FD-20/FSC-21.** Slave I has power before its energy-costing object weapon; its due first pulse can wait one frame. A Tartan serviced before an older projectile can shield damage that reaches hull in the remake; later recharge cannot restore that hull. This requires the stated registration, recharge and depletion gates, not recharge before every hit. Tartan hardpoints themselves cost no energy. **implementation: post-M2-sign-off**. |
| Hull cap before hardpoint fire (WFO-23; WCC-02) | partitioned hull-service follows early hits and precedes targeting | **Fixed.** Cap/drag runs once before weapons, with ordered loss notifications. Tests: `test_hull_drag_before_fire`, `test_hull_drag_lost_weapon_same_frame` and the phase-map guard. WCC-31's exact loss/current ratio is below 1 for bounded hardpoint health, positive hull and nonnegative constraint, but the existing nearest-Q24 rounding can finish a one-quantum hardpoint. The loss regression keeps a second hardpoint standing and verifies no fire event or new projectile in the weapon's loss frame, equal on 1/2/4/8 workers. |
| Damage rate before another ship's target choice (FS-14; FC-16; FD-09) | targeting precedes global rate publication | **Observable static difference.** FSC-10 keeps a damaged held target sticky in the original while the remake can select a higher-priority target using the old rate. The retained identity can differ beyond one frame. **implementation: post-M2-sign-off**; reuse tracking/targeting ownership. |
| Retained hardpoint aim after target movement (FS-16; FC-12; FD-12) | autonomous object weapons choose a best hardpoint per attempt | **Observable static difference.** FSC-13 retains a standing aimed hardpoint in the original while the remake can choose a newly nearer one. Explicit hardpoint orders already retain their standing target. **implementation: post-M2-sign-off**; reuse WCC-17/18. |
| Repair debit before another station's repair (FC-27; FD-13) | sparse player-budget reservations follow ascending unit ID | **Observable static difference.** FSC-14 has one credit and opposite ID/registration orders: different stations receive the increment and the unpaid payer drops. **implementation: post-M2-sign-off**; parallel preparation and ordered payer commits remain required. |
| Direct beam damage before older victim's fire (FC-24; FD-14) | enabled beam damage is late, after launch | **Observable static difference.** FSC-15 extends the swarm family to an active lethal beam registered later than its victim. Reverse registration changes the original boundary. **implementation: post-M2-sign-off**; use each handler's sourced interface rather than moving every ability before every shot. |
| Rate publication before shield script / threat consumer (FC-16/17/31; FD-15) | native shield check shares the global close; historical attacker decay incomplete | **Differs / missing.** FSC-16 compares recorded service boundaries and does not assume a universal one-second script phase. Per-unit tracking phase differs by up to 29 frames; TaskForce callback admission remains UFS-10. **implementation: post-M2-sign-off**; reuse tracking/AI work. |
| Per-unit positioning/proximity before movement requests (FC-19/20/34/35; FD-16) | complete per-unit services absent | **Missing interface.** First deadlines and ordinary writes are now bounded; exceptional resets, stock proximity applicability and exact callback admission remain UFS-10. No new timing fix follows from an unrelated player AI interval. |
| Income reset, income and production (WBF-20, 23; WFO-10, 29) | late economy: object losses before income, human purchases before income | **Same boundary** for every visible case (WFO-26/29). The last-frame income accumulator itself is not modelled (WFO-10), owned by the production rule walk (legacy EAWR-728). |
| Player AI before its build queue, global scripts after players (WBF-25; WFO-31/32) | world (with queues) then AI, then global scripts; AI commands enter the next world step | **Accepted for M2; static stock-visible re-check FD-21/FSC-22.** A fixed AI Interceptor purchase stamps its timer one frame later, shifting completion; producer destruction in that extra frame can cancel/refund instead of pooling the unit. Conversely, the remake AI sees its own freshly completed queue before its next requests, while the original AI precedes that completion. Actual next decision/coroutine service remains UFS-13. Global-script placement stays after players. **implementation: post-M2-sign-off**. |
| Space service prefix and post-object services (WBF-16, 22; WFO-06, 27/28) | no system prefix; collision and motion inside the phases; pending victory in the ordered commit | **No visible dependency.** Pending victory already blocks later same-frame damage (VP-01, WCC-40). Hazard, cinematic and modifier interiors belong to their walks. |
| Outer, UI and render order (WBF-13, 26; WFO-34) | Godot frame callbacks over published snapshots | **Presentation only**; no logical-frame consequence. |

The squadron pass appends these boundaries; it does not reopen the five M2
accepted deviations above. Evidence EFS-32..42 is a fresh debug-build reading,
and the cases are static acceptance inputs.

| Dependency | Current remake | Verdict / case |
|---|---|---|
| Reverse member service / immediate share (FS-18/20; FC-41) | ascending-ID member scan preparation, sparse target commit after movement | FD-22/23: target/scan winner and a later moving member's first directed turn can differ, FSC-23/24. **implementation: post-M2-sign-off**; direct TTK contribution remains UFS-18. |
| Earlier craft movement / chase reservation (FS-21; FC-42/43) | immutable preflight candidate/leader poses, container/craft-ID reservation order | FD-24/25: changed follow admission, pair identity and 300-frame reservation, FSC-25/26. **implementation: post-M2-sign-off**; no new weapon cadence inferred. |
| Container aggregation / normal follower input (FS-19; FC-39/40) | preflight member/leader inputs and late aggregate commit | FD-26/FSC-27: same normal preflight age; closed as internal organization absent an intermediate consumer. Maximum-error formula remains its existing squadron gap. Exceptional registration/restore is UFS-14. |
| Member hit/removal / leader and carrier capacity (FC-47/28) | movement before hits; refreshed live firing inputs; squadron losses before hangars | FD-27/28: live post-hit shot selection agrees, but movement guidance or a carrier-before-loss replacement can differ, FSC-28/29. Reserve-zero M2 is the no-replacement control. **implementation: post-M2-sign-off**. |
| Member completion / formation done check (FC-44..46) | early completion/escort approximations; complete destination-stack/reset branch absent | FD-29/FSC-30: missing replan/reset service stays with movement ownership; initialization/restore is UFS-16. **implementation: post-M2-sign-off**. |
| Member hit / squadron percentage reader (FC-48) | independent container hull; no new periodic member-health sum required | FD-30/FSC-31: demand mean of live hull percentages is a separate interface; closed as internal service organization. Every reader's timing remains UFS-19. |
| Target container refresh / range reader (FC-39) | end-of-prior-tick container centre is available before movement | FD-31/FSC-32: an earlier original reader can see an extra frame of target-centre age. **implementation: post-M2-sign-off**; range geometry and measured TTK contribution remain separate. |
| Admitted team Buzz request / later holder range (FS-23; FC-51) | one-shot point-range refusal, no retained request | FD-32/FSC-33: missing holder retry after an out-of-range event; priority-1 existing ability regression follow-up, separate from the priority-4 dogfight gate. |

WBF-15, WBF-17 and WBF-24 belong to the results, music and victory work and were not part of this
review.

## Existing notes and ownership

| Note / rules | Comparison |
|---|---|
| [Battle flow](battle-flow.md) WBF-12..26 | **same** nesting/placement; **missing there** exact service-list insertion, authored attachment, modifier endpoint and creation-tail order. |
| [Capital combat](capital-combat.md) WCC-01/02/03 | **same** interleaved projectile/hardpoint interfaces; **differs** WCC-02's claim of reverse authored order: reverse storage traversal preserves attachment order (WFO-15). Object service order is now WFO-12. |
| [Targeting](../space-targeting.md) CO-11, G-01 | **same** project ordered update distinction; **missing there** retail reverse registration. This settles the generic service-list unknown, not duplicate/reattachment or float/model-box questions. |
| [Visibility](../space-visibility.md) V-12/15/17/19 | **same** early grid decay/later reveal; **missing there** precise per-object interleaving and render-cell placement. |
| [Abilities](abilities.md) WAB-12 U-01 | **same** countdown arithmetic; **settled** pre-mode admitted-frame command reaches a later countdown. Other activation contexts remain caller-specific. |
| [Production](production.md), [heroes](heroes.md), [hazards](hazards.md), [reinforcements](reinforcements.md), [sensors/UI](sensors-ui.md) | Schedule amendments are explicit above and in each walk. Their subsystem interiors and non-frame-order gaps remain with those walks. |

The scheduling code reads no XML tag selecting a universal service phase. The relevant
direct inputs are `Behavior`, `SpaceBehavior` and `Pause_During_Cinematic_Anim`; their
stock applicability/examples are described above. `GalacticBehavior` / `LandBehavior`
are alternative-mode interfaces, not space ordering inputs. Registry inspection finds
`Behavior` still **todo** for SpacePrimarySkydome, SpaceSecondarySkydome and SpecialEffect
movement coverage (legacy EAWR-649). `Pause_During_Cinematic_Anim` has **presentation-later**
cinematic rows and **land-or-galactic** GroundInfantry / Props_Story rows; the registry has no
ordinary space-unit row for this tag. Ordinary
space `Behavior` / `SpaceBehavior` rows are marked applied, which proves existing readers,
not a faithful periodic schedule. This docs-only walk adds no reader and changes no registry
status. Hazard/hero/economy/visibility tag inventories remain in their subsystem walks.

## Unverified reads and captures

| ID | Still unverified | Minimal evidence that settles it |
|---|---|---|
| UFO-01 | **Narrowed:** tail admission settles same-player executable FIFO ties (FS-01/FC-01); standalone outer execution is outside logical admission (FS-02). Incoming sequence/remap correspondence, non-tail callback insertion and the complete paused producer set remain unknown. | UFS-01 / FSC-01 / FD-01 in the frame service schedule: competing one-player/two-player orders, paused acceptance and state mutation, and recorded playback. Preserve admission versus execution order and fog mode. Evidence EFS-01/02; retained EUS-32. |
| UFO-02 | **Narrowed:** modifier equality survives cleanup (FS-03/FC-02); generic ability expiration counts its first service and is attachment-dependent (FS-04/FC-03). Normalized arrival clocks and source-death/recipient-refresh/direct-reader races remain unknown; no universal endpoint follows. | UFS-02 / FSC-02 / FD-02: record activation, creation and first service, then fixed damage around final-1/final/final+1 in both registration orders. Kill a source before/after recipient cleanup and observe health/shields. FC-03 now distinguishes elapsed resume from invocation-count damage; FC-13/14 records `DEFEND` on/off deadline writes. Exceptional lifecycle resets remain UFS-08. Evidence EFS-03/10/15/16/19; retained EUS-30. |
| UFO-03 | **Narrowed:** delayed admission appends; equal deadlines create last-admitted first, and the traced ordinary positive-respawn destruction branch admits in destruction-call order (FS-05/FC-04). Immediate ability application creates through a distinct boundary (FS-08/FD-07); command execution and application must not be conflated. Exceptional producers, recursive callbacks, rounding and first-service consequences remain unknown. | UFS-03 / FSC-03 / FD-03, plus UFS-07 / FSC-08 for buzz droids: two equal-delay destructions with distinguishable ownership, creation notifications and first ordinary service; repeat admission order. Evidence EFS-04/09. |
| UFO-04 | **Narrowed:** an explicit remove/add pair uses ordinary front registration; registering an existing member does not move it (FS-06/FC-05). Duplicate behaviour attachment, exact save/load reconstruction and crossing outcomes remain unknown until their list producers and resumed service are checked. | UFS-04 / FSC-04 / FD-04: compare uninterrupted and loaded runs after re-registration, with fog on at a range/reveal boundary, recording contact, position and first fire for three frames. Evidence EFS-05; retained EUS-04. |
| UFO-05 | **Narrowed:** manager detach removes population registration; allowed queued deletion follows delayed creation (FS-07/FC-09). Ordinary pre-mode requests precede that flush and match the remake's retained shares through commands (FD-05). Exceptional death/team/ownership/direct-removal routes remain unknown. | UFS-05 / FSC-05: population at cap, two arrivals and ship versus craft death, then direct detach or ownership change before/after admission; record accepted request, pool and remaining room. Reinforcements U-4 remains route-specific. Evidence EFS-06; retained EUS-31. |

These follow-ups are evidence work, not new asserted original behaviours. No implementation
ticket is filed solely because an internal phase differs or because an unverified race might
be observable.

## Galactic HUD clock continuation

The 2026-10-10 **debug build** continuation is in the
[galactic frame schedule](../galactic-frame-service-schedule.md#hud-clock-continuation),
GFS-29..32, GFC-39..44, GFD-34..36 and GFSC-26..30. It specializes the ordinary
outer caller for campaign presentation; it adds no space-tactical object phase.

| Boundary | Schedule contract | Remaining qualification |
|---|---|---|
| Earlier outer HUD update → later logical player/story work | Advisor uses outer wall milliseconds; hint production uses active-mode frames. Shared transient messages service after HUD/input work and before logical admission. A later hint misses the already completed advisor update. | GFS-30/GFC-41/44; existing GFD-29. Physical device/foreground and lifecycle observations remain GFS-U10/11. |
| Later admitted render work → tutorial overlay | Mode and cinematic render service precede HUD render service. Tutorial expiry subtracts a synchronizer-derived render delta; teletype independently reads wall seconds. Tutorial update precedes the HUD-disabled test. A story request before this reader can reach it in the same outer iteration. | GFS-29/GFC-39/40/GFD-34. GFSC-26 records both pause sources, actual delta conversion and text pixels; a logical pause alone does not define every UI clock. |
| Enabled HUD render → radar attention | Component/planet-glyph animation precedes radar-map support and attention pulse/expiry. Attention uses the supplied render delta, with saved record state separate from cloned-glyph flash state. | GFC-42/GFSC-28. GFS-U12 retains full glyph flash activation/waveform and its restore/reset audit. |
| Earlier selected queue read versus later overview read | Selected progress uses retained production countdown state on alternating enabled outer updates, with additional direct selection refresh. Overview has a rolling status budget at its later draw reader; current logical production can reach only an admitted visit. | GFS-31/GFC-43/GFD-35/36/GFSC-29. Preserve immutable publications and ordered authoritative commits; no serial simulation sweep is required. |
| Archive read → staged HUD fix-up → first display | Tutorial and attention fields participate in HUD save/load; three-mode hint arrays participate in player save/load. This does not make advisor/transient/archive text durable campaign state. | GFS-32/GFC-41/GFSC-30. GFS-U11 retains epoch, clearing, pointer and first-prefix/first-consumer observations. |

The readiness audit and proposed acceptance cases live with those rows. Existing
WGP-128/129 request semantics, WGUI-82..86/118 transient rules and GFC-28/29 hint
versus advisor clocks remain authoritative. No retail recording, passing runtime
case or complete load continuation is claimed by this static placement.
Combat cadence follow-up: FS-11..16 and FC-10..36 provide the common first phase,
weapon-specific skips, recharge, tracking, fog/reveal/hide, AI windows, repairs,
launch and object-tail interfaces. FD-09..16 supplies the new dependency intake;
FSC-10..17 are static cases and UFS-08..12 retains lifecycle/runtime limits.
Every observable priority-2 follow-up waits for M2 sign-off before implementation.

## Hazard and capture schedule follow-up

The [frame service schedule](../frame-service-schedule.md#hazard-and-capture-follow-up)
adds FS-24..28, FC-52..56, FD-33..46 and FSC-34..44. The comparison is Skirmish
`fb01953428b7d6a0ebb001c36ed63aa9f294b35a`, where hazard and capture services
exist; WFO-19's earlier absence verdict remains a description of its earlier
audit tree. WHZ owns hazard eligibility/geometry and WBP/WPR own construction
and income. No formulas or the five accepted M2 deviations are reopened.
New observable differences have **implementation: post-M2-sign-off**.

| Dependency | Current remake | Disposition |
|---|---|---|
| Hazard tracking → recipient contact (FS-24, FD-33), dense cells → sampled reveal (FS-28, FD-38) | Copied geometry, partitioned recipient contact and dense-cell reveal | **Internal organization** for unchanged live volumes and identical sampled inputs. Contact age is not a dense-grid or whole-sensor-circle clock. Moving/spawned/destroyed hazard tracking and grid reconstruction remain UFS-23; hazards U-03 stays open. |
| Asteroid damage → affected hardpoint fire (FD-34), nebula entry → pre-nebula object weapon (FD-35) | Asteroid hits after launch; nebula contacts before all fire | **Observable static differences**, FSC-34/35: original asteroid damage can remove a firing point before its tail, while an X-wing's authored object weapon can run before entry. Neither implies a global hazard-before-all-weapons phase. |
| Nebula cancellation / storm refresh → another object's hit (FD-36/37) | Nebula cancellation before early hits; storm refresh after them | **Observable static differences**, FSC-36/37, with qualified damage mode or shield contact and stated registration. Reverse registration is the control. FC-53's five-second geometry window differs from FC-54's shield-service refresh. |
| Storm contact → shield collision surface (FS-25, FD-45) | Collision input consumes carried pool/depletion/contact | **Narrowed, still unverified:** the inspected original shield handler writes contact after recharge without another collision refresh. Other mesh callers and runtime lifetime remain FSC-44/UFS-23; hazards U-02 is not closed by absorption alone. |
| Capture phase / candidate state / contender order → progress and owner (FS-26, FD-39..41) | Global four-frame sweep, post-movement/post-hit candidates, ascending candidate IDs | **Observable static or conditional differences**, FSC-38..40. Normalize each original first due turn and actual query order; no exact stock float-completion frame or a majority-count rule is inferred. Forced transition/alliance/load/child callbacks remain UFS-24. |
| Capture owner → income, human build and AI (FS-27, FD-42) | Transfer before economy/post-world AI; human commands before capture | **Same containing boundaries**, FSC-41. Empty stock pads/merchant do not provide mine income; occupied mining pads retain owner. Custom income recipient remapping and individual AI callback admission remain UFS-26; reuse FD-21 for deferred AI requests. |
| Capture owner / immediate reveal → later firing or fog reader (FD-43) | Owner transfer follows fire; fog publishes late | **Observable static difference**, FSC-42. Isolate relation from fog and keep the existing FD-18 visibility acceptance; no M2 timing exception follows. |
| Stationary captured reveal → next pad-build request (FD-44) | Owner change forces refresh, releases old-team holds and marks new-team holds | **Same / current regression fixed**, FSC-43 and `test_captured_pad_reveals_for_build`. Explains the existing captured-pad build refusal without moving the pad or bypassing fog. **M2 candidate** regression intake only; coordinator decides. |
| Completion notification → mode/story reader (FS-27, FD-46) | Old/new payload accompanies the post-transfer capture event | **Internal organization** for payload-only readers; original completion notifies before owner assignment. Live getter/reentrant listener timing remains UFS-26 rather than an asserted new bug. |

UFS-11 is narrowed by FC-52..55's ordinary contact and capture writes; the
exceptional lifecycle questions move to the explicit plans UFS-22..26.
UFO-03/04 retain exceptional delayed creation and saved-list reconstruction;
this pass adds no new generic tie/restore guarantee. Static cases retain
partitioned preparation, ordered commits and equal 1/2/4/8-worker outcomes as
implementation requirements, without introducing a serial per-object tick.

## Space automatic repair boundary follow-up

The [frame service schedule](../frame-service-schedule.md) adds
FS-30..32, FC-59, FD-48..50 and FSC-47..51 for repair satellites
(legacy EAWR-2275), compared at Skirmish `ee1d29d9`. Fresh debug-build reads
settle the zero initial healing deadline, one-frame poll and 60-frame stock
interval consumed even by an empty scan. They narrow WFO-21's ability tail
without introducing a universal heal/damage phase. No recording is claimed.

| Original edge | Remake boundary | Verdict / qualification |
|---|---|---|
| Combat damage ↔ source's due heal (FD-48) | Early impacts before hull service and systems; no automatic heal consumer at this base | **Missing**. A universal post-impact repair would **differ** in FSC-48: source-before-hit skips the full recipient and ends at hull 80, while hit-before-source ends at 90. Reverse actual registration is the control. |
| Candidate movement/current ownership → source query (FD-49) | Committed movement precedes systems | **Missing** consumer; a later-moving target can miss this original due query and wait a full interval (FSC-51). Equal-health spatial ties and exceptional owner resets remain UFS-28. |
| Healed hardpoint/hull → recipient cap and hardpoint tail (FD-50) | Hull service before systems | **Missing** consumer. FSC-50 varies recipient/source registration to expose same-frame versus next-frame input age; paid repair and shield recharge keep their own rules. |

Ordinary construction/front insertion supplies a new repair source's first
turn on the next traversal, with no full-interval healing warmup (FSC-47).
Exceptional insertion, complete ownership/ion-stun/disable producer coverage
and restore/pause timing retain UFS-28's per-tick rig plan. Any implementation
keeps partitioned staging and ordered cross-object commits equal on
1, 2, 4 and 8 workers; research publication alone changes no simulation phase.

## Ordinary object turret boundary follow-up

The [frame service schedule](../frame-service-schedule.md#ordinary-object-weapon-turret-pointing-follow-up)
adds FS-33, FC-60/61, FD-51/52 and FSC-52..55 for defence satellites
(legacy EAWR-1172), compared at Skirmish `8199d00c`. Fresh debug-build reads
settle **targeting-turn pointing independent of weapon countdown**, followed
by the later ordinary weapon's consumption of aiming/ready state. The
stock satellite's separate turret turn supplies idle/rest work without an
extra tracking adjustment. This narrows WFO-18 and UA-05; it does not change
FC-36's hardpoint tail or authorize a general object-order rewrite.

| Original edge | Remake boundary | Verdict / qualification |
|---|---|---|
| Targeting angle/readiness → later object weapon (FD-51) | `targets()` before `launch_shots()`; ordinary object turret consumer absent at the comparison base | **Missing** consumer. FSC-52 requires turning during recharge/pulse delay; FSC-53 permits a same-frame shot when the final pointing step reaches readiness. Pointing only inside a due firing attempt, or after fire, fails these witnesses. |
| Other object's pose/relation → pointing (FD-52) | All movement committed before targeting/fire | **Observable static difference**, FSC-55, when the target originally moves after the shooter. The broader inter-object order remains **implementation: post-M2-sign-off**; the defence-satellite task consumes the local cadence/readiness rule. |

FC-61 records delayed return to rest with strict elapsed-seconds admission.
UFS-29 retains runtime and complete ion-stun/disable/ownership/construction,
pause and restore qualification. No retail capture or universal negative
lifecycle claim is made; implementation retains partitioned work and equal
1/2/4/8-worker outcomes.

## Peacebringer charged-shot boundary follow-up

The [frame service schedule](../frame-service-schedule.md#peacebringer-charged-special-follow-up)
adds FS-34/35, FC-62/63, FD-53..56 and FSC-56..60 for the charged SPECIAL
shot (legacy EAWR-946), compared at Skirmish `9bccc30f`.
Fresh debug-build reads settle the ordinary ability-tail-before-hardpoints
boundary and consumption inside projectile creation. They narrow WFO-21/23,
heroes WHE-35/36 and area-damage WAD-34..36; no recording is claimed.

| Original edge | Remake boundary | Verdict / qualification |
|---|---|---|
| Charge completion → same-turn SPECIAL attempt (FD-53) | `launch_shots()` before `systems()`; charged-state consumer absent at this base | **Missing**. FSC-56 exposes completion placed only after fire: the original's 150th admitted poll can feed that frame's later hardpoint attempts. |
| SPECIAL creation/spent budget → later hardpoint attempt (FD-54) | Shared charged-shot consumer absent | **Missing**. FSC-57 exposes a frame-frozen charged flag: the final charged creation immediately permits a later ordinary weapon, without waiting for presentation cleanup. No per-gun already-fired latch. |
| Zero shared budget → charged presentation cleanup (FD-55) | Charged presentation service absent; systems follow launches | **Missing**. FSC-57/58 expose cleanup after firing in the same frame: the original cleans on the next admitted ability poll, including completion with no eligible guns. |
| Another object's gun loss/disable → completion snapshot (FD-56) | Early impacts globally committed before launches | **Observable static difference**, FSC-60, for a damaging object originally serviced after the parent. This broader object-order question remains **implementation: post-M2-sign-off**; it does not authorize a global combat phase rewrite. |

UFS-30 retains runtime normalization and complete indirect cancel/disable,
stun, death, post-count gun loss, owner, pause and restore producers.
Weapon permission and ability admission remain separate gates. Implementation
uses partitioned inputs/staging and ordered commits with equal results on
1, 2, 4 and 8 workers.

## TIE pulse and muzzle boundary follow-up

The [frame service schedule](../frame-service-schedule.md#tie-pulse-countdown-and-muzzle-follow-up)
completes FC-11's ordinary TIE boundary with FS-36, FC-64/65,
FD-57..59 and FSC-61..64, compared at Skirmish `ac8a838c`.
Fresh debug-build reads confirm that a refused first pulse retries while a
refused begun second pulse keeps its already-written full recharge. Taking
a muzzle advances its cyclic index before cone refusal; earlier permission
refusal does not. These ordinary clock/cone boundaries agree with the base's
weapon service. Current targeting-before-launch phase direction agrees,
but the explicit ready-state input and extra selected-member range check
retain the known WWP-43/WDF-14/16 difference (FD-59).

FD-58 distinguishes that muzzle-state agreement from WWP-49's conditional
parent-targeting reference, which the target-reference correction owns.
FD-31 still qualifies target/container input age. UFS-31 carries the retail
Lua-debugger capture plan for per-service clocks, muzzle/target selection
and actual previous pose, rather than a guessed explanation or tuned rate
for the remaining dogfight shot deficit (legacy EAWR-1192, EAWR-2379).

## Waypoint arrival boundary follow-up

The [frame service schedule](../frame-service-schedule.md#waypoint-arrival-and-repeating-route-follow-up)
narrows WFO-27 and the ordinary queued-point part of FD-29/UFS-16 with
FS-37..39, FC-66..69, FD-60..64 and FSC-66..70. Ordinary movement and
targeting already ran when a formation consumes path completion, removes
the current point, returns a repeated leg to the queue tail and attempts
the next plan. Its one-second completed-range check is a separate gate.
The new plan does not cause a second object turn in that frame.

| Original edge | HUD completion seam | Verdict / qualification |
|---|---|---|
| Member arrival → next leg (FD-60) | Post-movement readiness, late ordered plan from tick+1 | **Same phase direction** for one layered ship; exact accepted/staggered start remains UFS-32. |
| Formation completion → its next leg (FD-61) | Partitioned per-ship readiness with stable independent next-leg admission | **Same**, FSC-67: a finished ship advances while another formation is still moving; executable HCB-59 fast/slow control. |
| Selected coordinator → unselected formations' queues (FD-62) | Queue retained only on addressed units | **Observable static difference**, FSC-67: a queued point can reach unselected formations of the reused original coordinator. |
| Current targeting → later point rotation (FD-63) | Targeting/fire precede late queue rotation | **Same phase direction**; internal organization is closed when intermediate queue state has no consumer, FSC-68. |
| Failed next-leg search → retained retry (FD-64) | Exhausted planner search propagates a failed step | **Missing retained retry**, FSC-70: original failure keeps the point and needs elapsed frames strictly greater than 2 x FPS before another initialization search. Shared planner follow-up (legacy EAWR-2406), separate from HUD ownership. |

These comparisons use HCB-59 at `2ff89ef0`, not the earlier queue-free
Skirmish base. They supply the named HUD waypoint seam; they do not reopen
generic object ordering or command latency. FSC-69 distinguishes accepted
replacement/Stop disconnection from member removal that retains surviving
destinations. FC-69 records a conditional queued-path start-delay write,
narrowing PC-02's broad tag exclusion; its actual first-motion consequence
remains UFS-32. Runtime phase, path acceptance and exceptional failure/reset
lifecycle remain UFS-32/33. No simulation code changes follow
from this research publication itself.
