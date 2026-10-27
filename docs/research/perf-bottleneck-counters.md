# Per-thread CPU / GPU busy / wait counters: design (spec only, no code)

Status: design, 2026-10-25, worker pf-nb-counters (no-build: code and log reading only; nothing here was run).
Base: origin/main `22fe69338`-era. Owner principle (10-25 19:10): every measurement carries per-core CPU, per-thread CPU time,
GPU busy % and wait/stall time next to frame/phase time, so the bottleneck (saturated vs idle resource) is named, not guessed.

## 1. What exists on main

| Source | Where | Gives | Gap for the principle |
|---|---|---|---|
| Perf trace CSV (`--eawr-perf-trace`) | `apps/viewer/src/perf_trace.cpp:55-146` (header `:55-70`, row `:99-146`), `perf_trace.hpp:30-97` | a wide per-frame column set (literal header groups at `:55-70` plus per-render-category loops; the count is not fixed, so not quoted here): `frame_ms`, `process_ms` (Godot TIME_PROCESS), `render_cpu_ms`/`render_gpu_ms` (root viewport query, skippable with `--eawr-perf-no-render-timing`, `:84-94`), draw calls, phase wall-clock ms (`FrameTimer`, `frame_timer.hpp`), `tick_wait_ms`, `ui_accessor_lock_wait_ms`, `sim_lock_ms`, `*_executor_wait_dispatch_ms` | All are main-thread *wall* times. No CPU time per thread, no per-core load, no real GPU busy %, no render-thread idle, no I/O wait. |
| Phase counters | `worker_phases` / `serial_phases` columns (sim `LiveTickCost`, `include/eawr/platform/live_session.hpp:80`), `ThreadWorkerAdapter::PhaseCounts` (`include/eawr/platform/sim_workers.hpp:47`) | per-named-phase wall ms of the sim tick; inline/escalated/pool counts | Wall time of the phase, not "workers busy vs parked" per thread. |
| Load marks / startup trace | `apps/viewer/src/startup_trace.hpp` over `eawr/core/load_profile.hpp` ((legacy EAWR-4971) recorder) | ordered load phase marks (`ready_ms`, `first_tick_ms`, memory snapshots) | Wall time only; a slow phase cannot be told from a *waiting* phase. |
| Census | `apps/viewer/src/memory_attribution.hpp`, `container_memory.hpp` ((legacy EAWR-4914), (legacy EAWR-4847)) | memory / capacity, not time | Not a CPU/GPU counter; only shares the opt-in switch style. |
| Rig process sampler | `tools/rig/drivers/perf-sample.ps1:55-115` | every 2 s (`intervalMs`): private bytes, working set, process CPU seconds, per-thread `TotalProcessorTime` + wait reason into `<name>.threads.csv`; `tools/performance/remake_bench.py:393-410` turns it into `process_cores_tail`, `busiest_thread_fraction`, `top_threads` | Thread *ids* only (no names, so "which thread" is a guess); 2 s resolution; PowerShell `Process.Threads` enumeration is heavy and its overhead is "not subtracted"; no per-core load; no GPU. Windows only. |
| Ad hoc | workers run `nvidia-smi` / `typeperf` by hand (no committed tool: grep of `tools/` and `docs/` finds none) | GPU util on NVIDIA only | Not reproducible, NVIDIA-only, not aligned to frames. |
| PresentMon runner | `Invoke-PresentMonViewer.ps1` in the pf-engine-5 worktree scratch (`out/pf-engine-5/pm`, untracked, not on main; target path on main: `tools/rig/drivers/`) | present intervals, GPU busy per present | Needs ETW access on the rig account and an extra process; not on main. |
| Threads in the product | `src/platform/sim_workers.cpp:94` (pool workers 1..N-1, atomic-wait park `:39-41,234,244`), `:388` (state-hash thread), `src/platform/live_session.cpp:310` (sim tick thread), `apps/viewer/src/gpu_memory_trace.hpp:114` (journal writer), net jthreads (`src/net/*`), Godot's own main / render / audio threads | | **None is named**: `SetThreadDescription` / `pthread_setname_np` occur nowhere in `src/`, `apps/`, `include/`. Name-less threads are why the sampler reports ids. |

Bottom line: we have a detailed record of what the main thread spent wall-clock on, and nothing that says who was running vs blocked while it spent it.

## 2. Proposal

Three layers, all **off by default**, each with its own flag so a run pays only for what it asks:
1. In-process thread clock sampler (`ThreadClocks`, `--eawr-perf-threads`) -> extra perf-trace columns.
2. GPU busy source (`--eawr-perf-gpu`) -> extra columns, vendor-neutral first.
3. Wait accounting at named sync points (`WaitScope`, `--eawr-perf-waits`) -> extra columns.

Plus an upgraded rig sampler (per-core, names) and one analysis script that prints the **bottleneck table**.

### 2.1 Thread naming (prerequisite, tiny, always on)
`platform::name_current_thread(std::string_view)`: Windows `SetThreadDescription`, Linux `pthread_setname_np` (15-char limit), macOS `pthread_setname_np(name)`. Called at the top of: sim pool worker (`eawr-sim-w<N>`, `sim_workers.cpp:94`), state hasher (`eawr-hash`, `:388`), live tick thread (`eawr-tick`, `live_session.cpp:310`), gpu-memory journal (`eawr-gpumem`), net threads (`eawr-net-*`), and the viewer main thread (`eawr-main`). It costs one syscall per thread start and also fixes the existing sampler's "thread id N" problem without any counter flag.

### 2.2 Per-thread CPU time (`--eawr-perf-threads`)
- A registry of named threads `{name, native handle, tid}` filled by `name_current_thread`. Godot-internal threads (render, audio, loader) are found at the first frame by enumerating this process's thread ids (`Toolhelp32` / `/proc/self/task`), reading back each thread's description (`GetThreadDescription`) when Godot set one; unmatched ones are `other-<tid>`. In Separate mode the render thread is the unregistered thread with the largest cycle delta while the frame runs (heuristic; see open points).
- Per frame, per registered thread: `QueryThreadCycleTime` (Windows; not clamped to the ~15 ms scheduler quantum like `GetThreadTimes`) or `clock_gettime(pthread_getcpuclockid)` (Linux), `thread_info` (macOS). Cycles are calibrated to ms once per run over the first 500 ms against `GetThreadTimes`, and `GetThreadTimes` stays as 1 Hz ground truth.
- Fixed column set (does not scale with core count): `cpu_main_ms`, `cpu_render_ms`, `cpu_tick_ms`, `cpu_simw_sum_ms`, `cpu_simw_max_ms`, `cpu_simw_min_ms`, `cpu_hash_ms`, `cpu_audio_ms`, `cpu_other_ms` = CPU consumed since the previous row. Derived: busy fraction = `cpu_ms / wall_frame_ms`; `simw_max` vs `simw_min` shows pool imbalance (one saturated, others parked).
- Per-core utilisation of the whole machine (so other processes' load is visible, a standing rig problem): once per second, Windows `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`, Linux `/proc/stat`. Appended to the trace as `core_busy_max_pct`, `core_busy_mean_pct`, `cores_over90`, `host_other_cores` (machine busy minus our process), and written in full to a `cores.csv` sidecar.
- No-admin sources: Windows machine total `GetSystemTimes`, per-core `NtQuerySystemInformation(SystemProcessorPerformanceInformation)` or PDH `\Processor Information(*)\% Processor Utility`; Linux `/proc/stat` per-core lines, per-thread CPU `pthread_getcpuclockid` + `clock_gettime`, `/proc/self/task/<tid>/stat`.
- Cost: ~10 threads x one `QueryThreadCycleTime` (~0.3 us each) ~ 3 us per frame; thread enumeration only at frame 1 and at most 1 Hz after. Budget < 0.05 ms/frame, inside the < 0.2 ms limit.

### 2.3 GPU busy (`--eawr-perf-gpu`)
Tiered by what the host allows; a `gpu_source` column names the tier so an unavailable tier is "not measured", never 0.
1. **Vendor-neutral process GPU engine time, no admin**. Primary on Windows (documented): the PDH counter set `\GPU Engine(*)\Utilization Percentage`, per process (`pid_<pid>_...`), per engine (`engtype_3D`, `Copy`, `VideoDecode`, ...), any WDDM 2.0+ vendor (NVIDIA, AMD, Intel), Windows 10 1709+, readable without admin and without ETW. Sum of our pid's `engtype_3D` instances = `gpu_busy_pct`; the all-pids sum = `gpu_busy_total_pct`. Refinement/fallback: `D3DKMTQueryStatistics` per-process / per-node `RunningTime` (what Task Manager reads), but it relies on largely undocumented structs that vary by Windows build, so it is a second source behind PDH, not the default. PDH is polled at 1 Hz from a sampler thread (a query is not free, so never per frame) Linux: `/sys/class/drm/card*/device/gpu_busy_percent` (AMD amdgpu, unprivileged), i915 engine busy via perf PMU / `intel_gpu_top -J` (needs `perf_event_paranoid` lowered or CAP_PERFMON), NVML `nvmlDeviceGetUtilizationRates` (NVIDIA, unprivileged). macOS: not planned.
2. **Timestamp queries around the frame**: Godot already provides `render_gpu_ms` (`perf_trace.cpp:84-94`), but the query stalls the Separate render thread (the reason `--eawr-perf-no-render-timing` exists). Diagnostic tier only: valid in Safe mode or when the stall is accepted and labelled.
3. **PresentMon** out of process (rig tool, 2.6): `GPUBusy`, `GPUWait`, `CPUBusy`, `CPUWait`, `msBetweenPresents` per present joined to the trace by timestamp. Needs ETW (Performance Log Users membership or admin); the rig accounts are unverified (the GTX rig returned ETW access denied on 10-25), so the rig tool probes first, then falls back to tier 1 and says so.
Derived: GPU is the limit only if tier-1 `gpu_busy_pct` >= ~90% and the render thread is not idle; GPU idle with a saturated main thread = CPU bound.
Cost: one syscall per adapter per second, negligible; no change to the render path.

### 2.4 Wait / stall accounting (`--eawr-perf-waits`)
`WaitScope w(WaitSite::pool_join)` adds the blocked wall time and a count to a fixed array of `std::atomic<uint64_t>` ns per site (no allocation, no lock); the trace reads and resets it once per frame, like `FrameTimer`. Where a wait is already timed it is reused, not duplicated.

| Site (column `wait_<site>_ms`) | Who is blocked | Touch point |
|---|---|---|
| `tick_join` | main waits for sim tick(s) | exists as `tick_wait_ms` (`apps/viewer/src/live_session_frame.cpp:261`) |
| `sim_lock`, `ui_accessor_lock` | main on LiveSession locks | exist (`sim_lock_ms` at `apps/viewer/src/map_mode_live.cpp:170`, `ui_accessor_lock_wait_ms` at `:194`) |
| `pool_join` | tick thread / caller waits for pool partitions | `sim_workers.cpp` around `done_.wait` (`:361`) and `await_change(busy_, ...)` (`:234`) |
| `pool_park` | pool worker idle between phases (idle share per worker) | `sim_workers.cpp:244` `await_change(generation_, ...)` |
| `hash_backlog` | producer waits for the hasher | `sim_workers.cpp:409` `room_.wait` |
| `render_sync` | main blocked on the render thread (Separate sync, frame-setup, render-time queries) | around the sync-prone `RenderingServer` calls in `perf_trace.cpp:84-90` and the submit path (`submit_ms`) |
| `present` / `vsync` | swap wait | derived from `wall_frame_ms - viewer_main_ms - outside_viewer_ms` today; exact from PresentMon `GPUWait` / present timing |
| `io_wait` | any thread blocked in file/VFS reads | VFS read wrapper (`src/vfs/*`) and asset-load waits; sums per-call read time |
| `gpu_upload` | main in buffer/texture uploads | `upload_ms`, `idle_upload_ms` exist; add load-time uploads |

`pool_park` plus per-worker CPU is the key pair: low `simw_max` busy and high `pool_park` = parallel phases starved (a serial dependency); `simw_max` > 90% with the rest parked = imbalance.
Cost: one steady_clock pair (~25 ns each) per wait entered, only on paths that already block; ~20-100 scopes a frame = < 0.01 ms.

### 2.5 Load profile
Each load mark ((legacy EAWR-4971) recorder, `startup_trace.hpp`) gets, since the previous mark: per-thread-group CPU ms, `io_wait_ms`, `pool_park_ms`. "Asset phase 9 s, one thread saturated 9 s, pool parked, I/O 0.2 s" then reads directly as sequential loading, the serial-dependency finding the owner wants.

### 2.6 Rig tools
- `tools/rig/drivers/perf-sample.ps1`: when the viewer wrote `<name>.perf-threads.csv` (2.2), read that (named threads, per-frame resolution, no PowerShell thread enumeration); keep the current 2 s `Process.Threads` path as fallback for builds without the flag. Add machine per-core via `GetSystemTimes`/`NtQuerySystemInformation` P/Invoke (not `Get-Counter`, also heavy). Write the sampler's own CPU seconds to its log so `remake_bench.py` can flag/subtract it.
- GPU: the PDH `GPU Engine` counters (1 Hz `Get-Counter` is acceptable for this one set; the viewer's own sampler is preferred when `--eawr-perf-gpu` ran) as default; `nvidia-smi --query-gpu=utilization.gpu -lms 1000` only as an optional NVIDIA cross-check, never a dependency.
- PresentMon: move `Invoke-PresentMonViewer.ps1` into `tools/rig` as opt-in `-PresentMon`, with an up-front ETW-access check and a one-line "tier 3 unavailable, using tier 1" fallback so ETW trouble degrades instead of failing the run.

## 3. Analysis script and the standard bottleneck table
New `tools/performance/bottleneck_table.py <trace.csv> [--cores cores.csv] [--window last60s|busiest300|all]`, used by `remake_bench.py` and `melee_parity_analyze.py` (both already parse the trace). It prints, per window, mean (p95):

```
Stage / resource        busy%   idle%   wait ms/frame                  verdict
main thread             93      7       render_sync 1.2, tick_join 0.4 SATURATED
render thread           41      59      -                              idle
sim tick thread         68      32      pool_join 3.1                  partial
sim pool workers (n=7)  max 88 / mean 31 / min 4   pool_park 71%       IMBALANCED
state-hash thread       3       97      hash_backlog 0                 idle
GPU (3D engine)         52      48      present/vsync 2.0              not limiting
CPU cores (machine)     max 100 / mean 34, cores >90% = 1               ONE CORE SATURATED
I/O                     -       -       io_wait 0.0                    not limiting
Top-3 bottlenecks: 1. main thread saturated (render_sync) ...   (saturated resource / idle resources / evidence / lever)
```
Rules: saturated at busy >= 90% over the window, idle at < 25%. A saturated thread is the bottleneck (its largest wait/phase column is named). Nothing saturated while frame time is high -> "SERIAL DEPENDENCY or WAIT": print the largest `wait_*` site and any thread sleeping while others are parked as the lever. A footer carries host, build sha, mode and method (pf-common). JSON output so `remake_bench.py` embeds it in place of today's `busiest_thread_fraction`.

## 4. Budget, defaults, portability
- All counter flags off in ordinary play: no registry reads, clocks or sidecars (the `FrameTimer` rule). Only thread naming (2.1) is always on.
- Combined budget with every flag: estimated < 0.1 ms/frame (2.2 + 2.3 + 2.4). The 0.2 ms/frame ceiling is the target given in this task's brief (coordinator/owner), not a measured limit. Verified by a same-host alternated A/B with and without the flags (PR C7); each row also records `perf_counters_ms` (self-cost) so the budget is auditable per run.
- Cross-vendor: only tier 1 GPU (PDH `GPU Engine`) and the CPU parts are required; NVML/sysfs/i915 are per-vendor refinements behind `gpu_source`.
- Linux: thread time `pthread_getcpuclockid`, cores `/proc/stat`, GPU per 2.3, task list `/proc/self/task`. macOS: names + `thread_info`; GPU not planned.
- Counters read clocks and OS statistics only: no sim state, command or hash is touched, so "no sim change" holds and replay76 is not needed for these PRs.

## 5. Implementable PRs
Lane-h basis: closed PRs, elapsed created -> merged (includes queue and review waits): (legacy EAWR-4936) +72/-12 8 files 0.1 h; (legacy EAWR-4934) +117/-5 6 files 0.4 h; (legacy EAWR-4961) +117/-16 3.9 h; (legacy EAWR-4940) +353/-8 6.0 h; (legacy EAWR-4847) +676/-7 7.4 h; (legacy EAWR-4914) +1178/-8 10.8 h; (legacy EAWR-4923) +315/-11 17.9 h. Only C1 has a two-PR class: small = (legacy EAWR-4934) (0.4) + (legacy EAWR-4961) (3.9) -> median 2.2 (mean of two), p80 ~3.5 (rounded toward the larger). C2 rests on (legacy EAWR-4940) alone (median 6.0), C3 on (legacy EAWR-4847) alone (7.4), C4 is sized like (legacy EAWR-4940) (6.0: one new sampler file plus perf_trace columns). Every p80 of a single-PR class and every row with no comparable PR (C5, C6, C7) is **judgement**, marked `j`, taken as about 1.5x the median. The C++ ones need one Windows pool build plus a short rig smoke each.

| PR | Scope | Touch points | Class | lane-h median / p80 (basis) |
|---|---|---|---|---|
| C1 thread names | `platform::name_current_thread` + call sites | new `include/eawr/platform/thread_name.hpp`, `src/platform/thread_name.cpp`; `src/platform/sim_workers.cpp:94,388`; `src/platform/live_session.cpp:310`; `apps/viewer/src/gpu_memory_trace.hpp:114`; `src/net/relay_client.cpp:59`, `src/net/online_lobby_source.cpp:339`, `src/net/port_mapping.cpp:354`; viewer main entry | small ((legacy EAWR-4934)/(legacy EAWR-4961)) | 2.2 / 3.5 (two PRs) |
| C2 thread CPU columns | `ThreadClocks`, calibration, fixed columns, 1 Hz core sidecar | new `apps/viewer/src/thread_clocks.{hpp,cpp}` (+ `sources.cmake`); `perf_trace.cpp:25-41` flag parse, `:55-70` header, `:99-146` row; test modelled on `tests/presentation/renderer/test_perf_trace_no_render_timing.py`; `docs/performance/battle-bench.md` | medium (legacy EAWR-4940) | 6.0 / 10 j (one PR) |
| C3 wait scopes | `WaitScope`/`WaitSite`, atomic ns array, pool / hasher / render_sync / io sites | new `include/eawr/core/wait_scope.hpp`; `src/platform/sim_workers.cpp:234,244,361,409`; `apps/viewer/src/live_session_frame.cpp:261`; `map_mode_live.cpp:119,194`; `perf_trace.cpp` columns; VFS read wrapper in `src/vfs/` | medium+ (legacy EAWR-4847) | 7.4 / 11 j (one PR) |
| C4 GPU busy | PDH `GPU Engine` sampler (+ optional D3DKMT, Linux sysfs/NVML), `gpu_source` column | new `apps/viewer/src/gpu_busy_sampler.{hpp,cpp}`; `perf_trace.cpp` header/row; CMake (system `pdh` only, no new dependency) | like (legacy EAWR-4940) | 6.0 / 10 j |
| C5 load-mark counters | per-mark thread CPU + wait deltas | `apps/viewer/src/startup_trace.hpp`, `eawr/core/load_profile.hpp`; after C2/C3 | small-medium, no comparable PR | 3 / 6 j |
| C6 rig + analysis | `bottleneck_table.py`, `perf-sample.ps1` upgrade, PresentMon into `tools/rig` with ETW fallback, embed in `remake_bench.py` | new `tools/performance/bottleneck_table.py`; `tools/rig/drivers/perf-sample.ps1:55-115`; `tools/performance/remake_bench.py:393-410`; new `tools/rig/drivers/Invoke-PresentMonViewer.ps1` (copied from the pf-engine-5 scratch) | medium, Python/PS only, no comparable PR | 5 / 9 j |
| C7 budget verification | A/B alternated trace with/without all flags: self-cost < 0.2 ms | rig runs only, no code | measurement, no comparable PR | 3 / 5 j |

Order: C1 first (tiny, makes names readable everywhere). C6's `bottleneck_table.py` needs no C++ and can start now against today's `threads.csv` and trace columns, gaining rows as C2-C5 land. C2, C3, C4 all edit the `perf_trace.cpp` header and row, so serialise them (shared-file lease) or land a column-group mechanism in C2 first. C7 waits for C2-C4 (it measures their combined cost). Total ~32.6 / 54.5 lane-h (medians 2.2+6.0+7.4+6.0+3+5+3; p80s 3.5+10+11+10+6+9+5; only C1's p80 is evidence-based); the Python-only C6 (~5 / 9) can run on a no-build worker.

## 6. Open points
- Cycle time vs wall time on laptop CPUs (E-cores, turbo): C2 calibrates and also reports `GetThreadTimes`; the first PF job decides whether cycles are worth the complexity.
- Identifying Godot's render thread by enumeration is heuristic; naming it in the patched Godot (fork `eawr/4.7.2-stable`) would be exact but needs the owner's GO per pf-common.
- PDH `GPU Engine` availability (needs WDDM 2.0 driver counters) and PresentMon ETW access on the rig accounts are untested here (no-build, no rig); C4/C6 start with a short availability probe per rig host.
