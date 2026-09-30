# Tag perturbation check

The owner's rule: every FoC XML tag has a target in our code, or it is known to have none
([docs/tag-coverage.md](tag-coverage.md) holds the registry). "A loader reads it" is not enough:
in EAWR-666 `Layer_Z_Adjust` was read for every unit and applied for craft, but ships ignored it. This
check can prove application mechanically: change the value, run the battle, see whether anything
changes. An unchanged battle leaves application unproven until its consumers are exercised.

## What it does

For every `applied` or `partial` registry row and every object type the row names:

1. The value is changed **in memory** for the M2 scene's objects of that type, never in a file:
   `data::with_overrides` for catalog objects (units, squadrons, hardpoints, projectiles,
   factions), `data::DocumentOverrides` for `gameconstants.xml` (both in `include/eawr/data/xml.hpp`).
   An override is the object's own definition authoring the new value, as a variant's own tag
   overrides its base: an inherited value is overridden for that object only.
2. The unit tables are rebuilt from the changed catalog, and the M2 battle runs with the AI on both
   sides (the soak's battle, `tests/skirmish/m2_battle.cpp`) for `--ticks` ticks at one worker.
3. It is compared with the unchanged battle tick by tick: the authoritative state hash (world and
   scripts; the setup's content identity is left out, so different tables alone do not count) and,
   per unit kind, the first tick a unit differs and the largest position and height difference.

The type vocabulary is the unit tables' kinds `station`, `ship`, `squadron`, `craft`, plus
`hardpoint`, `projectile`, `faction` and `constants`. A row's `class` (the XML element,
`SpaceUnit`) filters the objects: a `ship` check of a `SpaceUnit` row changes the scene's ship
types whose element is `SpaceUnit`. A tag shared by craft and ships is checked on both.

### The change

`auto` (the default) scales every number of every occurrence by 2, names between them staying (a zero becomes 1);
flips `yes`/`no` and `true`/`false`; drops the first entry of a list of names, or the last
occurrence of a repeated tag. A single name (an enum value, a reference) has no safe automatic
change: the row needs a hint, `set:<value>`, naming another valid value. `scale:<factor>`,
`flip` and `drop` force a kind. Membership lists (`Behavior`, `SpaceBehavior`, `Attributes`,
category/property lists, restrictions and exclusions) refuse `auto` and bare `drop`: use
`drop:<member>` to remove a named member, or `set:<value>` to state the complete replacement.
Removing an arbitrary first member cannot test the members the consumer uses. `set:` also applies to objects that do not author the tag (the
craft do not author `Layer_Z_Adjust`; `set:40` gives them one).

The hint lives in the registry row, in a `check` object the registry tool keeps as it is:
`"check": {"perturb": "set:40"}`, or `"check": {"skip": "<why it cannot be checked headless>"}`.

### Verdicts

| verdict | meaning |
|---|---|
| `changes` | the battle differs from the unchanged one; the row says from which tick, and which kinds moved how far |
| `no change` | identical battle in this window. The reason distinguishes an authored tag with no traced read, a read value dropped from the full typed tables, a changed table whose consumer the scenario may not reach, and **not authored; read unknown** when a `set:` change adds a missing baseline node. Whole-file reads give no per-tag read evidence. An unchanged battle alone does not establish that a tag is not applied. |
| `not exercised` | no object of the type in the scene authors or inherits the tag (give it a `set:` hint), or the scene has none of that type and class |
| `not checkable` | a presentation row (the viewer draws it; no headless viewer report yet), a type the check cannot change headless, a single name without a `set:` hint, or a row with a `skip`; also AI constants read as raw bytes (document overrides cannot reach them) and ambiguous membership-list changes |

Every row also has an **evidence** label: `proven` when the battle changes as expected,
`contradicted` when a missing type changes or authored values are traced as read but dropped
from the full tables, and `unproven` otherwise. The report counts these separately from verdicts.
The structural table comparison includes AI tech levels, spin-away inputs, abilities, diagnostics
and inputs; it leaves the replay content identity and pins unchanged. A read-and-dropped
contradiction concerns this load's tables; other scenario consumers still need review.

A **finding** is either a contradiction or an unproven registry claim:

- `no change on an applied type`: the row says the value is applied for that type, and the battle
  never changes (the EAWR-666 class). Either the code does not apply it for that type, or the registry
  is wrong, or the M2 battle does not reach it in `--ticks` (say so in the row's hint).
- `changes on a type the registry says is missing`: a `partial` row's missing type now changes the
  battle; the row should say `applied` (after EAWR-718, `Layer_Z_Adjust` on ships).
- `worker divergence`: a spot-checked row's changed battle differs at `--check-workers` workers.

A row that names no types is tried on every type its class can have; a type the scene has no
objects of drops out of the report, and a type that does not change is a finding (a `partial` the
registry does not know about).

## Determinism and cost

The unchanged battle runs twice, at one worker and at `--check-workers` (4); if they differ, no
row is checked and the run fails. Every `--check-every`th row (10) is run a second time at 4
workers and must match its one-worker run tick for tick.

The data is loaded once per run (about 9 s): the catalog, the model cache and the VFS are shared,
and each row copies only the definitions it changes, then rebuilds the unit tables and the battle.
The battle costs about 11 ms a tick at one worker on the owner's workstation. A row that changes
the battle stops 300 ticks after the first difference; a row that never does runs the full length
(about 40 s at 3600 ticks). Rows run in parallel, `--jobs` at a time, each battle on one worker.
A discovery run of every tag the M2 load reads (289 tags, 783 checks) took 14.5 minutes at 4 jobs
and 1800 ticks, so the full registry fits well inside an hour on 12 cores.

The M2 battle (seed 1) moves its ships from about tick 395 and fires its first shots at about
tick 2000 (the fleets meet between 1225 and 2300 depending on the seed), so the default `--ticks`
is 3600: at 1800 no weapon, projectile or damage tag changed anything. A tag applied only in a
later or rarer state (a death, an ability, a hardpoint's destruction, a depleted shield) can show
`no change` with the "never differs" reason: that is a finding to look at, and the fix may be a
focused staging, or a `skip` with the reason in the row.

If a changed value makes the data invalid (the battle does not build, for example a doubled
avoidance range out of the motion table's range), `auto` tries once more with the number halved;
if that fails too the row is `not checkable` and needs a `set:` hint.

## Running it

```
python tools/inventory/tag_applied_check.py run --registry docs/tag-coverage/<registry>.json \
    --binary <build>/tests/skirmish/foc_tag_perturb --out out/tag-check \
    --game-root "<FoC install>" [--ticks 3600] [--jobs 11] [--check-every 10]
```

It writes `plan.json` and `plan.tsv` (the checks), `results.jsonl` (the driver's line per check),
`report.json`, `report.md`, and `issue-drafts/<area>.md`: one issue per subsystem (the registry
row's `area`) with its findings, linking the subsystem's tracking issue (EAWR-649 movement, EAWR-650
combat, EAWR-651 fighters, EAWR-652 AI, EAWR-653 presentation, EAWR-654 economy). `tag_applied_check.py issues
--out out/tag-check --file` files them through the nightly soak's issue client (search, create
and comment only): an open issue of the same area, found by a key in its body, gets a comment
when its finding set or evidence labels change. A stable fingerprint stored in the issue or its
comments suppresses unchanged repeats across hosts and restarts.

The driver alone: `foc_tag_perturb --plan <plan.tsv> --out <results.jsonl>` (the plan is
`id class tag type change`, tab-separated; see the head of `tests/skirmish/foc_tag_perturb.cpp`).

The game-data CTest `tag_applied_check_smoke` (slow tier) checks end to end: a changed
ship `Max_Speed` and `MaxRotationsSpace` change the battle, an unread `Score_Cost_Credits` is a
finding, a presentation row is not checkable. It also checks raw AI constants, membership-list refusal
and named-member removal, missing baseline nodes, and tech-level table changes. It is not the full check, which runs nightly or on
demand.

### Nightly

`nightly_soak.py install-cron --tag-check docs/tag-coverage/statuses.json` writes the enable
option into the managed cron command; pass it again when reinstalling. For an on-demand run,
`nightly_soak.py run --tag-check docs/tag-coverage/statuses.json` (or the process environment
`EAWR_SOAK_TAG_REGISTRY`) runs the check after the soak on the same host and build, at nice 19 with
`--tag-jobs` (6) rows at a time and a one-hour budget; the report is
`~/eawr-soak/reports/tag-check-latest.md`, and the findings are filed as above (or drafted
without the token). Timeout kills the wrapper and driver as one process group, and the driver
is a build target only when enabled. It is off until the owner turns it on with the cron
(docs/nightly-soak.md).

## Proof on EAWR-666

On the integration head before EAWR-718, a registry that says `Layer_Z_Adjust` is applied for craft
and ships reports ships as `no change` ("in the unit tables, the battle never differs"): a
finding. On EAWR-718's head the same row reports ships as `changes` from tick 1 (the ships spawn at
their height). The craft change the battle on both heads.

## Limits

- Only what the headless M2 battle builds is checked: the pinned M2 unit types, their squadrons,
  craft, hardpoints and projectiles, the start's factions and `gameconstants.xml`. Other
  documents (the targeting priority sets, the enums, the AI's XML) are not overridden yet.
- A merge-mode list tag inherited from a base keeps the base's entries; the override replaces the
  object's own.
- Repeated containers resolve and override only the last occurrence; occurrence-specific
  perturbations are not supported.
- Attributes (`@Name`) are not changed.
- Presentation areas and registry targets under `apps/viewer/`, `src/presentation/`, or
  `src/scene/` are `not checkable` until a corresponding headless report exists.
- AI `GameConstants/AI_*` values use raw XML bytes and are `not checkable`; the document
  overlay cannot reach the AI setup. This is a limit of the probe, not a missing application.
