# Perf telemetry feed and capture

A local, write-only recorder of the simulation and frame timing the game already measures. Off by default: with no switch
there is no writer thread, no file and no ring (the sessions pay one atomic load when they start). Nothing in the
simulation, a hash, a save, a replay or the network reads it.

## Switches

- Command line: `--eawr-telemetry <dir>`.
- Settings file: `diagnostics.telemetry=<dir>` in the player settings file (the options screen keeps the value when it
  saves; there is no options-screen entry and no perf number is ever shown on an options screen).
- The command line wins. Both start the writer once at viewer start; it ends with the viewer host.
- The settings line alone is enough for a full player capture: it also turns on the frame side (the `frame` records below)
  without `--eawr-frame-diag`, the per-thread CPU rows, the `context` record, the memory rows and the fight counters.
- Developer chord **Shift+F3** (tactical and galactic views): write a capture now. Does nothing while the feed is off.
- The launcher toggle comes later with the mod-manager work.

## Files

`<dir>/eawr-perf-feed-NNNN.jsonl` are feed segments (a segment closes at 64 MiB, the newest 4 stay: 256 MiB at most).
Each segment starts with the `header` record and re-announces its sources and phase names, so one segment reads alone.
`<dir>/captures/capture-<UTC date-time>-<n>.json` are captures. Nothing in either carries a host, user, path or process
name; the PC context is aggregates only.

## Feed record schema (version 1)

JSON Lines, one object per line, `"r"` is the record kind. Times `t` are `steady_clock` microseconds (on Windows
QueryPerformanceCounter, so an external process on the same PC can align to them). `src` is the source id: one per attached
session (`label` `tactical` or `galactic`; a galactic campaign running a battle has two). Milliseconds are floats.

| r | fields |
|---|---|
| `header` | `schema`, `clock` (`steady-us`), `build`, `engine`, `t` |
| `source` / `source-end` | `src`, `label` (source only), `workers` (source only, when known: the session's simulation worker count), `t` |
| `phase-name` | `src`, `id`, `name` (once per segment) |
| `tick` | `src`, `n` tick, `t`, `ms` wall cost, `lag` pacing lag, `lock` session-lock wait, `cpu` sim-thread on-CPU share (-1 unknown), `iv` requested interval, `fl` flags (1 clock reset, 2 land, 4 scripted, 8 battle), `ph` `[[phase id, ms], ...]` |
| `frame` | `n` frame, `t` (frame start), `ms`, `main`, `render`, `gpu` (the newest GPU time the renderer had; its source frame is not known), `lock` (-1 = not measured), `stale`; in a tactical battle also the fight intensity of the frame: `u` own units seen, `eu` hostile units seen, `atk` units with an attack target, `proj` projectiles in flight, `fx` live battle effects, `part` live particles, `dc` draw calls, `obj` visible instances (the renderer's own counts of the preceding frame) |
| `context` | the PC and settings, once at start of every segment and again when a field changes (checked every 2 s): `cpu` model, `cores`, `logical`, `p_cores`, `e_cores`, `p_logical` (performance/efficiency split where the OS reports it), `topology`; `gpu`, `gpu_vendor`, `gpu_type`, `gpu_api`, `gpu_driver`, `renderer`, `rd_driver`; `res` `[w,h]`, `window`, `vsync`, `fps_cap`; `render_thread` (the running mode) and `render_thread_setting`; `video` (the saved detail and render choices); `threads` with `hardware` and per pool (`sim`, `particles`) `setting`, `automatic`, `mode` (`automatic` or `manual`), `resolved`. The worker count a session really uses is the `workers` field of its `source` record. Aggregates only |
| `thr` | once per second: `th` = `[[name, share, labeled], ...]`; `share` is CPU seconds per wall second (1.0 = one thread-equivalent). Labeled threads (`main`, `render`, `sim`, `gc-sim`, `sim-worker-N`, `gc-worker-N`, `particle-worker-N`, `particle-lane`) are always listed; other threads of the process (audio, engine) appear when above 2 %, under the name the engine gave them (or `t<id>`), at most 12 of them |
| `mem` | once per second: `commit_mb` (private bytes committed by this process), `ws_mb` (working set), `pf_s` (page faults per second, -1 for the first sample) |
| `marker` | `src`, `k` kind name, `n` tick (or frame when `fs`), `v`, `fs` |
| `hitch` | `kind` `tick` or `frame`, `src`, `n`, `t`, `ms`, `median`, `phase` (largest excess over its own median), `excess`, `markers` (kinds within +-2 ticks), `day`, `speed`, `mode` |
| `pc` | `t`, `busy`, `own`, `others` (shares 0..1, -1 unknown), `commit` ratio, `avail_mb`; once per second |
| `verdict` | `src`, `t`, `text` (the core verdict rules over the last 5 s), `d` delivered tick ratio, `p95` |
| `pattern` | `src`, `t`, `text`; only when the readout changed |
| `dropped` | `src` (-1 = frames, -2 = frame phases), `t`, `total` records lost to a full ring |

A tick's phases are the coarse phases always and the fine rows when fine sampling is on (not yet switched on by the settings
line). `fph` (`n` frame, `ph` `[[id, ms], ...]`, names announced as `phase-name` with `src` -1) is the main-thread phase row of a
frame; nothing produces it yet.

A tick's phases are the coarse phases always and the fine rows when fine sampling is on. A hitch record is written 3 ticks
after its tick, so the markers of the following ticks are in it. Unknown fields must be ignored by readers; a changed
meaning bumps `schema`.

## Cost

The writer thread runs at below-normal priority and wakes every 100 ms. It reads the diagnostics rings (copies of values the
sessions already published), formats one buffer per wake-up and writes it. A producer never waits for it: a full ring
overwrites its oldest records and the loss shows up in the `dropped` records. No game thread does file I/O. A session that
starts or ends while a feed runs waits at most for one drain pass of the writer thread (it takes a short mutex).
The writer keeps the last 60 s of records in memory (bounded, about 16 MiB per source) for captures.

## Capture

Chrome trace-event JSON, opens in `chrome://tracing` and the Perfetto UI (nothing is uploaded; opening it is the reader's
choice). One track per source for ticks, one track per phase (rows under 0.05 ms are left out), a track for markers and
hitches, a frames track, counter tracks for tick cost and other-process CPU. `otherData` carries the schema, build, engine and
the last pattern readout. Phases are placed at the start of their tick: a phase track shows size and recurrence, not
the order of work inside a tick.

## Reading a file

`python tools/performance/perf_capture_summary.py <feed.jsonl ...|capture.json>` prints the tick and frame percentiles, the
hitch list with the spacing to the previous hitch and the dominant phase, the repeating-pattern readout and the verdict. It
also reads the older `EAWR_GC_TICK_TRACE` series (the tool replaces `gc_tick_trace_summary.py`).

## Threads, memory and fight counters

`ThreadLabel` (include/eawr/core/perf_diag/thread_cpu.hpp) is how a thread names itself when it starts; it does nothing
(one atomic load) without a feed. The writer reads the CPU time of every thread of the process once per second (Windows:
Toolhelp snapshot + GetThreadTimes; Linux: /proc/self/task) on its own below-normal thread, so which thread is pinned is
answerable even for threads the game did not label (the audio thread, the engine's). The fight counters are read on the main
thread at the end of a tactical frame only while a feed runs (a loop over the snapshot's instances); draw calls and visible
instances are read inside the render-thread callback the frame side already queues. The AI lane has no thread of its own in
this build; when one exists it labels itself `ai-lane`.

## Reading the feed

`python tools/performance/perf_capture_summary.py <feed.jsonl ...>` prints, besides the tick and frame series, the context,
a per-thread CPU table (mean, p95 and max share per thread, the hottest labeled thread), the memory rows, and the fight
counters with their correlation to the frame time plus the frame time by fight-intensity quartile.

## Not yet

The in-game rows and the external web view are later phases. The per-phase sim rows (`FeedOptions::fine`) and the
main-thread frame phases (`fph` records, `FeedWriter::intern_frame_phase_active` / `push_frame_phases_active`) are in the
writer and the schema test but no player run switches them on yet.
