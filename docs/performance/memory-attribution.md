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


## Heap size classes and the region walk

Two further opt-in diagnostics split the steady private commit; neither changes
behaviour and both are off unless asked for.

- **Heap size classes** (`--eawr-memory-attribution`): the first-frame snapshot
  adds `attribution.size_classes` after the heap rows. For every captured
  Windows heap it walks all blocks once (the heap is locked for the walk, so the
  first frame stalls for as long as it takes, bounded at 20 s per run and flagged
  `truncated`) and reports busy blocks by `floor(log2(user bytes))` (count and
  bytes per bin), the free block count and bytes, and the allocator overhead.
  Large bins are vectors and buffers; small bins are object graphs. The tick-1200
  snapshot does not repeat it.
- **Region walk** (`driverArgs.regionWalkSeconds`, [N, ...] seconds after the
  process was found, in `tools/rig/drivers/perf-sample.ps1`): reads the run
  process's address space from outside with `VirtualQueryEx` and writes
  `<name>.regions.csv` (committed bytes by allocation kind and log2 size bin) and
  `<name>.regions.summary.csv` (one row per walk: `unix_us` to line up with the viewer's load marks, private bytes before and after,
  walked private/mapped/image committed bytes, and private minus walked). The
  difference is commit no user-space region accounts for, such as driver and
  kernel charges, plus anything that moved during the walk. Heap segments cannot
  be told apart from other partly reserved private allocations from outside
  (`private_partial`); fully committed private allocations (`private_whole`) are
  direct page allocations.

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

`EAWR load_mark <label> private_bytes=<n> cache_retained_bytes=<n> live_model_bytes=<n> unix_us=<n>` is one log line (written by
`MemoryAttribution::load_mark`, `apps/viewer/src/memory_attribution.hpp`) at the end of each load stage (`stage0` map,
`stage1` session, `stage2` effects, `stage3` interface, `stage4` scene; the numbers are the `presentation::ui::load_stage_*`
constants) and just before and after the post-ready model-cache trim (`pre_trim`, `post_trim`). `private_bytes` is the
process private commit, `cache_retained_bytes` the retained decoded-model payload of `VfsAssetCache`, `live_model_bytes`
the payload capacity of every live decoded model (the retained ones plus pins and renderer owners the cache does not
retain; 0 on marks that do not query it), `unix_us` the wall clock in microseconds, so the deltas between marks give each
stage's duration and memory step. `battle.scope_exit` is written right after a map-mode ready job ended (its model-load
scope restored the gameplay budget and trimmed), so `live_model_bytes - cache_retained_bytes` on `stage4`, `pre_trim`
and `battle.scope_exit` shows how much decoded model data other owners keep alive beyond the cache.

`model_cache.budget_replay` in the report JSON (`startup.runs[].model_cache`, `apps/viewer/src/startup_trace.hpp`) is a
list of `{"budget_bytes", "decodes"}`. It replays the real model-request sequence of the run against fresh LRU caches of
128, 256, 512, 1024 and 2048 MiB and gives the decodes each budget would have needed; it is a what-if on the recorded
requests, not a second cache, and it does not change what the run itself retains.

### Campaign load marks and the end-of-preparation trim

`--eawr-gc-prep-trim on|off` (hidden, default `on`; `off` is the previous behaviour) controls one budget call. A campaign
load decodes its unit models in preparation with an unbounded model cache and, before this flag, kept all of them until
`start_game`, after the model upload phase. With `on`, the cache is set to the gameplay budget
(`default_model_cache_budget_bytes`) right before `gc.build_scenes`; `start_game` sets the same budget again and still
starts the gameplay phase, so the zero-gameplay-decode gate is unchanged. Preparation decodes are not counted as gameplay
work, so the earlier trim cannot trip it. Any model requested between the trim and `start_game` is a load-thread decode and
shows up in `loads_between_trim_and_start`.

Census runs (`--eawr-memory-attribution`) add:

- `EAWR load_mark gc.<phase>.end ...` after every `gc.*` load phase (`gc.mount`, `gc.build_view`, `gc.camera_constants`,
  `gc.build_scenes`, `gc.upload_models`, `gc.build_overlay`, `gc.build_routes`, `gc.build_hud`, `gc.start_game`) and `gc.prep_trim.pre` /
  `gc.prep_trim.post` around the trim, in the `load_mark` format above. The `unix_us` field anchors the 500 ms process
  sampler to the phases.
- `EAWR load_detail gc.upload_models models_done=<n> private_bytes=<n> local_texture_bytes=<n> heap_allocated=<n>
  heap_committed=<n> heaps=<n> godot_static=<n> render_video=<n> render_texture=<n> render_buffer=<n> unix_us=<n>`
  every 12th distinct model of the upload phase (`MemoryAttribution::load_detail`). `local_texture_bytes` is the decoded
  texture payload held by the upload function's own texture map; the heap sums come from the Windows heaps and the
  `godot_*` / `render_*` values from the engine monitors. These overlap each other and `private_bytes`; they locate growth,
  they are not disjoint ownership.

`gc_model_cache` in the galactic report gains `prep_trim` (the cache budget is the gameplay budget after the trim call), `prep_trim_recorded`,
`prep_trim_retained_before_bytes`, `prep_trim_retained_after_bytes`, `prep_trim_freed_bytes` and
`loads_between_trim_and_start` (new decodes between the trim point and `start_game`; expected 0).

## Model-cache budget during a tactical load

A tactical (skirmish or land) battle load used to keep every decoded model strongly cached until the last load step
trimmed the cache to 128 MiB, so the load-time peak grew with the number of unit types the session decodes for their
metadata (a large mod holds well over a gigabyte there, stock content tens of MiB). The cache is now bounded from the
first decode, and the final 128 MiB trim at the end of load is unchanged.

- **Budget.** `clamp(installed physical RAM / 32, 256 MiB, 1 GiB)`: 8 GB gives 256 MiB, 16 GB 512 MiB, 32 GB and above
  1 GiB. The viewer reads the RAM size (`apps/viewer/src/map_mode_ready.cpp`) and passes a plain byte count to the scene
  library; `model_cache_load_budget` in `apps/viewer/src/model_cache_budget.hpp` is the pure formula.
- **Optional setting.** The hidden argument `--eawr-model-cache-load-budget <MiB|off>` replaces the scaled value; `off`
  restores the unbounded load of earlier builds, so one binary serves as its own A/B control. It changes when a model is
  decoded again, never what is decoded.
- **What it costs.** A model evicted during the load and requested again with no other owner is decoded again inside the
  loading gate. Nothing changes after the first frame: the end-of-load budget and the gameplay path are the same, and a
  galactic battle load keeps the cache unbounded (the call is ignored while its load scope is open).
- **Gameplay decode counter.** A decode after the end-of-load trim now counts in `model_cache.gameplay_model_loads` for a
  tactical battle too (it was galactic-only). A skirmish that reports
  more than 0 decodes models on the main thread at run time and is a finding for the renderer-recipe work.

## Compose texture release (initial space compose)

The initial compose of a space battle decodes every texture a surface names and used to keep the decoded copy in a
per-compose map until `compose_range` returned. The renderer copies what it needs on upload (and verifies and drops its
own snapshot), so the held copy only saved a repeat decode for the next surface naming the same path. On the BFC mod
(about 665 resident textures, 2.1 GB compressed) the map held about 1.9 GB of CPU texture payload at the end of the
load: the sampled private bytes of three GTX runs fell 10.85 -> 8.9-9.0 GB within one 5 s sample just before the
`stage4` mark, which is the largest part of the sampled load peak (the model-cache trim to 6.95 GB follows).

- **Release.** `ComposeTextureCache` (`apps/viewer/src/compose_texture_cache.hpp`) holds decoded textures by path under
  a byte budget, least recently used first. A texture is decoded once per path while it is held; surfaces of one model
  name their textures back to back, so they hit. The newest entry always stays; a texture over the budget is released
  by the next request. Nothing in what is uploaded changes, only when the CPU copy leaves.
- **Budget.** 512 MiB (`compose_texture_default_budget_bytes`), a constant.
- **Measured** (GTX 970, 5400 rpm HDD, BFC skirmish, same binary, legs off,on,on,off, 500 ms sampler): sampled private
  peak 10.864 / 10.870 GB without the release against 9.262 / 9.259 GB with it (-1.60 GB); the held CPU payload peaks at
  2.10 GB unbounded and 0.54 GB with the budget; `final_state_sha256` identical in all four legs. The budget costs 303
  repeat decodes (1788 against 1485 decode calls); the warm legs show `texture_decode` 147 s with against 149 s without.
- **Scope.** The initial load only (`compose_range` with `appending == false`, and the attract load queue). Arrivals
  composed during the battle keep an unbounded per-call hold, so no gameplay path changes.
- **What it costs.** A path evicted and named again is decoded again inside the loading gate. The log line
  `EAWR compose textures: budget_bytes= peak_held_bytes= held_bytes= decodes= hits= evictions= redecodes=` is printed once
  per initial compose; `redecodes` is that cost, `peak_held_bytes` is the CPU payload the hold reached.

## Compose model pins (initial space compose)

`VfsAssetCache::model()` returns a raw pointer and pins the decoded model in the calling wrapper until the wrapper dies.
The initial compose used one wrapper for the whole load, so every model the scene build, the classification and the
placement decisions touched (about 1.5 GB on the BFC mod) stayed alive until the compose returned, out of reach of the
load-time model budget; the sampler dropped by that amount in one step at the end of the compose.

- **Release.** `VfsAssetCache::release_raw_pins()` drops the wrapper's pins. The compose calls it before the first
  placement decision (the scene build's and classification's pins) and before each following one, and once after the
  last, on the initial load only. A model the strong cache retains stays; any other is freed and decoded again if a later
  decision names it. Raw model pointers are only used inside one decision.
- **Load phase only.** The call is refused (returns false, counted in `raw_pin_release_refused`) after `end_model_load`
  and in galactic gameplay outside a battle load, so no gameplay frame can pay for it.
- **Report.** `EAWR compose models: pin_release= calls= hits= misses= unique_loads= cache_retained_bytes= cache_evictions=
  pin_releases= pins_released= release_refused=` is printed once per initial compose; the difference in `misses` between
  the legs is the re-decode cost.
- **Measured** (GTX 970, 5400 rpm HDD, BFC skirmish, same binary, legs off,on,on,off, 500 ms sampler, after the texture
  release): sampled private peak 9.266 / 9.264 GB without the pin release against 7.687 / 7.688 GB with it (-1.58 GB);
  `final_state_sha256` identical in all four legs. Cost: model decodes 1361 -> 1639 (+278 re-decodes, 2012 -> 2295 decode
  calls); `ready_ms` 565 / 615 s without, 614 / 578 s with (flat within the leg noise on this HDD host).

## Texture owner census (`--eawr-texture-census`)

An opt-in, default-off diagnostic that answers *which* textures hold the GPU texture ledger. Pass
`--eawr-texture-census` in Godot's user arguments (after `--`); it is independent of
`--eawr-memory-attribution` and can be combined with it (both land in the same `startup.runs[]` entry).

**Cost.** Switch off: no record is kept and no snapshot is built; the only residue is a relaxed read of
the flag (at each shared or particle texture creation and release, once per battle start and once per probe
call) and a few scalar fields on each texture's ledger item.
Switch on: model textures are found at snapshot time from the renderer's own pool and recipes, so
uploading adds no per-texture record. Only textures no model holds (shared named textures and arrays,
particle textures) are noted when they are created and dropped when they are released. A snapshot lists
the shared textures of the probing renderer only; particle textures belong to process-wide particle
backends, so every snapshot lists all of them. There is no
per-frame traversal. Each snapshot is one main-thread walk of the resident resources, run once at the
first drawn battle frame and once at the first presented tick at or above 600; like the tick-1200
inventory it can hitch that one frame, so a census run is not a frame-time result.

**Report.** `startup.runs[].texture_census` is a list with one object per phase (`first_frame`,
`tick_600`). Each object holds:

- `textures`, `device_bytes`: every live GPU texture the census found and their resident bytes (after any
  mip drop; three-channel formats padded to four, as in the ledger).
- `cpu_snapshots`: `held` and `bytes` of decoded CPU copies a recipe still retains.
- `uncompressed_over_1mib`: count and bytes of non-BCn textures strictly larger than 1 MiB on the device.
- `ledger`: the renderer ledger's texture totals at the same moment, as a cross-check. The census can
  exceed or fall short of it by textures created or freed outside the walked tables; a large gap is a bug
  in the census, not a finding.
- `by_owner_kind`: `model`, `particle`, `shared_named`, `shared_array`, `unowned`, each with count,
  device bytes (split compressed/uncompressed) and CPU bytes. `unowned` is a pooled texture that no model
  resource references.
- `by_format`: count, device bytes and CPU bytes per pixel format (`bc1`..`bc7`, `bgr8`, `rgba8`, ...).
- `top`: the 50 largest rows by device plus CPU bytes. Each row names its `owner_kind`, `owner` (for a
  model, the model path of the lowest renderer asset id that holds the texture; for the others the shared
  or particle texture key), `role` (`base` or the material binding name), the canonical `path`, source id
  and `layer` of the decoded file, format, width, height, mip count, `device_bytes`, `cpu_bytes`,
  `cpu_snapshot_held`, `shared_by` (resources sharing the one GPU copy) and `resident`. A row with
  `resident:false` is a demand-only resource that holds only its CPU snapshot.
- `duplicates`: `same_content_across_owners` (the same pixels held as separate GPU copies, e.g. by a model
  and a particle backend; the model pool shares equal content by itself, so duplicates appear across
  owner kinds) and `same_path_distinct_content` (one canonical path that reached the GPU as several
  contents, e.g. another layer's file). Each gives `groups`, total `wasted_bytes` (all but one copy, or all
  but the largest for path variants) and the 20 worst groups.

The report is bounded (top 50 rows, top 20 groups per kind); the aggregates cover every texture.

**Load-phase journal.** With the switch on, the viewer also appends snapshots during loading to
`<report path>.texture-census.jsonl` (the user-data `logs` directory when no report path is given), opening,
appending and closing the file each time so a run that is stopped during loading keeps every earlier line.
Each line is `{"label", "renderer", "unix_us", "census": {...}}` with the same `census` object as above, one
line per live renderer that holds textures. Labels: `galactic_ready` (campaign view prepared),
`battle_load_start` (before the battle's content load; the campaign renderers are still alive),
`stage0`..`stage4` (each load stage complete), `pre_trim` and `post_trim` (around the model-cache trim) and
`load_periodic` (at most every 20 s while a stage reports progress). Only renderers created after the switch
was parsed are walked, and a renderer that holds no textures writes no line.

**Limits.** The owner is the first model, not an object definition: map a model path back to its unit or
building through the data. Textures owned by the engine (UI, render targets, shadow maps) are outside the
ledger and show up only as the gap between `device_bytes` and Godot's device texture counter. Names come
from the loader's canonical path; a texture whose recipe never knew a path (generated or transformed)
lists an empty `path`. Like the rest of this page the numbers overlap other counters and prove no
removable bytes by themselves.

## Load detail: where a battle load goes, and which types it needs

`--eawr-load-detail` (a viewer user argument; independent of `--eawr-memory-attribution`) turns on a measurement of the
battle load and the first play. It changes no gameplay or simulation state and no hash. Off, each place it would take a
sample is one flag test on the presentation thread; no thread is added and nothing runs per frame.

With it on, each run in the startup report (`startup.runs[*].load_detail`) and the startup trace carry:

- **Upload split.** Inside `surface_renderer_upload` (the exclusive upload time of the load table), new phases split
  `Impl::upload`: `upload_keep_texture` (recipe lookup and the content digest of the textures, hashed once per upload), `upload_count_texture`
  (the incoming-bytes count, which reuses that digest), `upload_identity_copy` (the byte copy for the upload identity),
  `upload_identity_submit` (handing it to the hash pool; it can block when the pool queue is full) and
  `upload_gpu_create` (the rest of the resource creation, which includes the existing `texture_gpu_upload` and
  `mesh_gpu_upload` scopes as children). Inside `mesh_gpu_upload`, `mesh_add_surface` is the `RenderingServer` surface
  create and `mesh_surface_count` the surface-count queries, a render-thread round trip each with a separate render
  thread. All are exclusive times like the other phases and appear as rows of `*.startup.csv` (zero calls when the
  switch is off). They are recorded on the presentation thread only, like every phase.
- **Slots by source and type.** `slots_by_source` and `slots_by_type` count every launch slot composed for the battle,
  keyed by the arrival plan source (`launch`, `purchase`, `reinforcement_pool`, `station_spawn`, `ability_object`),
  `campaign_warm` (the campaign reserves) or `initial` (the units and map objects placed at the start). A slot is
  counted when it is added to the compose list, so its death clone is not counted separately.
  `slots_grown_by_source` counts slots added later, during play (`launch` or `purchase`); they are included in the
  totals above.
- **Model table.** `models` lists, per model path, the placements composed (`composed`), `decode_ms` from the load
  profile, `compose_ms` (the hull compose stage of all its placements) and `upload_ms` (its surface uploads, which
  nest inside `compose_ms`); `composed_paths` is the number of distinct model paths composed and `decoded_paths` the
  number decoded during the run. The list is the 40 most expensive by compose time; `<trace>.detail.csv` holds the
  complete table.
- **Types used.** `used_types` counts, per unit type, the units that stood in the battle: placed at the start, or
  bound to a drawn ship for the first time. `types_composed` / `types_spawned` / `types_spawned_and_composed` give
  the ratio of types composed at load to types actually spawned. Squadron containers and map props are counted by
  their own type name, so a type can be spawned without a slot.

`<trace>.detail.csv` has the columns `run,kind,key,a,b,c`: `slots_source`, `slots_type` and `spawned_type` rows
carry the count in `a`; `model` rows carry `composed`, `compose_ms` and `upload_ms`.

Limits: the recorder is thread-local, so work on other threads (the identity hash workers, the audio load worker) is
not in these numbers; a cold disk cache and the machine's other load move every time, so compare runs on one host with
the same binary and the same storage state.
