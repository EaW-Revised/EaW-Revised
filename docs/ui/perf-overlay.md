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
`font`, `font_size`, `viewer_build` (`release`/`debug`, our library), `engine_build` (`editor, debug-enabled`, `debug template, debug-enabled` or `release template`: the Godot binary hosting it), `engine_version`, `engine_hash`, `rect` and `own_cost_ms` (`samples`, `avg`, `max`).
`tests/presentation/renderer/test_live_session.py` checks the key and the older fields on a GPU host (not `viewer_build`, `engine_build`, `engine_version` or `engine_hash`, whose text is covered by `ui_build_label_contracts`);
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

The network line also shows sealed lead ticks and input delay in milliseconds at the logical 30 Hz rate.
Per-seat tick time is observed input-frontier cadence, including stalls and packet batching; it is not remote CPU cost.
The `recv avg/max` figures measure local native-ready-to-consume age for accepted sealed batches when the transport
provides receipt timestamps. GNS timestamps refer to the native message that made an ordered application frame ready,
including a completing fragment or redundancy delivery. They do not measure the age of its first wire byte.

Wait counters use `B` for a missing peer batch, `Q` for a missing batch with decoded frames still queued for consumption,
`H` for a hash exchange, `T` for an agreed timing activation awaiting votes, and `O` for another admission barrier.
`Q` diagnoses consumer backlog; it does not assert that the particular missing tick is present in that backlog.
The summary log spells these out as `wait_batch_s`, `wait_queued_s`, `wait_hash_s`, `wait_timing_s`, `wait_other_s`
and corresponding `_ms` fields. Cause changes partition waiting time without resetting the longest overall stall.
Poll gaps and receipt age use one-second observation windows. Unknown receipt age remains unmeasured.

In an online or LAN match the panel gains a network section: a one-line verdict, the lockstep waits (waits per
second, milliseconds waited in the last second, the longest wait in the last 10 s), the seat the simulation waits
for right now, and per remote seat the round trip, jitter and packet loss (`rtt -` until the transport has a
sample; loss is 0 where the transport cannot tell) and a `route` row naming how that link is carried: `direct`, `relay` or `unknown` (not open yet, or a plain LAN stream).
A relayed link shows as `relay (own)` or `relay (cloudflare)` (plain `relay` when the transport cannot name the provider).
When the transport flags a relayed link as lossy for a while (receive loss at or above 15 % for 20 s, cleared below 5 % for
10 s) the section adds a hint row, `Network route degraded (relay own, 22% loss)`, and the net log gets a `route-degraded` /
`route-recovered` line with the loss; the summary line marks that peer `degraded`. Observation only: nothing switches the
route. The skirmish match and the co-op campaign share the row, the hint and the log lines.
The co-op campaign driver (galaxy and its retained child battles) feeds the same section, and logs `stall-start` and
`stall-end` lines with the waiting peer and duration, plus one `route` line per peer when its route becomes known. Every wait counts, however short: the `stall-start` log line
only fires after a second. The verdict reads `network: waiting on peer 1 640 ms` when waits add up to 100 ms or
more in the last second (or one runs that long now), `game: frame 48 ms, sim 31 ms` when the frame or tick average
is over one 30 Hz tick, otherwise `ok`. "network" means this machine waited for that peer's inputs: a slow link and a slow peer PC look the same.

While a match runs, godot.log gets one line every 5 s whether or not the overlay is shown:
`EAWR net summary seat=0 verdict=ok wait_peer=- wait_ms=0 stalls_per_s=0 stalled_ms_1s=0 longest_ms_10s=0 frame_ms=8.0 sim_ms=5.0 peers=1:route=direct,rtt_ms=80,jitter_ms=5,loss_pct=0.0`.
Map reports carry the latest numbers as a `net_perf` object (the last match's numbers; `null` if no match ran). The model is
`include/eawr/presentation/ui/net_perf.hpp`, covered by `tests/ui/net_perf_tests.cpp`. Presentation only: it never
touches lockstep, the simulation or any hash.

The panel shows two rows, "viewer: release" and "engine: 4.7.2 editor, debug-enabled", and a third with the engine signature. The window title says the same, e.g. "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, eawr 5b733a90f)"; with a launcher mod profile it reads "EAWR Viewer - <profile> (viewer: ...)". An engine with debug support is slower and says so in these rows, not in the window title: the viewer writes the title through the display server at startup and again after every engine title update, so the engine's own " (DEBUG)" suffix never shows.
The signature comes from the startup engine guard (`current_engine_signature()`, `docs/engine-guard.md`): "eawr <9-character hash>" for our patched engine, or "UNPATCHED ENGINE (<build>)" / "UNPATCHED ENGINE (unverified)" (red on the overlay, spelled out in the title) when the build or hash does not match the pin or the launcher passed `--eawr-engine-unverified`. The report adds `engine_signature` and `engine_patched`. The viewer only displays this; the rule lives in `include/eawr/presentation/engine_signature.hpp`.

## Degradation line (legacy EAWR-6846)

When the session has degraded something (docs/degradation-ledger.md), the panel shows one extra row above the render counts:
`degraded 12 in 3 classes` (distinct objects, then (code, source) classes), with `, 2 unknown` appended when some items have no
source. No row while nothing is degraded. The line is `core::degradation::overlay_line`; the caller refreshes it with
`set_degradation` at most once a second (the galactic view does), so the overlay only shows a string and takes no lock itself.
The line is observation only: the same distinct-object count the summary file carries, never an input of the session.
