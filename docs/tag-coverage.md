# XML tag registry

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
  (`GUIDialogs/Textures/*/Frame_Left`), planet, terrain and hint names, equation, enum and movement-class names. The 14,783 inventory rows
  become 8,202 pairs.

The game-data tier checks the inventory itself: every element and attribute name in the effective FoC XML must be in the
inventory, so a stale inventory cannot hide a new tag from the gate. The check is name-level: a name the inventory already
has, authored on a class it was not authored on before (a mod putting `Max_Speed` on a new object type), is not seen until the
inventory is regenerated.

## The registry

`docs/tag-coverage/statuses.json`, `schema_version` 2, one row per line, sorted by tag then first class. A row covers every
class it lists:

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

A tag we parse but do not apply is `todo` or `partial`, never covered. `area` is the subsystem. `types` are the unit kinds of
`include/eawr/units/unit_tables.hpp` (`station`, `ship`, `squadron`, `craft`) where the sim splits by kind, otherwise
empty (the whole class). `basis` is `reviewed` (someone read the code path) or `auto` (see the seed below); a reviewed
applied row names its rule IDs or a note saying no rule mentions the tag. `trace: unrecorded` marks a loader that reads under
an unrecorded trace (see below).

### What each status rests on

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
- **`applied`, `partial`, `todo` for a tag we parse** come from `tools/inventory/tag_apply_scan.py`: it finds where each
  loader parses a tag literal, the field it stores the value in, and where else the code uses that field. A use outside the
  loader that is not the identity hash or a test counts as applying it. `partial` rows come from the per-kind split of
  `src/units/unit_motion.cpp` (ship profiles and craft profiles fill different tags) against the kinds that author the tag
  in the FoC data; FoC's own per-type read is not verified in the debug build for those, which each note says.
- The M2 load trace (below) corrects the scan for classes in the scene: a tag the traced loaders read for other classes
  but never for this class' scene objects is `todo`, not applied.

The scan is a text scan. Rows it produced are `basis: auto`; they are the rows the perturbation check
(`out/specs/tag-applied-check.md`, tools/soak) proves or refutes mechanically. Hand decisions the scan cannot make are in
the `OVERRIDES` table of `tag_registry_build.py`.

The tag perturbation reassessment of AI, combat, economy and movement application fixes distinguishes an unchanged battle hash from a missing application. Reviewed
rows cite `CHECK-842` and retain `applied` when the consumer exists but the 9000-tick M2 scenario does not prove
its effect. A missing baseline XML node means not authored, not a failed read. `SpaceUnit/Damage_Type` applies
to an object's own weapon (WWP-48), not its ship hardpoints; `SpaceUnit/Space_Layer` lists authored ship layers,
while craft preserve the debug build's default of no layer (AV-21). Neither default creates a missing mechanic.
`SpaceUnit/Victory_Relevant` is deferred with combat tag coverage (legacy EAWR-650) for other victory conditions: the M2 default uses only stations
(VT-01 to VT-03). The station row remains applied even though the perturbation run did not change its result.

## The gate

- **`ci_tag_registry`** (label `ci-tools`, runs everywhere, no game data): every pair of the universe has a row, no row is
  stale, no pair has two rows, each status has what its table row above requires, every `applied` code location exists
  (the file, and the identifier in it), every rule ID is in a `docs/behaviour` note, rows are sorted and
  `statuses.json` is in its canonical layout. A defective row is reported by name with what it lacks; the gate does not
  stop at the first. `inventory_contracts` runs the same checks on synthetic fixtures.
- **`tag_coverage_m2_scene`** (game data, `EAWR_EAW_GAME_ROOT`): runs the M2 start with the trace on and off, joins the trace
  with the registry, and fails when a scene tag has no row, when an `applied` row citing traced code (`src/units`,
  `src/skirmish`, `src/sim`, `src/data`) is for a tag the scene never reads, or when the trace does not join. It then runs
  `tag_registry.py check-data`.

The gate does not run the code. It cannot see a PR that starts applying a tag whose row still says `todo` (the row goes
stale until a reviewer or the perturbation check notices), and it cannot tell whether an `applied` location is the
right one: the identifier check is a substring test in the file; the registry code check needs a more robust match (legacy EAWR-855).

### Changing a row

A PR that starts applying a tag (or stops):

1. Change the tag's row in `statuses.json`: `status`, `applied` (the code that applies it and the rule IDs of your
   behaviour note), drop `ticket` and `note` that no longer hold. To split a row (a tag now applied for one class only),
   move the class into a new row.
2. `python tools/inventory/tag_registry.py format`.
3. `python tools/inventory/tag_registry.py check`. It names every missing row, stale row, defective row and dead code
   location.
4. Keep `todo` rows honest: a mechanic you find missing gets one issue and its rows link it.

**Open PRs.** A PR that applies a tag updates that tag's row in the same PR; the gate will not tell you if you forget.
Rows are one per line and sorted, so two PRs conflict only when they edit the same tag. The counts and tables are not
committed: `render` writes them on demand to `out/tag-coverage/statuses.md` (ignored), so nothing but the rows
themselves can turn the integration head red after two clean merges. Whoever merges second re-runs `format` and `check`.

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
not judge rows that cite viewer, presentation or script code.

## Running it

```sh
python tools/inventory/tag_registry.py check                 # the gate, no game data
python tools/inventory/tag_registry.py check-data --game-root <install>
python tools/inventory/tag_registry.py format                # canonical layout of statuses.json
python tools/inventory/tag_registry.py render                # out/tag-coverage/statuses.md: counts, partial rows, top todo
sim_headless --skirmish m2 --game-root <install> --tag-trace-out <abs>/out/tag-coverage/m2-tag-trace.json
python tools/inventory/tag_coverage.py scene --game-root <install> --trace <abs>/out/tag-coverage/m2-tag-trace.json \
    --statuses docs/tag-coverage/statuses.json --out <abs>/out/tag-coverage
```

### Regenerating the seed

Only for a bulk rebuild; the registry is hand-edited otherwise. The inputs live in the ignored `out/`:

```sh
python tools/inventory/tag_apply_scan.py --out out/tag-coverage/apply-scan.json
python tools/inventory/tag_registry_build.py --game-root <install> --scan out/tag-coverage/apply-scan.json \
    --scene out/tag-coverage/scene.json --tag-tables out/research/tag-tables/all/tag-tables \
    --previous <schema 1 table> --out docs/tag-coverage/statuses.json
```

The tag-tables output (addresses, table rows) is derived from the game binary and never committed; the registry holds tag
names, our code locations, rule IDs and evidence ids only.
