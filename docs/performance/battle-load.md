# Battle startup measurements

The viewer's `--eawr-report` includes a `startup` object for battle loads. With
`--eawr-perf-trace`, the same measurements are written to a companion
`<trace>.startup.csv`; the frame CSV keeps its existing format.

A setup load starts at the Start handler, before selection validation, and ends
at `frame_post_draw` after the battle's first process step. A direct launch uses
the OS process creation time on Windows, anchored once to the monotonic clock;
all subsequent durations use `steady_clock`. The return duration ends at the
first completed draw of the restored setup screen. Each successive Start adds a
run to the report, so repeated battles remain distinguishable.

Other platforms currently label the direct-launch anchor `extension_load` rather
than claiming to include the engine's earlier process startup.

Phase timings report exclusive and inclusive milliseconds plus invocation counts.
Nested work contributes to its innermost exclusive phase. Sum exclusive costs to
compare phases; inclusive costs describe a caller's total cost and must not be
added together. Asset phases include VFS reads and validation. Texture upload is
separate from texture decode. Scene composition covers preparation outside the
named subsystems. First simulation tick, Lua and visibility costs come from the
simulation's existing diagnostic counters and are reported separately because
they can overlap work on the presentation thread.

Material creation contains nested shader compile, shader reflection and material
binding scopes. Mesh GPU upload, scene build, population and environment scopes
separate composition work. Source selection and generation remain in the outer
material scope. Driver pipeline
work also happens at first draw, so `first_draw_wait_ms` records the interval from
prepared scene to completed draw, and `pipeline_compilations` gives Godot's canvas,
mesh, surface, draw and specialization counters. That interval includes frame
scheduling and other renderer work; it is not an isolated pipeline compile time.

The GPU benchmark in `tests/presentation/renderer/test_battle_load.py` runs Polus,
the Maw and Coruscant three times each, for direct launch and two Starts from
setup, cold and warm. Run it through `Invoke-RigSuite` with `-Exclusive`,
`-Pattern test_startup_runs` and a private desktop. The suite runner's explicit `--eawr-load-bench-cache` groups
reuse a private writable root within a cold/warm pair; other launches retain
their usual isolation. Cold means a new Godot user shader-cache directory, not a
flush of the shared host's OS disk cache or graphics driver's global cache.

Captures use map lighting and shadows. Keep reports and images under ignored
`out/`; compare decoded pixels at the same fixed camera and held tick, and verify
the report's headless replay comparison before drawing a performance conclusion.

The setup screen retains an immutable mounted-content and catalog snapshot.
Start and subsequent battles share those inputs, with zero XML catalog loads
inside the measured Start interval. Sessions, unit tables, decoded assets,
meshes, textures and materials are still built for each battle. Content changes on disk require
restarting setup to refresh its snapshot; no snapshot crosses installations or
profiles. `--eawr-content-cache off` retains the original remount/reparse path
for measurement, and `-Pattern test_startup_before` runs that control matrix.

## Setup shader lifetime

Project rule R-LOAD-SHADER-01: one setup instance owns an optional immutable
shader cache. Its key is the exact final backend shader text, including the
compile probe, shadow variant and stored-colour adaptation. An entry owns only
an accepted shader RID, reflected uniforms and lexical tokens. Compiler and
portable admission run once for that exact source; each material's binding
admission still runs on every upload. Invalid sources and refused bindings
never add an entry. Mutable materials, textures, fog, lighting, wind and scalar
parameters remain per battle. Each resource retains its shader entry, so
teardown cannot free a shader still drawn elsewhere. No lookup runs per frame.

The cache retains at most 64 entries and 1 MiB of source keys. Once either
budget is full, new sources use the ordinary uncached lifetime; existing
entries are never evicted. These are project retention budgets, with no claim
about the original game's internal resource management. The cache is local to
one setup and cannot persist across installations or profiles in the viewer.
Restarting setup also releases it.

`--eawr-shader-cache off` disables this reuse in the same binary. The
`test_shader_cache_before` and `test_shader_cache_after` matrices keep content
reuse on in both controls, run three cold/warm pairs per map and three Starts
per process, and retain each battle's report, replay, tick hashes and lit
capture. They sample process-tree working set after teardown and again 300 ms
later, including the console launcher's engine child. Both controls cap the
engine at 60 frames per second to keep the restored setup visible for at least
one second between Starts. Work budgets count real
shader compiler calls: at most 64 on the first Start and zero on later Starts
for the benchmark maps. The runtime renderer exercise also checks invalid
binding rejection on a cache hit and uncached fallback at both cache budgets.
