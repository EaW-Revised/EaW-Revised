# Degradation ledger

The runtime drops, skips or substitutes things it cannot use (an unknown texture name, a clamped value, a skipped
object, a retired script instance) and carries on. The degradation ledger is the one tally of all of it for a session,
written as a small JSON summary and one log line, so a person or a gate can see what a mod load or a match gave up
without reading logs. The class inventory and the design are tracked in the degradation-ledger ticket (legacy EAWR-6846).

The ledger is **observation only**. Nothing in the simulation, the AI, replays, checkpoints, saves or hashes reads it, no
record call may change control flow, and the summary is never part of a content digest. A test steps a world at 1, 2, 4
and 8 workers with records arriving from other threads and between ticks and requires every state digest to equal the
silent run's.

## API

`eawr/core/degradation_ledger.hpp`, `eawr::core::degradation`:

| Call | Use |
|---|---|
| `record(code, source, object, detail)` | One degraded object. Thread safe (one short mutex). Call it on the owner/serial side or at a publish point, never from a sim worker's tick: partitions keep using `LoadNoteBuffer` and the owner records at the merge. |
| `record_load_notes(notes)` | The load-note adapter. Records every class C note, and every skipped object, skipped placement or runtime degradation that is not a refusal. Layers `stock`, `base`, `expansion` and empty become source `stock`; a layer id listed in the session's `mod_layers` (the headless mount chain's `mod`, `mod-parent-1`, ...) becomes the mod id at the same index of `mods`. Call it where notes are published, once the layer ids are final. |
| `record_script_fault(fault, source)`, `record_runtime_line(line, source)` | The script-fault adapters (step 1b). A fault is recorded under its code with the text without its tick as the object (the same fault on many ticks is one object). `is_script_fault_code` names the four codes. |
| `campaign_source()`, `source_for_session_layer(layer)` | The source of a record without a layer of its own: the one mounted mod, `stock` with none, `unknown` with several; and a layer id mapped to the session's mod id (what `record_load_notes` uses). |
| `overlay_line(snapshot)` | The F3 overlay's one line (`degraded N in M classes[, K unknown]`, empty when nothing is degraded); see docs/ui/perf-overlay.md. |
| `begin_session(info)` | Starts a session: clears the records, takes the mode and mod chain, generates the session id, build and commit when missing. |
| `describe_session(info)` | Updates the session fields without clearing the records (a process that records before it knows its mode). |
| `snapshot()` | A sorted, immutable copy. |
| `summary_json(snapshot)`, `summary_line(snapshot)` | The file body and the log line. |

`platform::write_degradation_summary(file, ended)` (`eawr/platform/degradation_summary.hpp`) stamps `written_at` and
publishes the file atomically (temp file, then rename). A failed write is reported and never fails the session.

`platform::ScriptFaultLedger` (same header) files the script faults a campaign step published: the layer of a fault's script
is looked up once per logical path through the mounted data (`vfs::Vfs::stat`, then `source_for_session_layer`), on the
owner side after the step, never in a worker's tick. A fault whose script the mounts cannot place is `unknown`, and counted.

## What a record keys on

A distinct key is (code, source, object).

- `code`: the `EAWR-...` code of the class.
- `source`: the mod id (the mounted layer's key) the item came from, `stock` for retail data, `unknown` when the site
  cannot tell. Unattributed records are counted separately (`totals.unattributed`) so they can be driven to zero.
- `object`: the authored object (a unit, a texture name, a file when the note has no object). Text is cut to 256 bytes,
  detail to 512, absolute paths are redacted, control characters become spaces.

Per (code, source) the ledger keeps `objects` (distinct objects, counted by a 64-bit hash of the bounded object text: a
hash count, so a collision of two different objects would count as one, and there is no per-class object cap because no
per-object text is stored beyond the one example, so the total never truncates), `occurrences` (every record call, which can differ with frame rate) and the first example:
the lexically smallest (object, detail) pair at the highest severity seen, so it does not depend on the order or the thread the records came
from. Each class publishes that highest `severity` (`info`, `warning` or `error`). A gate compares `objects`, not `occurrences`.

## Classes fed in step 1a

| Code | Source of the record | Feeds from |
|---|---|---|
| `EAWR-LOAD-0xxx`, `EAWR-LOAD-1xxx` | the note's layer id | load notes (docs/load-notes.md): class C notes, skipped objects and placements, runtime degradations, the skipped attract scenarios (`EAWR-LOAD-1235`) and the dropped skirmish start types. Recorded where the notes are published: the viewer's load-note publisher and the headless skirmish, scenario and galactic loads. |
| `EAWR-LOAD-1239` | the note's layer id | a model or animation the loaders refuse (empty, no root group, does not bind to its model): the object keeps its model and plays no clip, one note per file and stage (docs/behaviour/asset-degrade.md, ASD-01) |
| `EAWR-ASSET-NAME-0001` | the optional `source` argument of `assets::report_name_miss`, `unknown` when a site passes none | every asset-name miss, also the repeats and the names past the resolver's own 4096-name report cap |
| `EAWR-DEGRADE-0001` | not a class: the code of the one log line | the session end |

## Classes fed in step 1b (galactic)

| Code | Source of the record | Feeds from |
|---|---|---|
| `EAWR-SCRIPT-*`, except `-0218` print output | the script's mounted layer, or the AI campaign source when no module is attached | completed galactic step diagnostics from AI, story and game-object scripts, recorded by the live session after the step; internal errors/exceptions are ERROR, while `-0216` stand-ins and `-0217` sourced warnings remain WARNING |
| `EAWR-SCRIPT-0211` (refusal), `EAWR-GCFUND-0001`, `EAWR-GCAI-*` runtime lines | `campaign_source()` | the native AI's runtime lines (a bill the AI's ledger refused, a fiscal cycle skipped, ...): the serial side of each AI tick forwards the lines that arrived since the last tick |
| `EAWR-GCAI-PROVIDER-0001` and its cause's diagnostic code | `campaign_source()` | the serial retirement flush records ERROR entries with player, authored faction, tick, reason and original diagnostic id/text. The generic retirement load note uses the same provider object, so it does not add another distinct retirement. |
| `EAWR-GCAI-INTERNAL-0001` | `campaign_source()` | an unrecoverable native AI service Result, including infrastructure faults that do not retire a player: ERROR with available provider context and original diagnostic id; the original Result and control flow are unchanged |
| `EAWR-LOAD-1xxx` runtime-degraded notes | the note's layer | the runtime notes of a live galactic session reach the same sink as the load notes |

Fatal AI/script Results also reach the ledger before the session returns failure; a failed session must not erase its
error classes. A failed story-plot request returns to the script that asked and shows up as that script's fault.

## AI failure visibility

Owner policy: every native provider retirement is an ERROR, never an ordinary gameplay refusal hidden as a warning.
The serial owner flush logs one line and publishes one observer notice per retired player. Its authored faction label
comes from the campaign perception setup, not the AI player-type name. Internal script diagnostics carry their player,
faction, tick and diagnostic code; unavailable owner context is explicit rather than guessed.

The notice text is `AI player <faction> stopped after an internal error`. A completed service hands it to the campaign
as observer output; the campaign emits an existing `SCREEN_TEXT` presentation request for each active human owner
(including the multiplayer host's human recipients), deduplicating co-op seats. It remains visible for 15 presentation
seconds. It never enters native story events, plot inputs, gameplay commands, saves, hashes or replays. Loading a
retired provider does not itself replay a mid-campaign retirement notice. The underlying retirement state and its
checkpoint/replay behavior are unchanged. These are project diagnostics, not original-game behavior rules.

Not yet fed: tactical script and bridge drops, audio fallbacks, scene placement issues and render/shader fallbacks. To add a class, call `record` at the emit
site with the class's code, the source mod and the object, in the owner/serial context, and add a test that a forced
degradation lands in the summary.

## `degradation-summary.json`

```json
{"schema_version":1,"session_id":"20261113T093000Z-1a2b3c4d","mode":"skirmish","run":"<map name>","build":"0.3.4+<commit>","commit":"<commit>",
 "chain_fingerprint":"<loadcheck fingerprint or empty>","mods":["<mod id>"],"ended":"ok","written_at":"2026-11-13T09:35:12Z",
 "totals":{"items":12,"classes":3,"unattributed":0},
 "classes":[{"code":"EAWR-ASSET-NAME-0001","source":"<mod id>","severity":"warning","objects":12,"occurrences":40,
             "first_example":{"object":"texture ...","detail":"not found (site ...)"}}]}
```

- `session_id`: `<UTC start time>-<8 hex>`, generated at the session's first ledger use (`begin_session`, or the first record or write). The id carries the UTC start time and random bits, so it is not reproducible. The per-session load-notes file carries the
  same id (docs/load-notes.md), so the two files of a run belong together.
- `build` is the engine build identity and `commit` the commit inside it (empty for an identity without one).
- `mode`: `galactic`, `skirmish`, `scenario`, `replay` for headless runs, `viewer` for the viewer.
- `run`: the campaign id of a headless galactic run, the map name of a headless skirmish run (the setup id, `m2`, when no `--map` is given), with no folder and no `.xml`/`.ted` extension; empty for the other modes and for the viewer. It is part of the baseline key.
- `mods`: the mounted mod ids in chain order, no paths: the profile's layer keys for a profile run; for a plain `--mod-root`, `workshop:<item id>` when the folder is a Workshop item folder (`.../workshop/content/32470/<item id>`), otherwise the folder's path key (viewer and headless name mods the same way); in a headless run an empty segment of a `;` chain is named `unknown:empty-chain-segment-<layer id>` instead of after the working folder (the chain is split by `vfs::mod_chain_folders`, the same split the mount uses). The viewer takes its mods list from the mod catalog's records and still drops a segment without one, which shifts the layers after it (legacy EAWR-7074). `chain_fingerprint` is filled where the process has computed
  it (the viewer); empty otherwise.
- `ended`: `ok`, `failed` (a non-zero headless exit, or a viewer session that wrote a failure replay) or `aborted`
  (reserved).
- `classes` are sorted by (code, source), bytewise. `written_at` is stamped by the writer and is the only field that differs between two writes of the same ledger (`session_id` also carries a UTC start time, but it is fixed for the session).
- `totals.items` is the sum of `objects`; `classes` is the number of (code, source) entries.

## Where it is written

- **`sim_headless`**: `--degradation-out <file.json>` on every mode, and only with this option. It is taken out of the argument list in `main`, so one
  funnel writes the summary after the mode returns, on success (`ended: ok`) and on every failure exit (`failed`), and
  prints `EAWR-DEGRADE-0001 N items degraded in M classes (see degradation-summary.json)` to standard error. Without the
  option nothing is written.
- **Viewer**: `degradation-summary.json` beside `godot.log` (`user://logs/` unless the log path setting moves it), written
  in the host's destructor with the same log line, and once earlier (without the line) when a live session writes a failure
  replay, so a crash after a failure still leaves the file. Those are the only two write points. When the settings singleton is gone and no folder is known, a warning says the file was not written.

## Load notes per session

`last-<hash>/load-notes.json` used to be overwritten by every session of a profile. Each session now also writes
`last-<hash>/s/<session id>.json` (the newest 16 kept), and `load-notes.json` stays the newest session's copy for the
mods service and the launcher. The per-session copy is best-effort: a copy that cannot be written is listed in the publish report's `leftovers` and printed as a warning by the viewer and by `eawr_loadcheck` (code `EAWR-LOAD-0002`), and it never costs the main file. See docs/load-notes.md.

## Baseline and gate

`tests/degradation_baseline.json` is the checked-in record of what each mod (and stock, with no mod) is known to degrade. It
is written only by `degradation_gate.py update` and kept as LF bytes (`.gitattributes`):

```json
{"schema_version":1,"mods":{"<mod id>":{"<mode>":{"<run: campaign or map>":{
  "build":"<engine build>","commit":"<commit>",
  "classes":{"<code>":{"<source>":{"objects":12,"ticket":6913,"reason":"one line"}}}}}}}}
```

- **Key.** A summary belongs to the first id of its `mods` list (`stock` when the list is empty; a summary without a `mods`
  list is malformed and refused, never read as stock), its `mode` and its `run` (the campaign of a galactic run, the map of a
  skirmish run, written into the summary by `sim_headless`). One mod and mode can therefore be baselined for several campaigns
  or maps. Inside an entry, classes are keyed by code and source. A summary from a binary that does not write `run` takes it
  from its file name (`<mod>__<run>.json`), and `--run` names it for one given file; a summary with no run is refused by `check`
  and `update`, because counts of different runs do not compare. A summary of a run the baseline does not have is a new entry
  (`check` fails, `update` adds it), never compared with another run.
- **Counts only.** Per (code, source) the file holds the distinct-object count, a `ticket` (a ticket number, or `"untriaged"`
  until someone files or links one) and a one-line `reason`. No object names, no example text, no mod content.
- **`build`, `commit`.** The binary that produced the entry. A `check` of a different build or commit prints a note, not a
  failure.
- **Which mod ids.** Baselines are taken from Workshop items (`workshop:<item id>`), whose id is the same on every machine. A
  mod mounted from a plain folder gets a path key (`path:<hash>`, or `unknown:...` for a chain segment without an id) that
  depends on where the folder is: `check` fails with a line naming it as unbaselinable and `update` refuses it.

`python tools/ci/degradation_gate.py check --baseline tests/degradation_baseline.json <summary.json>...` (or
`--summaries <folder>`) compares new `--degradation-out` summaries with it and needs no mod data. Every difference fails
(exit 1) with a line, so the debt shown is the debt there is:

- a count that **rises**, or a class (code and source) that is **new**;
- a count that **falls**, or a class that is **gone**: the baseline is stale, run `update` and commit the result;
- a mod, mode and run without a baseline entry, or a mod with a path key;
- with `--expect-all`, a baseline mod, mode and run without a summary. The full sweep passes it, so a run that did not happen
  cannot pass by being absent.

Input the gate cannot trust is refused (exit 2) instead: a run that did not end `ok` (it stopped early and counted too
little), a malformed summary or baseline, a summary with no run, two summaries for one mod, mode and run, and, with
`--since <ISO time>` or `--commit <sha>`, a summary written before that time or built from another commit (the full commit id,
an exact match: a prefix is another commit).

`degradation_gate.py update` prints what it changes against the current baseline, then rewrites the entries of the (mod,
mode, run) triples the given summaries cover and leaves the others alone. An entry that stays keeps its `ticket` and `reason`;
a new one is `untriaged` with an empty reason until a ticket is written in by hand. `--run` names the run of exactly one
summary file (it is refused with `--summaries` or several files). The output is deterministic (sorted keys, two-space indent,
LF, one trailing newline).

### Ticket linkage

`python tools/ci/degradation_gate.py tickets --baseline tests/degradation_baseline.json` checks that every class with a
count names an open ticket or gives a reason:

- a ticket number that does not exist fails ("ticket N does not exist");
- a ticket that is closed while its count is still in the baseline fails ("ticket N closed, count remains"): either the
  problem is fixed and the next `update` drops the count, or the ticket was closed too early;
- a class with `"untriaged"` and no reason fails; `--allow-untriaged` lists such classes as a note instead (the interim while
  the older entries are triaged);
- a `reason` without a ticket number is enough (a known and accepted degradation).

The tracker is asked through `gh` (one call per distinct ticket; `--repo OWNER/NAME` picks the repository, the current
checkout's by default). `--offline` only checks the format, for tests and for CI without a token. A tracker that cannot
answer (no `gh`, no login, no network) is exit 2, never a verdict about the ticket.

### The sweep

`python tools/ci/degradation_sweep.py` makes the gate's preconditions real instead of stating them: it runs on a host that
has the mod data and the binary, deletes every `--degradation-out` target before its run, runs each baselined (mod, mode, run)
of `tools/ci/degradation_sweep_matrix.json` with `sim_headless` (one after the other, logs under the output folder), then calls
`check --expect-all --since <start of the sweep>` (and `--commit` when given) on the summaries and finally the ticket check (for now with `--allow-untriaged`, temporarily: classes with neither a ticket nor a reason are listed, not failed, until the older entries are triaged; `--require-triage` makes them fail). A
run that failed before writing leaves no file to be mistaken for a fresh one, and is named as missing. The matrix holds Workshop
item ids, campaign or map names and `sim_headless` arguments only; the mod folders, the game folder and the binary come from the
command line (`--mod-base`, `--game-root`, `--sim-headless`), so nothing machine specific is committed. A test requires a matrix
run for every baseline entry. `--only <mod key>` runs a subset and then leaves out `--expect-all`.

```text
python tools/ci/degradation_sweep.py --sim-headless <exe> --game-root <install> --mod-base <workshop content folder> \
    --out <fresh folder> --commit <full commit id of the binary>
```

To add a run (another campaign or map of a mod): add it to the matrix, run the sweep, and `update` the baseline from its
summary; the new entry is `untriaged` until a ticket or reason is written in by hand.

The stock galactic entry is not empty: a stock AI script retired five Lua instances (`EAWR-LOAD-1400`) by calling an unbound API (legacy EAWR-6913), a bug fixed
after the binary that produced the baseline, so the next `update` on a newer main drops it. The same holds for the stock-script
counts of the other galactic entries, and the Thrawn's Revenge and Fall of the Republic entries were taken before their AI
type was fixed: they stay as recorded until a later sweep updates them.

## On screen and at exit (galactic viewer)

The F3 overlay shows `overlay_line` (refreshed once a second). The exit line is the one the host teardown prints with the
summary file (`EAWR-DEGRADE-0001 ...`): every galactic way out (menu, score screen, window close) ends in that teardown, so the
galactic mode adds no second line.
