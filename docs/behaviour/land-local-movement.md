# Land squad following and retained contacts

`LandMovement` is a land-only service beside `LandPathService`.
The terrain-backed tactical host calls it for land move/stop orders and owns
an optional land archive section, as described below. Space state,
checkpoints and replay bytes remain unchanged.

A squad owns one accepted L4 path and an ordered tracker. Members retain their
L2 company profile, locomotor identity, effective instance locomotion values and
forward/side slot offsets. Slot destinations are inputs from the placement and
formation adapter, rather than independent per-member searches. Path failure
cannot install a route or consume an order. Replacement requires the path's
start to match the current squad center.

The caller supplies seconds per admitted simulation step. Parent motion first
produces immutable squad proposals; members then write disjoint proposals in a
second barrier of `land-member-proposals`. The ordered commit visits members
by ascending object ID and publishes a complete L1 occupancy snapshot while
preserving unrelated scenery. Both proposals and the L1 commit must succeed
before physical state or the logical tick advances. This is the standalone
integration seam for LR1-03/D5: ordinary movement runs once before late contact
and coordinator work. It does not invent a second movement turn after contact
publication.

Member speed uses LR3-14/15: the inclusive one-unit longitudinal deadband,
linear lag blend over ten units, quadratic overshoot blend, current maximum
clamp, separate acceleration/deceleration and zero requested speed while
turning. Lateral steering clamps the error/10 contribution to half. Facing
uses a shortest wrapped yaw error and the instance turn budget (LR13-23).
WLPA-30 supplies parent slowdown beyond rear extent plus two units. Static
cell/footprint validation covers crossed cells, including conservative diagonal
corner rejection, before positions are committed.

Walk start/stop, turn, deployment and undeployment remain explicit member
states. `LandMotionGates` supplies start/stop/turn almost-finished predicates,
permitted stop points, attack-animation movement admission, turret alignment
and deployment completion. Animation transition/deployment frame inputs also
round trip. These are deterministic caller inputs, not wall-clock or guessed
clip durations. The R13-17..21 asset/animation adapter and full stride-aware
stop-velocity policy remain integration work; this service represents their
gates without claiming a complete animation player. Deployed movement and
terrain body pitch/roll are outside this first following service.

Yield, recovery, dropped and reached are distinct order states (LR9-07/08).
Recovery accepts a strictly increased progress observation for the retained
blocker identity; elapsed frames never recover or drop an order. Pair contact
qualification and temporary routes are described below. A path's last tracker alone is insufficient
for reached: every member must settle at its slot and stop.

The service publishes each member's cell membership with zero additional restriction
bits; members do not become static pathfinding obstacles to themselves or
their own squad. Swept pair admission and copied local-path exclusions own
dynamic collision restrictions. Static
scenery restrictions remain effective. No zone flood fill is called during
movement. Proposal and membership buffers retain warmed capacities. Typed
`Checkpoint`/`from_checkpoint` includes every authoritative field; derived
partition/order buffers are rebuilt on restore.

`land_movement_contracts` covers obstacle following, slot settlement,
1/2/4/8-worker and reverse-partition equality, arbitrary-tick checkpoint
continuation, speed boundaries, animation gates, explicit yield/progress/drop,
and warmed heap allocation. It prints a synthetic 882-member stress timing for
each worker count. This is a service microbenchmark, not the session
`--serial-profile` harness. Its zero-radius members skip swept-pair checks and
its timed loop omits contact service, so these numbers are not comparable to
the radius-one session profile below.


## Late contact service

The standalone caller invokes `service_contacts` after ordinary `step`, with an
explicit synchronization frame and copied `LandContactSettings`. The settings
keep prediction seconds, minimum prediction distance, complexity quota and
reset interval separate; no stock defaults or unit-name list is substituted.
LR1-09 orders quota reset, retained immediate contacts, scored requesting
contacts, resolved-contact cleanup and finally discovery from committed L1
occupations. A duplicate or older synchronization-frame call does no work.
Discovery visits ascending member IDs, reads immutable poses/occupancy, and
writes disjoint per-member lists in `land-member-proposals`; ordered reduction
retains canonical member-pair identities. There is no second physical turn
following a late installation (LR1-03/D5).
Retained pair subtypes refresh from the live moving/stopped inputs before any
dispatcher runs, so a stopped pair can become a moving/stopped maneuver without
losing its canonical identity. A subtype change resets maneuver qualification
to the new dispatcher's initial state and counter 30; progress baselines refresh
on every visit, including states that return without searching.

`LandContactKind` separates terrain, stopped/stopped, moving/stopped and
moving/moving dispatch. `LandContactState` retains the LR3-08 / WLPA-56 state
inventory. Implemented producers cover initial, resolving, resolved,
unresolvable, retry-later, path-around, existing-path, pass, follower-speed,
same-direction-speed, offset-exchange, deadlocked and recovery. Stopped pairs
retain retry-later without inventing a repush distance. Repulsion, attack,
infantry-push and no-action remain named states for qualified later producers;
listing them does not claim their complete retail branches are implemented.

LR3-09 prediction is rechecked before maneuver choice. Compatible distinct
same-formation slots keep their offsets; incompatible slots choose follower
speed roles by longitudinal slot order and member ID. Same-direction pairs
cap the trailing squad to the leader's maximum speed. Opposing pairs choose
primary by collision class, formation length, higher first-member ID, then
lower maximum speed when both formations are unavailable (LR3-10). Ordinary
path-around routes the yielding actor; pass and deadlock escape route primary.
Pass admission, offset compatibility, path acceptance, collision lock and the
two escape-direction admissions are explicit instance inputs. Crush/flee and
attack decisions are outside this service.

Requesting contacts sort by ascending distance score, with canonical object
IDs supplying a total-order tie break. Distance is the alternative planner's
explicit score, not a claim to have recovered every retail virtual score.
Admission is strictly `spent < quota`; zero and equality refuse. A refusal
retains ordinary state service and is separate from a completed no-route.
Reset interval below two clears each admitted late invocation; other intervals
clear only at synchronization-frame residue zero (LR3-01/06/07).

L4 searches receive copied hard counterpart cells and soft other-mover cells.
The first pass excludes soft collision cells; fallback permits them while
retaining hard exclusions. Each attempted pass charges the entire cell graph
before checking maximum speed, including zero-speed attempts (LR3-02/03).
An admitted request, including role alternatives, is not interrupted or gated
again internally. The ledger uses alternative cell-graph node counts, separate
from expansion work; it does **not** treat authored retail visibility-graph
node quotas as numerically equivalent performance limits. Graph building and
search remain synchronous event work, not a universal per-frame search cap.

A temporary result needs more than one node, subtype admission and compatible
locks before installation. Base path, temporary path, contact identity,
tracker and destination remain separate. If the counterpart occupies the base
destination, the alternative planner selects a lateral temporary endpoint
using grid pitch and the pair radii, then resumes the retained base tracker.
Finishing that lateral path cannot mark the base order reached. Search success,
installation and later observed motion are independently retained (LR3-13).
Another contact involving an already-routed squad observes existing-path rather
than replacing its lock. Replacement/drop invalidates related contacts and
routes (WLPA-45). No zone-cache preparation runs in either movement phase.

LR3-11 deadlock tracks remaining qualifications, initially/reset 30, and a
recorded pair member. A qualified visit requires valid pair inputs, no observed
pair progress, assigned speeds below positive acceleration thresholds (0.05
when nonpositive), and collision-modified speeds below those thresholds.
Nonqualifying visits preserve the counter; elapsed frames are never an input.
At exhaustion, LR3-12/13 counts only retained moving-pair deadlock sources.
Exactly the counterpart permits escape: primary may move and the counterpart
stops, including when the queued-search quota denies new ordinary requests.
The first qualified escape retains that ordinary-service exemption. After a
failed escape, subsequent attempts join the scored quota queue, with the retry
admission state retained through checkpoints. This alternative-planner work
bound prevents repeated full searches on every unchanged stalled visit; it
introduces no elapsed-time timeout or invented periodic retry clock.
Facing difference of at least a quarter turn selects the opposed escape
admission; aligned and opposed options are retained distinctly. Recovery keeps
its temporary tracker and speed roles until completion or invalidation.

Physical pair guards evaluate relative swept proposals in a partitioned phase,
then stop blocked members and parent trackers in ordered commit. A current
overlap may separate, but a new movement cannot deepen it or cross through the
other proposal. WLPA-60 qualifies one infantry exception (a stop-gap until the contact selector reads the authored collision class that members now carry, WLPA-73): an existing moving/stopped
repulsion pair is soft for the active mover's dynamic sweep. Its local avoidance,
terrain/animation gates and idle peer pose remain effective; all other pair guards
retain the conservative rule. This bounded exception follows the inspected retail
member displacement chain, which writes its proposed XY position without a
dynamic pair-sweep veto. Canonical contact lookup is performed only for a proposal
that would otherwise be blocked. The conservative alternative-cell-planner policy does not
claim retail multi-contact arbitration is fully recovered. Buffers retain
high-water capacity: ordinary warmed motion, retained state service and
discovery allocate no heap storage. New contacts and route-search/install
**events** may grow retained storage; a search allocates its owned request and
result while the L4 arena reuses search scratch.

WLPA-68/79: a path accepted while the squad is not moving first puts it in a saved
walk-turning state (the parent rotates at the slowest member turn rate with no
translation; a squad with a non-positive member turn rate never enters it). Members
mirror the state, and slot correction or avoidance steer facing without zeroing speed.

All settings, quota epoch/charge, contacts, counters, roles, locks, speed limits,
blocked terrain target and ordinary/temporary trackers are typed checkpoint
fields. Restore validates canonical identities, states and trackers and rebuilds
scratch only. The new `land_contact_contracts` covers quota equality/reset,
two-pass accounting, zero speed, deterministic role choices, prediction recheck,
terrain and stopped-pair dispatch, lock rejection, dynamic-cell search restore,
qualified deadlock escape and moving/stopped passage. An eight-squad,
sixteen-member chokepoint run compares trajectory traces and full checkpoints
on 1/2/4/8 workers, reverse partitions and seven restore boundaries, printing
per-worker timings. `land_movement_contracts` additionally measures the warmed
retained-contact hot path for heap allocations.

## Remaining qualification

U-WLPA-1 still owns exceptional maintenance-counter resets, first/load service
rank and the session's authored collision/approximation countdown adapter.
This standalone late service does not claim LR1-14's periodic discovery clocks
are session-wired. U-WLPA-3 retains full arrangement/stride normalization and
first-pose qualification. U-WLPA-6 retains virtual policy/score producers,
authored repulsion eligibility and remaining repulsion branches, attack/infantry-push,
retained stopped-pair lifecycle alignment, full failed-escape reset/retry closure,
locomotor-lock lifecycle, retail save-load reconstruction and multi-source
arbitration. LR9 crowd outcomes remain capture targets; the synthetic choke
proves deterministic service behavior, not stock-unit gap thresholds or a
bridge capacity policy. Crush, viewer and AI remain outside this change.

## Tactical movement host

`TacticalContent::land_terrain` admits the land-only session host when the caller
also supplies the occupancy Clear bit and effective company/member bindings.
Without terrain the existing provider-unavailable diagnostic remains. Company
roots in the setup expand through `plan_land_company`; failed type creation or
L4 placement removes only that member before grouping survivors (LC-01..03).
The team speed is the authored team maximum (LC-06), supplied independently of
member locomotion values. Placement gives surviving members parent-relative
slots using authored formation spacing. The current host lays those slots in
a sideward row; this is a remake sandbox policy, not a recovered retail slot
layout. Direct objects use a one-member coordination host. Setup identity and
the same expansion inputs reconstruct the initial roster on replay.

The host accepts squad move and stop commands. One L4 request belongs to each
squad, with the common authored movement mask and formation width. Remake
policy constants allow 4096 search work per tick and 256 work per admitted
slice, independent of measured time and worker count. The scheduler's quota
and possible single-step overshoot keep their L4 meanings; these units do not
claim equivalence to retail graph work. Path results commit in ascending squad
ID. LR1-03/D5 orders `land-path-slices`, partitioned
`land-member-proposals`, late retained-contact service, then
`land-occupancy-commit`. Late contact installation never invokes ordinary
motion a second time. Occupancy reduces objects in ascending ID; partitioned
`land-publication` copies committed poses into the session snapshot. Every new host
member and phase is guarded by the land domain; the space phase sequence is
unchanged.

Section 18 version 1 carries the terrain identity, occupancy, searches,
movement and contacts. Its payload contributes to land canonical state only;
space checkpoints omit it. `land_session_contracts` compares final state,
checkpoint bytes, replay and direct restores across 1/2/4/8 workers and reverse
partitions and prints an 880-object stress profile with nonzero member radii
and contacts enabled. Disjoint wall scopes report path slices, member
proposals, contacts, occupancy commit, publication and the serial remainder;
these diagnostics never control simulation work. The initial profile measured
303/149/79/50 ms per tick at 1/2/4/8 workers. Swept-pair checks and contact
discovery currently scan all pairs; a separate optimization will address the
16.7 ms target at six workers. `land_session_stock_map`
runs synthetic companies against one decoded installed land terrain field.
Start markers, ownership/lobby building, combat, land AI and multiplayer remain
separate adapters. Authored collision/approximation countdowns and animation
producers remain the qualification seams listed above.
