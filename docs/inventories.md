# Offline inventory contracts

P0-08 and P0-09 share `tools/inventory/corpus.py`. It is a bounded, read-only,
offline corpus reader, not a runtime asset API. Runtime game/mod I/O remains exclusively
behind the C++ VFS. The reader accepts the profiles `eaw`, `foc`, and `remake`, extensions
including the leading dot, and modes `raw` or `effective`.

## Corpus identity and precedence

Each source exposes `logical_path`, `source_id`, `origin`, `layer_id`, exact bytes, SHA-256,
archive identity, active state, and winner state. Logical paths use ASCII-only case folding.
Layers are highest first: repeated MODPATH layers in command-line order, expansion, base.
Pass each MODPATH as a separate repeated `--mod-root` option, leaf first. The leaf retains
the stable `mod` layer/source-ID prefix used by one-root runs; dependencies are identified
as `mod[1]`, `mod[2]`, and so on. Supplying the same resolved root twice is an error.
Within a layer, loose files win, followed by active archives searched in reverse manifest
order. `MegaFiles.xml` declarations retain document order, followed by the three exact SFX
conventions and `Patch.meg`, `Patch2.meg`, `64Patch.meg`. Only manifest-active archives
participate in `effective`; `raw` additionally reads every discovered inactive MEG so that
unreferenced files remain accounted for.

MEG counts, name bytes, 20-byte entries, indices, offsets, lengths, member reads, duplicate
canonical names, and traversal are checked before use. A missing data root or active manifest
is a failed run. No filename sort is used to invent archive precedence.

## Canonical JSON

All outputs are UTF-8 without a BOM, use LF newlines, sorted object keys, two-space
indentation, preserved non-ASCII characters, no NaN/Infinity, and exactly one terminal
newline. Arrays use the semantic orders below. A `manifest_id` is the lowercase SHA-256 of
the complete canonical manifest payload with the `manifest_id` member omitted. File hashes
are lowercase SHA-256 of exact bytes. Timestamps and absolute roots are forbidden.

Stable diagnostics are:

| Code | Meaning |
|---|---|
| `EAWR-INV-LUA-0001` | source Lua encoding, lexing, or parsing failed |
| `EAWR-INV-LUA-0002` | compiled PGLua structure, validation, or resource policy failed |
| `EAWR-INV-XML-0001` | XML is malformed, unsafe, or exceeds policy |

Corpus/manifest/MEG failures terminate the command with exit status 2 and a deterministic
field/archive description; they never create an empty passing inventory. Per-file failures
use the codes above and remain in both raw and effective outcome arrays.

## Lua inventory schema version 2

`lua-api.json` has exactly the top-level keys `schema_version`, `manifest_id`, and `rows`.
Version 2 supersedes the proposed ticket version 1 only for the location shape. Every row
has exactly:

- `profile`, `symbol`, `call_kind` (`global`, `method`, or `dotted`), and
  `receiver_type` (string or null);
- `classification` (`engine` or `candidate`) and integer syntactic `call_count`;
- `reference_status` (`present`, `absent`, or `unresolved`) and sorted
  `reference_locations` (empty: the committed declaration index no longer records the
  reference release's file and line locations);
- `example`, with exactly `source_id`, `logical_path`, `source_kind`, `line`, `column`,
  `prototype_id`, and `pc`.

FoC and Remake rows also have `receiver_types`: a sorted list of every receiver class
whose FoC registration matches the call name. A shared method name is `present` even
when its specific receiver is unknown; `receiver_type` remains null in that case.
Dotted calls can match FoC method or member registrations by the name after the dot,
with `receiver_type` null and the plausible classes in `receiver_types`. Base EaW
rows keep the GlyphX matching rules and row shape.

Text sites have positive one-based line/column and null prototype/PC. PGLua sites have the
positive persistence identifier and zero-based instruction PC; stripped coordinates remain
null. A PC is never a byte offset. Rows sort by profile, symbol, call kind, and receiver type.
Examples use deterministic source/location order. The companion `lua-manifest.json` retains
every constituent site and every raw/effective file outcome, so row counts can be recounted.
`lua-unresolved.json` version 2 uses the same location shape and retains dynamic, shadowed,
merged-control-flow, and unknown-receiver cases with reasons.

`lua-unresolved.json` has exactly `schema_version`, `manifest_id`, and `rows`; each row has
exactly `profile`, `symbol` (string or null), `reason`, and `location`. `lua-manifest.json`
has exactly `schema_version`, `manifest_id`, `tool`, `tool_version`, `tool_sources`,
`declaration_index`, and `profiles`. Profiles are ordered eaw/foc/remake and contain exactly
`profile`, `raw`, `effective`, `raw_file_count`, `effective_file_count`,
`raw_parse_failures`, `parse_failures`, `raw_file_outcomes`, `file_outcomes`, and
`call_sites`. Outcomes contain source metadata, a nullable diagnostic, parsed state, format,
call/prototype/instruction counts, and ordered unsupported-instruction records.

Source Lua is tokenized and parsed with lexical scopes; comments and strings are never
treated as code. Script helpers and standard-library calls are excluded from candidate rows.
Aliases preserve their proven name, while overwritten or computed callables stay unresolved.
PGLua is decoded as the pinned Lua 5.0.2-derived record order and calls are emitted only at
`CALL`/`TAILCALL` after control-flow register-provenance analysis. `LOADNIL` invalidates its
complete inclusive destination range; fixed-result calls invalidate every encoded result
destination, while open-result calls conservatively invalidate from A through the declared
stack. CFG joins retain a definite callable only when every incoming provenance agrees.
String constants alone are not calls. Opcode IDs 28 and 32 are structurally decoded and
explicitly reported as unsupported semantic evidence; they do not disappear.

Short Lua string literals reject unescaped CR or LF. Escaped line endings, including CRLF,
remain valid Lua continuations and ordinary escape sequences are accepted.

Decoder policy is 64 MiB per chunk, 16 MiB per string, prototype depth 128, and one million
aggregate entries independently for instructions, constants, prototypes, locals, upvalue
names, and line information. These are tool policies, not claims about the original game.

## XML inventory schema version 1

`xml-tags.json` has exactly `schema_version`, `manifest_id`, `schema_revision`, and `rows`.
Every row has exactly `profile`, `object_type`, `tag_path`, `node_kind`, `tag_name`,
`usage_count`, `file_count`, `status`, `schema_ref`, and `example`. `node_kind` is `element`
or `attribute`; an attribute path is `parent/path/@OriginalName`. Examples contain exactly
`source_id`, `logical_path`, one-based `line`, and one-based `column`.

Element and attribute spellings are preserved. Matching uses locale-independent ASCII case
folding because the pinned schema treats its identifiers case-insensitively; examples keep
source spelling. Definition type comes from the schema's file registries, singleton/direct
content metadata, or a registered nested type. Otherwise the actual root/config type is
retained with an unresolved reason; `GameObjectType` is never fabricated. Paths are relative
to the current definition boundary, keeping same-name leaves in different contexts distinct.

Schema status is `known`, `deprecated`, or `unknown`; deprecated wins if any applicable
pinned entry says so. `schema_ref` points to the pinned YAML entry. Applicability and version
metadata are retained in `xml-manifest.json` alongside registry resolutions and per-file
ordered-occurrence hashes. `xml-unresolved.json` version 1 retains every unknown shape and
reason. Row order is profile/type/path/kind/name using ASCII folding.

`xml-unresolved.json` has exactly `schema_version`, `manifest_id`, and `rows`; each row has
exactly `profile`, `object_type`, `tag_path`, `node_kind`, `tag_name`, and `reasons`.
`xml-manifest.json` has exactly `schema_version`, `manifest_id`, `schema_revision`,
`schema_files`, `tool`, `tool_version`, `tool_sources`, and `profiles`. Profiles are ordered
eaw/foc/remake and contain exactly `profile`, `raw`, `effective`, raw/effective counts and
outcomes, failure counts, `registry_type_hints`, and `schema_matches`. Schema matches retain
the row identity, schema reference, and applicability. Source/file/schema/tool arrays use
ASCII-folded path order; call and occurrence records use their documented source order.

Every XML parser installs an Expat doctype callback after encoding detection, rejects all
doctype declarations for UTF-8/16/32 alike, disables parameter-entity parsing, and rejects
external-entity callbacks. This applies both to inventoried documents and schema file
registries; no byte-pattern prefilter is used as a security boundary.

The schema input must be a pinned git checkout with an MIT license and valid `_index.json`
manifests. Missing or invalid schema inputs fail; they never turn the whole vocabulary into
`unknown`.

## Commands

```text
python tools/inventory/lua_inventory.py --game-root <install> --mod-root <leaf-Data> [--mod-root <dependency-Data> ...] --reference-index plan/inventories/lua-declarations.json --out plan/inventories
python tools/inventory/xml_inventory.py --game-root <install> --mod-root <leaf-Data> [--mod-root <dependency-Data> ...] --schema-root <pinned-eaw-schema> --out plan/inventories
python -m unittest discover -s tests/inventory -v
python tests/inventory/run_corpus_acceptance.py --game-root <install> --mod-root <leaf-Data> [--mod-root <dependency-Data> ...] --schema-root <pinned-eaw-schema>
```

The Lua command uses the GlyphX index for the EaW profile and defaults to
`plan/inventories/foc-lua-registrations.json` for FoC and Remake. Pass
`--foc-index <path>` to test another FoC index. The committed inventory outputs
are the earlier pinned corpus snapshot; rerun the command to produce reports
using the FoC registration index.
