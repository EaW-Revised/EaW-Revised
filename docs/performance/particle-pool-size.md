# The viewer's particle pool size (step 0 sweep, 2026-11-03)

`particles::default_particle_worker_count` (`include/eawr/presentation/particles/worker_count.hpp`) picks the
default size of the viewer's particle pool, the calling thread included:

- below 8 hardware threads: a quarter of them, at least 1 (the calling thread alone);
- 8 to 11 hardware threads: half of them, at most 4 (`particle_worker_cap_small`);
- 12 to 19 hardware threads: half of them, at most 6 (`particle_worker_cap_large`);
- from 20 hardware threads up: half of them, at most 8 (`particle_worker_cap_huge`).

`--eawr-live-particle-workers n` (1 to 64) overrides it. The pool size never changes the output: commits
run in batch order and every effect has its own random seed, so the streams are the same with any number
of workers (`particle_render_contracts`: serial against 1, 2, 4, 6 and 8 threads).

## Measurement

The cap of 8 from 20 threads comes from the owner-PC run below (20 LP, Remake Korriban 8 v 8, patched engine fad06b579, Separate, lane on: viewer main 20.0 ms lane off, 17.8 ms pool 4, 15.6 ms pool 8, pool 12 equal to 8 within noise; `out/pf/owner-pc-korriban/result.md`, 2026-11-04). The cap of 6 comes from a pool-size sweep with the particle lane on (the lane thread is the pool's
caller; pool N = the lane + N-1 helpers).

- **Scene:** RaW60 skirmish, 63 against 63 capital ships, seed 4645, uncapped frame rate, vsync off,
  6 simulation workers.
- **Build:** the viewer at the particle lane's head (dd4a31d2) with the Separate render thread. Fine
  particle timings and render timing were on, so these are diagnostic numbers.
- **Runs:** pool 4, 6, 8, 8, 6, 4, plus a lane-off pool 4 pair, on each host. N=2 per cell. Means of the two runs.
- **Window:** rows with tick >= 1800 up to the battle's last tick, about 60-75 s per run. The rows the trace keeps
  writing after the session ends (the tick frozen at its last value, the scene torn down) are left out.

| host | pool | lane wall ms | lane µs per stream | viewer main ms (mean / p95) | wall frame ms | ticks/s | in-batch ms | wait_dispatch ms |
|---|---|---|---|---|---|---|---|---|
| 28 threads (i7-14700HX) | 4 | 35.8 | 7.14 | 46.5 / 71.8 | 61.8 | 31.8 | 22.8 | 2.1 |
| 28 threads | **6** | 31.8 | **6.28** | 42.0 / 61.6 | 57.9 | 31.0 | 18.9 | 3.1 |
| 28 threads | 8 | 30.8 | 6.10 | 41.0 / 60.7 | 59.2 | 31.4 | 17.9 | 4.7 |
| 8 threads (i7-4790K) | **4** | 65.4 | 14.71 | 85.6 / 111.0 | 115.6 | 16.5 | 39.0 | 3.2 |
| 8 threads | 6 | 58.8 | 13.20 | 78.6 / 100.9 | 116.6 | 16.1 | 32.7 | 5.4 |
| 8 threads | 8 | 56.2 | 12.66 | 75.7 / 97.4 | 118.7 | 16.1 | 30.0 | 7.3 |

## Reading

- **12 threads and up:** pool 6 cuts the lane time per stream by about 12 % on 28 threads. Pool 8 is another
  3 % faster per stream, within the run-to-run spread of N=2 (up to about 7 %), so it is not shown better. The
  idle wait between batches (wait_dispatch) grows with the pool. The 20-thread case was measured on 2026-11-04
  (above): pool 8 gains there, so the cap is 8 from 20 threads up.
- **8 threads:** pools 6 and 8 make the lane 10-14 % faster per stream, but the frame does not move. It stays
  near 116 ms because the render thread binds, and its CPU time rises with the pool (67 to 76 ms). Ticks/s
  also drops 2.4 %, because the extra helpers share full cores with the simulation workers. So 8 to 11 threads
  keep 4.
- **The serial part of the lane:** at pool 8 it is 39 % of the lane time on 28 threads and 45 % on 8 threads,
  and it does not shrink with the pool. That is why a larger pool stops helping.
