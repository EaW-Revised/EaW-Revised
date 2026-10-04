<a id="battle-benchmark-the-close-range-melee-601"></a>

# Battle benchmark: the close-range melee

The owner's request (2026-09-29): spawn a lot of ships and fighters within range of each other,
with no stations and no long distances, and find the performance hotspots of the battle itself.
FoC performance parity is a hard requirement, so the same fight is also staged in the original
game for a frame-rate baseline.

## The scenario

`skirmish::build_melee` (`include/eawr/skirmish/melee.hpp`) builds the fight as a replay from a
seed (default 601): both M2 sides' capital ships and fighter/bomber squadrons in two lines across
the M2 map's centre (Coruscant), 1400 units apart and so inside every capital ship's weapon
range, with no station and no map object. Ship *i* of a side attacks the enemy ship at the same
place in the placement order (scaled to the enemy's count); every squadron attack-moves onto the
enemy ships' centre. A ship whose target dies is left to its own targeting, so the fight runs
until one side has nothing left in range. It runs with the replay path's content: the M2 fog
grid, the ability table and the fixture's victory rules (no star base, so the battle is never
decided early). The whole combat path runs: target choice, turbolasers, lasers, missiles and
torpedoes, projectiles and their collision against the meshes, shields, hardpoint damage, the
hangars' launches, dogfights, spin-away deaths, fog and visibility.

| Size | Rebel | Empire | Units at tick 0 | Meaning |
|---|---|---|---:|---|
| S | 4 Corellian corvettes, 4 Nebulon-B, 3 Calamari cruisers; 6 X-wing and 4 Y-wing squadrons | 6 Tartan cruisers, 5 Acclamators; 4 TIE fighter, 3 TIE interceptor and 3 TIE bomber squadrons | 145 (22 ships, 20 squadrons, 103 craft) | Each side at its highest FoC space cap: about 42 Rebel and 37 Empire population (level-5 star base plus the reinforcement points; the owner's anchor, 2026-09-29) |
| M | twice S | twice S | 290 | Past the cap, as mods raise it |
| L | four times S | four times S | 580 | Stress |

Population values from the FoC data: corvette and Tartan 2, Nebulon-B and Acclamator 3,
Calamari cruiser 4, a squadron 1. Which cap binds a real skirmish (the factions' tactical unit
cap or the star base capacity plus reinforcement points) is **unverified**; S follows the
owner's anchor.

The S fight at seed 601 lasts about 4200 ticks (140 s): the fighters are gone by about tick
1800, the last Empire ship dies at about tick 4200 and six Rebel ships survive.

## How to run it

- Sim: `path_bench --melee s|m|l [--seed n] [--ticks 4500] [--workers 1,2,4,8,hardware]
  [--csv prefix] [--replay-out file] [--list 1] [--execution live|legacy] [--profile on]`
  (needs the game data). The default `live` mode uses by-cost partition dispatch and a
  threaded state hasher, resolving completed digests after the timed stepping loop.
  `legacy` uses always-pool dispatch and synchronous hashing for comparisons. It prints
  every phase's mean, p99 and worst cost per tick against the 33.3 ms tick budget, the serial
  remainder, the live unit, craft and projectile counts, and fails when two worker counts
  disagree on a tick's state hash. On the build hosts:
  `windows_build.py --host laptop --melee-bench-out <dir>` (maintainer worker offload).
- Viewer: `--eawr-live-session melee --eawr-live-melee s|m|l` plays the same fight (the viewer
  builds it with the same code); `--eawr-perf-trace <csv>` writes one row per frame.
- Retail: `Invoke-FocMapCapture.ps1 -MapId coruscant-space -StagingProbe melee -NoFog
  -DebugBuild -DebugCommands FPS` stages size S from the capture mod's scoring script and shows
  the debug build's FPS readout in the stills (original capture).
- CTest `path_bench_melee_workers`: size S, the first 600 ticks, hash-identical at 1, 2, 4 and 8
  workers (skipped without the game data).
- Viewer profile: `path_bench --profile-attach <pid of the viewer> --profile-seconds <n>` samples the
  running viewer from outside (the sampler never runs inside the viewer).

The separate 20-ship movement scenario is `path_bench --selection owner --ticks 300
--workers 1,2,4,8 --timing off --execution legacy|live --csv <prefix>`. Its default `legacy`
retains always-pool dispatch and synchronous world-and-script hashing. `live` uses the live
session's by-cost dispatch, asynchronous world hashing and request-only script hashing. Both
modes check every completed world digest across worker counts; completed hashes are resolved
outside timed stepping. Use a different CSV prefix for each mode. The existing search-work
pin can be checked in either mode with `--pin tests/skirmish/fixtures/path-bench-owner.work.csv`.
On Windows x64, add `--profile on --profile-interval 1000` to sample the movement scenario's
threads and AI preparation; build with `EAWR_DEBUG_SYMBOLS=ON` to resolve function names.
Sampling perturbs the reported tick times, so use a separate unprofiled run for comparisons.

The movement CSV retains its search columns and appends `world_ms`, `engine_ms`, `service_ms`,
`other_ms` and each partition/commit phase's cost. `other_ms` is the tick minus the three outer
measurements: command routing, script command processing, authoritative hashing and remaining
bookkeeping. Partition/commit costs are nested within the outer measurements; some commits also
contain other phases, so these columns overlap and must not be added together. Tick numbers are
zero-based stepping ticks (row 855 completes tick 856). The
console also prints row 855 and the slowest post-order row, to diagnose the historical no-search
hitch without assuming that path planning caused it.

## Owner movement: AI threat preparation (legacy EAWR-589)

On 2026-10-03, the 20-ship movement scenario ran on one exclusive Windows x64 host (i7-14700HX,
28 hardware threads), with verified FoC data and identical MSVC release/symbol flags.
Before is `b1888835fdbfd61e16ded2106b09bf16630e14b0`; after is
`aa2d019821bc87e312f2c97ee2f83259dea3911a`. Both use `--execution live`.
Each run completes 600 warmup ticks, issues the same order, then measures 300 ticks
(`--selection owner --ticks 300 --timing off`). Five before/after pairs ran in sequence
at four workers; the first pair also swept 1/2/8/28 workers and checked the search-work pin.
An additional after sampling pass ran separately and contributes no timings below.

| Four workers, five runs | Before | After |
|---|---:|---:|
| Mean tick ms | 9.293 | 7.247 |
| Standard deviation of the five run means, ms | 0.304 | 0.265 |
| Mean AI preparation ms | 3.708 | 2.009 |
| Mean of each run's nearest-rank p99, ms | 23.407 | 21.808 |
| Worst measured tick over all five runs, ms | 26.882 | 26.833 |
| Worst measured tick with no path search, ms | 25.326 | 13.872 |
| Mean row-855 tick / AI preparation ms | 12.910 / 8.033 | 7.597 / 2.686 |

Mean tick time falls by 2.046 ms (22.0%); AI preparation accounts for 1.699 ms of that
reduction. The remaining difference includes world time and run variation. The worst tick
barely changes: this change reduces repeated threat perception, while expensive path-search
ticks remain. The single sweep's mean tick times at 1/2/8/28 workers are respectively
10.613/10.006/9.318/9.070 ms before and 9.054/8.411/7.008/5.344 ms after; those counts
have one run each and do not establish a repeated scaling result.

The first baseline four-worker run reproduces a no-search hitch at row 855: 25.326 ms,
including 20.713 ms in AI preparation. The other four baseline row-855 times range from
8.762 to 10.694 ms; after, all five range from 7.084 to 8.398 ms. This demonstrates a
reduction of the observed AI cost, without establishing that every historical hitch had
the same cause. Initial sampling identified repeated `ThreatGrid::total_force` and `force`
queries during goal evaluation. The implementation prepares independent entity/cell values
in 64 fixed ranges, folds them with the original rounded player/entity and row/column order,
and reuses identical pure queries only during serial AI preparation. The scope clears before
parallel Lua readers. The after profile records phase shares (6.2% threat totals, 3.5% threat
cells of busy samples), but its copied executable did not resolve its function symbols;
those shares include warmup and all process threads and are not elapsed-time attribution.

All 18 unprofiled world traces and the separate profile trace match, including the
1/2/4/8/28 sweep; the existing work pin passes. A paired Linux capture also compares all
76 replay runs at each source (19 fixtures at 1/2/4/8 workers): every raw hash, event and
snapshot CSV is byte-identical between workers and sources. No replay pin changed.
Raw timing CSVs, source/executable/PDB hashes and the verified aggregates are retained in
`out/feat-perf-2/owner-comparison-retry/comparison.json`; the paired replay receipt is
`out/feat-perf-2/hashes/paired/receipt.json`. A first comparison attempt failed before timing
on a private helper's git-directory check; its failure log is preserved separately.

The remaining four-worker tail is a known limit: the five after runs' worst rows are
607/608/606/610/604, within eleven ticks of the order. Their tick times are respectively
23.888/22.648/26.833/26.159/23.187 ms; measured `plan-searches` costs are
8.515/14.811/16.734/16.566/12.055 ms. These ticks remain within the 33.3 ms budget at
30 Hz. Independent collision layers and sliced searches already run on the pool; searches
on the same layer preserve planning order because each reads the preceding prediction and
the remaining expansion budget. Dispatching them together would change the result.

A follow-up profile reused the identical after executable and PDB on the same exclusive
host, with an explicit symbol search path and a 1/4-worker pair. Both world traces and the
work pin still match. Function names now resolve; its four-worker worst row 611 is
28.213 ms (world 25.408, AI 2.788, pooled searches 16.014). Profiling perturbs that timing.
Only about 42 of its 1509 busy samples belong to `plan-searches`, because sampling covers
all 900 ticks and all threads; this cannot reliably rank the short burst's inner functions.
The next investigation should sample the post-order window before selecting another pool
boundary. Raw data and verified provenance are in `out/feat-perf-3/worst-ticks.json` and
`out/feat-perf-3/worst-profile/verified.json`; this follow-up adds no production change.

## Simulation (RTX 4070 Laptop host, i7-14700HX, 28 hardware threads, 2026-09-29)

Cost per tick over the 4500 ticks (150 s of battle) of seed 601, against the 33.3 ms budget of the
30 Hz tick. *Base* is the integration head with the benchmark; *trial* adds the dogfights of PR
Dogfights and spaced group moves (head 20a4fa6c, a detached trial merge). Every row's run is hash-identical to its size's
1-worker run. `projectiles` is the phase's mean and p99; the serial remainder is the tick minus its
partitioned phases (staging, commits, the state hash, the commands).

| Build | Size | Workers | Mean ms | p99 ms | Worst ms | Ticks over 33.3 ms | projectiles ms (mean / p99) | Serial ms (mean) |
|---|---|---:|---:|---:|---:|---:|---|---:|
| base | S | 1 | 7.7 | 36.8 | 57.6 | 103 | 4.3 / 28.0 | 2.2 |
| base | S | 2 | 6.6 | 26.6 | 33.9 | 1 | 3.2 / 18.4 | 2.2 |
| base | S | 4 | 5.7 | 17.4 | 21.3 | 0 | 1.9 / 9.0 | 2.2 |
| base | S | 8 | 5.8 | 15.9 | 18.3 | 0 | 1.6 / 6.8 | 2.3 |
| base | S | 28 | 5.6 | 10.3 | 16.5 | 0 | 0.8 / 3.1 | 2.4 |
| base | M | 1 | 19.3 | 119.4 | 139.3 | 488 | 13.7 / 102.7 | 3.1 |
| base | M | 4 | 10.4 | 41.5 | 48.4 | 225 | 4.7 / 28.2 | 3.2 |
| base | M | 8 | 9.2 | 28.2 | 32.1 | 0 | 3.2 / 15.3 | 3.3 |
| base | M | 28 | 7.4 | 20.9 | 30.2 | 0 | 1.6 / 10.4 | 3.4 |
| base | L | 1 | 75.0 | 297.3 | 584.6 | 3425 | 62.0 / 266.2 | 6.3 |
| base | L | 4 | 29.5 | 94.1 | 107.3 | 957 | 17.9 / 69.4 | 6.3 |
| base | L | 8 | 21.2 | 59.5 | 67.4 | 774 | 10.0 / 36.2 | 6.3 |
| base | L | 28 | 16.1 | 44.4 | 66.6 | 546 | 6.4 / 25.8 | 6.6 |
| trial | S | 1 | 9.3 | 39.8 | 53.7 | 109 | 5.1 / 29.9 | 2.4 |
| trial | S | 4 | 7.0 | 18.9 | 23.6 | 0 | 2.5 / 9.6 | 2.6 |
| trial | S | 28 | 6.4 | 11.4 | 19.9 | 0 | 1.0 / 3.4 | 2.7 |
| trial | M | 1 | 16.7 | 116.8 | 148.8 | 692 | 11.7 / 98.1 | 2.7 |
| trial | M | 28 | 6.7 | 21.1 | 30.1 | 0 | 1.3 / 10.0 | 3.1 |
| trial | L | 1 | 90.8 | 304.3 | 350.7 | 3796 | 75.2 / 262.1 | 6.8 |
| trial | L | 28 | 17.7 | 44.4 | 61.5 | 555 | 7.3 / 24.3 | 7.0 |

The full tables (every phase, every worker count) are the bench logs of `windows_build.py
--melee-bench-out`. What they show:

- **S holds the budget from 2 workers** (one tick over at 2), but a single worker goes over on 103
  ticks and the worst tick is 58 ms: the heavy ticks are the fighters' first passes, when the
  projectiles in flight peak (359 at once).
- **Scaling stops at about 1.4x for S** (5.6 ms at 28 workers against 7.7 at 1): the serial
  remainder (2.2 to 2.4 ms) does not shrink, and each small phase *costs more* with more workers
  (abilities, hangars, fog-reveal, squadron-targets, squadrons and visibility each go from under
  0.03 ms at 1 worker to 0.13 to 0.2 ms at 28: waking the pool costs more than their work).
- **M and L are projectile-bound.** L at 28 workers still goes over on 546 ticks (p99 44 ms);
  `projectiles` is 62 ms of the 75 ms mean at 1 worker. Its cost grows faster than the unit count
  (S to L is 4x the units and 3.6x the average projectiles in flight, but 14x the phase time at 1
  worker): each projectile's candidate list grows with the crowd around it.
- **The dogfight change** adds the `dogfight-chases` phase (0.18 ms mean, 2.1 ms p99 at 1 worker at
  S) and more shots: S at 1 worker 9.3 against 7.7 ms (+21 %), at 28 workers 6.4 against 5.6 ms
  (+14 %). It also changes the battle (the Empire keeps 14 units at the end of S; M has fewer
  projectiles in flight, 109 against 145 on average, so it runs faster than the base), so its
  M and L rows compare two different fights.

<a id="simulation-after-656-quiet-host-2026-09-30"></a>

## Simulation after inline small phases and off-thread hashing (quiet host, 2026-09-30)

Integration head 9ed921f5 (pool hashing and by-cost dispatch from inline small phases and off-thread hashing, the projectile broad phase with per-unit reach and reused buffers),
`path_bench --melee s|m|l --seed 601 --ticks 4500 --workers 1,2,4,8,hardware`, Release with debug symbols, on the
RTX 4070 Laptop host (i7-14700HX, 28 hardware threads) under an exclusive lease: no GPU job, build or other
process ran (total CPU load 0.0 to 0.1 % before and after every run). One binary serves both columns:
`--execution live` (the default: by-cost dispatch, threaded state hasher) and `--execution legacy` (always-pool
dispatch, synchronous hashing). Five repetitions each, live and legacy interleaved in alternating order. The
sequence digest of the 4500 tick hashes is identical for every worker count and both modes (S
865cfcb8a5fd002889b6ae27706ddd07fc644b448ddefd59157e4649abce83c8, M
512ac827575a384463c60b62dd56bcf98fb3e86dd7c0b37bf39a1e77d7c3bc9c, L
80fccae5aac9add590d72f1eed3474c327454ce6b5ffe3f86f85d40c2816fde8). These supersede the contended absolute numbers
quoted for the inline-phase and off-thread hashing change (a shared host under load 17 to 28): the direction held, the magnitudes were different.

*Live* columns are the mean over the five runs of each run's mean tick cost (median and standard deviation of the
five run means after it), the mean of the runs' nearest-rank p99, the serial remainder and the ticks over 33.3 ms.
*Legacy mean* and *change* compare the same binary in legacy mode.

| Size | Workers | Live mean ms (mean / median / stdev) | p99 ms | Serial ms | Ticks over 33.3 ms | Speed-up vs 1 worker | Legacy mean ms | Live vs legacy |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| S | 1 | 4.33 / 4.33 / 0.03 | 13.2 | 1.66 | 0.0 | 1.00x | 4.71 | -8 % |
| S | 2 | 3.65 / 3.65 / 0.02 | 10.8 | 1.43 | 0.0 | 1.19x | 4.89 | -25 % |
| S | 4 | 3.25 / 3.24 / 0.02 | 9.2 | 1.36 | 0.0 | 1.33x | 5.00 | -35 % |
| S | 8 | 3.10 / 3.10 / 0.08 | 8.8 | 1.37 | 0.0 | 1.39x | 5.51 | -44 % |
| S | 28 | 2.61 / 2.62 / 0.05 | 5.3 | 1.29 | 0.0 | 1.66x | 6.47 | -60 % |
| M | 1 | 6.27 / 6.35 / 0.17 | 29.0 | 2.22 | 2.4 | 1.00x | 6.64 | -6 % |
| M | 2 | 5.11 / 5.13 / 0.08 | 24.1 | 1.84 | 0.0 | 1.23x | 6.40 | -20 % |
| M | 4 | 4.25 / 4.27 / 0.04 | 18.1 | 1.86 | 0.0 | 1.48x | 5.73 | -26 % |
| M | 8 | 3.92 / 3.92 / 0.04 | 16.2 | 1.88 | 0.0 | 1.60x | 5.81 | -33 % |
| M | 28 | 3.04 / 3.03 / 0.02 | 9.4 | 1.67 | 0.0 | 2.06x | 6.30 | -52 % |
| L | 1 | 17.52 / 17.42 / 0.67 | 64.6 | 4.18 | 817.8 | 1.00x | 18.83 | -7 % |
| L | 2 | 13.44 / 14.00 / 1.49 | 46.6 | 4.11 | 524.4 | 1.30x | 16.57 | -19 % |
| L | 4 | 10.04 / 10.68 / 1.23 | 32.1 | 4.08 | 53.2 | 1.75x | 13.65 | -26 % |
| L | 8 | 8.19 / 8.97 / 1.32 | 23.8 | 3.95 | 2.8 | 2.14x | 12.30 | -33 % |
| L | 28 | 6.66 / 6.79 / 0.21 | 18.2 | 3.67 | 2.0 | 2.63x | 11.30 | -41 % |

- **Every size scales now, and never gets worse with workers.** S reaches 1.66x, M 2.06x and L 2.63x at 28 workers.
  In legacy mode S at 28 workers is 0.73x (6.47 ms against 4.71 at 1 worker) and M is 1.05x: waking the pool
  for small phases cost more than their work.
- **The whole budget holds from 2 workers for S and M** (M at 1 worker goes over on 2 to 4 ticks of 4500). L needs 8
  workers to get under it (3 ticks over; 818 at 1 worker, 524 at 2, 53 at 4).
- **Serial remainder:** 1.3 to 1.7 ms (S), 1.7 to 2.2 ms (M), 3.7 to 4.2 ms (L), against 2.4 to 2.8, 2.8 to 3.1 and
  6.0 to 6.6 ms in legacy mode. The serial part is now the limit of the speed-up (S: 1.3 ms of 2.6 ms at 28 workers).
- **Noise:** S and M run-to-run stdev is at most 0.17 ms. L at 2 to 8 workers is noisier (stdev 1.2 to 1.5 ms):
  one of the five runs is 25 to 30 % faster than the other four (run 2: 10.9 ms at 2 workers against 13.6 to 14.7).
  The speed-ups for L at 2, 4 and 8 workers are therefore indicative; 1 and 28 workers are stable (stdev 0.7 and 0.2).

<a id="projectile-broad-phase-636-before-and-after"></a>

## Projectile broad phase: before and after

Fix 1 of the [ranked fixes](#ranked-fixes). The index query is unchanged (the step's box grown by
the largest collision reach), but its bodies come as unsorted positions into a buffer each
partition keeps from tick to tick, and a hostile candidate reaches the model-space tests only
when the sphere of its **own** reach about its position meets the step: the furthest corner of its
collision box or mesh bounds (sqrt(3) times its collision box's for a craft with a DG-37 sphere,
whose hit needs the step to meet that box turned into the world and made axis-aligned), plus
1/1024 of that and one unit for the Q24 rounding of the exact tests. That test is exact integer arithmetic and conservative, so the hits
are those of before: the digest of every tick's state hash is the same before and after at every
size, and every worker count agrees.

`path_bench --melee s|m|l --workers 1,4` on a 20-thread desktop, seed 601, 4500 ticks. *Before* is
the integration head 667c939c with the benchmark; *after* adds the projectile broad-phase optimisation. Before and after ran
in turns, three times each; the table has the medians of the three runs. Each run held one slot of
the host's shared build lane at below-normal priority; other workers' builds, tests or GPU jobs
held up to two of the other slots and both GPU slots during some runs, so the absolute times are
noisier than the laptop's above (4 workers is the lane's limit).

| Size | Workers | Build | Mean ms | p99 ms | Ticks over 33.3 ms | projectiles ms (mean / p99) |
|---|---:|---|---:|---:|---:|---|
| S | 1 | before | 6.43 | 27.4 | 24 | 3.25 / 19.0 |
| S | 1 | after | 2.68 | 7.4 | 0 | 0.23 / 1.16 (-93 % / -94 %) |
| S | 4 | before | 2.83 | 8.4 | 0 | 0.78 / 4.39 |
| S | 4 | after | 2.00 | 4.3 | 0 | 0.09 / 0.34 (-89 % / -92 %) |
| M | 1 | before | 8.57 | 60.8 | 384 | 5.84 / 50.5 |
| M | 1 | after | 3.37 | 14.7 | 1 | 0.47 / 3.53 (-92 % / -93 %) |
| M | 4 | before | 3.97 | 21.2 | 1 | 1.69 / 14.2 |
| M | 4 | after | 2.26 | 7.3 | 0 | 0.14 / 0.95 (-92 % / -93 %) |
| L | 1 | before | 42.12 | 144.7 | 1974 | 34.24 / 122.5 |
| L | 1 | after | 11.04 | 33.4 | 46 | 2.54 / 8.67 (-93 % / -93 %) |
| L | 4 | before | 15.14 | 46.0 | 539 | 9.80 / 33.5 |
| L | 4 | after | 6.13 | 15.1 | 2 | 0.74 / 2.38 (-92 % / -93 %) |

Again on the integration head after the benchmark hotspot report landed (32467877, which also brings
ion weapons and aimed hardpoint damage, so the fights differ from the table above), the same
commands and method. This time every run shared the lane with three other workers' jobs (all
four slots busy), which shows in the whole-tick columns: worst ticks up to 1.2 s and S's
ticks over budget growing from run to run whichever build ran. The `projectiles` column, measured
inside the phase, stays consistent; the tick columns of this table are not a fair comparison.

| Size | Workers | Build | Mean ms | p99 ms | Ticks over 33.3 ms | projectiles ms (mean / p99) |
|---|---:|---|---:|---:|---:|---|
| S | 1 | before | 5.83 | 26.9 | 12 | 2.76 / 18.7 |
| S | 1 | after | 4.76 | 30.1 | 35 | 0.33 / 2.57 (-88 % / -86 %) |
| S | 4 | before | 4.07 | 19.1 | 10 | 0.93 / 6.36 |
| S | 4 | after | 3.44 | 15.9 | 21 | 0.15 / 1.06 (-84 % / -83 %) |
| M | 1 | before | 11.45 | 78.3 | 481 | 7.77 / 64.7 |
| M | 1 | after | 4.15 | 19.6 | 4 | 0.59 / 4.70 (-92 % / -93 %) |
| M | 4 | before | 4.69 | 24.5 | 7 | 2.02 / 16.2 |
| M | 4 | after | 2.66 | 9.6 | 3 | 0.17 / 1.18 (-92 % / -93 %) |
| L | 1 | before | 50.34 | 233.4 | 1838 | 39.95 / 199.7 |
| L | 1 | after | 11.59 | 53.8 | 400 | 2.54 / 15.1 (-94 % / -92 %) |
| L | 4 | before | 15.75 | 63.0 | 650 | 9.31 / 47.1 |
| L | 4 | after | 5.76 | 17.3 | 3 | 0.67 / 2.93 (-93 % / -94 %) |

The broad phase's work is now a deterministic count, the same on every host and at every worker
count (`TacticalTick::projectile_candidates` and `projectile_exact_tests`; `path_bench --melee`
prints both): of the bodies the index hands out, 0.1 to 0.2 % reach the exact tests. On
32467877 that is S 55,394 of 35.7 million, M 122,569 of 97.1 million and L 362,614 of 452 million
over the 4500 ticks (on 667c939c: S 73,513 of 41.7 million, M 135,112 of 94.0 million, L 455,729
of 551 million). The synthetic budget test (`damage_tests`, projectile broad-phase optimisation) holds a far station's reach and
two corvettes out of the exact tests.

What is left: a sampling profile of L at 1 worker after the change (on 667c939c, `--profile on`, a PDB build)
puts `projectiles` at 23 % of the busy samples (82 % before), below the serial state hash
(`sha_transform` alone 12 %). The model-space box test no longer shows; the phase's cost is now the
sweep over the index's bodies, since the query is still grown by the largest reach (a capital
ship's): the reach test (`within_range`, 8.6 %), the index's box test and cell walk (4.8 %). The
next step, if the phase matters again, is a query grown by the small units' largest reach plus a
short list of the few large-reach units tested directly.

<a id="the-script-state-hash-off-the-live-tick-895-before-and-after"></a>

## The script state hash off the live tick: before and after

The scripted M2 battle (the FoC AI on both sides; the melee above runs no scripts) saved and
hashed every Lua instance on the stepping thread each tick, for a combined world-and-script hash
the live session never read. The live session now turns it off
(`ScriptedTacticalSession::set_authoritative_hash`); replays, the soak, the tools and the tests
keep it every tick, so no hash or pin changes.

`foc_soak_tests --seeds 12 --ticks 2500 --no-invariants --workers 1|4` on the integration head
0c94ad5d plus off-thread script hashing, timing each `step` (the world step, the AI engine, the script service and the
hash). *Before* keeps the per-tick combined hash, as the live session did; *after* turns it off,
as the live session does now. The two alternated three times at each worker count on a 20-thread
desktop, each run in one slot of its shared build lane at below-normal priority, with other
workers' jobs in the other slots; the table has the medians of the three runs. The timing was a
local change to the soak and was not committed.

| Workers | Build | Mean ms | p99 ms | Worst ms |
|---:|---|---:|---:|---:|
| 1 | before | 6.31 | 14.3 | 24.2 |
| 1 | after | 3.10 (-51 %) | 9.4 | 17.7 |
| 4 | before | 6.10 | 13.7 | 23.3 |
| 4 | after | 2.83 (-54 %) | 9.5 | 18.7 |

The hash cost 3.2 to 3.3 ms of every scripted live tick on this host, where the audit measured
7.2 ms a tick on the laptop at head 7ca8aa2f. `lua_bridge_workers` checks at 1, 2, 4 and 8
workers that the world hashes and the replay are the same with the hash off and that the combined
hash asked for on request is the one every tick would have had.

## Viewer

`--eawr-live-session melee` with `--eawr-perf-trace` at 1280x720 (the retail captures' size), the
lit look (`--eawr-environment map --eawr-lighting sh --eawr-shadows on`), the HUD on, fog revealed so
every unit is drawn, vsync off (`--eawr-benchmark`), one tick per frame (`--eawr-live-step 1`, so
a frame shows the same tick on any host and waits for it when the simulation is behind) and the
tactical camera (`--eawr-map-camera-config config/coruscant-live-session-camera.xml`) following both
fleets' ships (`--eawr-live-follow-group <tick>:<entity ids of both sides' ships>`; the camera config
alone stays on the opening camera over the Rebel start, where the fight is off screen). The first five frames (loading) are left out. The viewer's
replay of the fight is hash-identical to the benchmark's (`headless_hashes_equal`).

| Host | Size | Frame ms (mean / p99 / worst) | Busiest 300 frames: mean ms (FPS) | Render CPU / GPU ms (mean) | Draw calls (mean / peak) | Peak projectiles, effects, particles |
|---|---|---|---|---|---|---|
| Rig (GTX 970) | S | 12.8 / 33.3 / 141.9 | 18.5 (54) | 2.4 / 2.0 | 1033 / 1797 | 283, 234, 4.8k |
| Rig (GTX 970) | M | 19.8 / 54.1 / 146.5 | 42.3 (24) | 3.4 / 2.8 | 1175 / 2692 | 570, 256, 9.7k |
| Rig, rerun (6 workers, the default) | S | 12.4 / 22.2 / 47.1 | 18.6 (54) | 2.5 / 2.7 | 1033 / 1797 | 283, 234, 4.8k |
| Rig, rerun with 4 workers | S | 12.8 / 23.8 / 93.9 | 18.9 (53) | 2.6 / 3.0 | 1033 / 1797 | 283, 234, 4.8k |
| RTX 4070 Laptop | S | 6.9 / 13.0 / 69.5 | 10.1 (99) | 1.2 / 1.3 | 1033 / 1797 | 283, 234, 4.8k |
| RTX 4070 Laptop | M | 10.3 / 23.5 / 34.2 | 20.4 (49) | 1.6 / 2.1 | 1175 / 2692 | 570, 256, 9.7k |
| RX 7900 XTX workstation, fight off screen | S | 9.9 / 18.5 / 26.9 | 13.3 (75) | 1.4 / 0.2 | 632 / 781 | 283, 234, 4.8k |
| RX 7900 XTX workstation, fight off screen | M | 17.8 / 42.1 / 69.0 | 35.4 (28) | 2.4 / 0.2 | 728 / 1113 | 570, 256, 9.7k |

Frame statistics leave out the two capture frames (the frames after ticks 600 and 1500, 130 to
140 ms each, which read the image back). In this pacing **a frame waits for its tick**: it asks the
simulation thread for the next tick and blocks until it is done, so a frame's time is the tick plus
the frame's own work, one after the other. In real-time play the simulation runs ahead on its own
thread at 30 Hz and the frame interpolates, so the two overlap; these frame times are the
conservative case.

- **What the two engines measured, side by side, with what does not line up.** On the rig the remake's
  busiest 300 frames of S average 54 FPS (M: 24), and the FoC debug build's readout on the same GPU
  (preset `Default_3`, AA 4, see below) shows 21 to 29 while both fleets are whole. On the laptop the remake
  averages 99 FPS at S and 49 at M, and FoC (preset `Default_0`, AA 1, the lightest) shows 73 to 136.
  These are not parity results: the remake's figure is the mean frame time of its busiest 10 s, FoC's is
  its own smoothed readout at the moment of a still; the fights differ (the retail one is shorter and
  one-sided, placed by teleport with jitter); the graphics settings and cameras differ (on the laptop most of
  all: the remake at its full lit look against FoC's lowest preset); the remake's frames wait for one
  tick each; and the FoC figures are from the debug build. Only the rig pair shares a GPU and even
  that pair is not a like-for-like window.
- **The first rig run hitched; the rerun did not.** The first run had 27 frames over 50 ms in S
  (up to 142 ms, from the first shots at ticks 17 to 21 through the fight) and 53 in M, with normal
  GPU and render CPU time. A rerun of S on the same host with the same build had none at the
  default 6 workers (worst 47 ms, p99 22 ms) and 3 with the simulation capped at 4 (worst 94 ms);
  The laptop has one (S, tick 17, 69 ms). So they are not a steady cost of the viewer, and capping the
  workers at the physical cores does not remove them; the first run most likely shared the rig
  with other work. The busiest-stretch frame time is the same in every run (18.5 to 18.9 ms).
- The GPU is not the limit on the GTX 970 either (2 to 3 ms a frame); draw calls peak at 1800 (S)
  and 2700 (M).
- The trace's `process_ms` (Godot's process-time monitor) is not a per-frame figure (its mean exceeds
  the frame time) and is left out.

The RX 7900 XTX workstation runs (taken while the owner's PC was open to workers) had no tactical camera, so the view
stayed on the opening camera over the Rebel start and the fight at the map centre was off screen;
the follow-group needs `--eawr-map-camera-config`. They still say something: with nothing of the
battle drawn (GPU 0.2 ms, render CPU 1.4 to 2.4 ms), M's busiest stretch still takes 35 ms a frame
(28 FPS), so that cost is the main thread's own frame work (posing the units, the CPU particle
systems of the battle effects and unit emitters, the projectile streams, the HUD) and the wait for
the tick, not rendering. The runs with the fight on screen are the rig's and the laptop's.

<a id="the-viewers-particles-638"></a>

### The viewer's particles

Ranked fix 4. The battle's particle systems (the unit emitters, the battle effects and the breakoff
props) advance and present their effects in one batch per 30 Hz sample
(`EffectRegistry::advance_all`, `present_all`). Each effect steps its CPU system and builds its
emitter streams as a task on a small pool of its own, in contiguous slices of the batch (at most 64,
as [EnTT storage decision](../architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids)'s partitions); the main thread then uploads the streams to Godot in the batch's order.
An effect reads only itself and the shared camera and writes only itself and its own statistics
slot, so the streams, the statistics and the upload order are the same with any pool size
(`particle_render_contracts`: the serial run against 1, 4 and 8 threads and a reversed task order).
The live battle's registries no longer hash their streams (nothing there reads the hashes; the effect
and map reports and the tests still do, `set_stream_hashes`). The Godot upload writes the packed
arrays through their storage instead of one checked `set()` per element, and an emitter that drew
nothing and draws nothing again is left alone. `RegistryWorkCounts` holds the per-frame work to a
budget: one step and one stream build per effect and emitter, one upload per drawn stream, no hash
in the live battle, at most 64 tasks a batch (`test_batch_work_counts`).

**Pool size.** `ParticleWorkers::default_count`: a quarter of the hardware threads, at least 1 (the
main thread alone) and at most 4, the main thread included (`--eawr-live-particle-workers n` sets
it). The simulation's pool already takes all but two hardware threads
(`LiveSession::game_worker_count`), so every particle worker shares a core with a simulation worker
when a frame overlaps a tick. Kept small, it does not slow the tick: the real-time S runs below take
2.79 ms a tick with the pool and 2.75 without on the rig, 1.78 and 1.80 on the laptop. A larger pool buys
nothing: 4 workers on the rig (8 threads, default 2) leave M's busiest frames at 45.3 ms against
44.0.

The same benchmark as above (1280x720, lit, HUD on, revealed, one tick per frame, the tactical
camera following both fleets), 2026-09-30, on the integration head with particle worker-pool simulation. *Before* is that
head with only the `particle_ms` column added; *after* is the final head. `particle_ms` is the main
thread's time in the frame's unit emitters, battle effects and breakoff props (the whole of their
frames: posing the emitters, stepping, building, uploading, the projectile streams). The fight has
changed since the close-range battle benchmark table (peaks S 263 projectiles, 155 effects, 5k particles; M 428, 256,
9.5k), so compare within this table only. S replays hash-identical in every run
(`headless_hashes_equal`); M runs with the verification off, as in the close-range battle benchmark.

| Host | Size | Build | Frame ms (mean / p99) | Busiest 300 frames: mean ms (FPS) | `particle_ms` mean / p99 (busiest 300) | Tick ms (mean) |
|---|---|---|---|---|---|---|
| Rig (GTX 970, 8 threads, pool 2) | S | before | 14.9 / 33.3 | 24.7 (40) | 7.7 / 47.2 | |
| Rig | S | after | 12.1 / 25.0 | 20.5 (49) | 4.8 / 6.3 | 2.77 |
| Rig | M | before | 24.6 / 64.2 | 54.9 (18) | 13.7 / 46.7 | |
| Rig | M | after | 19.6 / 50.0 | 44.0 (23) | 8.5 / 12.3 | 6.20 |
| Rig | M | after, 1 worker | 20.4 / 51.5 | 45.8 (22) | 9.5 / 13.0 | 6.21 |
| Rig | M | after, 4 workers | 19.9 / 50.0 | 45.3 (22) | 8.5 / 12.0 | 6.28 |
| Laptop (RTX 4070 Laptop, 28 threads, pool 4) | S | before | 7.6 / 16.7 | 11.8 (85) | 3.4 / 6.2 | |
| Laptop | S | after | 6.5 / 13.3 | 11.0 (91) | 2.6 / 4.0 | 1.71 |
| Laptop | M | before | 12.0 / 30.3 | 21.9 (46) | 6.5 / 12.6 | |
| Laptop | M | after | 10.3 / 24.2 | 20.1 (50) | 4.9 / 7.7 | 2.93 |
| Laptop | M | after, 1 worker | 10.6 / 25.6 | 20.4 (49) | 5.2 / 7.8 | 2.91 |

What it says:

- **The main thread's particle time drops by a third to 40 %** (rig M 13.7 to 8.5 ms in the busiest
  stretch, the laptop M 6.5 to 4.9) and its spikes are gone (rig p99 47 to 12 ms). The busiest frames
  gain 10 to 20 % on the rig and 5 to 8 % on the laptop.
- **Most of the gain is the hashes and the upload, not the pool.** With the pool off (1 worker) the
  rig's M still drops from 13.7 to 9.5 ms; the pool takes it to 8.5, the laptop's from 5.2 to 4.9. With
  the hashes gone, stepping and building the streams is a small part of what is left.
- **Serial main-thread work remains in the same frames**: Godot submission, placing each ship's
  emitters on its bones (fixed-point transforms), projectile kite and beam streams and death-clone
  proxy posing. Surface recreation is reduced by the [later reuse change](#particle-surface-reuse-2026-10-04).
- **The tick is not slowed**: the tick costs above and the real-time runs (rig S 2.79 ms a tick with
  the pool, 2.75 without; the laptop 1.78 and 1.80).
- The melee's effects look the same: a clip of M from tick 640 to 760 before and after on the laptop has
  121 of 121 frames pixel-identical.

### Particle surface reuse (2026-10-04)

The particle upload now retains a Godot surface when its vertex and index counts match.
It updates positions, colour and UV regions, sends indices only when topology changes, and
refreshes the culling bounds from the current vertices. An empty stream hides its instance
while keeping storage for repopulation; a different size creates a new surface. Simulation,
stream construction and ordered main-thread submission are unchanged. The measured hotspot
was the backend submission, so this change concentrates on that work; emitter bone placement,
projectile stream preparation and proxy posing still have serial work.

Before/after on one exclusive GTX 970 lease, Godot 4.7.2, 1280x720, lit Coruscant, revealed fog,
HUD/audio off, matched cameras, four simulation workers, the default particle pool (two on this
GPU host), two ticks per frame. Both builds include the same continuous-particle capacity
and mesh-birth offset fixes. Instrumented baseline `7337263f`, candidate `9dedc3a9`, base
`e15075a9`. The size-L melee uses a fixed camera, seed 601, 580 starting entities and 1,800 ticks. Each
window below contains 151 samples from an uncaptured run.

These were driven capture-clock runs: the viewer requests and waits for two simulation
ticks before preparing each drawn frame. Their retained launch specs omit
`--eawr-benchmark`, so VSync remained at the enabled project default. The frame deltas
therefore include the simulation wait and display pacing; they are not an uncapped
renderer throughput measurement. Both sides used the same settings, so the relative
reported surface-reuse gains remain a valid paired comparison.

| Case / ticks | Frame median ms before / after | Particle median ms before / after | Backend submission median ms before / after |
|---|---:|---:|---:|
| L melee, 300–600 | 93.939 / 72.917 | 28.543 / 13.878 | 15.708 / 3.056 |
| L melee, 900–1200 | 95.334 / 73.331 | 37.249 / 21.185 | 20.235 / 6.002 |
| L melee, 1500–1800 | 86.111 / 62.963 | 41.251 / 24.475 | 21.780 / 7.005 |
| Filled death-clone proxy, 300–600 | 35.050 / 27.367 | 23.665 / 17.603 | 7.908 / 2.609 |

The filled case stages the native large-ship death model on a valid existing clone host; it
is a proxy stress fixture, not a battle with that ship; both sides follow the same staged host.
Both sides reach 13,761 clone particles.
The lit captures run separately from the cost samples. All seven filled-case images are
pixel-identical before/after. An earlier size-L pair with four particle workers also has
seven pixel-identical before/after images and seven identical one/four-worker images on
each build; its particle medians fall 43–52% and frame medians 20–26%. Compare within each
pair: worker count, capacity and fight content differ from the older tables above.

| Uncaptured run | Submitted drawable streams (both) | Nonempty uploads (both) | Surface replacements before / after |
|---|---:|---:|---:|
| L melee | 1,979,075 | 1,337,899 | 1,337,899 / 165,320 |
| Filled proxy | 160,530 | 156,037 | 156,037 / 30,661 |

Every recorded per-frame population, particle, submitted-stream and upload count matches,
and hashes/replay bytes match in all runs. The existing batch contracts check stream-build
budgets and serial, four/eight-thread and reversed completion equality. Backend contracts
check packed layout, changing positions/topology and size-dependent replacement. Focused GPU
checks cover moving particles, drain/reappearance, mesh proxies, ship/death emitters and
projectile trails/hits. No simulation or replay pins change.

The optional trace columns distinguish packing (`particle_conversion_ms`), RenderingServer
submission (`particle_submission_ms`), submitted drawable streams (`particle_streams`, including
empty calls), nonempty uploads and surface replacements. Submitted streams exclude nondrawable
plans and are distinct from all streams built by the registries. Replacements count surface
creation, not internal engine or driver staging allocations. The measurements are one pair,
not a performance guarantee across fleets or hardware. Ignored receipts:
`out/perf-particles/latest-performance.json`, `latest-pair-lease.json`, `rebased-performance.json`
and the before/after rig folders beside them.

<a id="the-renderers-submit-888"></a>

### The renderer's submit

Every frame the Godot renderer used to adapt the whole snapshot to floats, route and stable-sort it by
pass, build a map of the live entities and set every piece's transform, moved or not. It now keeps
its pass order while the snapshot's (entity, asset) sequence and the uploads are unchanged (a rebuild
is a linear split into the four passes, sorting only a pass that is not already in entity order),
sends a piece's transform only when its fixed transform differs from the one it last sent, and scans
for removed instances only when a submit did not carry all of them. A piece moving by interpolation
differs every frame and is sent every frame; parked units, static props and the sky are not.
`GodotRenderer::submit_work()` counts the work and `renderer_contracts` holds it (the order equals
the reference sort; a frame with 40 moving pieces out of 1000 sends 40).

The M benchmark above on the rig, 2026-09-30, capture ticks 300 to 3000 (the read-back frames left
out). `submit_ms` is the main thread's time in the space view's snapshot builds and the renderer's
submit, `pieces` the snapshot's pieces, `sent` the transforms that reached Godot (new trace columns).
*Before* is the integration head with only the `submit_ms` column.

| Host | Build | Frame ms (mean / p99) | Busiest 300 frames: mean ms | `submit_ms` mean / p99, all frames | `submit_ms` mean / p99, busiest 300 | Pieces / sent (busiest 300) |
|---|---|---|---|---|---|---|
| Rig (GTX 970) | before | 16.1 / 38.9 | 35.3 | 0.55 / 1.58 | 0.79 / 1.73 | 1159 / all |
| Rig | after | 15.5 / 36.7 | 34.1 | 0.20 / 1.13 | 0.35 / 1.19 | 1140 / 414 |

- **The submit costs less than the audit's estimate** (0.8 ms of the busiest frames on the rig, not 2
  to 3): Godot's server calls are cheap to queue. The change removes about 55 % of it (60 % over the
  whole run), well within the frame noise of the busiest stretch.
- **The pictures are identical**: the eight lit captures (sh lighting, map environment, shadows on) of
  the before and after runs are byte-identical PNGs.
- The laptop was busy with builds for the whole run and was not measured.

## Hotspots

A sampling profile of each size at 1 worker and at 28 (`path_bench --melee <size> --profile on`,
a PDB build, samples every 1 or 2 ms; waits left out). Percentages are of the busy samples.

| # | Where | Phase | Share | Why |
|---|---|---|---|---|
| 1 | `step_projectile` → `segment_enters_box` (the swept test against each candidate's collision box and mesh bounds), `dot`, `SignedWide::product`/`add` | `projectiles` | S 54 % (inclusive), L 82 % | Every projectile step queries the space index with its segment's box grown by the **largest collision reach of any unit** (`CombatWorld::collision_reach`, a capital ship's), so around a fleet each shot gets many candidates. Each candidate then transforms both segment ends into model space in Q24 wide arithmetic (6 dot products of 128-bit products and 6 divides) before any cheap rejection; the per-unit sphere test (`within_sphere`, 5 to 9 %) runs after it. The candidate list is a fresh heap vector sorted per projectile (`std::sort` 1.3 % at S, 4.8 % at L; heap 2 to 4 %). |
| 2 | `core::sha256` (`sha_transform`), `state_sha256`, `append_u64` | serial | S 21 % at 1 worker, 21 % of the busy time at 28 | The canonical state is encoded and hashed every tick on the stepping thread: about 1.6 ms of the 2.2 ms serial remainder at S, the largest single serial cost and the scaling limit. |
| 3 | `NtAlertThreadByThreadIdEx` (waking parked pool threads) | every phase | 36 % of the busy samples at 28 workers | The pool wakes every worker for every phase, about 15 phases a tick; at 28 threads the wake-ups cost more than the small phases' work. |
| 4 | `step_craft` → `Locomotor::run`, `cordic_sin_cos`, `normalize` | `movement` | 6.6 % | Per craft per tick: exact trig and normalisation in fixed point. |
| 5 | `to_matrix`, `transform_point` | `collection-boxes`, `unit-systems`, serial | 4.6 % | Each unit's matrix is built from its quaternion several times a tick (collection boxes, unit systems, the combat world). |
| 6 | `RtlAllocateHeap`/`RtlFreeHeap`, `Result<...>` | `projectiles`, serial | 4 to 6 % | Per-call vectors and results on hot paths. |

### The viewer

The viewer on the laptop playing M, sampled from outside for 45 s from 5 s into the fight
(`path_bench --profile-attach` through the rig driver `profile-attach.ps1`, every 2 ms, the
extension's PDB; Godot's own functions have no symbols). Percentages are of the busy samples of all
the process's threads (a thread blocked on its message queue left out).

| # | Where | Thread | Share | Why |
|---|---|---|---|---|
| 1 | `EffectRegistry::advance`, `EffectRegistry::publish` (`build_stream`, `append_quad`, `sample_track`, `CpuSystem::step_segment`), `GodotParticleBackend::update`, from `UnitEmitters::frame` (the ships' emitters, their death clones' emitters) and `BattleEffects::frame` | main | about 30 % of the busy samples, about 70 % of the extension's frame work | Every particle system of the fight (about 10k particles at M) is stepped and turned into a vertex stream on the main thread, one emitter after another, and uploaded every frame. |
| 2 | `particles::stream_hash` | main | 8 % (self) | Every frame each emitter's finished vertex stream is hashed twice (its own hash and the frame's chained one) for the frame statistics, which only the JSON reports read. |
| 3 | `RtlAllocateHeap`/`RtlFreeHeap`, critical sections | main, simulation | 11 % | Per-frame vectors in the particle and effect code, the simulation's per-call vectors. |
| 4 | `TacticalSession::step` on the pool (`step_projectile`, `segment_enters_box`, `SignedWide`) and the state hash | simulation (26 workers) | 27 % | The simulation's own hotspots, as in the table above. |
| 5 | Godot (no symbols), the Vulkan driver | main, render | about 30 % | Scene and render-list work for about 1200 to 3000 objects. |

The rig's viewer playing S, sampled the same way, has the same shape: the particle systems about
30 % of the busy samples (`EffectRegistry::advance` and `publish`, `GodotParticleBackend::update`),
`stream_hash` 5 %, the heap 9 %, the simulation 23 %.

## FoC baseline (the debug build, 1280x720)

The same size S staged in the original game (`-StagingProbe melee`, fog off, the AI suspended), on
the rig (GTX 970; its FoC profile runs preset `Default_3` with AA 4) in 13 cycles over about 18
minutes of game time, and on the laptop. The table is the rig's. The staging
put every unit at its place with a teleport (the line's `T`). Each still shows the debug build's
own FPS readout, the logical frame rate (the game's 30 Hz simulation) and the rendered one, and the
advisor line dates it. Thirty stills; the table groups those whose line is legible by the state of
the fight.

| Moment of the fight (stills) | Logical FPS | Rendered FPS |
|---|---|---|
| Both fleets whole, first 25 s (2, 4, 8, 12, 21, 27) | 21 to 28, mean 24.5 | 22 to 29, mean 24.5 |
| Empire down to 1 to 3 ships (3, 6, 19, 24, 28) | 23 to 28 | 23 to 29 |
| Empire ships gone, squadrons left (7, 9, 15, 20, 22, 25, 26, 29) | 28 to 30 | 29 to 68 |
| Between cycles, nothing fighting (16, 18) | 29 to 30 | 80 to 117 |

What the baseline says:

- **FoC does not hold 30 Hz in this fight on the rig.** While both fleets are whole the logical rate
  drops to 21 to 28: FoC runs a logical frame per rendered frame when it cannot render faster, so
  its simulation slows with the renderer (a slow-motion battle, not a hitch). This is the debug
  build, whose own code may be slower than retail's. A later observation using an already
  installed timer is [below](#retail-pacing-observation-2026-10-04); the debug stills remain their own baseline.
- **The lowest readout in these stills is about 21 FPS at the melee's heaviest moment on a GTX 970**
  (preset `Default_3`, debug build), and the logical rate FoC keeps is 24 to 28 while the fleets are
  whole. Whether that is the floor the remake has to match depends on the caveats above.
- The retail fight is shorter and one-sided: the Empire is down to 0 to 3 ships 35 to 50 s after
  the attack order and has no ship left by 45 to 75 s and the Rebels lose at most 3 ships, where the remake's S fight lasts about 140 s
  and the Rebels lose 5. The staging differs (a teleport to places near, not on, the benchmark's
  exact positions with its own random jitter; FoC's own targeting after the orders), so this is
  **unverified** as a sim difference; it is on the fidelity list.
- **On the laptop (RTX 4070 Laptop, i7-14700HX; the host's FoC profile ran preset `Default_0`, AA 1)**
  the same staging keeps the logical rate at 30 (29 on two stills) and renders 73 to 136 FPS while
  both fleets are whole (stills 1 to 5 and 14, mean 119), 106 to 185 later in the fight and 178 to
  232 between cycles (17 stills, 16 with a readable readout).

## Retail pacing observation (2026-10-04)

A retail size-S staging run on the same GTX 970 at 1280x720, `Default_3`, AA4, Vsync off,
revealed fog and suspended AI used an existing identity-gated timer and its installed runtime.
No new dependency was added. The map environment was unpinned. Both 20-second arms passed
thread identity, ordering and zero-drop checks; all hooks restored and the capture adapter
stopped its owned game and removed its task.

| Measured interval | Mean / median / p95 ms | Rate |
|---|---:|---:|
| Loop marker, first arm | 4.559 / 4.148 / 7.906 | 219.34 iterations/s |
| Render entry, second arm | 5.031 / 3.978 / 11.012 | 198.73 rendered frames/s |
| Logical advance, second arm | 34.196 / 33.940 / 35.940 | 29.26 updates/s |

The phase arm records 588 consecutive logical advances and 3,994 rendered/presented frames
over 20.098 seconds. Loop iterations include work without a render and are not RFPS.
The last pre-timer still shows the initial staging wait; the marker arm includes spawning,
and the phase arm covers an early first-cycle interval. Later stills show an active second
cycle, but unit counts were not sampled at the timed boundaries, so this does not assert that
both fleets remained whole throughout the measured interval.

Retail holds near 30 logical Hz here and renders far faster than the older debug-build stills
(21–28 logical, 22–29 rendered FPS with both fleets whole). The debug result is not a reliable
retail performance floor. This is an observation rather than an isolated executable-speed
comparison: exact fight stage, camera, sample method and unit counts were not paired. It also
cannot establish a numeric gap to the viewer's size-L fixed-camera runs. The sample never
exercises retail below 30 rendered FPS, so retail pacing there and exact staging parity remain
unverified (legacy EAWR-1262). No simulation change follows from this measurement. Ignored receipts:
`out/perf-particles/retail-pacing.json`, `retail-report.md` and `retail-stills-adopted/` beside it.

## Ranked fixes

Expected gains are estimates from the profiles' shares, not measured; every fix keeps the state
hashes (each is a faster way to the same result, or work moved off the critical path).

| Rank | Fix | Where | Expected gain |
|---|---|---|---|
| 1 | **Done:** projectiles -83 to -94 % ([before and after](#projectile-broad-phase-636-before-and-after)). Projectile broad phase: query each candidate with its own collision reach instead of the largest one's, reject with a cheap integer sphere and box test before the model-space transform, and reuse a per-partition candidate buffer the index fills in ascending ID (no per-projectile heap vector or sort). | `step_projectile`, `SpaceIndex::box`, `segment_enters_box` | `projectiles` down 60 to 75 %: S at 1 worker about 7.7 to 5 ms mean and p99 37 to about 15 ms (no tick over budget); L at 28 workers about 16 to 11 ms and most of its 546 ticks over budget gone. The biggest win at every size. |
| 2 | **Done:** by-cost dispatch starts small named phases inline and sends remaining partitions to the pool when their work exceeds the inline budget. | `ThreadWorkerAdapter` | Together with off-thread hashing, S at 28 workers takes 2.61 ms against 6.47 in legacy mode; M 3.04 against 6.30 and L 6.66 against 11.30 ([exclusive-host measurements](#simulation-after-656-quiet-host-2026-09-30)). The combined measurements do not isolate dispatch's share. |
| 3 | **Done:** hash frozen canonical world bytes on a separate thread; live scripted sessions skip the combined script hash they never read, while verification paths retain it. | `TacticalSession::state_sha256`, `ThreadStateHasher`, `ScriptedTacticalSession::set_authoritative_hash` | Combined world-hash/dispatch gains are above; skipping unused script hashes separately removed 3.2 to 3.3 ms per scripted tick on the measured host ([before and after](#the-script-state-hash-off-the-live-tick-895-before-and-after)). |
| 4 | The viewer's particles: step the effect and emitter particle systems on the worker pool (partitioned by emitter, the stream built there too), keep the main thread to the upload, and hash the streams only when a report or test asks for the statistics. | `EffectRegistry`, `UnitEmitters::frame`, `BattleEffects::frame`, `stream_hash` | Most of the extension's main-thread frame work (about 70 % of it at M): an estimate of 4 to 5 ms of the laptop's 20 ms frames at M (the frame less the tick and the render CPU time, times the profile's share), more on the rig. The hash alone is about 8 % of the busy samples. **Done**: the main thread's particle time down a third to 40 %, the busiest frames 5 to 20 % faster ([below](#the-viewers-particles-638)). |
| 5 | One transform per unit per tick, shared by the collection boxes, the unit systems and the combat world. | `to_matrix`, `transform_point` | 3 to 4 %. |
| 6 | Allocation churn on hot paths: per-partition scratch buffers, fewer `Result` wrappers in the inner arithmetic. | projectiles, serial commits | 3 to 5 %. |
| 7 | Craft locomotor: keep the sine and cosine of a heading that did not change. | `Locomotor::run` | 2 to 4 % at S, more with the squadron dogfights. |

## Fidelity list

- The retail S melee is decided faster and more one-sidedly than the remake's (above). **Unverified**
  as a sim difference: the staging's placement and FoC's own targeting after the orders differ.
  Retail frame times and staging parity remain a separate baseline follow-up (legacy EAWR-1262).
- FoC lowers its logical frame rate with the rendered one when it cannot render 30 frames a second
  (the debug build's readout on the rig: LFPS equal to RFPS below 30), so its battle slows down
  instead of skipping frames. The remake runs the simulation on its own thread at 30 Hz whatever
  the frame rate. Whether retail does the same as the debug build is **unverified**; the retail
  pacing baseline follow-up tracks that question (legacy EAWR-1262).
- The viewer reuses same-size particle surfaces ([measurements](#particle-surface-reuse-2026-10-04)); emitter bone placement, projectile stream preparation and death-clone proxy posing retain serial work. Remaining preparation cost stays on the performance follow-up (legacy EAWR-1261).
