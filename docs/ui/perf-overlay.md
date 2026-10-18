# Performance overlay

A dev tool of the production viewer: FPS, the frame-time graph and the simulation's cost per
tick, drawn over the view. It is not part of FoC's look; clarity comes first. It is presentation only:
it reads wall-clock timers, never a command, a snapshot, the replay or a hash, so a run with it shows
the same tick hashes as a run without it.

## Use

- **Key: F3** toggles it in the tactical views (space or land, live session or not) and in the galactic view
  (`--eawr-galactic`: the galaxy map and the planet views). Modifier chords do not.
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
| TICKS/S | Simulation ticks completed per real second, measured: the ticks of the last second over that second. Colour: green within 10% of the rate the speed setting asks for, amber down to 60%, red below. In the galactic view the right side shows the clock in effect, `speed x4.00 = 120/s` (the factor of the displayed snapshot over the 30 Hz base), or `paused`. A speed setting that is not delivered shows here as a measured rate under the asked one; the Sim tick row says whether the tick cost explains it. |
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
session's existing lock. `platform::LiveGameSession` (the galactic view) does the same for each parent step, with
the phases `command-source` (the callback that runs the GC AI) and `session-step`. The overlay takes whatever phases a tick carries, so another sim phase timing
(for example the order tick's path-search work) appears by appending a `LivePhaseCost` to the tick it
belongs to; nothing in the overlay names a phase. The timers sit beside `TacticalSession::step`, never
inside it.

## Report

Every map report has a `perf_overlay` object (`shown: false` while it was never shown). When shown it adds
`fps`, `frame_ms`, `frame_ms_avg`, `frame_ms_worst`, `tick_source` (`live_session` or `none`), `ticks`,
`ticks_per_second`, `clock` (`null` outside the galactic view, else `speed_factor`,
`nominal_ticks_per_second` and `paused`), `tick_ms`, `tick_ms_avg`, `tick_ms_worst`, `tick_phases`, `draw_calls`, `visible_units`, `toggles`,
`font`, `font_size`, `rect` and `own_cost_ms` (`samples`, `avg`, `max`).
`tests/presentation/renderer/test_live_session.py` checks them and the key on a GPU host;
`tests/ui/perf_stats_tests.cpp` covers the model (`include/eawr/presentation/ui/perf_stats.hpp`).
`tests/presentation/renderer/test_viewer_galactic_command_bar_runtime.py` runs the live galaxy at 1x and with
fast forward and prints the measured TICKS/S and tick cost (an observation, not a gate); the gate is the schedule
arithmetic on injected time in `tests/galactic/live_game_session_tests.cpp`, and `tests/ui/perf_stats_tests.cpp`
covers the rate arithmetic.

## Fast forward and the live pacer

The galactic live pacer (`LiveGameSession`, ADR-013) runs one parent step per interval of
`1 / (30 x speed)` s on a fixed-rate schedule (`LivePaceSchedule`): step n after a rebase is due at
`start + n x interval`, so wake-up latency and the step's own cost do not add to every period. A resume,
pause or speed change starts a fresh schedule; a backlog beyond `max_catch_up` (5, the tactical pacer's value)
is dropped, so a world whose ticks cost more than the interval is bound by its tick cost and never spirals.
The factor itself is data: WGM-210 (4x), `galactic::fast_forward_factor_raw`.

## Network section (legacy EAWR-4043)

In an online or LAN match the panel gains a network section: a one-line verdict, the lockstep waits (waits per
second, milliseconds waited in the last second, the longest wait in the last 10 s), the seat the simulation waits
for right now, and per remote seat the round trip, jitter and packet loss (`rtt -` until the transport has a
sample; loss is 0 where the transport cannot tell). Every wait counts, however short: the `stall-start` log line
only fires after a second. The verdict reads `network: waiting on peer 1 640 ms` when waits add up to 100 ms or
more in the last second (or one runs that long now), `game: frame 48 ms, sim 31 ms` when the frame or tick average
is over one 30 Hz tick, otherwise `ok`. "network" means this machine waited for that peer's inputs: a slow link and a slow peer PC look the same.

While a match runs, godot.log gets one line every 5 s whether or not the overlay is shown:
`EAWR net summary seat=0 verdict=ok wait_peer=- wait_ms=0 stalls_per_s=0 stalled_ms_1s=0 longest_ms_10s=0 frame_ms=8.0 sim_ms=5.0 peers=1:rtt_ms=80,jitter_ms=5,loss_pct=0.0`.
Map reports carry the latest numbers as a `net_perf` object (the last match's numbers; `null` if no match ran). The model is
`include/eawr/presentation/ui/net_perf.hpp`, covered by `tests/ui/net_perf_tests.cpp`. Presentation only: it never
touches lockstep, the simulation or any hash.
