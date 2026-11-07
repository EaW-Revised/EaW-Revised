# Async galactic AI quality harness

Measures whether the asynchronous galactic AI (snapshot every K = 15 ticks, decisions applied D = 30 ticks later) plays
like the synchronous reference it replaces. It is test-only: nothing here changes the simulation, and it reads the
public live-session API only. The design it implements is the quality plan of the async-AI design note (single
behaviour, a test-only synchronous reference kept for comparison).

| Piece | Where |
|---|---|
| Quality-run driver (one seed, one arm, N ticks, decision log) | `tests/galactic/ai_quality_run.hpp`, mode `--quality-run` of `galactic_ai_campaign_tests` |
| Case matrix (the registered runs) and CTest registration | `tools/gc_quality/sweep_matrix.json`, `cmake/EawrGcQuality.cmake` |
| Sweep runner (plan, submit, collect, report, smoke) | `tools/gc_quality/runner.py` |
| Comparison report | `tools/gc_quality/compare.py` (+ `metrics.py`, `stats.py`, `logs.py`) |
| Corpus upgrade dry run | `tools/gc_quality/corpus_upgrade.py` |
| Contract tests (synthetic logs) | `tests/performance/test_gc_quality.py` (`python_performance_gc_quality`) |

Run every command from the repository root; the Python modules live in `tools/` (`python tools/gc_quality/runner.py ...`
also works through `python -m gc_quality.runner` from `tools/`).

## The two arms

* `reference`: today's schedule, every AI service inside the frame loop (K = 1, D = 0 inline).
* `async`: K = 15, D = 30. **Until the async lane exists this arm is a stub**: `async_arm_available` in
  `ai_quality_run.hpp` is `false`, the driver runs the reference schedule and writes `"stub": true` into the log. The
  whole pipeline is therefore testable as reference against reference; `compare.py` then expects identical decisions and
  zero tick shift, and prints a warning that the thresholds were not exercised. The slice that lands the batch driver
  flips the constant and selects K = 15, D = 30 in `run()`; nothing else changes.

## Running a sweep

1. Copy `hosts.example.json` to `config/gc-quality-hosts.local.json` (ignored by git) and list, per profile, the
   pool hosts that can read that data (names as in `config/build-hosts.json`). Stock data and mod data are different
   pools; a host that cannot read a profile's data must not be listed for it. `runner.py smoke` runs one 300-tick case
   per host and profile: a host without the data reports a skip, never a failure (mod cases exit 77 when the workshop
   folder is absent).
2. Plan: `python tools/gc_quality/runner.py plan --out out/gcq/sweep1 --profile stock --seeds 1-4 --noise-seeds 101-104
   --ticks 27000 [--probes near1,nearall,deep1 --probe-seeds 1-2]`. Seeds are independent jobs; both arms of a seed go
   to one host (same host and build for the pair); seeds are spread over the pool, the longest first.
3. Submit **detached** (a pool wait can exceed a worker tool's two-hour limit, and a killed client loses its place):
   `Start-Process -WindowStyle Hidden python -ArgumentList 'tools/gc_quality/runner.py','submit','--out','out/gcq/sweep1'`
   with output redirected into `out/gcq/`. `submit` runs one pool job at a time (`--max-parallel` raises it only on the
   coordinator's word), always passes `--host`, `--lock-timeout 28800` and `--strict-probe-base`, and collects each job
   as it ends. Run it from a checkout of the commit under test: it submits the working tree (`--dirty`) with the marker
   file `tools/gc_quality/sweep.enabled`, which makes CMake register the quality cases (below), and removes the marker
   afterwards.
4. `python tools/gc_quality/runner.py status --out out/gcq/sweep1`, then `report --out ...` writes
   `out/gcq/sweep1/report.md` (+ `.json`). `compare.py` also takes any mix of log files or folders directly.

**Compact output only.** The driver writes nothing on the host: the decision log goes to the test log as `GCQ <json>`
lines (the only thing a pool job returns). `collect` keeps one gzip NDJSON per run under `<sweep>/logs/` and deletes the
raw test log (`--keep-raw` keeps it). Delete the sweep folder when the report has been read. Never keep 54,000-tick
traces, replays or job output folders on a pool host.

### Case matrix

A pool job cannot pass arguments to a test, so every (profile, arm, seed, ticks, probe) is a registered CTest case named
`galactic_ai_quality_<profile>_<arm>_s<seed>_t<ticks>[_<probe>]` (`sweep_matrix.json` is the only list; the CMake
registration and the runner both read it, and a contract test checks they agree). The cases take minutes to hours, so
they are registered only with `-DEAWR_GC_QUALITY_SWEEP=ON` or the marker file; ordinary and batch runs never list them.
Profiles: `stock` (installed Underworld sandbox), `raw` (the Republic-at-War campaign, Republic AI), `rawsep` (the same mod with the human on the Republic side and the Separatists as the AI, reference arm only) and `bfc` (workshop 2712013423, reference arm only). `python -m gc_quality.techup <logs>` (from `tools/`) reports per faction whether the galactic tech level reaches its cap in 15 minutes, the level per minute and the tech goals the AI held. Runs shorter than 3,000 ticks are host smoke tests (identity checks only). Runs of different lengths are reported separately. Lengths: 54,000 ticks (30
game minutes, the acceptance run), 27,000 (the 15-minute progression gate) and 18,000 (smoke), 7,200 for the three
defence probes, 300 for host smoke. Seeds 1.. are the paired seeds, 101.. are reference-only noise seeds.

## Decision log (schema `gcq-1`)

NDJSON, one object per line, `r` is the row kind. Players are galactic player ids, planets and fleets entity ids.

| `r` | Fields |
|---|---|
| `meta` | `schema, seed, arm, arm_effective, stub, profile, campaign, ticks, probe, probe_tick, human, workers, sample_period, ai: [{p, type, faction}]` |
| `ev` | `t` tick, `k` kind. `movement_begin`: `o` owner, `s` fleet, `from`, `to`, `tc` target class (`own`/`enemy`/`neutral` at that tick), `n` ships. `movement_finished`: `o, s, at, po` (owner of the planet arrived at). `production_started/completed`: `o` (planet owner), `s` planet, `ty` type, `sq`. `fleet_destroyed`: `o, s, at`. `planet_owner_changed`: `s, v` new owner, `prev`. `conflict_pending, planet_combat_begin, auto_resolve_won/lost`: `s, v` (+ `inv, def`). `command_rejected`: `o, seat, why` |
| `smp` | every `sample_period` ticks and player: `t, p, cr` credits, `tech, mx` tech level and cap, `pl` planets owned, `fl` fleets |
| `goals` | `t, line`: the native AI goal reader's summary line per player (goal types and cumulative refusals by reason) |
| `probe` | `t, scenario, issued, target, target_owner, hops, fleets, fleet_ids` (or `reason`) |
| `hash` | every 900th tick hash (digest identity between runs of one arm) |
| `tech` | `t, p, tech, mx`: galactic technology account (`current_tech`, `maximum_tech`) of an AI player or the human at tick 1 and at every change |
| `tgoal` | `t, line`: per-goal line of a goal whose function or plan name contains "tech", every `sample_period` ticks |
| `drd` | tech-steal trace: the droid-team hero actor at tick 1 and at every change: `t, id, o` owner, `pl` planet, `po` planet owner, `ct` container, `fl` fleet, `at` fleet orbit, `tr` in transit, `dead, del`; `gone` when it vanishes |
| `abil` | ability result: `t, req, actor, pl, o, out` outcome, `k` kind, `item, tech` |
| `end` | `ticks, hashes, hash_fnv, ai_orders, ai_rejected, wall_s`. A run without `end` is a cut log and is excluded with a warning |

Decisions are the AI's orders: `movement_begin` (the target is `to`) and `production_started` (the target is planet and
type). The human seat answers prompts with the ordinary auto-resolve policy.

## What the report checks

One markdown table, PASS / FAIL / REVIEW / WARN / NA per row, then the detail tables behind each row.

The report verdict is FAIL (exit 1) when any row fails; **NOT-RUN** (exit 2) when an async arm is the reference schedule
(stub: its identity and digest rows are a harness self-check only); **INCOMPLETE** (exit 2) when no seed group had both
arms, or a gated row could not be evaluated (NA: dead async arm, run shorter than the tech gate, smoke run, no goal
summaries); REVIEW (exit 0) when only a defence probe needs the owner's eye; otherwise PASS. Only the stub latency floor,
the first-attack row and the defence probes may stay NA. A goal summary whose refusal list cannot be read is a FAIL row
(and `metrics.refusal_totals` raises), never zero refusals.

| Check | Rule |
|---|---|
| Early-window identity | the first 40 decisions per player: kind and target identical in >= 85 % (100 % for the stub arm) |
| First attack order | same target in every (seed, player) that attacks in either arm |
| Tick shift | median tick difference of matched decisions in 25..50 (D + j is 30..45, +-5); 0 for the stub arm |
| Reaction latency | trigger = enemy fleet arriving at an AI planet, an AI fleet lost, an AI planet lost or captured; latency = ticks to that player's first order of any kind after it (within 1,800 ticks; orders touching the trigger planet are information). **Reference-relative gate**: async minus reference at p50 and at p95 must each be <= 45 ticks (the design maximum added latency is D + K - 1 = 44), on the paired seeds; the absolute values are printed with it. Async must never answer in fewer than 30 ticks |
| Distributions | per 10-minute window and AI player: attack, expansion and reinforcement orders, builds, captures, losses, planets, credits, tech; first-attack tick and target-class shares, pooled and per faction. KS test, Holm-adjusted over all metrics, plus median shift (<= 10 %, <= 15 % for counts below 20) |
| Noise floor | the same statistics between disjoint seed groups of the reference runs (all balanced splits, or 200 seeded draws). A metric fails only when it is significant **and** beyond the 95th percentile of the reference-vs-reference KS D, or when the median shift exceeds both the limit and the noise 95th percentile. Needs at least four reference seeds (paired plus noise seeds) to form two groups |
| Tech progression | every AI player at tech >= 3 within 27,000 ticks (15 minutes); tech 4 listed; reference misses are inherited. Runs shorter than the gate are NA |
| Refusal rate | engine refusals outside planning and commit rejections of AI seats, each as a share of (that + AI orders): async may exceed the reference by at most 1 percentage point. The goal reader's `alloc:*` and `goal:*` counters (a goal found no budget, units or plan) are planning outcomes, about 95 % of all counts in the reference, and are listed but not gated |
| Goal types | no goal type seen in the reference disappears (Poisson 95 %) |
| Defence probes | a scripted human fleet is sent at an AI planet (`near1`: strongest fleet to the nearest AI planet, `nearall`: all idle fleets, `deep1`: strongest fleet to the farthest reachable AI planet) at tick 3,000; outcome per battle (AI defended / lost) with Wilson intervals. Async losing more than the reference interval allows is REVIEW: the owner sees the numbers before default-on |

Not measured here: faction win rate (a 30-minute run rarely ends in victory; the final planet and credit samples are in
the log), wall-clock performance (that is the tick profile's job).

## Corpus upgrade dry run (format bumps F1..F5)

`tools/gc_quality/corpus_upgrade.py` regenerates the 12 checked-in galactic replays, their hash CSVs and manifests with
the repository's own generators (the same list `tools/replay/upgrade_galactic_corpus.py` uses; the upgrade script now
shares it) into a staging folder, twice, and writes `report.md`/`report.json`: byte-identical generation, every file as
unchanged / moved / new / not regenerated with the first differing offset or CSV row, and the publish-scan pin of every
replay (stale pins, pins that would move). It never writes below `--root`. CTest case `galactic_corpus_upgrade_dry_run`
(labels `corpus` and `batch-only`: batch and nightly runs select it, routine PR checks leave it out) runs it with `--expect clean`: until a format bump lands the regenerated corpus must equal the checked-in one. A bump
slice runs it with `--expect moved` for review, reads exactly what moved from the report, then runs the real upgrade
(`galactic_corpus_upgrade`) in the same change.

## Hosts and wall time

* Stock needs the installed game data on the host; the mod profile also needs the workshop folder beside the game root.
  Verify a host with `runner.py smoke` before placing runs on it. A pool host without the data cannot run a seed and
  must not be in the pool file (the build-only hosts of `config/build-hosts.json` are not).
* The reference arm runs about 33 minutes per 54,000 ticks on the stock data and about 65 minutes on the mod data
  (design estimates); the async arm about 20 and 45 minutes. Six seeds (4 paired + 2 noise) at 27,000 ticks is
  roughly 2 hours of one host. Measured wall time per host and arm comes from `runner.py walltime --out <sweep>` once a
  sweep has run; the measurements of this harness's own dry run are in the pull request description.
* One pool job at a time per worker; a mod-profile run only on a host the owner has released for it.
