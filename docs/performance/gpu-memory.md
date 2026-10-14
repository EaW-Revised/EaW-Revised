# GPU memory accounting and texture sharing

The viewer's `--eawr-report` includes a `gpu_memory` object. Each sample holds
Godot's own device counters (`device.textures`, `device.buffers`, `device.total`,
in bytes) and the renderer's ledger of what it uploaded, by category
(`textures`, `texture_arrays`, `meshes`):

- `live` / `live_bytes`: resources currently held and their estimated bytes;
- `unique` / `unique_bytes`: distinct contents among them, as if each were held once;
- `duplicate_bytes`: `live_bytes - unique_bytes`;
- `compressed_bytes` / `uncompressed_bytes`: block-compressed (BCn) and other formats;
- `uploads`: every creation since the process started; `reuses`: requests served by a
  resident copy instead.

Samples are taken when the setup screen is ready, when a battle starts from it, at
the battle's first frame, at presented ticks 300, 1800, 18000 and 54000 (10 s, 1,
10 and 30 minutes), when the battle is released, when the setup screen draws
again and after 30 setup frames (`setup_settled`, after Godot's deferred frees).
`current` is read when the report is written. Setup and transition samples
also print an `EAWR gpu memory` line. Periodic samples are persisted by one native
file worker, keeping console flushes off the battle frame while retaining history
if the view fails before writing a report. Engine counters are still read on the
main thread; only immutable JSON text crosses the worker boundary.

The journal is `<report path>.gpu-memory.jsonl` when `--eawr-report` is supplied;
otherwise it is `logs/gpu-memory-<process id>.jsonl` under the viewer's user-data
directory. Its path is announced during startup and appears as `journal_path` in
the report. `journal_failed` reports an I/O failure. Reports drain pending writes,
and viewer shutdown joins the worker before library unload. A sample's
`console_printed` distinguishes startup/transition logging from periodic journal
records, and its report-only `cost_ms.query` and `cost_ms.log` separate counter
sampling from formatting/enqueue or console work.

Ledger bytes are estimates (a texture's stored
mip chain, three-channel formats padded to four; mesh vertex streams as uploaded);
the `device` counters are authoritative. Device textures beyond the ledger's are
render targets, shadow maps, particles and UI.

## Rules

- **GM-05: demand residency under memory pressure.** While projected allocations fit
  the device's usable budget, uploads stay eager and submitting a frame performs no
  residency scan. When an upload would cross the budget, later mesh and texture
  uploads retain immutable CPU recipes instead. Measured pressure also activates
  this tier when render targets become measurable, without needing another upload.
  Shaders, materials, skin metadata
  and asset leases remain available. Submitted snapshot instances define demand:
  unused meshes and their pooled texture references are released before demanded
  recipes are uploaded. Reserve, construction and garrison slots that have no
  submitted pieces consume no mesh or texture residency in this tier. A texture
  still referenced by another resident model stays shared. Fog declarations and
  stored skin poses survive eviction and bind again at admission. CPU texture
  recipes share content by the same two digests as the GPU pool.
  A refused admission is remembered until its demand leaves and returns or the
  configured device capacity or mip drop changes. Measured usage fluctuations do
  not re-arm refusals. Each refusal emits
  one diagnostic; unchanged frames never retry it. Rigid rest-pose validation runs
  before the eager/dormant split, so budget pressure cannot hide an invalid model.
  The report adds `residency` with cumulative `deferred`, `admissions`, `evictions`,
  `admission_failures`
  and the current `dormant` resource count. Fitting cards have zero admissions and
  evictions. This policy changes presentation only.
  `VideoSettings::texture_mip_drop` stores a project-authored 0..4 detail floor,
  applied through the existing settings application path. It does not map the
  retail basic or advanced detail presets; their XML mapping remains separate.

- **GM-01: one GPU texture per content.** A texture upload is keyed by two
  independent 64-bit digests of the bytes it hands to the GPU (format, size, mip
  chain). Every resource that samples the same pixels holds a reference to one
  pooled texture; the last reference frees it. Retail likewise keeps one texture
  per file name and shares it between models (debug build); keying by content
  also shares equal pixels under different names and never shares different
  pixels under one name. Textures are never written after creation, so sharing
  cannot leak a change from one model to another.
- **GM-03: one surface upload for every team colour.** A space battle uploads each
  model surface once. A colorized surface's team colour is set on each piece's
  instance (the colour the colorize shaders already take per instance, stored 0..1
  and linearised by the shader exactly where the material colour was), so capture
  tints and fog copies still override it per instance.
- **GM-04: adaptive texture budget.**
  - **The device's memory.** On first use the renderer reads the device-local memory
    of the GPU it renders on: on Vulkan the `VK_EXT_memory_budget` budget of the
    device-local heaps, else their sizes. Other drivers report nothing, and nothing is
    then constrained.
  - **The budget.** The process may fill nine tenths of that memory. Every pooled
    texture counts its resident bytes. Everything else on the device (render targets,
    meshes, buffers) is `budget.other_bytes`: Godot's device counter less the pool, and
    the pool may hold the rest (`budget.texture_budget`).
  - **Measuring.** The counter asks the allocator for its heap budgets, so the process
    reads it once every 30 frames, when a renderer submits. It reads 0 until frames are
    drawn, so it is read every frame until it first reads nonzero; a battle that loaded
    over budget steps down on its first frames. A texture freed by the pool stays on the
    device until the engine releases it, so its bytes are left out of `other_bytes` for
    three frames after the frame it was freed in.
  - **Mip drop.** A card with room for everything keeps every mip. When the pool is over
    budget, the process-wide mip drop rises one level (at most 4) and every resident
    texture is rebuilt in place without its top mip: read back once, same RID, so
    materials keep it. All textures keep the same detail whatever order they loaded in.
    At an upload (loading) every texture is rebuilt at once, including any an
    unfinished in-frame rebuild had not reached yet. In a running battle each
    renderer rebuilds 4 textures per frame until its pool has reached the drop, so a
    raise never stalls one frame with hundreds of read-backs. A drop never removes the
    last stored level, never leaves a top edge under 64 texels, and never touches a
    texture without a stored mip chain. A rebuild the engine refuses leaves that texture
    at its older drop and is counted (`rebuild_failures`). The drop stays while textures
    are resident and starts over from the detail setting once the pool is empty.
  - **Switches.**
    - `--eawr-texture-mip-drop 0..4` is the detail setting: a floor under the drop on
      any card.
    - `--eawr-vram-budget <MiB>` budgets as if the device had that much memory (1 to
      16777216; 0 means the device's own figure), for small-card checks.
    - `--eawr-vram-budget off` turns the budget off: no probe, no counter reads. It is
      for measurement only.
  - **Report.** The report's `gpu_memory` samples carry the `budget`: `constrained`
    (whether a device budget applies at all), device bytes and source, texture budget,
    other bytes, resident bytes, the current drop (`mip_drop`), the detail setting's
    floor under it (`mip_drop_floor`), `downsized` textures, `over_budget` checks
    (uploads and frames) that still found the pool over budget at the deepest drop, and
    `rebuild_failures`.
  - **Scope.** Texture arrays, shared named textures and meshes are outside the pool;
    they count as other memory.
- **GM-02: measurement switch.** `--eawr-texture-share off` gives every texture
  request its own GPU copy again (the footprint before GM-01), so one build reports
  both. It is for measurement only.

Measured with GM-01 (RX 7900 XTX, 2-player stock skirmish, device total at tick
1800): Coruscant 540 MB unshared, 174 MB shared; Polus 517 MB and 169 MB; Polus
started from the setup screen 525 MB and 203 MB. Before GM-01 every stock texture
was uploaded about 17 times (once per model surface and team colour).


## Shared surface measurements (GM-03)

Measured on 2026-10-14 in one exclusive GTX 970 4 GB rig job, with stock maps
and identical original measurement arguments. Values are the presented-tick-1800
samples; MB is decimal. Device totals are authoritative; mesh bytes are ledger
estimates. Main is `7a5bd7093f791fbdf0ba7ac693a3fe71f63cc7c5`; the shared-surface
viewer is `837dfd4fcc1492a920e8467ec83540acb2055d45`.

| Case | Main device MB | Shared device MB | Main meshes / MB | Shared meshes / MB | Main / shared surface uploads |
|---|---:|---:|---:|---:|---:|
| Coruscant direct | 183.634 | 179.928 | 1342 / 31.048 | 1246 / 27.392 | 1322 / 1226 |
| Polus direct | 179.270 | 174.893 | 1284 / 28.757 | 1161 / 24.445 | 1269 / 1146 |
| Polus from setup | 216.576 | 214.305 | 1227 / 26.681 | 1161 / 24.445 | 1212 / 1146 |

The two-colour corvette fixture assigns both hulls the same surface asset while
preserving their distinct blue and green colours. Its images are pixel-identical
to main at ticks 0, 170 and 340. Full world-image comparisons of the three table
cases are also identical; those extra renders hide the HUD's wall-clock minimap
ping, while the memory runs retain their original HUD arguments. Direct and
replay suppress the same 41 map records.

Receipt: `out/sk-2856-r3/paired-gpu/20261014-180058-70eb4f/summary.json`.
Detailed samples: that run's `tmp/eawr-team-paired-5ipzhtpu/comparison.json`.
These stock cases establish mesh savings on a 4 GB card; the complete Remake
memory budget remains a separate acceptance check.

`populate.team_colour.colour_variants` counts per-instance colour assignments.
`populate.live_units.surface_assets_by_unit` records composition asset IDs;
these numeric receipts survive teardown without retaining GPU resources.
