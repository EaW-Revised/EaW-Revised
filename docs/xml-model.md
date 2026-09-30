# XML object model and variant resolution

P0-05 implementation contract, 2026-09-21. This document was written before the
corresponding registry and merge cases were implemented. It records the permitted
MIT evidence used by the loader; it does not assign gameplay meaning to XML tags.

## Pinned evidence and schema boundary

The schema contract is the accepted P0-09 inventory contract at eaw-schema revision
`3e1b825a124fbc13b2293665f34a36dd4d4be80f` (MIT, copyright 2026 Alamo Engine
Tools). The checked inventory manifest contains 130 hashed schema files and the
runtime's generated lookup is derived from `plan/inventories/xml-tags.json`; it is
not a hand-maintained tag list. The variant reference is pg-starwarsgame-lsp revision
`4461416d401b0f665bc1fe82aad60b90bd707fa9` (MIT), especially
`EffectiveObjectResolver.cs`, its original tests, and the schema merge/replace tests.
The private installed corpus is unchanged from the accepted P0-09 generation.

Every parsed node retains its original element spelling, ordered attributes, direct
text (without numeric conversion), ordered nested children, logical VFS path, source
ID, layer ID, and 1-based line/column. Repeated elements are separate occurrences.
Unknown and deprecated nodes remain in the tree and produce diagnostics in P0-09's
profile/type/path/tag vocabulary. No DTD, external entity, network fetch, embedded
code, native path, or direct runtime filesystem read is allowed.

### Evidence-bounded nonstandard container names

The unchanged Remake corpus has two required GameObject includes whose document
containers are `<181st>` and `<80s_Visor_Man>` (effective byte hashes
`505082046b1af360370f2bc234827c107abadb664e7b880dcbe8f1fd0aff3a54` and
`00b8cbc967e134e5f29d207f6632f9d142a5b2d6d112d29cab82f55c5c9d34df`). A leading
digit is not an XML-standard name start, so strict pugixml rejects both before reaching
their registered child definitions. The pinned LSP's HtmlAgilityPack path reports one
parse error for each and discards the numeric container, promoting the first `UniqueUnit`
to document root; its normal registry parser therefore cannot index the complete file
either. Both files are nevertheless unconditional active includes in the installed
Remake registry. This is an evidenced parser-compatibility gap, not absent input.

The loader therefore has one narrow compatibility retry: for UTF-8 input only, after
the strict parser reports an invalid tag type, it may temporarily replace the first
byte of an otherwise name-shaped, balanced numeric-leading *document root* with `_`,
parse the same-length buffer, and restore the authored root spelling in the public
tree. It never repairs child names, mismatched/unbalanced tags, declarations, doctypes,
entities or any other syntax. Same-length substitution preserves all child byte/line
locations. This is a bounded compatibility rule for measured active data, not a general
HTML fallback or silent malformed-input recovery.

## Active registries and include order

The pinned schema's `eaw/meta/metafiles.yaml` declares the five file registries below.
Abilities have no sixth file registry: registered ability object types are indexed as
nested definitions in the GameObject documents (notably the `Abilities` sub-object
lists) and therefore share GameObject include order. A registry value is resolved
relative to `data/xml/`, canonicalized by the VFS, and opened through the effective
winner. Direct `File` children are consumed in document order; a missing or malformed
registry/include is diagnosed and retained in the scan outcome.

| Catalog | Registry path | Required root | EaW includes | FoC includes | Remake includes |
|---|---|---|---:|---:|---:|
| GameObjects | `data/xml/gameobjectfiles.xml` | `Game_Object_Files` | 65 | 120 | 687 |
| Hardpoints | `data/xml/hardpointdatafiles.xml` | `Hard_Point_Files` | 2 | 3 | 140 |
| Abilities | nested registered ability types in GameObject includes | enclosing document | inherited | inherited | inherited |
| Factions | `data/xml/factionfiles.xml` | `Faction_Files` | 1 | 2 | 3 |
| Campaigns | `data/xml/campaignfiles.xml` | `Campaign_Files` | 4 | 5 | 6 |
| SFX | `data/xml/sfxeventfiles.xml` | `SFXEvent_Files` | 20 | 25 | 26 |

The installed registry winner hashes, which pin the full ordered root lists without
copying 687 private include names into this public note, are:

| Profile | GameObjects | Hardpoints | Factions | Campaigns | SFX |
|---|---|---|---|---|---|
| eaw | `ad78629b6a2c5e0dfd9a414ce6f943030be8523d4ef467c3740d84e8fcad2e39` | `d98c7890052f6cca4c804397ec8f6c994cb51ee90c4dba7768ea73e5719ab4ef` | `0e468c86e282fca0541f55ca942565fb711bfe3715e6db728439416273dcf4ee` | `a8b1df28ee5ee5b82fb7170a3310266fe84f71fb66e1b81af4e6357807be6a5c` | `9ae31576e46b9c0e5fc7bdfdcead6131ad5e7897a151bcc2d602f7798331fe8d` |
| foc | `13dbe663ad6ea55b345fc0890d2043de22fedecc8742122706236e02b45fe307` | `eb8f2bf4b8f2f091c63b94275f1144701a3b4699d58d1853e5d55af43c26a31e` | `f4303479d725bed4d1b6809878592218692e0c2e2c4f0a2d35119a400f5ea4ae` | `84037a8345a96742c42846e068413055b61c3684211e6722ca37f75bf1a35d39` | `06bef3cb96bd69005ab23d87cca03cb98eff8ecb722ea5bcc7cb59bf4cb80e96` |
| remake | `7fa07d7fc09ce20ff2c9e22db201eeda5bdc2231f837a7a9d34d7cfc88290613` | `2f91bbdc34178d3a1bd0e7acb461ef65403d0d1b994f903fbdb0147c5a5b0927` | `42598a7ee2d569c45dadc010ae3fecb9d925b3bc79ead1edbb0306e1acf468bb` | `3824152907488895973ca2dbacde1a3b60ece3d2c56dea25c3db944cd0f6b9a5` | `9682973be1ac39423d2a41443fce3615effc15dd2abc68f2523ebae57351c7d4` |

The full ordered lists and each include outcome are emitted in the private scan and
summarized by hash/count in `plan/inventories/xml-load-summary.json`. Registry
replacement is profile-wide: the VFS winner of the registry supplies that profile's
complete include order; a weaker registry is physical/shadow inventory, not appended.

## Object identity and duplicates

The LSP `GameIndex` and its original tests use `OrdinalIgnoreCase`: `X-Wing` and
`X-wing` are the same ID. The portable loader applies ASCII case folding, matching the
accepted VFS boundary and every identifier present in the measured corpus, while
preserving original spelling for display/provenance.

Definitions in stronger VFS layers override weaker-layer definitions. Within one
layer, registry include order is authoritative: later definitions win, while all
definitions and a duplicate diagnostic remain available. This extends the LSP's
evidenced rule (highest layer rank wins; same-rank duplicates are errors and the index
retains all sites) with deterministic registry order required by the game's ordered
file lists. VFS file precedence alone never selects between differently named include
files. Exact duplicate occurrences in one file likewise retain every site and select
the later occurrence. Raw APIs expose all candidates; `find` returns the winner.

## Variant chain and merge rules

`Variant_Of_Existing_Type` is metadata and is not emitted as an effective value. IDs
are followed case-insensitively from derived to base, then applied base-first. The
public chain remains derived-to-base, as in the LSP. A missing base fails resolution
with the complete attempted chain. A cycle fails with the complete chain including the
repeated ID. Resolved values are cached only inside a catalog and keyed by that
catalog's monotonically assigned generation.

The following cases come directly from the pinned resolver and original tests:

- Replace is the default for every tag absent an explicit schema `variantMode`. The
  derived occurrence replaces the immediately preceding value while retaining both
  sources; first-seen tag position is stable. An empty derived element is a real empty
  override, not "inherit". Nested elements and attributes stay attached to the winning
  occurrence rather than being flattened.
- Merge tokenizes only on comma and ASCII whitespace, removes empty separators, and
  appends base tokens followed by derived tokens. It does not deduplicate. The pinned
  eaw-schema revision explicitly marks `GameObjectType/Death_Clone` as merge at
  `eaw/tags/GameObjectType.yaml#L1356`; all other observed tags default to replace.
- A merge tag that permits multiple occurrences keeps each tuple occurrence
  independent, in base-then-derived contribution order. Tokens inside one tuple are
  never unioned with another tuple. This preserves repeated leading tuple values.
- Repeated ordinary replace occurrences are applied in source order; the last write in
  a layer wins, matching `EffectiveObject.ValueOf`. List- and tuple-shaped tags are not
  additive merely because of their shape.
- The schema's four `validationOverride.mode: replace` users
  (`Land_Terrain_Model_Mapping`, `Music_Event_List_Ambient`,
  `Music_Event_List_Battle`, and `Presence_Induced_Animations`) replace validation
  handlers only. The LSP's `EawSchemaReplaceOverrideScopeTest` proves this scope; it is
  not a variant merge instruction and therefore cannot silently change effective data.
- `VariantMode.Ignored` is implemented as in the permitted resolver: a root/base value
  may be inherited, but a derived layer cannot add or replace it. The pinned schema has
  no currently observed ignored tag, so this is synthetic coverage rather than an
  invented corpus classification.

### Pinned-schema compatibility risk

The permitted LSP revision contains a newer source-only regression policy in
`EawSchemaVariantMergeModeTest.cs`: it says `Death_Clone` should replace and describes
100 owner/tag merge pairs. That policy contradicts the shared inventory-pinned schema,
which marks only `GameObjectType/Death_Clone` as merge, and the LSP checkout neither
vendors nor pins an alternative schema containing the claimed owner/tag mapping. The
resolver itself asks its supplied `ISchemaProvider` for the mode; it does not contain
that mapping. Reconstructing the list from prose or maintaining a private overlay would
violate the shared-schema gate.

Compare the pinned resolver against the same shared schema revision. Do not adopt
the newer test policy through a private overlay: it needs a separately pinned,
shared schema update. Comparison normalizes registry type names, leading/trailing
whitespace, line endings and sub-object InnerText; columns and an LSP element-level
type are outside that comparison.

Each effective occurrence carries provenance kind (`own`, `inherited`, `added`,
`overridden`, or `merged`), source object, logical file/source/layer and line/column.
Overridden and merged values also retain the immediately displaced occurrence.
Numeric values remain text. Optional conversion calls only
`eawr::sim::math::Fixed::from_decimal`; malformed and overflow outcomes propagate the
accepted math diagnostic and never produce a default, saturation, or infinity.

## Physical inventory versus active data

`load_catalog` uses only the five active registry graphs above. Separately, it asks the
VFS for raw and effective XML records so reports can account for inactive archives,
shadowed files, unreferenced/disabled XML, and malformed physical files. Such files do
not become active definitions by discovery. Their parse outcomes and provenance remain
visible; an unexplained malformed active include or unresolved required definition
prevents a full-pass result rather than disappearing from counts.

## Versioned scanner input receipt

`xml_scan --input-receipt <json>` optionally writes a schema-version-1, metadata-only
receipt alongside the existing scan report. It records the selected profile and pinned
schema revision, five ordered attempted registry roots, and every ordered direct `File`
include attempt. Abilities have no separate root: their nested definitions follow the
GameObject registry and include order. Each row has a category, logical VFS path,
`loaded`, `missing`, `read_error`, or `parse_error` outcome, source ID and layer when
`stat` identified a winner, and SHA-256 when a readable buffer reached the parser.
Wrong-root registry XML is a `parse_error` with the readable buffer's digest. Missing
and unreadable inputs have no digest. Root attempts remain present even when they
produce no include rows. The existing `RegistryFile` path, order, loaded flag, source,
diagnostics, inventory, and scan report fields retain their earlier meaning.

The digest is taken from the original `vfs.open` byte vector immediately before
`parse_document` receives that same vector, including on parse failure. The bounded
numeric-root compatibility retry may edit a temporary copy inside the parser; the
receipt hashes the authored bytes. The scanner's older sample `input_sha256` report
field remains a later VFS lookup for report compatibility and is not the custody
record. Only active registry roots and includes enter the receipt identity; physical
inventory probes and shadowed bytes do not. A failed VFS mount or fatal catalog load
produces no receipt because there is no completed catalog of attempted XML inputs.

`identity_sha256` hashes a canonical UTF-8 byte stream. It starts with three
length-framed fields: `eawr-xml-input-receipt-v1`, profile, schema revision. Each
field is encoded as its decimal byte length, a colon, then its bytes. Next come all
root rows in registry order, then all include rows in loader order. Each row frames:
kind (`root` or `include`), category, logical path, include order (empty for roots),
outcome, source-presence marker (`0` or `1`), source ID or empty, layer or empty,
digest-presence marker, digest or empty. Include rows additionally frame their
registry path. This binds content, failed attempts, source winners, and order without
absolute installation paths or XML values. Relocating the same mounted content does
not change the identity; changing bytes or a winning layer/archive does.

The receipt describes **the project scanner's observed XML inputs in its enumerated
VFS mounts**. It does not establish complete retail mounted content, absence outside
those mounts, or runtime observation. It does not include raw XML, object values,
native paths, executable activity, or any claim about unmounted archives.
An authored include value containing a colon, doubled slash, or `..` is
represented in the receipt by `invalid-logical-path:` plus its SHA-256. The
loader still attempts the original value and preserves its existing diagnostic;
the receipt does not print a path-like XML value that could name a host location.
