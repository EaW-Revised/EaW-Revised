# Deterministic simulation harness

P0-04 provides a renderer-free synthetic world for proving portable state evolution.
It is not an implementation or claim of original Empire at War gameplay rules. The
wire and state-hash contract is frozen in [replay-format.md](replay-format.md), and all
authoritative numeric component values use the accepted Q24 API in
[fixed-point.md](fixed-point.md).

## Public boundary

The public API is split by responsibility:

- `commands.hpp` defines nonzero persistent `uint64_t` entity IDs, entity component
  values and the four replay command payloads. Persistent IDs are never ECS handles.
- `replay.hpp` parses and writes bounded replay-v1 byte spans and exposes the SHA-256
  utility used by canonical state hashing. It performs no file I/O.
- `world.hpp` owns authoritative state, applies one atomic tick at a time, and accepts
  only an abstract deterministic partition executor. It contains no OS or threading API.
- `snapshot.hpp` publishes an owned, read-only copy of the completed tick and
  ascending-ID render instances. Each instance contains only stable IDs, an opaque
  asset ID and a Q24 `Mat3x4`; presentation performs any float conversion.

EnTT v3.15.0 is vendored under `third_party/entt` and used only as private component
storage. `World` maintains a separate stable-ID-to-handle map and assembles sorted
values before commands, movement, hashing and snapshot publication. Deliberately
rebuilding EnTT storage in reverse order does not change authoritative results.

## Tick execution

`World::step` first copies the current state, then applies all commands for the current
tick in encoded order. It advances SplitMix64 once and runs the `movement` phase over the
64 fixed partitions of the copied ascending-ID entity list (partition p is the range
`floor(p*N/64)..floor((p+1)*N/64)`, see [workers and the phase map](#workers-and-the-phase-map)).
Movement computes nearest-even
`velocity_raw * tick_numerator / tick_denominator` without quantizing tick duration,
then calls the checked Q24 `add` operation for the position result.

Only a successful evaluation is committed, in stable-ID order. A command, arithmetic
or executor error leaves entities, next ID, command cursor, tick, RNG state, hash and
published snapshot unchanged. When multiple movement items fail, the ascending-ID
scan and x/y/z component order select the diagnostic independently of thread completion.
`eawr::platform::ThreadWorkerAdapter` is the external OS adapter: the persistent worker
pool that runs the partitions on actual threads.

Canonical SHA-256 input includes versions, rational cadence, completed/final ticks,
next ID (including exhausted zero), RNG state, tick nonce, content identity, every live
component in stable-ID order and every pending command including its length prefix.
It excludes EnTT handles/order, worker count, addresses, caches and presentation state.

## Headless command

```text
sim_headless --replay input.eawr-replay --hash-out hashes.csv [--workers n|hardware]
             [--events-out events.csv] [--snapshot-out snapshots.csv] [--census-out start.json]
sim_headless --skirmish m2 --game-root <install> [--census-out start.json]
             [--replay-out start.eawr-replay] [--ticks n]
```

A replay-v2 input runs the tactical session below. Only replay v2 accepts the event,
snapshot and census outputs. `--skirmish m2 --game-root <install>` builds tick zero of the
pinned FoC skirmish instead of reading a replay ([skirmish-start.md](skirmish-start.md)).

The entry point is the only replay path that performs host file I/O. It stages all CSV
text until parsing and simulation finish, then writes UTF-8 without a BOM and with LF
lines. The exact header is `tick,sha256`; rows contain completed ticks starting at one.
A zero-tick replay writes only the header. Invalid arguments, input/parse/simulation
failure and output failure all return nonzero, and failed simulation never publishes a
partial hash file.

Stable simulation diagnostic categories are:

| Code | Meaning |
|---|---|
| `EAWR-SIM-0101` | malformed/truncated/trailing replay data |
| `EAWR-SIM-0102` | unsupported version, opcode or reserved bits |
| `EAWR-SIM-0103` | byte/count/tick resource limit |
| `EAWR-SIM-0104` | initial-ID or command-key ordering violation |
| `EAWR-SIM-0105` | invalid live-ID or next-ID command |
| `EAWR-SIM-0106` | stable ID space exhausted |
| `EAWR-SIM-0107` | movement displacement or checked position overflow |
| `EAWR-SIM-0108` | step requested after final tick |
| `EAWR-SIM-0109` | unsupported or failed worker execution |

## Tactical session (P2-03)

`eawr/sim/tactical/` is the deterministic home of a space skirmish session. It is separate
from the synthetic `World`, and it leaves the replay-v1 contract untouched. The wire format,
tick rules and hash encodings are frozen in
[replay-format.md](replay-format.md#tactical-replay-format-v2-p2-03). One tick is 1/30 s
([tactical tick rate](behaviour/tactical-tick-rate.md)).

- `types.hpp` defines players (team, faction, command flag), units (type, owner, Q24
  position and rotation, current order), the stop, move and attack commands, and the event
  and reason codes.
- `replay.hpp` parses, validates and writes replay v2 and records a session's setup.
- `session.hpp` provides `TacticalSession`. `submit` validates and queues player commands
  in canonical order whatever order they arrive in. `step` executes one atomic tick through
  the same `PartitionExecutor` as `World`, and `record` returns the session as a replay.
  Units live in private EnTT storage keyed by stable ID (ADR-009).
- `space.hpp` provides `SpaceIndex`, the box and range queries of one tick. Results come in
  ascending stable ID, whatever the insertion order, worker count or cell layout.
- `visibility.hpp` provides the sensor table (`SensorProfile`, from the EAWR-65 unit tables),
  the per-tick `SensorField` and `fog_grids`, which turns a snapshot into the fog grids the
  presentation cache reads ([space visibility](behaviour/space-visibility.md)).
- `durability.hpp` provides the durability table (from the EAWR-65 unit tables), hull and
  hardpoint health, their consequences (weapon, engine, shield, fighter bay), unit death, the
  per-tick hull/hardpoint service and the repair rule
  ([space hardpoints](behaviour/space-hardpoints.md)).
- `combat.hpp` provides the combat table (`units::combat_table`), the hardpoint opportunity
  service of [space targeting](behaviour/space-targeting.md), the per-unit combat state and the
  combat events: ship-level target choice, attack orders and weapon fire
  ([space weapon fire](behaviour/space-weapon-fire.md)). Shots are events for projectiles (EAWR-74).

Rules v1 moves units, chooses targets and fires weapons (shots are events), records orders,
applies scripted damage, services durable units, removes dead ones and publishes per-player
sensor visibility and health in each snapshot. Projectiles, damage from shots and spawning are
later tickets that add systems to the per-unit phases and events to the stream.

| Code | Meaning |
|---|---|
| `EAWR-SIM-0301` | malformed/truncated/trailing replay-v2 data |
| `EAWR-SIM-0302` | unsupported version, tick rate, opcode, flags or reserved bits |
| `EAWR-SIM-0303` | byte/count/tick/unit-list resource limit |
| `EAWR-SIM-0304` | ID or command ordering violation, including an out-of-order submission |
| `EAWR-SIM-0305` | invalid setup: undeclared owner, non-unit rotation, setup unit with an order; invalid sensor or durability table |
| `EAWR-SIM-0306` | issuer not declared or not allowed to command |
| `EAWR-SIM-0307` | late command: its tick has already executed |
| `EAWR-SIM-0308` | invalid command payload: empty or unordered unit list, ID zero, negative damage |
| `EAWR-SIM-0309` | warning: unit orders of a command were rejected at execution |
| `EAWR-SIM-0310` | unsupported worker count, executor or system failure |

## Workers and the phase map

Multithreading is a core requirement (EAWR-267): per-entity simulation work runs on every core
the host gives it, and no result may depend on how many that is (ADR-009).

The sim library holds no thread API; a step sees only `PartitionExecutor`. A phase is
`executor.execute_phase("<name>", sim::tick_partition_count, partition)`. Every phase splits
its N ascending-ID inputs into the same 64 partitions, partition p being
`sim::partition_range(p, N)` = `floor(p*N/64)..floor((p+1)*N/64)`, whatever the worker count,
and writes one output slot per input. So neither the worker count nor the order in which
partitions run can change a result, even for a later phase that combines values within a
partition. `InlineExecutor` runs every partition on the calling thread.

`eawr::platform::ThreadWorkerAdapter(n)` is the worker pool. It starts n - 1 threads once;
the thread that steps the simulation is worker 0. Between phases the threads park on an
atomic wait, without polling (polling workers slowed the working ones on SMT cores);
nothing starts a thread per phase or per tick. Worker w runs
partition w first, then the workers claim the remaining partitions in turn. One phase runs at
a time per pool, and a phase started from inside a partition runs inline. A partition that
throws fails the step with `EAWR-SIM-0109` naming the lowest partition that threw, and the
step commits nothing. `sim_headless --workers` takes 1 to 256 or `hardware`
(`ThreadWorkerAdapter::hardware_worker_count()`).

Waking the parked threads costs more than a small phase's work (EAWR-637): an empty 64-partition
phase took a median 3 us on the pool at 2 and 4 workers, 23 us at 8 and 43 us at 20
(`sim_bench --dispatch-cost 1` on a 20-thread host), against under 1 us on the calling thread;
in the EAWR-601 melee at 28 workers the small phases cost 0.1 to 0.2 ms each. So a pool made with
`Dispatch::by_cost`, which the live session uses, keeps a named phase whose last run's work was
under `inline_budget(workers)` (3 us per worker) on the stepping thread, in partition order.
When such a phase runs past the budget (a burst: the first projectiles, a round of path
searches), the pool takes the partitions not started yet; a phase that was bigger than the
budget goes to the pool at once. The phase's work is the time its partitions took, added up
over the threads that ran them. Work timed on the pool includes the threads' contention (SMT
siblings, memory, a loaded host), so a phase under four budgets tries the stepping thread again
every 16th run. The timer is checked after each indivisible partition, so the try can cost
the budget plus the last partition's work; the budget supplies no hard wall-clock bound.
This changes only which thread runs a partition, never a
result. `Dispatch::always_pool`, the default, wakes the pool for every phase of more than one
partition; the determinism tests and `sim_headless` use it, so they keep exercising the
threads. `tactical_contract_tests` also runs its golden replay with `by_cost` and the hashing
thread at every worker count.

### Fog row storage

Fog cells retain shared row buffers for values and holds (EAWR-890). A staged session copies
only row pointers and anchor ownership, then `fog-cells` clones each changed row at most
once. Service of a zero or held value that stays unchanged copies no value bytes. All rows
and anchors commit only after the phases succeed, so a failed fog or visibility phase
leaves the session unchanged. `copied_grid_bytes()` counts the row payload bytes copied
by the last advance, excluding pointer metadata and canonical serialization.

Live fog snapshots share immutable value rows. The viewer reads those rows directly;
retaining or publishing a snapshot never flattens a grid. The compatibility `values()`
accessor flattens on demand and caches its result until the next advance; it is absent
from the stepping and live-publication paths. Canonical encoding still appends every
player's row bytes in the original order, keeping all replay hashes and pins unchanged.

### Phase map

`TacticalSession::step`, in order:

| Step | Runs | Why |
|---|---|---|
| Stage the units, ascending ID | serial | Copies the units out of storage into the inputs every phase reads (ADR-009). |
| `movement`: advance each unit's move plan to completed tick + 1 (EAWR-70) | partitioned | Per unit: reads its own copied state and plan, writes its own slot and error slot. Runs before the commands, so an order acts from the next frame (MV-02). |
| `combat-world`: fill moved units | partitioned | Preallocated ascending-ID slots for transforms, health, combat state, player indices and published visibility; errors reduce in ID order. |
| Combat index and squadron overrides | serial | One space index over the filled slots, then ordered squadron overrides (EAWR-73). |
| `collection-boxes` (EAWR-469): each unit's world box | partitioned | Per unit with a combat profile: reads its own moved transform, writes its own box slot ([CO-11](behaviour/space-targeting.md#candidate-collection-order-469)). |
| Collection tree update | serial | The per-player trees that order target scans take the boxes in ascending ID and are serviced (CO-04 to CO-07). The tree's order is its history, so each update depends on the ones before it, as in FoC; the work per unit is a short tree walk. |
| `targeting`: ship-level target choice and weapon service (EAWR-73) | partitioned | Per unit: reads the immutable combat world, writes its own combat state, events and error slot; draws are keyed by seed, frame, unit and weapon ([P-02](behaviour/space-weapon-fire.md#project-choices)). Before the commands, so an attack order acts from the next frame. |
| Combat commit | serial | Stores each unit's combat state and appends its events in ascending shooter ID, and takes what its object weapon fired from its own energy pool ([EN-05](behaviour/space-damage.md#energy-pool)); moves only. |
| `orders` (EAWR-452): approach checks | partitioned | Only in ticks where a unit's attack, attack-move or guard on a unit is due its check (every `MovementReevaluationFrameCount` frames after the order, [OR-06](behaviour/space-orders.md#rules)). Per unit: reads the moved units with this tick's targets, writes its own approach slot and error slot. |
| `projectiles`: one flight step per projectile in flight (EAWR-74) | partitioned | Per projectile, with damage rules and only while one flies: reads the targeting phase's immutable world view, writes its own outcome and error slot ([DP-01](behaviour/space-damage.md#project-choices)). Its broad phase (EAWR-636) takes the units in the step's box grown by the largest collision reach from the space index into its partition's reused buffer, and passes to the exact tests only the hostile units whose own reach (the furthest corner of their boxes, sqrt(3) times the collision box's for a DG-37 sphere, whose hit needs the step to meet that box turned into the world, with a margin for the Q24 rounding) meets the step: a conservative reject that never changes a hit. `TacticalTick` counts both (`projectile_candidates`, `projectile_exact_tests`). |
| `tracking` (EAWR-71): predictions at the tracking windows' boundaries | partitioned | Only with avoidance rules and a move (or a waiting group member's plan, EAWR-344) due this tick. Per tracked moving unit: reads its own copied plan, writes its own sample slot (46 predictions from the layer's rolled anchor and, when that is not this frame, 46 from this frame). |
| Hit commit | serial | Applies the hits to the units they reached in ascending projectile ID ([DG-34](behaviour/space-damage.md#projectiles)): a hit changes another entity, so it is staged per projectile and applied here, never by a worker. Then this frame's shots join as projectiles. |
| `plan-searches`: sliced path searches (EAWR-520) | partitioned | Only while a sliced search runs ([PC-08](behaviour/space-movement.md#planning-cost-503)). Per search: one slice (`search_slice` expansions; in its landing frame to its end) over its own copied layer views and scratch. Its plan lands later as a job of its lane ([path searches](#path-searches-503)). |
| Approach plans (EAWR-452) | serial | Ascending unit ID, after the attack turns and before the commands: each approach plans its path against the predictions submitted before it (AV-15, [OP-03](behaviour/space-orders.md#project-choices)). |
| Commands | serial | Canonical (tick, player, sequence) order, and each command sees the ones before it: scripted damage can kill a unit that a later command names. The work is per command, not per entity. A move's path search is queued here and planned in per-layer lanes of the `plan-searches` phase (AV-15, PC-07, [path searches](#path-searches-503)): FoC plans orders one ship at a time and each plan submits its prediction before the next ship plans, so a later order in the same layer must see the earlier plans. The layer views are assembled from the `tracking` samples once per layer and after each plan in that layer; the only per-unit work left here is sampling the planned unit's own new prediction. Group moves (EAWR-344) map their slots here as well: each slot depends on the slots mapped before it (FM-05), so the mapping is sequential; waiting group members and deferred searches due this frame are queued first (FM-12). |
| `unit-systems`: durability service (HS-01), energy and shield recharge on the unit's own phase (EAWR-74, EAWR-361), snapshot instance | partitioned | Per unit, from its own copy into its own slot. |
| Destruction events, survivors | serial | Appends the events in ascending ID; moves only. |
| `spins` (EAWR-447): service each killed craft spinning away | partitioned | Per spin, from its own copy into its own slot; only while any spins ([SP-04 to SP-08](behaviour/space-fighter-deaths.md)). |
| Spin commit, spin starts | serial | Drops the ended spins in ascending ID; then walks the tick's deaths in destruction order, one keyed draw each, and starts the spins (SP-02). Its work is per death, never per unit. |
| `squadrons` (EAWR-271): live craft and bounding-box centre | partitioned | Per squadron, from the survivors into its own slot; only when squadrons exist. |
| Squadron commit | serial | Moves each container, removes emptied ones, in ascending container ID. |
| Victory (EAWR-77) | serial | Only with victory rules: walks the tick's `unit_destroyed` events in order against the few counted star bases and decides the outcome ([VP-01](behaviour/space-victory.md#project-choices)). Its work is per destruction event, never per unit. |
| Sensor field build | serial | One index over all observers. It is a few percent of the serial remainder at 1500 units, so splitting it does not pay yet. |
| `fog-reveal` (EAWR-274): mark again? | partitioned | Per revealer, against the committed circles; only with fog rules. |
| Fog changes | serial | Lists the circles to release and mark in ascending revealer ID; a few per tick. |
| `fog-cells`: service, release, mark | partitioned | By bands of grid rows, every player's grid; each band applies the changes in ascending revealer ID. Immutable rows share storage until the first changed value or hold stages a private row (EAWR-890). |
| `visibility` | partitioned | Per unit, a query against the immutable sensor field, and per spinning craft its pose and the same query (EAWR-447). The largest phase today. |
| Commit | serial | The ordered commit alone writes storage (ADR-009). |
| State hash | serial, or on the hashing thread | SHA-256 is one sequential chain over the frozen canonical encoding. With a state hasher (EAWR-637) the stepping thread only encodes the state; the digest is computed on the hasher's thread while the next tick runs. |
| Snapshot | serial | Moves the staged instances and events. |

`World::step` (replay v1) runs its commands serially, `movement` partitioned, and its commit
and hash serially.

`ScriptScheduler::service` (EAWR-247, [Lua sandbox](lua-sandbox.md)), once per tick:

| Step | Runs | Why |
|---|---|---|
| Route due events and timers | serial | Key order (tick, producer, entity, sequence) into each instance's inbox. |
| `script-instances` | partitioned | Per instance in ascending ID: reads only its own Lua state, inbox and the tick's inputs; writes only its own staging (commands, posts, timers, diagnostics). |
| Commit | serial | Ascending instance ID: publishes commands and diagnostics, queues posts and timers, removes exited and faulted instances. |

Every new per-entity phase runs partitioned: movement (EAWR-70), tracking predictions (EAWR-71),
targeting and fire (EAWR-73, the `targeting` phase), approach checks (EAWR-452, `orders`), projectiles and damage (EAWR-74), squadron launches (EAWR-75) and abilities (EAWR-76). It copies its inputs, runs
under its own name over `tick_partition_count` partitions and writes only its own inputs'
slots. Effects on other entities (damage, spawns, events) are staged per source and applied
by the serial commit in canonical order, never written by a worker. A phase runs serially
only with a reason stated in its pull request and a row in this table. `test_phase_map` in
`tests/replay/tactical_tests.cpp` fails when a listed phase (including `script-instances`)
stops reaching the executor, loses its name or changes its partition count; a new phase adds
its name there. `tracking` runs only in ticks that plan a move with avoidance rules;
`tests/replay/pathfind_tests.cpp` checks that it is named, partitioned and runs exactly then.
`orders` (EAWR-452) runs only in ticks where an approach check is due; `tests/replay/combat_tests.cpp`
checks the same for it.

### Scaling

`sim_bench` generates a synthetic battle from a seed, with no game data: four players in two
teams whose fleets overlap, six unit types from fighter to capital ship with sensors and
hardpoints, and per player and tick a move order, an attack order and scripted hull damage.
It steps the battle with 1, 2, 4, 8 and the hardware worker count and prints ticks/s, the
time per tick of each named phase and the serial remainder. It exits 1 when two worker counts
disagree on any tick's state hash; CTest runs it as `sim_bench_worker_equality`.

```text
sim_bench [--units 1200] [--ticks 300] [--warmup 30] [--repeat 3] [--seed n] [--workers 1,2,4,8,hardware]
```

The partitioned phases scale with the cores. The serial remainder does not: at 1500 units
the state hash is about half of it (the SHA-256 alone), and staging and committing storage is
most of the rest. It bounds the speed-up.

### The state hash off the stepping thread (EAWR-637)

`TacticalSession::set_state_hasher` takes the SHA-256 off the stepping thread. `step()` still
encodes the completed tick's canonical state (the next tick changes it), then hands the bytes to
the hasher and returns; `TacticalTick::state_hash` resolves to the digest, and `state_sha256`
stays empty. `platform::ThreadStateHasher` is one thread that hashes in submission order, lets
at most four jobs wait in the default queue plus one active job (a faster stepping thread
then waits for queue space). A plain tick submits one job; a scripted tick submits two,
the world hash and its combined derivation. The hasher finishes every job
before it is destroyed. The digest is the one the synchronous path computes, so determinism
checks, replays and pins see the same value at the same tick.

One paired run of the EAWR-601 melee per size and worker count (`path_bench --melee`, seed 601,
4500 ticks), 2026-09-30, on the Linux build container with 12 exposed CPUs. Both builds used
the GCC Release preset. *Before* is integration `34970c73`, with always-pool dispatch and
synchronous hashing; *after* is its merge with this change (`3e7eb60d`), with `by_cost`, a
threaded state hasher and digest retrieval after stepping. Tick timing includes canonical
encoding and queue backpressure. Every run has the same tick hashes across both builds and
workers 1, 2, 4 and 8.

The shared build lock was held, but host load was elevated and varied between modes:
1-minute load at S starts was 28.10 before / 18.16 after; M was 19.44 / 17.10.
The host was not exclusively idle. **Direction confirmed, absolute numbers pending a quiet
rerun.** The original 2026-09-29 samples may include interference during the owner's
identified 23:10–23:35 window; their exclusive-host condition, absolute times and hardware
worker-count claims are unconfirmed. This table replaces those samples with the reviewer's
paired observations; private receipts identify the execution host and measured load.

| Size | Workers | Mean ms before / after | p99 ms before / after | Serial ms before / after | Ticks over 33.3 ms before / after |
|---|---:|---|---|---|---|
| S | 1 | 10.134 / 9.470 | 47.861 / 46.747 | 1.808 / 0.835 | 145 / 145 |
| S | 2 | 8.143 / 7.023 | 37.707 / 33.858 | 1.889 / 0.829 | 84 / 50 |
| S | 4 | 6.368 / 5.895 | 23.857 / 24.562 | 2.025 / 1.205 | 0 / 10 |
| S | 8 | 6.588 / 5.172 | 23.970 / 19.931 | 3.214 / 1.634 | 0 / 0 |
| M | 1 | 20.591 / 20.161 | 151.165 / 149.378 | 2.307 / 1.495 | 916 / 909 |
| M | 2 | 14.779 / 13.878 | 113.325 / 109.442 | 2.241 / 1.411 | 801 / 695 |
| M | 4 | 11.702 / 11.423 | 79.665 / 74.935 | 2.458 / 1.638 | 526 / 663 |
| M | 8 | 10.383 / 8.667 | 56.673 / 56.447 | 3.565 / 1.960 | 489 / 408 |

These observations show lower means at every measured worker count: at 8 workers, S improves
21.5% and M 16.5%. S at 4 workers has a slightly worse p99 and more ticks over budget despite
its lower mean. These contention-sensitive results do not establish a quiet-host baseline or
the original 28-worker speed-up. M and L remain projectile-bound (EAWR-636).

Who hashes where:

| Consumer | Hash |
|---|---|
| Live session (the viewer's game), world alone or scripted (EAWR-79) | On the hashing thread; `tick_hashes()` resolves them when a report asks. A scripted live tick hashes only the world: the combined hash (the world's with the scripts') is off (`ScriptedTacticalSession::set_authoritative_hash(false)`, EAWR-895), because the save and hash of every script instance cost more than the world step on the stepping thread and nothing live reads it. `script_state_hash()` gives the scripts' hash on request at a tick barrier. |
| Scripted sessions outside the live game (the soak, the FoC AI tests, tools) | The combined hash every tick (the default), derived on the hashing thread after the world's when one is set; the scripts' own hash is taken on the stepping thread at the tick barrier. |
| `sim_headless`, replays and pins, `headless_tick_hashes`, the tests | On the stepping thread (the default): each writes or compares every tick's hash at once. |

### Path searches (EAWR-503)

The path searches of the Commands step and of the waiting ships before it (FM-12) are due in
their planning order, but a search reads only its own layer and the static layer
([AV-01](behaviour/space-movement.md#how-foc-avoids)). So they are queued, and planned before
anything else reads or changes a layer and after the commands: each dynamic layer is a lane,
and each round takes the next search of every lane. The round's layer views are built
serially, its searches run in the partitioned `plan-searches` phase (one per search, each
writing only its own unit's plan, work count and error slot), and their predictions are
submitted serially in planning order. That is exactly the serial result
([PC-05](behaviour/space-movement.md#planning-cost-503)); the first failed search in order
answers. A round of one search runs inline. A search may spend only the rest of its layer's
budget in the tick (PC-07), counted in expansions, so every worker count gives up and defers
the same ones. Those run sliced (PC-08): each copies its layer view and the static layer when
it starts, so its slices, one per tick in the `plan-searches` phase before the due waits, run
side by side on any worker and read nothing the tick changes; the search lands as a job of
its lane in its landing frame. `test_plan_searches_phase` and `test_single_lanes` in
`tests/replay/formation_tests.cpp` check that a group's front ships and staggered pair, and
single moves of two layers, reach the executor as `plan-searches`, partitioned, in those
ticks only, with the 1-worker hashes; `test_search_budget` checks the landings frame by frame
on 1, 2, 4 and 8 workers and with other slice sizes, and `tactical_path_cost_contracts` that
a search in slices of 1, 97 or 1,500 expansions is the whole search.

The searches were the whole of EAWR-503's stall: every Q24 multiply and divide ran a 192-step
bitwise division, so one cross-map search took hundreds of milliseconds. The fixed-point core
now shifts for the rounded multiply, divides a one-limb denominator limb by limb, starts its
square root from the root of the top bits, and CORDIC shifts instead of dividing; each result
is bit for bit the old one (`math_contract_tests` cross-checks them against the 192-step
reference, and the `math_million_operations` digest is unchanged). `motion_detail::Calc` uses
the `try_*` forms, which skip the `Result` on success.

### Path search cost

The path search's work is counted, never timed, by the simulation (EAWR-520):
`plan_space_move` fills `PathSearchStats` (tries, expansions, children, collision queries,
windows, leaves and exact tests), and `set_path_search_probe` installs a `PathSearchProbe` that
receives every call's counts and, if it gives a clock, the time of the search, its collision
queries and its open and closed sets. Without a probe the simulation reads no clock.

- `tactical_path_cost_tests <fixtures>` pins every search's work counts in both modes
  (`fixtures/path-cost.work.csv`, `--update` re-pins) and the bounded search's bounds against
  FoC's ([PC-06](behaviour/space-movement.md#against-focs-search-520)); `--report` prints
  each search and `--profile` the time split.
- `path_bench` (needs the game data) loads the M2 start with the FoC AI, stages the owner's
  benchmark (20 capital ships of the four M2 types that move, a group move of 12 and 8 single
  moves in one tick at tick 600, across the map through the centre's stations and pads) or
  orders every human ship (`all`, `large`), and prints p50, p99 and max of the 300 ticks after
  the order, the order tick, the plan-searches phase and the searches' work. It exits 1 when
  two worker counts disagree on a hash. `--csv` writes the ticks and the searches. `owner-group`
  stages the same 20 ships and gives them one group move, a player's order (EAWR-613, the eye-check
  clip's order); `--ships` writes the staged ships' positions and plans after the order, and
  `--search-budget` replaces the per-tick search budget (a large one plans every search in its
  frame, FoC's timing). With
  `--pin` (the CTest `path_bench_owner_work`, skipped without the game data) it compares every
  search's work in the owner benchmark with `tests/skirmish/fixtures/path-bench-owner.work.csv`
  (`--update-pin 1` re-pins) and fails when a search starts in a layer that has spent its
  budget in the tick (PC-07).

The searches run on exact Q24 arithmetic, so their speed comes from the arithmetic and the
broad phase, all bit for bit: a grid of 1024-unit cells over each window's leaves (the
rectangle prefilter's candidates), the query facing and window fractions computed at the first
exact test, 128-bit steps and 64-bit limbs in the wide multiply and divide, the Calc success
paths inline, and a per-thread cell table, heap and node list (never read for their capacity).

### Scripts and workers (EAWR-247)

A Lua state and its coroutines run on one thread at a time, so script work parallelises
across script instances, not within one. An instance owns its Lua state, reads only the
tick's frozen world view, and writes its commands, events and other effects to its own
staging buffer, which the scheduler commits in stable instance order; the
`script-instances` phase above runs partitioned over instances in instance-ID order. It
stays result-identical for every worker count because:

- The soft-float backend and the host bindings keep no shared mutable state (EAWR-246).
- A random draw does not depend on how many draws other instances made in the same tick:
  the stream is keyed by seed, tick, instance ID and draw index (`eawr-script-rng-v1`).
- Shared script state (`GlobalValue`, one story state that several plots use; EAWR-79) must be
  written through staging and read from the previous service, or its instances share one
  partition and run serially in instance order.

Instances that share one Lua state stay serial among themselves. A fault or quota stop is
reported for the lowest instance ID, like a throwing partition.

### Game build

`eawr::platform::LiveSession` (`live_session.hpp`, EAWR-80) is how the game build runs a tactical
session: on its own simulation thread, which is worker 0 of a pool of the hardware count minus
the Godot main and render threads (at least one, `LiveSession::game_worker_count()`). It hands
the main thread only immutable snapshots: the two newest ticks, which presentation interpolates
between, and a short history. The main and render threads never call `step`, and the header
names no session type, so presentation cannot reach one (UI-07 boundary).

Orders enter as command values on two paths, both submitted by the simulation thread right
before the step they belong to. A local player's input comes from `Options::command_source`
(the UI-07 `CommandScheduler::take`), called once per tick with the tick about to run; its
commands keep the scheduler's keys. Scripted and debug orders (`LiveOrder`) are held until
their tick (untimed ones take the next tick) and then take their issuer's next sequence, one
past the last the session accepted from either path, so a preloaded later order never blocks
earlier input. An exception on the simulation thread becomes the session's `failure()`; the
thread stops stepping and never terminates the process. Both paths become replay input like
any recorded command, and `record()` returns the replay that
reproduces the run: `headless_tick_hashes` of that replay equals the live per-tick hashes,
whatever the worker count or when the orders arrived. Real-time pacing keeps tick n due at
n/30 s and drops time it cannot catch up; driven pacing steps only to the tick the caller asks
for, which captures and tests use. The viewer's `--eawr-live-session m2` runs the M2 start this
way (`apps/viewer/README.md`); `sim_headless --replay <recording> --game-root <install>` replays
its recording with the same content tables.

## Determinism evidence

`replay_evidence` runs the frozen independent replay with 1, 2 and 4 workers and emits
three CSV files. Every CI matrix job uploads those files with an exact target identity.
The dependent `replay-compare` job requires the named MSVC, clang-cl, Linux GCC,
Linux Clang and Linux ARM64 GCC artifacts, all three worker results per target and
exact agreement with the frozen independent audit. `tools/compare_replay_hashes.py`
reports the first missing or divergent tick and rejects absent or misidentified target
directories. The contract tests compare 1, 2, 4, 8 and the hardware worker count
(`platform::determinism_worker_counts()`) and an odd count against the same goldens.
