# Performance overlay

A dev tool of the production viewer (EAWR-558): FPS, the frame-time graph and the simulation's cost per
tick, drawn over the view. It is not part of FoC's look; clarity comes first. It is presentation only:
it reads wall-clock timers, never a command, a snapshot, the replay or a hash, so a run with it shows
the same tick hashes as a run without it.

## Use

- **Key: F3** toggles it in any map run (space or land, live session or not). Modifier chords do not.
- **`--eawr-perf-overlay on|off`** (default `off`) starts a run with it shown, for captures. The key
  still toggles it afterwards.
- It costs nothing while hidden: no timer runs and the font cache is only read the first time it is
  shown.

Why F3: nothing in our own bindings uses it (the tactical camera, the battle input and the HUD), and
the debug build's hotkey survey puts its function-key tools on the main menu only. FoC's retail tactical
keymap has not been checked key by key ("unverified"); the key is one constant
(`EawrPerfOverlay::key_name` and `MapMode::input`) if a clash turns up.

## What it shows

| Row | Meaning |
| --- | --- |
| FPS, frame ms | Frames of the last second; the last frame's wall-clock time. Green at 55 FPS or more. |
| avg, worst / 5 s | The average and the slowest frame of the last five seconds. |
| frame graph | One bar per 1/150 of the last five seconds, each the worst frame in it, so a hitch is a spike. Lines at 16.7 and 33.3 ms; the scale steps up when a frame exceeds it. |
| Sim tick | What the newest tick cost the simulation thread (`step` plus the `fog` copy for the world), its average and worst over five seconds, and a second graph. Lines at 16.7 and 33.3 ms (the 30 Hz tick period). |
| phases | The newest tick's named parts. |
| draw calls, units | The renderer's draw calls in the last frame, and the units the local player sees. |
| overlay | The overlay's own CPU time per shown frame (recording the samples, plus the draw commands it rebuilds at 20 Hz; the canvas keeps them between rebuilds). |

Bars are green up to 16.7 ms, amber up to 33.3 ms and red above. The panel sits at the top left and its
text scales with the view height (13 px at 720 lines, 39 px at 2160), in the HUD's `EmpireAtWar-Medium`
face from the font cache (the engine font without one).

## Tick-cost source

`platform::LiveSession` times each tick on the simulation thread (`LiveTickCost`: a total and named
`LivePhaseCost` parts) and keeps the newest 4096 ticks; `tick_costs_after(tick)` hands them out under the
session's existing lock. The overlay takes whatever phases a tick carries, so another sim phase timing
(for example the order tick's path work of EAWR-520) appears by appending a `LivePhaseCost` to the tick it
belongs to; nothing in the overlay names a phase. The timers sit beside `TacticalSession::step`, never
inside it.

## Report

Every map report has a `perf_overlay` object (`shown: false` while it was never shown). When shown it adds
`fps`, `frame_ms`, `frame_ms_avg`, `frame_ms_worst`, `tick_source` (`live_session` or `none`), `ticks`,
`tick_ms`, `tick_ms_avg`, `tick_ms_worst`, `tick_phases`, `draw_calls`, `visible_units`, `toggles`,
`font`, `font_size`, `rect` and `own_cost_ms` (`samples`, `avg`, `max`).
`tests/presentation/renderer/test_live_session.py` checks them and the key on a GPU host;
`tests/ui/perf_stats_tests.cpp` covers the model (`include/eawr/presentation/ui/perf_stats.hpp`).
