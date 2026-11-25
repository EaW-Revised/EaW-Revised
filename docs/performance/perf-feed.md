# Perf telemetry feed and capture

A local, write-only recorder of the simulation and frame timing the game already measures. Off by default: with no switch
there is no writer thread, no file and no ring (the sessions pay one atomic load when they start). Nothing in the
simulation, a hash, a save, a replay or the network reads it.

## Switches

- Command line: `--eawr-telemetry <dir>`.
- Settings file: `diagnostics.telemetry=<dir>` in the player settings file (the options screen keeps the value when it
  saves; there is no options-screen entry and no perf number is ever shown on an options screen).
- The command line wins. Both start the writer once at viewer start; it ends with the viewer host.
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
| `source` / `source-end` | `src`, `label` (source only), `t` |
| `phase-name` | `src`, `id`, `name` (once per segment) |
| `tick` | `src`, `n` tick, `t`, `ms` wall cost, `lag` pacing lag, `lock` session-lock wait, `cpu` sim-thread on-CPU share (-1 unknown), `iv` requested interval, `fl` flags (1 clock reset, 2 land, 4 scripted, 8 battle), `ph` `[[phase id, ms], ...]` |
| `frame` | `n` frame, `t`, `ms`, `main`, `render`, `gpu`, `lock` (-1 = not measured), `stale` |
| `marker` | `src`, `k` kind name, `n` tick (or frame when `fs`), `v`, `fs` |
| `hitch` | `kind` `tick` or `frame`, `src`, `n`, `t`, `ms`, `median`, `phase` (largest excess over its own median), `excess`, `markers` (kinds within +-2 ticks), `day`, `speed`, `mode` |
| `pc` | `t`, `busy`, `own`, `others` (shares 0..1, -1 unknown), `commit` ratio, `avail_mb`; once per second |
| `verdict` | `src`, `t`, `text` (the core verdict rules over the last 5 s), `d` delivered tick ratio, `p95` |
| `pattern` | `src`, `t`, `text`; only when the readout changed |
| `dropped` | `src` (-1 = frames), `t`, `total` records lost to a full ring |

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

## Not in P6

The frame side's producer (`FeedWriter::push_frame_active`) is the hook the frame-side work calls; until it does, feeds hold
no `frame` records and the verdict's frame rules read "not measured". The in-game rows and the external web view are later
phases.
