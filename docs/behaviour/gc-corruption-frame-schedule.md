# Galactic corruption and hero service schedule

## Scope and evidence

S5 preparation, research dated 2026-10-09. This is a consumer map of the
[galactic frame schedule](galactic-frame-service-schedule.md), not a second
outer scheduler. It covers corruption, prepared galactic actions, hero absence
and replacement, and the presentation consumers WGP-19..22 in the
[presentation walk](walks/gc-presentation.md). Arithmetic and actor admission
remain in the [corruption/heroes walk](walks/gc-corruption-heroes.md) and the
[movement/control walk](walks/gc-movement-control.md).

Sources are static **debug build** readings and explicitly cited earlier rules.
There is no retail recording or runtime equivalence claim. Logical galactic
frames, synchronization service arguments, render frames and fiscal cycles are
distinct clocks. A retained galaxy is not serviced alongside its tactical child.

The comparison base is `d7180d985ebf9b85c421ef3dc9d6a09234652296`.
The inspected `src/sim/galactic/session.cpp`, `commands.cpp`, movement,
observer and income services have movement/visibility corruption booleans and
some arithmetic interfaces, but no complete corruption transition/action host
or due-hero creation pass. `src/campaign/setup.cpp` has a multiplayer starting
hero placement provider; it is not a respawn service. Existing walk-wide
"missing" counts describe their historical bases, not every current component.

## Order rules

Rows inside object service apply to that object's admitted turn. They do not
authorize a new serial world sweep; independent work uses immutable inputs,
disjoint staging and ordered commits under ADR-009.

| ID | Rule and first consumer | Evidence |
| --- | --- | --- |
| GFS-C-01 | Synchronized command execution precedes ordinary object service. Prepared action execution resolves persistent actor/target identities and performs its action-specific checks/mutations there; preparation in a local menu is not a debit or transition. There is no universal atomic transaction: corruption can debit before a prerequisite recheck, while ordinary sabotage pays only after successful target removal. | GFS-01/04; WGCH-05/09/29/30; WGEP-80 |
| GFS-C-02 | At an admitted planet turn, corruption completion precedes control and allegiance. It commits current type and clears the transition before story/sound, then faction upgrades, one-time money, tactical rewards, unlocks, ability callbacks and revolt. Those direct consumers see the new state. A listener notification does not establish immediate resumption of every Lua thread. | GFS-07/23; GFD-28; WGCH-12..17 |
| GFS-C-03 | Ability service is local to an unpaused object's turn, after ordinary behavior/model work and before its object Lua. Declared enabled handlers need positive service intervals and due deadlines. Arrival, corruption change, owner change and source destruction can invoke distinct synchronous ability routes outside that periodic call. No global hero phase or universal action cooldown follows. | GFS-19; WGM-14/34/83; WGCH-16/68/70; WGUI-101 |
| GFS-C-04 | Scheduled hero creation is after ordinary objects and before delayed creation/conflict/fiscal processing. It requires a selected galactic mode and unpaused gameplay. Stored-order due records can place before the later fiscal owner/source census, but cannot retroactively enter an already completed ordinary object turn. Newly attached-object first-service order remains a runtime question. | GFS-10; GFD-22; WGCH-43..47; WGM-132..135 |
| GFS-C-05 | Fiscal collection reads the state left by object/action/creation work. Recurring corruption income, siphon expiry/payment and accrued piracy use their own readers; transition completion's one-time reward is earlier and separate. Player AI and story processing are later consumers, while direct notifications can already have run. | GFS-11..15; GFD-21/28; WGEP-04/12/13; WGCH-14/60 |

## Cadence and reset rules

| ID | Rule, initialization and due boundary | Reset, skip and unresolved boundary | Evidence |
| --- | --- | --- | --- |
| GFC-C-01 | Fresh corruption has current/target none, inactive increase/removal, and zero start/end/last-change frames. Interval-1 planet service tests active transition at current frame **>= end**. Begin stamps current frame and end = start + **truncate(seconds × logical FPS)**. | Begin rejects an active transition; increase needs current none, removal current nonempty. Exactly zero removal substitutes **0.1 s**, unlike increase. Completion clears flags/start/end/target and stamps last change before rewards. Pause skips ordinary progress. | GFC-26/31; WGCH-11/12/51 |
| GFC-C-02 | Direct corruption actions and successful tactical corruption requests use **5 s**; ownership-triggered transitions use **10 s**. Hero removal uses the acting ability's `Corruption_Removal_Time_In_Secs`, with constructor default **10 s**. These are separate one-shot deadlines. | Ownership interruption follows WGM-77, and ruins destruction aborts active transition and applies old-state cleanup. Do not replace an interrupted transition with an automatic reward-bearing completion. Exact pause/ownership/restore ties require the probes below. | WGM-77/81; WGCH-08/09/19/67; GFC-26/31 |
| GFC-C-03 | Hero scheduling initializes deadline = galactic frame + **int(seconds × logical FPS + 0.5)**. Nonpositive requested seconds select `Default_Hero_Respawn_Time`, stock **360 s**. This rounded conversion differs from corruption's truncation. | Eligibility, duplicate and parent/team checks precede scheduling. Forced exact-type respawn changes existing deadlines to current frame; it does not place immediately. Failed placement retains an already due record; success erases. No owned planet or an invalid backing-structure count can erase without creation. | GFC-25; WGM-78..80; WGCH-43..47 |
| GFC-C-04 | Action-specific actor absence is prepared or supplied by that action, rather than the corruption duration or generic death timer. Neutralization prepares two timers; theft can double its prepared timer on one failure result; black-market arrays use buyer displayed tech minus one. | Zero/invalid black-market or theft range yields zero, which later enters the hero default-time rule. Hack success destroys its actor without scheduling; Lua `Despawn` is a separate destruction interface. Generic consumed abilities schedule with their absence getter before destruction. | WGCH-21/35/36/38/66/69/70/71; WGEP-81 |
| GFC-C-05 | Fiscal period is authored `Fiscal_Cycle_Time_In_Secs`, stock **45 s**. Siphon duration is expressed in fiscal cycles. A noncorrupted planet tests siphon expiry at frame **strictly greater** than end; corruption bypasses that expiry test. | An income preview does not consume piracy accrual; actual collection does. Action deployment, transition removal, source termination and fiscal collection are distinct boundaries. A newly acquired conflict pause can still permit already due fiscal payment; final clock advance independently rereads pause. | WGEP-02/04/12/13; GFS-20; WGCH-39/40/60/67 |
| GFC-C-06 | Generic ability periodic admission requires a positive interval and due deadline; it is not evidence for an absence duration, corruption deadline or fiscal period. | Construction/attach/enable/disable/load of each action's periodic handler is not completely qualified here. Retain the handler-specific seam; do not invent a shared 1-frame or 1-second hero cadence. | GFS-19; GFC-04/19; GFS-U04/08 |
| GFC-C-07 | Line growth starts at sentinel -1; its first valid emitter update initializes 0. Each valid line/group update submits the previous percentage, then clamps an already >=1 value to 1 or adds `1/(Corruption_Line_Grow_Seconds × logical FPS)`, stock 1/210 at 30 Hz. Paths and attached planet particles instead read the transition fraction. | Forced emitter transform reset recomputes basis/height without resetting an already nonnegative growth percentage. Missing emitter/group or unresolved neighbour skips growth. Save/load, pause-admitted presentation and exact last increment require C-06; the 7 s line growth is not the 5 s corruption deadline. | WGP-47..49; debug build S5P-01..03; WGP-03 |

## Qualification cases

These are recipes, not passing tests. Frame-service ownership stays with
GFSC-17/23/25; the cases specify the S5 actor/state inputs that those probes
need. Use one selected galaxy clock, persistent identities and recorded seeded
draws; include an uninterrupted control and a save/load continuation.

| Case | Fixture and observations | Expected static boundary / residual question |
| --- | --- | --- |
| C-01 Transition due with fiscal | Start corruption ending exactly at frame t with fiscal due at t. Stop immediately before/after transition, effect callbacks, control and fiscal; repeat removal and corrupt-militia revolt. | Current type is committed and reward dispatched before control/fiscal. Actual listener/source cleanup and resulting owner census remain to record. |
| C-02 Pause, interruption and child | Pause at end-1 and at end, nest two pause owners, enter/return from a tactical child; change owner or destroy planet just before due. Record flags, start/end/last-change, rewards and source modifiers. | Ordinary paused/retained-parent progress is skipped; ruins aborts. Establish exact first eligible completion and absence of duplicate callbacks after load. |
| C-03 Due heroes | Two equal-deadline records and equal-distance destinations in both insertion orders. Lose/destroy the scheduled planet, fill ground capacity, add blockade, then remove all owned planets or backing structures. Record primary/secondary results and container/member identities. | Primary success consumes record; secondary-only success does not. Placement tie order, first newborn service and full restoration are unqualified. WGM-133/134 already sources authored companions. |
| C-04 Prepared actions | Open a menu, then change actor owner, target allegiance/corruption, money, selected structure/hero or siphon identity before sending. Record payload, re-resolution, debit, attribution, result and actor absence. | Preserve action-local mutation order. Static execution gates do not prove a stale menu can be submitted through ordinary UI. |
| C-05 Arrival and consumption | Move ability-dependent heroes to final and intermediate planets, with/without conflict, instant-land and raid requests. Compare direct arrival action, consumed probe, neutralization, ordinary death and Lua despawn. In a land takeover without remaining conflict, put consecutive stealth/noncombat transports on the surface and record evacuation order. | Record membership/link callbacks, ability invocation, timer owner and cleanup; no universal despawn/schedule policy. WGM-14/34 owns movement gates; WGCH-79 sources surface-to-orbit evacuation without hero recreation, retaining its forward-list mutation qualification. |
| C-06 Corruption presentation | Two adjacent planets, increasing then completed corruption and removal; overview/focus/pause/child-return/save. Record type/target, progress, particle parameters, endpoint geometry and line age. Trigger sabotage and hack separately. | WGP-19..22 owns consumer admission; qualify visible interpolation, growth and one-shot routing without using appearance to infer simulation state. |

## Implementation ownership

Corruption state/effects use existing work (legacy EAWR-1140, EAWR-1141, EAWR-1130).
Neutralization/sabotage use existing action work (legacy EAWR-1142); theft/siphon/spy
use existing deployment work (legacy EAWR-1143). Hack/destruction/protection use
existing special-action work (legacy EAWR-1144). Hero scheduling and stock due
placement retain return ownership (legacy EAWR-1116, EAWR-2486); presentation retains
corruption visual ownership (legacy EAWR-2003). This map adds qualification seams,
not duplicate implementation owners. Every later consumer needs its own feature
case, checkpoint continuation and deterministic 1/2/4/8-worker result.
