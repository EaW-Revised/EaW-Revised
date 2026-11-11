# Tactical service ordering and frame clocks

## Applicability

Design contract for the tactical scheduler (legacy EAWR-6418), based on the
[frame service schedule](frame-service-schedule.md) FS-45/46, FC-77/78,
FD-75/76 and FSC-80..82, and the
[frame-order walk](walks/frame-order.md) WFO-11/12/15/20..24.
The debug build establishes the bounded producer/reader relationships below.
The proposed scheduling, persistence and validation interfaces are project
decisions, not claims about the original's implementation.

At comparison base `4c7035bbc3c83122207d0e01ed98d6ec4f76a02c`,
`src/sim/tactical/session_step.cpp` admits global projectile work before
the ordinary systems tail. `session_world.cpp` derives recipient jamming
from current emitters while preparing a combat snapshot. A single immutable
snapshot for both sides of every service boundary cannot satisfy FD-75/76.
Moving every automatic handler before or after every projectile does not
satisfy them either.

Evidence ETF-01..05 is a fresh read-only debug-build index of dispatcher,
emitter, recipient-stamp, object stamp-forwarding and projectile reads.
The existing schedule supplies service-registration insertion semantics. No new
runtime recording, measured performance or complete retained-child clock
continuation is claimed.

## Ordering contract

| ID | Required observable relationship | Source / status |
| --- | --- | --- |
| TFA-01 | Preserve an object's service-registration position independently of its entity ID. Inside that turn preserve authored periodic-behaviour order, then model work, then effective declared special-handler order, then object Lua and the hardpoint tail. Admit each handler through its own enabled/due gate. A projectile's movement readers occupy that projectile's service turn. | Debug build: FS-45/46, WFO-11/12/15/20..24; ETF-01/02. |
| TFA-02 | A projectile reads emitter registration/counter and recipient stamp as of its reader position: all preceding relevant writes are visible and following writes are absent. A source's scan publishes recipient stamps directly; the recipient need not take its own turn first. Emitter termination removes its defence registration independently of any recipient stamp already written. | Debug build: FD-75/76, FC-78; ETF-02..05. |
| TFA-03 | Retain service registration and projectile-defence registry order as distinct state. The former places object turns; the latter orders the emitters examined by a projectile. Handler declaration order is a third order within one source. Preserve ordinary remove/add and front-insertion semantics; creation/ID sorting cannot reconstruct these orders. | Debug build: FS-06, FS-45/46, WFO-12; ETF-04/05. |
| TFA-04 | Treat a handler's outer deadline, its private counter/deadline, emitter registry membership and recipient stamp as separate state. A due automatic jammer poll refreshes 10, stamps recipients and ends at 9; it reschedules once for supplied mode frame + 1. A skipped frame causes no catch-up polls. Other automatic kinds keep their own sourced cadence. | Debug build: FC-24/59/77/78; ETF-01/02. |

The ordering key must describe manager pass, current service-registration
position, local service position and declared handler position, with a stable
sequence for multiple effects at that position. It is not a permanent ordinal
assigned at creation. Re-registration changes future placement. Distinguish
registration from eligibility in the already-started traversal: ordinary
front insertion during that traversal waits for its next pass (WFO-12).
An immediate activation before traversal can publish registration before its
first periodic poll. Exceptional insertion/restore producers remain unknown.

## Partitioned execution and ordered publication

TFA-05 is a **project architecture decision**: retain copied immutable inputs,
disjoint partition outputs and ordered commits. Describe service events with
their semantic order and relevant reads/writes: emitter membership/count,
recipient stamps, position/ownership/nebula state, projectile flight/liveness,
and damage or removal that can affect a later service. Include hit-driven
reactions at hit delivery, rather than registering them as periodic handlers
(FS-46). Independent work can execute in the same partitioned batch.

A later event that reads a preceding write must prepare its immutable input
from the staged state *after* that write commits. Use dependency batches or
versioned input views; a commit-sort alone cannot repair a result calculated
from the wrong version. Spatial queries must cover every possible affected
recipient for that batch, including movement across query boundaries.
Incomplete dependency discovery must use a conservative dependency, not
silently reuse an old view. Conflicting writes and synchronized random draws
resolve in semantic order; parallel preparation must not consume extra draws.

This permits parallel work on unrelated objects and queries while ordering
only the necessary publications. A dense dependency chain can require several
batches. It does not justify replacing the tick with a serial per-object
simulation loop. An implementation must explain dependency coverage and count
work deterministically; no speedup or retail performance parity is established
by this design note.

TFA-06 is a **project transaction decision**: intermediate commits above are
private to the staged tick. Publish the completed session only after every
batch succeeds. A failure discards stamps, registration changes, deadlines,
damage, RNG state and clock admission together. A reader cannot observe a
half-completed tick through a public session snapshot.

## Two frame clocks

| ID | Input or retained state | Contract |
| --- | --- | --- |
| TFA-07 | Mode frame, synchronization frame, logical FPS | Supply both frame labels explicitly for an admitted service context. The containing mode supplies the jammer scan's stamp and the handler's next due frame. The stamp reader uses the synchronization frame. Keep clock roles distinct even when standalone inputs have equal values. Source: FC-77/78, ETF-01..04. |
| TFA-08 | Recipient's last jamming stamp | Preserve the written mode-frame value independently of emitter lifetime and current geometry. A negative stamp does not qualify. For a valid stamp, use FC-78's strict comparison: synchronization-frame age in logical seconds is less than effective `Sensor_Jamming_Time`. At aligned 30 FPS and 0.5 seconds, ages 0..14 qualify and 15 expires. Source: FC-78, ETF-02/03. |
| TFA-09 | Replay, checkpoint and retained-child admission | Project decision: persist stamps, their clock context, both clock inputs or enough authoritative recorded admission state to reproduce them, service/registry order, handler order/state/deadlines and synchronized RNG state. The parent/standalone admission adapter supplies clock labels; an automatic ability must not infer them from completed tactical ticks or apply a private campaign offset. Include these inputs in deterministic replay/checkpoint validation. |

The completed tactical tick remains a simulation progress counter; it is
insufficient to reconstruct both external labels after a retained-child
transition. Paused admission and a clock-only parent operation must be
represented explicitly by the host interface. They must not manufacture
extra object services, refresh counters or clear stamps. Host clock advance,
pause and restore mapping require measurement before claiming retail parity.

Do not reset recipient stamps when a source leaves range or terminates, and
do not convert them into a freshly recomputed boolean. Do not substitute the
emitter's ten-count refresh for recipient lifetime. Clock origin, signed
subtraction, wrap and a synchronization frame earlier than the saved mode
stamp need explicit implementation review and runtime qualification; this
note supplies no clamp, offset correction or wrap policy for those cases.

## Synthetic acceptance cases

These are required future implementation tests, not recorded observations.
Hold unrelated defences and randomness fixed; record both clocks and actual
service/registry order. Run every case at 1, 2, 4 and 8 workers and require
equal authoritative state, ordered events and replay/checkpoint continuation.

| ID | Setup and expected result |
| --- | --- |
| TFC-01 | Repeat FSC-81 with aligned frame 15 and last stamp 0. Source then missile yields stamp 15 before the path reader and direct flight. Missile then source reads expired age 15 and chooses guided flight. Choose geometry where paths visibly diverge. Reverse registration without changing IDs; both orderings must survive serialization. |
| TFC-02 | Use a registered positive-count emitter, no passive-shield fallback and a source turn that terminates in nebula. A projectile read before termination sees the earlier emitter state; after termination it sees no registered emitter. A recipient stamped before termination retains its own lifetime. This distinguishes registry removal, counter changes and recipient expiry (FSC-81/82). |
| TFC-03 | Use a synthetic source with two distinguishable due handlers. Reverse only their effective declaration order and verify the corresponding ordered effects; skip one through its own due/enable gate. Preserve the same source/projectile order. No global ordering by ability kind may override the data (FS-45). |
| TFC-04 | Poll an automatic jammer at mode frames 100 and 101: each admitted ordinary poll ends at 9 with next due 101 and 102 respectively. Omit service at 102 and resume at 104: one poll, next due 105, no catch-up. Disabling service preserves the distinction between removed registration and the still-aging recipient stamp (FC-77/78). |
| TFC-05 | With no further producer, keep stamp 100, FPS 30 and lifetime 0.5. Mode frame 101 / synchronization frame 114 still qualifies; the same mode label / synchronization frame 115 expires. Vary only the mode label while retaining the stamp and synchronization frame: the reader's result stays unchanged. This is a synthetic unequal-clock input, not a measured campaign offset. |
| TFC-06 | At mode 100 / synchronization 110, an admitted source writes stamp 100. An immediately later reader sees age 10, not age 0. Remove the source and advance synchronization to 114 then 115 without new stamps: retain then expire. An implementation that stores synchronization 110 as the producer stamp fails this case. |
| TFC-07 | Save a completed boundary after a stamp/registration change; reload with explicit continuation clock inputs and compare uninterrupted execution through expiry and re-registration. Inject a worker failure before final publication: retry must yield the same clocks, stamps, orders, deadlines and RNG as uninterrupted success (TFA-06/09). |
| TFC-08 | Create an ordinary newly registered source during an existing manager traversal. It gets no periodic poll in the already-started pass; compare activation before traversal. Repeat explicit remove/add versus redundant registration (FS-06, WFO-12). Do not manufacture an extra service by rebuilding order between batches. |

## Unmeasured boundaries

UFA-01 retains UFS-38's queued-rig plan: actual construction first-poll rank,
source/projectile service ranks, nebula exit re-registration, every relevant
owner/death/enable/restore producer, and counter versus presentation changes.

UFA-02 requires a retained campaign-child and uninterrupted standalone
control. Record parent/child mode identity, mode frame, synchronization frame,
stamp writes and reader results across child entry, pause/resume, return,
re-entry and save/load. A picture or final boolean alone cannot establish
clock origins, advance policy or normalization. Negative age and wrap remain
unqualified. Synthetic unequal-clock tests validate the interface without
claiming that a particular offset occurs in retail.

UFA-03 requires implementation measurements of dependency discovery cost,
batch count and invalidation work on sparse and dense synthetic scenes.
Keep deterministic work budgets and equal 1/2/4/8-worker outcomes. This design
closes the shared interface question; simulation integration and measured
retail continuation remain implementation and research work respectively.
