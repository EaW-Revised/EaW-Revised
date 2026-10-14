# Installed replay batch checks

The `installed` group is a batch/release check. A combat or motion PR may also
select it as its own feature test. It always mounts `--game-root`; the `synthetic`
group keeps the existing replay scope and excludes this directory. The installed
root is supplied by the caller or `EAWR_EAW_GAME_ROOT`, including a verified mirror.

| Command script | Ticks | Coverage |
| --- | ---: | --- |
| banked-hardpoints.json | 1500 | Acclamator turns, moves, attacks and fires hardpoints while banked |
| special-blast.json | 1500 | Peacebringer SPECIAL and charged BLAST during an attack turn |
| leech-shields.json | 1500 | Kedalbe attacks and activates targeted Leech Shields |
| carrier-waypoints.json | 2000 | Acclamator launches TIE squadrons; fighter attack; three queued move points |

JSON files are the committed command scripts. `append: true` on a move is the
native queued-waypoint payload (HCB-57); it is rejected on other actions and must
be a boolean. No scenario uses table overrides, forced damage, or staged spawns.
The carrier's `observed` fighter binds to a squadron the session launches itself.

Record the fixtures natively, with a fresh output directory:

```powershell
python tools/replay/record_installed.py --program <sim_headless> --game-root <installation> --out <recordings> --check
```

The recorder checks actual banked releases, charged shots, an active Leech,
fighter launches/fire and waypoint arrival in order. It preserves trace/combat
CSV files and a `recording.json` receipt. `--check` uses a fresh child directory,
runs all four recordings on 1/2/4/8 workers, and emits `baseline.json`. Review and
copy the four native `.eawr-replay` files and baseline into this directory. An
existing tracked baseline is checked when present; initial generation is explicit.
Do not replace the baseline to dismiss a failure.

Fetch the exact executable from a successful pool root build, retaining its provenance:

```powershell
python tools/offload/windows_build.py --ref <head> --preset windows-msvc-ninja --no-test --no-werror-probe --fetch-sim-headless out/replay-bin
```

Use `out/replay-bin/sim_headless.exe` as `<sim_headless>` below, and keep the adjacent
`provenance.json` with the receipts. This also supplies the executable for synthetic
compare76 runs. See worker offload.

Run the frozen batch comparison:

```powershell
python tools/replay/compare_installed.py --group installed --program <sim_headless> --game-root <installation> --out <fresh-receipts>
```

Hashes, events and snapshot CSV bytes must match the frozen baseline and each
worker count. Receipts include the executable and replay SHA-256 values, mounted
content requirement and every output digest. The native replay loader validates
the recorded content identity against the installation before running commands.
A mismatch is a failure, never a fallback to empty tables.

`--group synthetic --baseline <receipt.json>` runs the original group separately
(the previous compare76 receipt format is accepted). Use the tracked driver for
batch runs: the older ignored compare76 driver discovers every tracked replay and
assumes exactly 76 runs. Its discovery must exclude paths under
`tests/skirmish/fixtures/installed/`, before constructing its 76 rows.

Targeted Windows game-data validation selects `^installed_replay_recording$` via
the normal offload runner. It records the native scripts and checks 1/2/4/8 equality;
the test is labelled `batch-only;installed-required` and fails without installed content.
Routine PR selection excludes the batch-only label.

The command scripts opt into `installed_replay_audit: true`. Their recorder traces
use the separate `eawr-installed-replay-audit` format (version 1), with signed Q24
positions and bank angles, charged release counts, and ability activity. Ordinary
scenario traces retain the existing `eawr-trace` format and fields; the native
recording check also verifies that default contract. Audit diagnostics are not
part of the binary replay format.
