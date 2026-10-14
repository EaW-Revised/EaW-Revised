# Galactic benchmark and work budgets

`gc_bench` measures the current galactic foundation with synthetic galaxies of
56 and 224 locations. Each galaxy has four factions, one fleet per location,
three members per fleet, and the synthetic generator's ring and shortcut routes.
No installed game data is required.

Run the executable on a build pool or rig host, using an optimized build:

```text
gc_bench --ticks 300 --warmup 30 --workers 1,2,4,8,hardware
```

The default worker list is 1, 2, 4, 8 and the hardware thread count, with duplicates
removed. An explicit list must start with 1. Every run compares every tick's state
hash, snapshot hash (including events), perception checksum and work totals with
the one-worker baseline. A disagreement or failed dispatch exits 1. Warmup ticks
also participate in equality; timing excludes them and excludes the equality
hashing and budget assertions. Pool startup and session construction are excluded.

CSV reports total time, the observed `galactic-gather` phase, the
`synthetic-perception` phase and their serial remainder. Phase measurements include
dispatch and wait costs. The remainder includes command admission and commit,
sorting, section encoding/hashing, clock service, snapshot publication, and the
perception allocation and ordered reduction. These serial services have no
individual timing hooks yet. Work totals include warmup. Times are observations,
never pass/fail budgets, and a run on a busy host is not a performance parity claim.

The command load issues one ownership change per planet and one orbit order per
fleet each tick. Existing debug commands stand in for AI plans and locomotion;
this does not measure travel, production, combat, Lua or a real AI. The perception
stub evaluates each faction against each published planet, route and fleet in
partitioned work with immutable inputs and an ordered reduction (ADR-009).
Replace the stub when the production evaluator lands, reviewing its work pins.

`gc_bench_worker_equality` exercises both sizes on 1/2/4/8/hardware workers.
`gc_bench_work_budgets` runs 60 ticks of command-loaded, quiet and paused galaxies
on 1/2/4/8 workers. It checks the existing session's actual work counters and
observed dispatch counts on every tick, plus the stub's counted object visits:

| Work per serviced tick | 56 locations | 224 locations |
| --- | ---: | ---: |
| Gathered objects | 182 | 728 |
| Gather partitions | 1 | 64 |
| Perception object visits | 728 | 2912 |
| Perception partitions | 64 | 64 |
| Loaded command commits | 112 | 448 |
| Encoded sections, first tick | 5 | 5 |
| Encoded sections, later loaded ticks | 3 | 3 |
| Encoded sections, later quiet/paused ticks | 1 | 1 |

The synthetic fiscal window is ten frames: exactly one cycle at each tenth
serviced frame, and exactly one periodic check at each thirtieth frame. Paused
ticks gather nothing, dispatch no service work and perform no fiscal or periodic
checks. The one initial pause command still commits. Encoded plus reused sections
must remain five; no existing campaign or tactical replay pin is regenerated.
Future services need their own measured phase and deterministic work budget.
