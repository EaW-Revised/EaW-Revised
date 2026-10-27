# Mod chain resolution

`eawr/mods/chain.hpp` is the engine-free API. `discover()` enriches each installed
record with metadata and top-level script hints. `resolve_chain()` takes the leaf
key, records, known chains and optional player edges; it performs no file reads.
Vanilla FoC/EaW roots are mounted separately by the engine, outside this mod list.

## Inputs

The reader was written from the public
[eaw.modinfo format description](https://github.com/AlamoEngine-Tools/eaw.modinfo),
sections II, III.1, III.2 and IV.1; no reference implementation was read or copied.
It reads `name`, optional string `version`, and ordered `dependencies` with
`modtype` 0 (Mods-folder path) or 1 (Workshop id). Comments and trailing commas
are accepted for this format only. Unsupported dependency types or layouts are
malformed metadata, reported as `EAWR-MODS-0011`; discovery preserves a fallback
record and can use lower-confidence sources. Version text is preserved rather
than interpreted as an install constraint.

The default layout is `ResolveRecursive`. `ResolveLastItem` expands only the last
listed parent; `FullResolved` preserves the listed parents without expanding
them. Recursive dependencies produce a stable topological order, preserving the
declared sibling order and choosing earlier discovered nodes first. Contradictory
order constraints fail with the cycle diagnostic.

Each valid `<name>-modinfo.json` adds a record with key
`<base key>#variant:<filename>`. A main file supplies omitted properties; variant
dependencies replace the whole dependency list. Each variant must supply its own
name. With only variant files, no extra base entry is produced. Malformed variants
are omitted with a warning; at least a fallback entry survives. Name-qualified
dependency references such as `100:Variant Name` resolve to the variant's stable
record key. Mods-folder paths accept slash or backslash separators and a leading
`./Mods/`. Absolute paths can match an already discovered record; they do not
cause the resolver to discover or open another folder.

The EAWR optional `role` extension accepts `parent-only`, `playable` or `unknown`.
Parent-only layers can supply assets as parents, but cannot be selected as the
playable leaf. Every mounted layer must have a Data folder.

The launcher supplies reviewed known chains in this strict JSON schema:

```json
{
  "format": "eawr-known-chains",
  "version": 1,
  "chains": [
    {"id": "100", "name": "Synthetic leaf", "parents": ["200", "300"]},
    {"id": "200", "name": "Synthetic library", "parents": [], "role": "parent-only"}
  ]
}
```

Ids are decimal Workshop ids; parents are an already ordered, complete parent
list. Optional `role` has the same values as above. Unknown fields are ignored.
Duplicate ids, invalid types, malformed JSON or an unsupported format/version
reject the whole input with `EAWR-MODS-0012`. `apply_known_chains()` fills display
names only when modinfo is absent and roles only when unknown.

Only `STEAMMOD=` and `MODPATH=` tokens in a mod's own top-level `.bat`/`.cmd`
files supply hints. Quoted paths can contain spaces. No script executes; variables
and command substitution are never expanded. These hints may include comments
or inactive branches, so they refuse launch until the player supplies confirmed
edges. A confirmed empty edge list explicitly disables fallback.

## Resolution and diagnostics

For each node the first present source wins: player, modinfo, known-chain,
launch-script. Sources replace the complete parent list; they are never unioned.
An explicit empty dependency list therefore also disables fallback. Rows carry
the canonical record key, state and the source that first introduced the layer
(`leaf` for the selection). A repeated parent within one child's list retains one
layer and warns. Shared parents across different children are normal and do not
warn. Cycles, missing
layers, downloading layers and layers without Data refuse launch. A missing or
downloading diagnostic includes whether the record is installed and whether
Steam listed it: true, false or unknown. A record-less item or an unreadable
manifest gives unknown, including explicit "Steam listing is unknown" text in
missing/downloading diagnostics. Only a readable manifest establishes false.
The separate edge list retains every selected child/parent relationship and its
source, including multiple incoming relationships for a shared parent. Synthetic
ordering constraints between siblings are not dependency edges in that list.

| Code | Meaning |
| --- | --- |
| EAWR-MODS-0003 | Required layer is not installed |
| EAWR-MODS-0010 | Steam download unfinished |
| EAWR-MODS-0011 | Malformed or unreadable modinfo |
| EAWR-MODS-0012 | Malformed or unsupported known-chains input |
| EAWR-MODS-0013 | Dependency cycle or contradictory order |
| EAWR-MODS-0014 | Repeated parent within one dependency list removed |
| EAWR-MODS-0015 | No Data folder, or selected leaf is parent-only |
| EAWR-MODS-0016 | Script hint needs player confirmation |
| EAWR-MODS-0017 | Non-retail XML/MEG in FoC Data |
| EAWR-MODS-0018 | Bounded scan incomplete |

The cache fingerprint is SHA-256 over domain `eawr-chain-fingerprint:1;`, then
length-prefixed UTF-8 fields for each ordered key, presence marker, manifest,
update time, ACF size, tree hash, tree file count and capped marker. Decimal
numbers have no padding. Paths, names and provenance do not enter this digest.
It is a cache input, separate from the later multiplayer chain code. A capped
record fingerprint retains its existing "unknown beyond the cap" semantics.

## Read-only bounds and overwrite rule

Each text input is at most 1 MiB. A top-level scan visits at most 2048 entries
per mod, then warns. Symlink files and directories are not followed during these
scans. The sole existing native text adapter opens read-only and bounds the
actual read, including files that grow after their size was queried.

**MM-OVERWRITE-1:** list regular XML/MEG filenames recursively under the install's
`corruption/Data`, without reading asset bytes. Compare ASCII-lowercased relative
paths against a caller-supplied authoritative retail inventory. Warn once if any
path is absent. Non-XML/MEG files do not count. The caller must supply the correct
inventory for that install; the core embeds no FoC filename whitelist and does
not infer retail membership from mod filenames. Replacement bytes at a retail
filename are outside this cheap filename-only rule. The default cap is 200,000
entries; a cap or listing error produces the incomplete-scan warning. No file is
modified, moved or removed.

**Product integration is deferred to MM-S1b's `game.find`.** This change delivers
the detector and synthetic positive/negative contracts, not an active product
warning. There is no shipped complete retail physical-file inventory in `data/`
or `tools/`: the unit census lists selected logical XML inputs, including archive
members, and the game-data sync tool generates manifests from the installation
it is given. Neither establishes a clean retail directory baseline. `game.find`
must supply an authoritative inventory for the selected install and call
`detect_data_overwrite`, returning its warnings through install validation. It
must not label the current, possibly modified directory listing as retail truth.

## Explicit player order checks

`check_order()` checks a highest-priority-first selection through the same graph builder as `resolve_chain()`.
Every explicitly selected layer seeds the graph. It retains dependency-source precedence, layouts, canonical
keys and sibling constraints; it adds no alternate dependency rules. Missing selected dependencies are errors,
including installed parents omitted from the order. An empty order is valid unmodded play.

Reversed graph constraints warn with `EAWR-MODS-0019`; duplicate selected layers warn with
`EAWR-MODS-0014`. The caller may accept warning codes for this check. Errors always block launch regardless
of acceptance. Only the first explicit layer must be a playable leaf; lower parent-only layers are valid.
See `docs/mods-service-protocol.md` for the `chain.check` transport contract.

`chain.resolve` keeps its existing verdict for scan warnings. In `check_order`, only an incomplete top-level
metadata scan (`EAWR-MODS-0018`) adds a blocking diagnostic, because it may have missed dependency metadata.
Fingerprint change-check limits (`0004`, `0008`) do not block either API. Repeated dependencies declared by
an upstream modinfo are deduplicated without requiring player acceptance; only explicit player duplicates
or aliases warn in the order check.
