# XML tag registry

Browse the [human-readable coverage report](tag-coverage/report/README.md) for status totals
and tables by area and object class family. It is generated only from the registry below.

XML tag coverage (legacy EAWR-628). FoC is data-driven: very little is hardcoded, and
a tag the remake does not apply is a behaviour it cannot have. Owner rule (2026-09-30): every XML definition has a target
in the code, or it is known to have none. The registry (`docs/tag-coverage/statuses.json`) gives every (object class, tag
path) of the FoC XML exactly one status, and a gate fails when one has none.

It replaces the loader tag-trace table, which listed only the M2-scene tags nobody reads: a tag read once for any object type dropped
out as covered. That is how `Layer_Z_Adjust` slipped (per-unit flight height: craft applied it, ships did not) and how
`Targeting_Stickiness_Time_Threshold` and `Special_Ability_Name` counted as consumed while nothing applied them.

## The universe

`plan/inventories/xml-tags.json`, FoC profile: every element without an element below it and every attribute of every
effective FoC XML file. `tools/inventory/tag_registry.py` reads it and

- keys each tag by **(class, tag path)**: the class is the XML element the object is written as (`SpaceUnit`, `HardPoint`,
  `Goals`, `GameConstants`), the tag path what is below it (`Max_Speed`, `Abilities/Stun_Ability/Stun_Range`,
  `@Name`), ASCII case folded as FoC matches names;
- collapses the segments that are data, not tags, to `*`: goal, function-set and AI-template names, dialog control names
  (`GUIDialogs/Textures/*/Frame_Left`), planet, terrain and hint names, equation, enum and movement-class names. The FoC profile has 14,783 inventory rows
  (of 53,564 across all profiles), which become 8,202 stock-authored pairs.

The game-data tier checks the inventory itself: every element and attribute name in the effective FoC XML must be in the
inventory, so a stale inventory cannot hide a new tag from the gate. The check is name-level: a name the inventory already
has, authored on a class it was not authored on before (a mod putting `Max_Speed` on a new object type), is not seen until the
inventory is regenerated.

## The registry

### Engine names absent from stock authoring

[`engine-tags.json`](tag-coverage/engine-tags.json) is a separate catalog of debug-build
XML table entries, with descriptive table kinds, value types and opaque evidence IDs.
It also carries reviewed nested ability dispatch names. The sound-event seed names
use their full `SFXEvent_` XML spelling, not the abbreviated event labels. A name may
have multiple rows when several tables
read it with different owners or value types. It contains no binary locations.

The full headless `engine-tags` query reads typed table symbols, stops at the next
table boundary or null key, and dumps the parser-type enums. Raw output stays in
ignored research folders. `tools/inventory/engine_tag_catalog.py` generates the public
catalog from that dump and a private review mapping of owners/types to descriptive
names and evidence IDs; missing translations fail generation.

`xml_inventory.py` reports an element as `known-engine` only when the pinned
schema has no row for its (object type, tag) pair and the catalog lists that tag
under a table kind belonging to that object type. Table kinds match canonical
schema type names without hyphens, with explicit owner aliases in `engine_tags.py`
(for example, `object-definition` belongs to `GameObjectType`). File-registry
hints supply canonical types for concrete unit definitions. A tag owned only by
another object type, or an owner without a matching canonical type, remains
`unknown` and unresolved. Schema matches and deprecation retain precedence.
FoC and the FoC-compatible Remake profile use this evidence; attributes and the
EaW profile do not. Table-scoped recognition does not establish acceptance in a
particular nested context or application by the remake. ML-28's context and
warning distinctions still apply. Misspellings remain `unknown`.
The inventory manifest fingerprints the catalog and classifier source.

The registry covers those 8,202 stock-authored pairs plus six reviewed `engine_default` pairs,
8,208 in all: `SpaceUnit`, `SpaceBuildable` and `SpecialStructure` each have
`Turret_XY_Only` and `Turret_Rotate_To_Rest_Delay_Sec` defaults. Other catalog entries
do not add rows automatically. Recognition alone never implies `applied` or `foc-ignores`.

Reviewed mod-only pairs can name `inventory_profile` (for example, `remake`) on
their registry row. The gate admits only that row's exact class/tag pairs from
the named committed inventory profile; a missing profile, class or tag fails.
Other mod tags do not enter the stock coverage requirement automatically. These
rows still need ordinary status evidence and consumers; profile recognition does
not establish application. The galactic shard additionally registers
`Campaign/AI_Victory_Planet_Names`, `Campaign/Story_Name` and
`Planet/Idle_Anim_00_Rate_Mod` this way.

`docs/tag-coverage/statuses.json` holds the schema version, shared evidence and shard list, with no rows.
Its `statuses-<area>.json` shards use `schema_version` 2, one row per line, sorted by tag then first class.
A row covers every class it lists:

```json
{"applied": [{"code": "src/units/unit_motion.cpp#max_thrust", "rules": ["ESU-16"], "types": ["craft"]}],
 "area": "combat", "basis": "auto", "classes": ["SpaceUnit", "UniqueUnit"], "missing_types": ["ship"],
 "note": "the sim applies it to craft only", "status": "partial", "tag": "Max_Thrust", "ticket": 843}
```

| Status | Meaning | The row must have |
|---|---|---|
| `applied` | Our code applies the value: it reaches the simulation or the presentation, not only the loader's table. | `applied`: the code (`path#identifier`, checked to exist), the rule IDs (checked to exist in `docs/behaviour`), `types` where the sim splits by kind |
| `partial` | Applied for some types FoC applies it to, not others (the `Layer_Z_Adjust` gap: craft applied it, ships did not; fixed by per-unit placement). | as `applied`, `types` naming where it applies, `missing_types`, and a ticket |
| `todo` | FoC applies it and we do not. | a ticket: the tracking issue of the mechanic or subsystem (movement, combat, fighter, AI, presentation and economy tag coverage, a `[Walk]` or logic-review issue), one issue per missing mechanic, not per tag |
| `foc-ignores` | FoC's parsers do not read it. | evidence citing the debug build (an id of the table at the top of the file) |
| `land-or-galactic`, `multiplayer`, `presentation-later`, `deferred` | Out of scope for now. | a ticket, or the reason as evidence |
| `by-design` | Our implementation intentionally replaces this mechanism. | evidence citing the rule ID that says so (checked to exist in `docs/behaviour`) |

A tag we parse but do not apply is `todo` or `partial`, never covered. `area` is the subsystem. `types` are the unit kinds of
`include/eawr/units/unit_tables.hpp` (`station`, `ship`, `squadron`, `craft`) where the sim splits by kind, otherwise
empty (the whole class). `basis` is `reviewed` (someone read the code path) or `auto` (see the seed below).
Older rows may omit `basis`; omission makes no claim of manual verification. A reviewed
applied row names its rule IDs or a note saying no rule mentions the tag. `trace: unrecorded` marks a loader that reads under
an unrecorded trace (see below).

### What each status rests on

An `applied` or `partial` row may also contain `no_targets`, an array of explicit per-kind exclusions:
`{"types": ["ship"], "status": "foc-ignores", "evidence": "rule or debug build evidence"}`.
The kinds are `station`, `ship`, `squadron` or `craft`; each exclusion cites the debug build,
directly or through the registry's evidence table. Excluded kinds cannot overlap an applied
consumer, a missing kind or another exclusion. The row remains `applied` when every relevant kind has either
a consumer or a sourced exclusion. A remaining implementation gap still uses `partial` and
`missing_types`; authoring a value on a kind alone does not establish that FoC consumes it.
The public report shows each excluded kind, its status and its resolved evidence.
An exclusion may cover ordinary flight while a distinct consumer remains missing on that kind;
its evidence states the flight scope and `missing_types` names the separate consumer explicitly.
For example, ignored ship thrust in ordinary flight does not cover optional attack positioning.

- **`foc-ignores`** normally rests on a whole-image scan of the debug build for the tag name (`Invoke-GhidraQuery.ps1 -Query tag-tables`,
  the `DB-NOTAG` evidence): the name is in no tag table row, string or format template. about 250 pairs (`python tools/inventory/tag_registry.py render` prints the count; mostly typos and stale
  authoring: `Autoresolve_Health`, `Facing_Adjust`, `Sensor_Range`). The scan does not assign tables to classes, so
  "FoC reads this tag for this class" means "the debug build knows the name and the data authors it on this class". A
  per-class refinement (which behaviour tables an object type pulls in) is not done by that scan.
  Reviewed exceptions cite a behaviour rule: DG-24 for craft `Fire_Inaccuracy_Distance`, and AB-15 for
  squadron-authored `Mod_Multiplier` (non-team commands use the craft's data). These tags may be known to the
  parser while their values on that class do not drive the relevant behaviour.
- **`land-or-galactic`, `presentation-later`** by class or tag-name family, with a scope reason (ground classes, planets,
  campaigns and story, cinematics, graphics settings, menus). These are scope decisions (M2 is the space skirmish), not
  claims about FoC.
- A tag name's scope word (`Hyperspace`, `Land_`, `Piracy`, `Galactic`) labels `land-or-galactic` only for classes that are not
  fought in a tactical battle. On the classes of `TACTICAL_CLASSES` (`tools/inventory/tag_registry.py`: `SpaceUnit`, `UniqueUnit`,
  `TransportUnit`, `Container`, `Squadron`, `StarBase`, `SpaceBuildable`, `SpaceStructure`,
  `SpecialStructure`, `SecondaryStructure`, `HeroUnit`, `GenericHeroUnit`, `Mobile_Defense_Unit`) such a tag stays `todo`
  until someone has read what a tactical battle does with it (legacy EAWR-4002), as for the `Hyperspace`
  arrival (legacy EAWR-3956); only pure ground words (`Garrison`, `Ground`, `Infantry`) still scope those classes out. Fields only the galactic map reads (`GalacticBehavior`, `Piracy_Value_Credits`,
  `Galactic_Fleet_Override_*`, `Disallows_Hyperspace_Retreat`) stay `land-or-galactic` on every class.
- A constant that FoC reads but whose consumer our code replaces on purpose (the path finder's grid and look-ahead constants) is
  `by-design` with the rule that says so (PC-10 for the grid cull sizes); a tag whose retail effect is on a model we do not draw
  (`Select_Box_Z_Adjust`, WSU-66) is `by-design` with that rule as evidence. A constant no note covers yet stays `todo`.
- An `applied` or `partial` row of a tactical class (`TACTICAL_CLASSES`) must cite code outside `src/campaign/auto_resolve_*` and
  `include/eawr/campaign/auto_resolve_*`: galactic auto-resolve applying a value does not apply it in the battle (the 2026-10-16
  tag audit) (legacy EAWR-4002). The gate does not check that the other location is more than a loader's table; reviewing that is part of
  `basis: reviewed`.
- **`applied`, `partial`, `todo` for a tag we parse** come from `tools/inventory/tag_apply_scan.py`: it finds where each
  loader parses a tag literal, the field it stores the value in, and where else the code uses that field. A use outside the
  loader that is not the identity hash or a test counts as applying it. `partial` rows come from the per-kind split of
  `src/units/unit_motion.cpp` (ship profiles and craft profiles fill different tags) against the kinds that author the tag
  in the FoC data. Unreviewed rows state that FoC's per-type read is unverified. Reviewed motion rows cite
  WMV-40..WMV-44: ordinary-flight exclusions are explicit, while optional attack positioning and formation
  upkeep retain their distinct implementation gaps.
- The M2 load trace (below) corrects the scan for classes in the scene: a tag the traced loaders read for other classes
  but never for this class' scene objects is `todo`, not applied.

The scan is a text scan. Rows it produced are `basis: auto`; they are the rows the perturbation check
(`tools/inventory/tag_applied_check.py`, tools/soak) proves or refutes mechanically. Hand decisions the scan cannot make are in
the `OVERRIDES` table of `tag_registry_build.py`.

The tag perturbation reassessment of AI, combat, economy and movement application fixes distinguishes an unchanged battle hash from a missing application. Reviewed
rows cite `CHECK-842` and retain `applied` when the consumer exists but the 9000-tick M2 scenario does not prove
its effect. A missing baseline XML node means not authored, not a failed read. `SpaceUnit/Damage_Type` applies
to an object's own weapon (WWP-48), not its ship hardpoints; `SpaceUnit/Space_Layer` lists authored ship layers,
while craft preserve the debug build's default of no layer (AV-21). Neither default creates a missing mechanic.
`SpaceUnit/Victory_Relevant` is deferred with combat tag coverage (legacy EAWR-650) for other victory conditions: the M2 default uses only stations
(VT-01 to VT-03). The station row remains applied even though the perturbation run did not change its result.

## Registry shards

`docs/tag-coverage/statuses.json` lists all eight shards (`ai`, `combat`, `data`, `economy`,
`fighters`, `galactic`, `movement`, `presentation`) in its `shards` array. Shard names
are relative to the containing file. Registry readers combine the rows and evidence before checking coverage,
so every (class, tag) pair still has exactly one status across all files. Conflicting evidence definitions,
duplicate pairs, missing shards and repeated shard references fail the gate.

The galactic shard owns Campaign, Planet, TradeRoute and TradeRouteLine pairs, the Particle
`Galactic_Model_Name` pair, and the GameConstants and Faction pairs already identified by
`SCOPE-GALACTIC`. It also retains 55 presentation-area rows; area and file ownership differ there. This split preserves every status, target, ticket and evidence value;
it does not claim that a deferred mechanic is applied. Other rows live in the shard named by their `area`:
`movement`, `combat`, `fighters`, `ai`, `presentation`, `economy` or `data`.
When applying a galactic tag, edit its existing row in `statuses-galactic.json`.

`write_registry` routes new pairs and rows whose area changes to the listed area shard; existing galactic
pairs retain their ownership. New evidence definitions go in the root header, and existing definitions stay
where they are. A no-op write preserves the files. `relocate --map` uses the same row routing and evidence
ownership. Missing area references, rows in the root, or an area row in the wrong shard fail the gate.

`check`, `check-data`, `render` and `format` follow the shard list by default. Each file retains its own
canonical layout, and `format` sorts each shard without changing ownership. For independent registry files,
repeat `--statuses path` to supply a list of roots. Report and census readers use the same combined view.

## The gate

- **Public report** (`docs/tag-coverage/report/`, generated, no game data): not a per-PR gate. Many PRs edit
  `statuses.json` at once, so the generated pages are refreshed in the batch on main:
  `python tools/inventory/tag_report.py --batch`, then `python tools/inventory/tag_report.py --check`.
  The per-PR form is `python tools/inventory/tag_report.py --pr --base origin/main`: it loads `statuses.json`
  to validate its format and renderability, and rejects changed report pages that differ from regeneration;
  a stale, untouched `report/` passes. This command does not check missing (class, tag) pairs or application
  coverage: keep `python tools/inventory/tag_registry.py check` as the per-PR coverage gate.
  The bare `python tools/inventory/tag_report.py` command is the older regeneration form, equivalent to
  writing the pages without the batch command's convergence check.

- **`ci_tag_registry`** (label `ci-tools`, runs everywhere, no game data): every pair of the universe has a row, no row is
  stale, no pair has two rows, each status has what its table row above requires, every `applied` code location exists
  (the file, and the identifier in it), every rule ID is in a `docs/behaviour` note, rows are sorted and
  each shard is in its canonical layout and has its rows in the correct area. A defective row is reported by name with what it lacks; the gate does not
  stop at the first. `inventory_contracts` runs the same checks on synthetic fixtures.
- **`ci_xml_literal_gate`** (label `ci-tools`, no game data): `tools/inventory/xml_literal_check.py` fails when code under `src/`,
  `include/` or `apps/` names a registry tag (in a string, a trailing comment, or the comment block directly above a
  declaration) next to a numeric literal. Read the value from the XML; a default that stands in for a missing tag carries
  `// xml-fallback: <Tag>` next to a load that reports the missing tag (the gate accepts the marker only when <Tag> is a
  registry tag and the code within 20 lines names it as a string next to a log/warn/report/error call).
  For defaults declared in headers, append `; load: src/path.cpp#function` to the marker. The referenced definition must
  use the exact tag as a load key and connect its missing branch to a diagnostic call, directly or through a same-file
  helper. The heuristic follows local lambda parameters and range-loop tag tables; command-bar field keys resolve
  through the ordered XML schema. Missing paths/functions and unsupported or unconnected load patterns fail.
  This checks supported source patterns rather than proving runtime control flow; loader contracts verify
  that missing tags trigger the diagnostic and stock data triggers none. Optional per-object tags with sourced FoC
  defaults stay silent and retain reasoned allowlist entries (legacy EAWR-2540). Existing findings sit in
  `config/xml-literal-allowlist.json`, each with its issue number (or `false-positive` and a reason); the list may only
  shrink: an entry whose finding is gone fails the gate until it is deleted, and more than `max_entries` fails too. The gate compares the list with the one at the merge base with `origin/main`: a
  new entry fails unless the list carries a reviewed `growth_override` (`ticket` + `reason`); an unused override fails.
  Known limits: a tag on a continuation line of a multi-line declaration is missed, and an identical line elsewhere in the
  same file is covered by the same entry (the key is file + line text).
  `ci_xml_literal_check` tests the checker.
- **`tag_coverage_m2_scene`** (game data, `EAWR_EAW_GAME_ROOT`): runs the M2 start with the trace on and off, joins the trace
  with the registry, and fails when a scene tag has no row, when an `applied` row citing traced code (`src/units`,
  `src/skirmish`, `src/sim`, `src/data`) is for a tag the scene never reads, or when the trace does not join. It then runs
  `tag_registry.py check-data`.

Applied targets may declare `trace_scope` with `object_types` (resolved object IDs from the trace),
`ability_types` (the enclosing `Unit_Ability/Type` names), or
`exclude_behaviors` (tokens from the object's `Behavior`), or
`exclude_space_behaviors` (tokens from the object's `SpaceBehavior`). These explicit XML selectors refine the scene
read requirement; the descriptive `types` text is not parsed as a selector. A scoped target is checked separately
for each represented object and ability handler, so a read on another handler or object cannot hide a missing read.
An absent consumer has no read requirement. Unscoped targets retain the class/tag check. For ordinary
`SpecialStructure` body fields, `SPECIAL_WEAPON` sources are excluded under WSW-01/14/21; ordinary orbital bodies
retain their coverage and their read requirement. The `CORRUPT_SYSTEMS` decal distance uses an ability selector
under WHE-33, preserving its application without requiring another area's handler to read it.
The WST-03 upgrade stacking category is scoped to the cloak-duration upgrade types; unrelated battlefield
modifiers retain their partial coverage without an irrelevant cloak-duration read requirement.
For `MiscObject` affiliation, behavior and health fields, `MULTIPLAYER_BEACON` is excluded by its
`Behavior` token under HCB-12a..12f: setup resolves only the authored beacon type and lifetime.
Other MiscObjects, including mod objects without that behavior, retain their read requirement.

The gate does not run the code. It cannot see a PR that starts applying a tag whose row still says `todo` (the row goes
stale until a reviewer or the perturbation check notices), and it cannot tell whether an `applied` location is the
right one: the identifier check matches whole words outside C/C++ comments, with a minimum length of six and a
stoplist for generic names. Cite a specific consumer or its enclosing function. Missing identifiers report other
source files containing the same whole word. Rule IDs must be declared in a behaviour-note heading or the first
cell of a rule table, a list item or a bold paragraph; only the leading rule ID counts. Unresolved legacy citations carry a
`TODO #2566:` note, printed by the gate for follow-up without guessing a consumer or rule.
The gate caps these notes at 30 rows and requires a tracking ticket on each TODO note; lower the ceiling as gaps close.

### Changing a row

A PR that starts applying a tag (or stops):

1. Change the tag's row in `statuses-<area>.json` (or its existing galactic shard): `status`, `applied` (the code that applies it and the rule IDs of your
   behaviour note), drop `ticket` and `note` that no longer hold. To split a row (a tag now applied for one class only),
   move the class into a new row.
   For bulk edits or an area change, load the merged registry with `load_registry` and write it with
   `write_registry` so the rows are routed automatically; do not add rows to the root.
2. `python tools/inventory/tag_registry.py format`.
3. `python tools/inventory/tag_registry.py check`. It names every missing row, stale row, defective row and dead code
   location.
4. Keep `todo` rows honest: a mechanic you find missing gets one issue and its rows link it.

### Moving code

A PR that moves or renames code a row cites (`applied[].code` is `path#identifier`) or a census region
(`data/census/mechanic-review.json`) relocates the anchors with the move, not by hand:

1. Write `moves.json`, a list of `{"old": "path#symbol", "new": "path#symbol"}`. An entry with no `#` on either side
   renames a whole file: every anchor in `old` keeps its symbol and takes the new path. A symbol entry wins over a
   file entry for the same anchor. Entries chain (A to B, then B to C, ends at C); a cycle is an error.
2. `python tools/inventory/tag_registry.py relocate --map moves.json` rewrites every registry shard and the census
   regions in one step (only the files that change are written, in their canonical layout and line ending, and
   region order is kept). It fails with exit 2, writing nothing, when an `old` matches no anchor, a moved registry anchor's final file (after chains) is
   missing or does not mention its symbol (census keys are logical names, checked by region text instead), or a move leaves two applied entries of a row on one anchor; the files
   are written all or none. `--dry-run` runs every check and counts what would move without writing;
   `--allow-unmatched` only reports a move that matches no anchor; `--census PATH` picks another review file.
   `data/census/space-units.json` is not rewritten: its anchors are constants in `tools/inventory/unit_census.py`, so
   update them there and regenerate with `unit_census.py --output`.
3. `python tools/inventory/tag_registry.py check`. A region's `start`, `end` and `sha256_lf` are not touched: text that
   moved verbatim still verifies, and text that changed stops the census until it is re-reviewed.

**Open PRs.** A PR that applies a tag updates that tag's row in the same PR; the gate will not tell you if you forget.
Rows are one per line and sorted, so two PRs conflict only when they edit the same tag. After
changing the registry, leave `docs/tag-coverage/report/` alone: the batch on main regenerates it. Whoever merges
second re-runs `format` and `check`. The older `tag_registry.py render` remains an optional ignored summary.

A new FoC tag (a regenerated inventory) needs a row: add it to the registry, `todo` with its subsystem ticket if nothing
reads it.

## The consumed set: a load-time trace

`include/eawr/data/tag_trace.hpp`. The loaders call a hook where they take a value from a node:

| Hook | Where |
|---|---|
| `used(node)` | `EffectiveObject::value` (every catalog value a loader asks for, found), the variant link in `Catalog::resolve`, the units loader's list tags, `Unit_Abilities_Data` fields, constants, enums and priority sets, the skirmish start's faction flags, forces, fog and lobby-colour constants, and the fog, battle-message and minimap presentation loaders |
| `used_attribute(node, name)` | an attribute a loader uses (priority set and minimap names) |
| `object(root, id)` | `Catalog::resolve`: every definition of the resolved object's variant chain; the skirmish start's faction definitions; each priority set the units loader takes |
| `document(root)` | a loader that uses a whole file: gameconstants.xml (units, skirmish start), the category and property enums, the priority set registry |

- **Off costs one atomic load.** Nothing is recorded unless a `tag_trace::Recording` is live.
- **It does not change what is loaded.** A hook only copies the node's name and source location into the trace.
- **An index is not a use.** The map loader's type table (and the obstacle footprint loader) runs under
  `tag_trace::Unrecorded`; a registry row for a tag only they read carries `trace: unrecorded`.
- **A read is a use of the tag, not an application.** A tag can be consumed and still `todo` (read, applied nowhere).
- `sim_headless --skirmish m2 --game-root <install> --tag-trace-out <file.json>` writes the trace of the M2 start.

**Not traced.** The viewer's own loaders and the tactical AI's XML reader keep no source locations, so the scene check does
not judge rows that cite viewer, presentation or script code. The M2 start trace also ends before AI service initialization;
adding locations to that reader alone would not include its loads in this command. The
[fighter and AI coverage audit](behaviour/tag-coverage-fighters-ai.md) records the existing AI constant consumers and
the source of each scope or deferral decision. It is a static consumer audit, not a runtime trace receipt.

## Running it

**Campaign-only scene consumers.** An applied row may declare `scene: "campaign"`
when every listed consumer belongs to campaign scenes and the standalone M2
skirmish start cannot observe its loader. This requires explicit `basis: "reviewed"`,
a note explaining the scene boundary, and behaviour rule IDs on every applied
target. The registry still checks each code location and rule declaration. Review
must establish that those rules source the campaign boundary; the annotation is
not inferred from a tag name or a code directory. Invalid annotations fail the
registry gate and do not exempt an applied/partial target from the M2 read check.

The six `Default_Bounty_By_Category_SP/MP` and SpaceUnit/Squadron
`Override_Bounty_SP/MP` pairs use this exception under WGCH-65/WGP-189: both
loaders require a campaign scene, and campaign battle economy resolves the current
victim type's bounty. Their real application remains `applied`. The exception
affects only the M2 skirmish trace requirement, not galactic scene coverage,
inventory coverage or source/rule validation. `trace: "unrecorded"` remains reserved
for loaders running under `tag_trace::Unrecorded`.

An applied row may declare `engine_default: "GameObjectType"` for a recognized
object-definition tag absent from the effective FoC XML. The gate checks the tag
and its evidence ID against `docs/tag-coverage/engine-tags.json`, and still checks
the consumer's code and rule references. Its note states the engine default when
absent; engine recognition alone does not establish an applied consumer.

```sh
python tools/inventory/tag_registry.py check                 # the gate, no game data
python tools/inventory/tag_registry.py check-data --game-root <install>
python tools/inventory/tag_registry.py format                # canonical layout of statuses.json
python tools/inventory/tag_registry.py render                # out/tag-coverage/statuses.md: counts, partial rows, top todo
python tools/inventory/tag_report.py --batch                 # batch job: regenerate the committed public report
python tools/inventory/tag_report.py --check                 # batch/release freshness gate (not per PR)
python tools/inventory/tag_report.py --pr --base origin/main # per-PR gate: report/ untouched or exact regen
python tools/inventory/tag_registry_reclassify_ground.py --game-xml <effective Data/XML> [--write]  # todo rows only ground/galactic objects carry -> land-or-galactic
sim_headless --skirmish m2 --game-root <install> --tag-trace-out <abs>/out/tag-coverage/m2-tag-trace.json
python tools/inventory/tag_coverage.py scene --game-root <install> --trace <abs>/out/tag-coverage/m2-tag-trace.json \
    --statuses docs/tag-coverage/statuses.json --out <abs>/out/tag-coverage
```

### Regenerating the seed

Only for a bulk rebuild; edit existing shard rows for ordinary changes and persist hand decisions
in `tag_registry_build.py` OVERRIDES. The optional `--previous` flag still exists; it accepts
the legacy schema-1 seed, not the current sharded v2 registry. The inputs live in the ignored `out/`:

```sh
python tools/inventory/tag_apply_scan.py --out out/tag-coverage/apply-scan.json
python tools/inventory/tag_registry_build.py --game-root <install> --scan out/tag-coverage/apply-scan.json \
    --scene out/tag-coverage/scene.json --tag-tables out/research/tag-tables/all/tag-tables \
    --previous <old schema-1 statuses.json> --out docs/tag-coverage/statuses.json
```

The tag-tables output (addresses, table rows) is derived from the game binary and never committed; the registry holds tag
names, our code locations, rule IDs and evidence ids only.

### Reviewing applied claims

`python tools/inventory/tag_applied_verify.py --out out/tag-coverage/applied-verification.json`
checks every applied row for its tag leaf literal and anchor in each cited file, and selects
100 rows with random seed 1021 for manual consumer review across the registry. Textual misses
are listed, not downgraded: renamed fields, generated keys and downstream consumers commonly
make literal checks miss real application. Review the sampled classes through to a sim or
presentation consumer; a successful textual check alone is not that proof. Current status and
area counts are generated in the [report overview](tag-coverage/report/README.md).
