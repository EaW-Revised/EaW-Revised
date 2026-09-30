# Virtual file system contract

P0-02 supplies the only runtime path from a logical game/mod asset name to bytes. Callers
do not receive native paths and must not open a game or mod file directly. The public API is
`include/eawr/vfs/vfs.hpp`; the scanner is `asset_scan`.

## Logical paths and records

`canonicalize` converts `\\` to `/`, removes empty and `.` components, resolves internal
`..`, rejects traversal above the mount, and rejects absolute and drive-qualified paths.
Only ASCII `A`–`Z` is folded. Bytes outside ASCII are preserved exactly, so behavior does
not depend on the process locale and two differently cased non-ASCII names remain distinct.

`AssetRecord` exposes the canonical path, original spelling, layer ID, loose/archive origin,
byte size and stable source ID. It deliberately exposes no native path. The operations are:

- `Vfs::mount(ordered_layers)`: mount layers from highest to lowest precedence.
- `open(path)`: return bounded bytes for the winner.
- `stat(path)`: return the winner's provenance.
- `enumerate(prefix, extension)`: return effective winners sorted by canonical path.
- `enumerate_raw(prefix, extension)`: return all raw records for inventory work.
- `candidates(path)`: return winner then shadowed records in precedence order.
- `resolve_manifest_mount(layer, data_root)`: create an evidence-bounded active archive list.
- `probe_meg_archive(path, source_id)`: validate one MEG independently of active mounting.

Enumeration and lookup share the same canonical keys. Same-layer loose files that collapse
to one key are a deterministic mount failure, which prevents host directory enumeration
order from selecting a winner on Linux. Archive duplicate keys are also rejected.

## Precedence and its evidence

Layers are highest first. For the standard profiles they are:

1. each `MODPATH` in command-line order (leaf mod first, dependencies after it);
2. Forces of Corruption expansion;
3. Empire at War base.

Within one layer, loose files precede that layer's active archives. Archives are listed
lowest first and searched in reverse, so a later active archive wins. Layer selection
happens before source-kind selection: a mod archive therefore beats an expansion loose
file, while a mod loose file beats every mod archive.

The ordering gate is grounded in these permitted sources, pinned in the implementation
report:

- Each installed `Data/MegaFiles.xml` is the active non-patch list. The manifest's document
  order is retained. An absent declared localized archive is reported, not fabricated.
- alo-viewer's MIT `Assets.cpp` processes manifest children in document order, appends the
  three exact SFX archives and `Patch.meg`, and searches the resulting archive vector in
  reverse. It also searches the supplied base-path vector in caller order.
- pg-starwarsgame-lsp's MIT `MegLoadOrderResolver` and tests establish the patch slots
  `Patch.meg`, `Patch2.meg`, `64Patch.meg`, later-wins. Its launch-layer implementation and
  tests state that repeated `MODPATH=` arguments are consulted in command-line order and
  emit the leaf first.

Accordingly `resolve_manifest_mount` activates only manifest entries that exist, then the
exact SFX conventions, then the three exact patch slots. It never glob-mounts all MEGs and
never alphabetically invents precedence. Archive probing is separate and does discover
every `*.meg`. In the current Remake manifest, `RemakeDisabled.meg` is explicitly listed;
despite its name it is therefore active. Changing that requires changed manifest evidence,
not a filename heuristic.

`MegaFiles.xml` is parsed as XML, not searched as raw text. The document must be
well-formed, contain exactly one document element, and have a `Mega_Files` root; only its
direct, case-sensitive `File` children are declarations. Comments, processing instructions,
and nested `File` elements do not activate archives. Text and CDATA content are combined,
XML character/entity references are decoded, and a `File` containing child elements or no
text is rejected with `EAWR-VFS-0010`. Input is bounded to 4 MiB. pugixml auto-detects
supported Unicode XML
encodings (UTF-8 and BOM/declaration-signalled UTF-16/UTF-32). UTF-8/16/32 sequences are
validated after detection, and a declared encoding must match the detected Unicode family;
legacy encodings, mismatches, and malformed byte sequences fail as invalid manifests.
Document type declarations are rejected, and the parser performs no external entity
resolution or network access.

The parser dependency is pugixml 1.15, vendored from the pinned upstream release under
`third_party/pugixml/`. CMake builds that unmodified source locally as
`pugixml::pugixml`; configuring and building the VFS therefore requires no dependency
download. Release provenance, per-file hashes, and validation evidence are recorded in
`docs/reports/P0-02-dependency.md`.

## MEG safety and diagnostics

The reader supports the MEG-v1 layout encountered in all 36 archives of the current
three-layer corpus: little-endian filename and entry counts, length-prefixed filenames,
and 20-byte entry records. Counts, cumulative filename bytes, filename indices, offsets,
lengths, allocations and reads are bounded before use. A malformed active archive fails
the mount; a malformed discovery-only archive remains an individual probe error.

Stable codes:

| Code | Meaning |
|---|---|
| `EAWR-VFS-0001` | invalid, absolute or escaping logical path |
| `EAWR-VFS-0002` | logical/native item not found |
| `EAWR-VFS-0003` | native adapter I/O failed |
| `EAWR-VFS-0004` | case-insensitive loose/native component collision |
| `EAWR-VFS-0005` | truncated MEG structure |
| `EAWR-VFS-0006` | MEG entry is outside archive bounds |
| `EAWR-VFS-0007` | invalid MEG filename/path index data |
| `EAWR-VFS-0008` | parser/read safety limit exceeded |
| `EAWR-VFS-0009` | duplicate case-insensitive path inside one MEG |
| `EAWR-VFS-0010` | missing or invalid active manifest |
| `EAWR-VFS-0011` | invalid mount contract |

## Scanner

From the installation root used in `README.md`:

```text
asset_scan --profile eaw --game-root <install> --report out/vfs/eaw.json
asset_scan --profile foc --game-root <install> --report out/vfs/foc.json
asset_scan --profile remake --game-root <install> --mod-root <mod-or-data-root> --report out/vfs/remake.json
```

Reports contain only metadata: manifest declarations, missing declarations, active order,
one outcome per discovered archive, raw/effective counts, winners, shadowed provenance and
stable diagnostics. Root aliases replace private absolute paths. Full reports belong under
ignored `out/`; the compact committed summary is `plan/inventories/vfs-summary.json`.

`tests/vfs/test_asset_io_boundary.py` rejects native file APIs outside VFS/platform source
adapters. Entry points may still read CLI configuration and write reports, as required by
the shared Phase 0 boundary.
