# Galactic replay corpus and pinned hashes

The synthetic campaigns in `tests/galactic/fixtures/corpus/manifest.json` are the
frozen baseline for the [galactic replay format](galactic-replay-format.md).
Every listed replay runs through `sim_headless --galactic-replay` with 1, 2, 4 and
8 workers. Each completed tick must match its committed `*.hashes.csv` pin.
Comparing all workers with pins catches both scheduling differences and a rule
change shared by every worker count. Failures identify the case, worker count
and first divergent or missing tick.

| Campaign | Coverage |
|---|---|
| `quiet-minimal` | Two planets, one human side, no fiscal service, quiet ticks |
| `fiscal-boundary` | Two human sides, stock 1350-frame fiscal boundary and periodic clock gates |
| `coop-orders` | Two human seats on one side, AI opponents, ownership/orbit orders, rejected orders, nested pause and resume, pacing speed |
| `partitioned-orders` | 257 planets, eight factions, uneven partitions, coop and the same command mix with frequent fiscal windows |

The corpus is project-authored synthetic data and needs no installed game.
`galactic_replay_corpus_generate` creates campaigns through the public
`GameSession` API, records the executed typed commands and writes pins from the
live run. CTest replays those frozen recordings independently through the CLI.
`galactic_replay_corpus_contracts` changes a valid recording's fiscal rule and
requires the actual gate to fail at tick 1. It also checks missing worker-8
evidence, incorrect target identity, truncated pins and divergent hashes.

## Batch regeneration

Regenerate once on the integrated branch after reviewing an intentional state
or rule change. Never hand-edit hash rows. With a configured build directory:

```sh
cmake --build <build> --target galactic_replay_corpus_generate sim_headless
<generator> tests/galactic/fixtures/corpus
python tests/galactic/replay_corpus_check.py --program <sim_headless> \
  --corpus tests/galactic/fixtures/corpus --out out/gc-corpus --target windows-msvc
ctest --test-dir <build> -R galactic_replay_corpus --output-on-failure
```

Use the project's build-host route or bounded local lane for these commands.
For a multi-configuration build, add `--config Release` and `-C Release` and use
the executable paths under `Release/`. Review the replay and pin diff together:
the manifest is explicit so a missing campaign cannot silently reduce coverage.
New corpus cases ship their own recordings and pins.

Each batch target can retain evidence with a distinct `--target` under the same
`--out` directory. Compare the named outputs with the shared tool:

```sh
python tools/compare_replay_hashes.py --evidence-root out/gc-corpus \
  --galactic-corpus tests/galactic/fixtures/corpus \
  --target windows-msvc --target linux-x64-gcc
```

The existing tactical `--audit` and bundle checks keep their behavior; they can
also be combined with `--galactic-corpus` in one invocation.

## Stale GC pins in feature work

Feature PRs retain the integrated branch's existing GC corpus recordings and
pins. If an intentional change moves these hashes, report `GC pins stale:
<cases>` in the PR body with the changed rule and first divergent tick. The
coordinator reviews that difference and regenerates once after the batch merges.
This is separate from tactical pins: every tactical replay must still preserve
its established behavior unless that change explicitly alters a tactical rule.
