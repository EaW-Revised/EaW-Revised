# Battle benchmark: the close-range melee (EAWR-601)

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
  [--csv prefix] [--replay-out file] [--list 1] [--profile on]` (needs the game data). It prints
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

## Simulation (RTX 4070 Laptop host, i7-14700HX, 28 hardware threads, 2026-09-29)

Cost per tick over the 4500 ticks (150 s of battle) of seed 601, against the 33.3 ms budget of the
30 Hz tick. *Base* is the integration head with the benchmark; *trial* adds the dogfights of PR
EAWR-585 (head 20a4fa6c, a detached trial merge). Every row's run is hash-identical to its size's
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
- **EAWR-585 (dogfights)** adds the `dogfight-chases` phase (0.18 ms mean, 2.1 ms p99 at 1 worker at
  S) and more shots: S at 1 worker 9.3 against 7.7 ms (+21 %), at 28 workers 6.4 against 5.6 ms
  (+14 %). It also changes the battle (the Empire keeps 14 units at the end of S; M has fewer
  projectiles in flight, 109 against 145 on average, so it runs faster than the base), so its
  M and L rows compare two different fights.

## Projectile broad phase (EAWR-636): before and after

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
the integration head 667c939c with the benchmark (EAWR-621); *after* adds EAWR-636. Before and after ran
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

Again on the integration head after EAWR-621 landed (32467877, which also brings the ion weapons of
EAWR-561 and the aimed hardpoints of EAWR-669, so the fights differ from the table above), the same
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
of 551 million). The synthetic budget test (`damage_tests`, EAWR-636) holds a far station's reach and
two corvettes out of the exact tests.

What is left: a sampling profile of L at 1 worker after the change (on 667c939c, `--profile on`, a PDB build)
puts `projectiles` at 23 % of the busy samples (82 % before), below the serial state hash
(`sha_transform` alone 12 %). The model-space box test no longer shows; the phase's cost is now the
sweep over the index's bodies, since the query is still grown by the largest reach (a capital
ship's): the reach test (`within_range`, 8.6 %), the index's box test and cell walk (4.8 %). The
next step, if the phase matters again, is a query grown by the small units' largest reach plus a
short list of the few large-reach units tested directly.

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
  build, whose own code may be slower than retail's; retail has no frame-rate readout, and a
  frame-timing tool is a new third-party dependency (not added; ask first).
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

## Ranked fixes

Expected gains are estimates from the profiles' shares, not measured; every fix keeps the state
hashes (each is a faster way to the same result, or work moved off the critical path).

| Rank | Fix | Where | Expected gain |
|---|---|---|---|
| 1 | **Done (EAWR-636):** projectiles -83 to -94 % ([before and after](#projectile-broad-phase-636-before-and-after)). Projectile broad phase: query each candidate with its own collision reach instead of the largest one's, reject with a cheap integer sphere and box test before the model-space transform, and reuse a per-partition candidate buffer the index fills in ascending ID (no per-projectile heap vector or sort). | `step_projectile`, `SpaceIndex::box`, `segment_enters_box` | `projectiles` down 60 to 75 %: S at 1 worker about 7.7 to 5 ms mean and p99 37 to about 15 ms (no tick over budget); L at 28 workers about 16 to 11 ms and most of its 546 ticks over budget gone. The biggest win at every size. |
| 2 | Pool dispatch: run a phase inline, or on fewer workers, when its input is small, and let the workers spin briefly between the phases of a tick before they park. | `ThreadWorkerAdapter` | About 1.5 ms a tick at 28 workers (the small phases' 0.1 to 0.2 ms each and the wake-ups): S 5.6 to about 4 ms; the speed-up curve stops falling with more workers. |
| 3 | The state hash off the stepping thread: hash the frozen canonical bytes on a worker while the next tick runs (the hash is published a tick later), or with the CPU's SHA extensions where it has them (the same digest). Whether the live game needs a hash every tick at all is the owner's call. | `TacticalSession::state_sha256`, `core::sha256` | 1.3 to 1.6 ms of the 2.2 ms serial remainder at S (2 to 3 ms at L): the scaling limit moves from 1.4x to about 2x at S. |
| 4 | The viewer's particles: step the effect and emitter particle systems on the worker pool (partitioned by emitter, the stream built there too), keep the main thread to the upload, and hash the streams only when a report or test asks for the statistics. | `EffectRegistry`, `UnitEmitters::frame`, `BattleEffects::frame`, `stream_hash` | Most of the extension's main-thread frame work (about 70 % of it at M): an estimate of 4 to 5 ms of the laptop's 20 ms frames at M (the frame less the tick and the render CPU time, times the profile's share), more on the rig. The hash alone is about 8 % of the busy samples. |
| 5 | One transform per unit per tick, shared by the collection boxes, the unit systems and the combat world. | `to_matrix`, `transform_point` | 3 to 4 %. |
| 6 | Allocation churn on hot paths: per-partition scratch buffers, fewer `Result` wrappers in the inner arithmetic. | projectiles, serial commits | 3 to 5 %. |
| 7 | Craft locomotor: keep the sine and cosine of a heading that did not change. | `Locomotor::run` | 2 to 4 % at S, more with the EAWR-585 dogfights. |

## Fidelity list

- The retail S melee is decided faster and more one-sidedly than the remake's (above). **Unverified**
  as a sim difference: the staging's placement and FoC's own targeting after the orders differ.
- FoC lowers its logical frame rate with the rendered one when it cannot render 30 frames a second
  (the debug build's readout on the rig: LFPS equal to RFPS below 30), so its battle slows down
  instead of skipping frames. The remake runs the simulation on its own thread at 30 Hz whatever
  the frame rate. Whether retail does the same as the debug build is **unverified**.
