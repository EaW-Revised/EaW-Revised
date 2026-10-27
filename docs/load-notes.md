# Load notes

Load notes record authored content that is refused (class A), accepted under a
retail rule (B), or accepted with a safe degradation while retail behaviour is
unknown (C). They are diagnostics only: never simulation state or replay bytes.
Class A requires a cited refusal rule. Class C consumers clamp to their documented
representable range, skip the affected object/placement, or use a documented default.

## Public API

`eawr/core/load_note.hpp` defines `LoadNote`, `LoadNoteBuffer` and
`LoadNoteCollector`. A loader can return notes alongside its existing diagnostics.
Each partition exclusively owns a buffer; `append` and `runtime` perform no I/O,
callbacks or locking. After joining the phase, the simulation worker merges its
buffers into the match collector. No additional viewer main-thread battle work is
required. Runtime notes use `runtime`, which marks `runtime_degraded`.

Groups sort stock first, then by zero-based chain position, layer key and file.
Within a group, notes sort by file, line (null last), code, object, tag and the
remaining fields as deterministic tie breakers.
It retains one note per (code, file, object, tag), and one runtime note per
(code, object) across all phases of that match. Lexically first content wins when
duplicate producers disagree, independent of partition or merge order. Buffers
are drained at merge, and retained storage grows with unique notes. Use a new
collector (or `reset`) for a new match. Do not share a buffer between writers or
read a collector while its owner merges.

`clamp_note`, integer `saturate_note<To>` and `default_note` return the effective
value and append a class C note only when they change/default it. Supply the
authored spelling in the record; helpers never reconstruct it from parsed numbers.
The consumer supplies its sourced/default range; infrastructure invents no
game-specific limits. These helpers use arithmetic types; fixed-point consumers
can pass their integer representation. Runtime callers append their note through
`runtime` after applying their consumer's degradation.

`bind_load_note_layers` resolves a source layer ID to its zero-based mounted mod
position or stock. Supply mod IDs and stock layer IDs separately. A mod ID takes
precedence if the same string occurs in both lists. The record's `file` is relative to the layer root, uses forward slashes, and
never contains an absolute installation/user path. VFS paths such as
`data/xml/gameobjectfiles.xml` meet that contract. The viewer resolves VFS aliases
to the selected profile's actual chain keys (Workshop or hashed folder keys).
Authored text is limited to 128 UTF-8 bytes, effective values to 256, and diagnostic
messages to 512, including `...[truncated]` when cut, without splitting a character.
Messages must use layer-relative paths; absolute-path suffixes are redacted before
publication. Code, object, tag, retail text, layer keys and paths have 512-byte
publication limits as a final bound.

## Sinks and JSON

`format_diagnostic(load_note_diagnostic(note))` uses the existing engine-log line
format and severity. It does not replace existing syntax diagnostics.
`platform::write_load_notes(user_data, header, notes)` atomically overwrites
`mods/cache/loadcheck/last-<sha256(profile-id)>/load-notes.json` beneath user data.
The hash is the same one used by the mods service's `last-<hash>.json` marker.
Unprofiled runs hash `chain:<chain_fingerprint>` instead. There is no session
history. Redirected descendant folders/files are rejected. Call the sink during
loading or at match shutdown, never in a battle phase; publication failure never
changes content admission or match state.

The version 1 envelope contains `schema_version`, `chain_fingerprint`,
`profile_id` (string or null), `game_version` (the engine build identity),
`written_at` (UTC ISO 8601), `totals` (`{A,B,C}`), `truncated` and `groups`.
The fingerprint uses the exact shared mods-service loadcheck algorithm: engine
identity, ordered layer keys/root/tree fingerprints, then expansion/base trees.
An unavailable identity is empty, so readers treat the report as unverified.
The launcher can compare profile, fingerprint and build identity to mark it stale.
Every group contains `layer`, `file`, `totals`, `truncated` and `notes`.
Totals count all unique notes before publication limits, even if some are omitted.
Publication emits at most 1,024 notes and at most 512 KiB including the newline;
`truncated` is true when fields or entries are cut. A truncated group keeps its
full totals. Every note contains the following fields:

| Field | JSON type / meaning |
|---|---|
| code | string, stable `EAWR-LOAD-nnnn` |
| class | string, `A`, `B`, or `C` |
| layer | object: `mod_id` string and `chain_position` unsigned integer or null for stock |
| file | string, layer-relative path of the authoring source |
| line | unsigned integer or null |
| object | string, authored type or runtime instance identity |
| tag | string, XML tag or consumer field |
| authored_value | string, original spelling limited to 128 bytes |
| action | `refused`, `accepted`, `default_used`, `clamped`, `skipped_object`, `skipped_placement`, or `runtime_degraded` |
| effective_value | string, consumer value or description of omission |
| retail | string, rule ID plus one line, or `unknown` |
| phase | string, `load` or `runtime` |
| diagnostic_code | string, original validator diagnostic code, or empty |
| message | string, original diagnostic text with safe paths and UTF-8 truncation |

`eawr_loadcheck --notes [--json]` adds grouped output to the existing positional
invocation (`profile user-data report game-root [key index]`). Its normal report
and progress protocol remain available. Every pass replaces the canonical profile report
under user data; the listing reflects that pass, including an empty groups array
when there are no notes.

## Normalization seam

`normalize_owned_load_table(source, notes, normalize, validate)` copies a table,
normalizes that owned copy, checks its final invariants, and returns it. Family
normalizers must be deterministic and idempotent. `prepare_durability` is the
concrete seam shared by direct session creation, replay creation and land content;
its `normalize_durability(DurabilityTable&, LoadNoteBuffer&)` body is identity in
the infrastructure change. Family work supplies the normalization body without
removing the final validator. A second normalization, including replay playback,
must preserve both effective content and replay bytes.

```cpp
core::LoadNoteBuffer notes;
auto table = prepare_durability(authored, notes); // owned copy -> normalize -> validate
// Unit-table loaders can likewise accept an optional LoadNoteBuffer* side channel.
auto session = TacticalSession::create(setup, content, &notes);
core::LoadNoteCollector collected;
collected.merge(notes);
```

All four session create/from_replay overloads accept an optional final
`LoadNoteBuffer*`. Notes stay outside session state. Platform startup collects
them and invokes `LiveSession::Options::load_note_sink` synchronously before the
simulation thread starts. The viewer merges them with catalog diagnostics and
publishes the profile report. Recovery reconstructs through the same normalizer
without invoking a presentation sink. Runtime family consumers own partition
buffers and merge them on the simulation owner at a joined phase boundary;
only load/shutdown code publishes them.

## Code allocation

Ranges are centrally assigned; never derive a code from diagnostic text.

| Range (`EAWR-LOAD-`) | Owner / family |
|---|---|
| 0001–0099 | Infrastructure; 0001 = missing registry include, XML R-10; 0002 = report sink warning |
| 0100–0149 | Tactical durability and damage |
| 0150–0199 | Motion |
| 0200–0249 | Fighters and garrisons |
| 0250–0299 | Stealth and ability/combat globals |
| 0300–0349 | Visibility and fog |
| 0350–0399 | Tactical economy |
| 0400–0449 | Lobby and registry |
| 0450–0499 | Placement and per-type skip |
| 0500–0549 | Scoring |
| 0550–0599 | Tactical AI |
| 0600–0649 | Tactical runtime arithmetic |
| 1000–1999 | Galactic lane, subdivided by its coordinator |

The worked example preserves XML R-10: an unavailable initial include remains
omitted and its existing diagnostic/health count stays intact. Its additional
class B note points at the registry's authored `File` element. Unreadable admitted
files and malformed content retain their existing handling.
