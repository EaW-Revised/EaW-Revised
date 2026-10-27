# Opt-in memory attribution

Pass `--eawr-memory-attribution` in Godot's user arguments (after `--`) to
add an `attribution` object to the existing post-load, first-frame and
tick-1200 memory snapshots. It records Windows heap summaries, Performance
memory monitors and RenderingDevice reads scheduled on the render thread.

With this flag absent, the attribution capture returns before any heap,
Performance or RenderingDevice probe, and no `attribution` field is emitted.
The flag lookup itself still occurs. The existing CPU recipe/capacity counters
are independent and retain their behavior; this flag does not gate them.

`--extra-gpu-memory-tracking` is a separate **engine diagnostic option**, placed
before `--`. Attribution only observes whether it was requested; it never
enables it. The output labels this option `diagnostic_only`. Use a separate
diagnostic run when enabling it, and check the engine debug/editor features
recorded in the report. Driver-host/device-report zeros without supported
tracking do not mean zero driver allocation.

The extension's `template_release` build target does not identify the engine
binary variant. Record the actual Godot executable path, hash and version for
comparisons; the player package currently ships the official editor executable.

Heap summaries, payload capacities, engine monitors, GPU memory and process
private commit overlap. They are not a disjoint ownership ledger and do not
prove removable bytes. Heap-handle root regions do not enumerate every heap
segment. Reports distinguish pending, unsupported and unrequested device reads.


## Retained-container inventory at load, tick 600 and tick 1200

The opt-in space live viewer records `live_session.memory_capacity` at the first
presented tick at or above 1200. The report includes the presented tick, snapshot
tick, the live simulation's separately locked completed tick, and query time.
Registration is guarded by `--eawr-memory-attribution`; without it the frame call
returns before any container traversal or session query. The diagnostic does not
step, collect garbage, compact a heap, mutate a simulation struct, or enter a hash,
checkpoint or replay. Core seams are const accessors only.

A separate `live_session.memory_capacity_post_load` snapshot uses the same reads
immediately after standalone session start, while the existing initial pause is
still held at completed tick 0. This means before the first simulation step,
not before construction of the tick-zero state or the whole loading pipeline.
It never pauses a running session. Borrowed feeds, unpaused sessions and sessions
already past tick 0 are excluded. Its boundary label, timestamp and locked sim
completed tick identify the seam; later-initialized fog/HUD backing can be absent.
The separate `live_session.memory_capacity_tick_600` snapshot uses the same
frame boundary at the first presented tick at or above 600. It records the actual
presented and snapshot ticks; an overshoot must not be relabelled exact tick 600.
The tick-1200 snapshot remains separate. All three are opt-in and one-shot.

Vector capacity is retained backing, while element counts describe occupancy.
Neither is process-private ownership or a measured removable saving. Existing
per-phase HeapSummary allocated and committed values can distinguish those
reported heap measures only for successfully captured heaps. Their difference
includes allocator overhead and idle committed storage; it does not establish
allocator high-water, peak live bytes or releasable fragmentation. Uncaptured
heaps and unlisted allocations remain unknown, including at the load seam.

The inventory lists separate owners for UnitTables collision triangles, viewer
SessionContent CombatTable, and the live simulation CombatTable (including its
retaliation profiles). The exact collision rows are `unit_tables/collision_triangles` (raw UnitType
meshes), `viewer_content_combat/triangles` (placed profiles) and
`sim_combat/triangles` (the deep copy in SessionStoreRules). They are queried
separately at all three phases, never estimated by multiplying another owner's
count. Structure capacities and nested triangles/BVH nodes are
separate rows. It also covers the listed sim gather vectors, viewer entity/clone
vectors, space population storage, parked clip storage, projectile model
pool indices and beam/kite buffers. Prepared clip geometry now holds borrowed spans;
that row counts the retained span table only, excluding its shared payload. It is a partial retained-container inventory,
not total sim/ECS or total presentation memory: unlisted maps, strings, nested
weapon allocations, animation caches and allocator metadata are excluded.

Lua 5.0's `lua_getgccount` is queried once per root Sandbox, under the existing live
session mutex after its workers have joined. It returns KiB rounded down, converted
to bytes. Coroutines share their root allocator and are not counted again. This
value excludes C++ sandbox/scheduler storage and is independent of logical quotas;
no `collectgarbage`, threshold change or other VM mutation occurs.

The latest published fog rows are deduplicated by shared row identity within that
snapshot, and explicitly overlap the simulation's retained history. World fog and
minimap front/back vector capacities are CPU backing only. Unique retained audio
streams from sample, speech and music caches are deduplicated by stream identity;
WAV/MP3 `get_data().size()` uses Godot's shared COW array getter without detaching,
mutating, decoding or copying the payload. Audio rows are sizes rather than C++
vector capacity, overlap Godot's own accounting, and exclude decoder/mixer/playing
voice allocations. Unknown stream types are reported by count rather than zero
allocation claims.

Output has at most 128 fixed rows. `rows_complete=false` means row truncation or an
unsupported owner query, not a complete allocation inventory. Even when true,
only the named storage is counted. No row establishes removable private bytes.
The diagnostic extra GPU tracking option remains independent and must be passed
before `--`; enabling it may change the footprint, so that run stays diagnostic.

The opt-in one-shot query takes the live session's existing blocking mutex on the
viewer thread. It may wait for one simulation step and cause a frame hitch at
600 or 1200; `query_ms` includes that wait. This diagnostic run must not be used as an
ordinary frame-time result. Synthetic native fixtures verify the guard, copied
profile capacity arithmetic and row overflow; they do not establish the actual
Lua/audio/fog/CombatTable values, which require the real runtime capture.

Pinned-engine interpretation: Godot ed1daf0bf's Vulkan
[allocation callback implementation](https://github.com/godotengine/godot/blob/ed1daf0bf/drivers/vulkan/rendering_context_driver_vulkan.cpp#L218)
counts requested callback allocation sizes, excluding alignment/header overhead.
Its separate internal-allocation notifications are ignored (lines314-326), so a
small tracked result cannot exclude untracked driver-private storage. This is a
tracked lower bound, not an upper bound on all driver host memory.
[Aligned allocation](https://github.com/godotengine/godot/blob/ed1daf0bf/core/os/memory.cpp#L52)
uses malloc without updating the normal static-memory counter; a tracking-ON run
must not treat MEMORY_STATIC as including these callback payloads automatically.

## Load-phase timeline and model-cache budget replay (census only)

Both additions run only with `--eawr-memory-attribution`; without it they do no work and print nothing.

`EAWR load_mark <label> private_bytes=<n> cache_retained_bytes=<n> unix_us=<n>` is one log line (written by
`MemoryAttribution::load_mark`, `apps/viewer/src/memory_attribution.hpp`) at the end of each load stage (`stage0` map,
`stage1` session, `stage2` effects, `stage3` interface, `stage4` scene; the numbers are the `presentation::ui::load_stage_*`
constants) and just before and after the post-ready model-cache trim (`pre_trim`, `post_trim`). `private_bytes` is the
process private commit, `cache_retained_bytes` the retained decoded-model payload of `VfsAssetCache`, `unix_us` the wall
clock in microseconds, so the deltas between marks give each stage's duration and memory step.

`model_cache.budget_replay` in the report JSON (`startup.runs[].model_cache`, `apps/viewer/src/startup_trace.hpp`) is a
list of `{"budget_bytes", "decodes"}`. It replays the real model-request sequence of the run against fresh LRU caches of
128, 256, 512, 1024 and 2048 MiB and gives the decodes each budget would have needed; it is a what-if on the recorded
requests, not a second cache, and it does not change what the run itself retains.
