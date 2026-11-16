# Unit-table model facts store

Loading the unit tables decodes every model of every unit type a battle or campaign can reach, but the tables keep only
each model's bone frames and collision geometry. The model facts store keeps exactly that on disk, so a later load of the same
file reads, hashes and decodes nothing for it. (The record format has an optional file-hash field for a later consumer; slice 1
does not fill it.)

## What a record holds

The simulation projection of a model (`units::simulation_projection`): every bone (name, parent, the twelve transform
values as stored), and for each collidable mesh its name, bone, bounds and, per submesh, the vertex positions, each
vertex's first skin bone, the indices and the skin palette. Floats are stored as their raw bits. Everything else in the
model (materials, lights, proxies, non-collidable meshes, normals, texture coordinates, weights) is never read by the
tables and is not stored. A new table reader of another model field must extend the projection and the record format
(bump `facts_format`); `unit_model_facts_contracts` compares tables built with and without the store.

## Where it is used

A hit and a miss feed the same unchanged unit-table loader through `LoadInput::model`: a hit rebuilds the projection from the
record, a miss decodes as before and records the result. The battle loads (`load_start_tables`: skirmish, saved battle,
attract scene, viewer replay) and the headless simulation use it when a store is configured. A decode-ahead hint is not
offered for a model a record already covers. A VFS with a content contract (the multiplayer preparation view) never
exposes native locations, so it neither reads nor writes records.
Every multiplayer skirmish entry also turns the store off explicitly (`use_model_facts` false: the LAN/online lobby's option
preview and the LAN/online battle start, host, client and rejoin), because the lobby's plain mount does expose native
locations: single-player skirmish, saved battle, attract and replay loads keep using it.

The galactic campaign Start (`prepare_stock_space_entry`, single player) uses the same provider behind its bounded model
preparation: a hit returns the record's projection (the preparation is not asked, so nothing is read, hashed or decoded
and the decode-ahead readers are not offered the path), a miss goes through the preparation as before and is recorded
when the Start finishes loading. A multiplayer campaign plan never reads or writes the store (its models are hashed from
verified bytes into the provider identity); `galactic_space_facts_stock` proves the prepared closure is identical with no
store, an empty store and a warm store, and that a multiplayer plan leaves the store untouched.

## Store layout (`platform::DerivedStore`)

A folder (`<user data>/mods/cache/facts`; `--eawr-facts-cache <dir>` names another) of immutable segment files
`seg-<time>-<random>.efs`. A segment is a 48-byte header (magic, segment format, facts format, a 16-byte epoch from the
engine build identity, creation time, header check) followed by frames: length, a 16-byte check (SHA-256 prefix of the
payload) and the payload (kind, 16-byte key, stamp, body). A process writes the records it derived as one new segment
(temporary name, then rename) and never changes an existing file.

- **Memory:** only an index (key, stamp, place in a segment file) is resident, about a hundred bytes a record. A record body is
  read from its segment file when it is asked for, and its frame check is verified again at that moment; records not yet written
  (the open segment, until the next flush) are the only bodies in memory. A store at its size cap therefore costs index memory
  only, not its size. A segment that cannot be written (read-only or full folder) is dropped, not kept; with no folder the
  memory is bounded by its own, smaller cap (64 MiB). Torn `*.efs.tmp` files older than ten minutes are deleted when the store opens.
- **Opening:** a store that no record was put into and no lookup asked is never opened: flush and finish do nothing then.

- **Key:** canonical path, origin, layer, source and container path of the winning source.
- **Stamp:** the container's size and modification time as of the mount, and the entry's offset and size in it (a loose file is
  its own container). A record is served only when its stamp equals the live stamp (`Vfs::source_stamp`; no byte of the asset is
  read). A loose file edited while a session runs is picked up at the next launch, like every other mounted file the session
  already read; nothing is stat'ed again on the hot path.
- **Epoch:** a segment of another facts format or engine build is ignored, so a new build starts with an empty store. The epoch
  text is the build identity plus a hash of the source files that decide what a record holds (everything under the asset readers and codec, the unit tables and their projection and the VFS, plus the store, the providers and the setup map facts;
  the directory list is in `cmake/EawrFactsCodeId.cmake`, so a header the decoder includes cannot be missing from it). The build identity already
  carries the commit, so clean builds from different commits differ anyway; the hash is for dirty and warm trees. A build step hashes them whenever one changes, so a development tree that is edited after its last configure, or two dirty trees on one commit, get
  different epochs and never read each other's records. A change to anything outside that list that alters a record's meaning
  bumps `assets::model_facts_format`.
- **Damage:** a bad header, a torn or flipped frame (at open or at a later read), a stale stamp or a vanished segment is a miss and the file is decoded
  as before; the miss writes the record again. A bit flip or truncation ends the scan of that one segment.
- **Bounds:** one compaction per process when there are more than eight segments or the folder passes 512 MiB (newest records
  are kept); a record above 8 MiB is not stored (that model is decoded at every Start; `put` returns false and the provider counts it as skipped). Measured size on disk: about 112 MB for the Remake chain, 6 MB for stock.
- **Several builds and processes on one folder:** a compaction merges only the segments this process wrote and the segments it
  loaded that are at least an hour old (`adopt_age`); a younger loaded segment may belong to a concurrent process of the same
  build that is still reading it, so it is left alone. If an old segment is merged while another process still holds it, that
  process misses and derives the record again (load time only). Segments of other builds (another epoch or facts format) are
  never merged, read or rewritten. They are deleted only when older than 14 days (`foreign_keep_age`), or when newer ones already
  hold 512 MiB (`max_foreign_bytes`, newest kept first), or when the file is no segment at all. Two builds that alternate on one
  folder therefore keep each other's records, and the folder is bounded by about the size cap plus that foreign budget.
- **Readback:** `platform::model_facts_stats_line()` / `format_derived_store_stats` give one line (`derived-store: records=...
  on_disk_bytes=... segments=... foreign_bytes=... open_us=... compact_us=... hits=... misses_*=... kindN_records=...
  kindN_body_bytes=...`). The viewer prints it to its log at shutdown; `sim_headless` prints it to stderr when
  `EAWR_FACTS_CACHE_DIR` is set.
- **Trust:** the same as the mod content digest (`mod-content-digest.md`): path, size and modification time. It is not an
  integrity or anti-cheat check.

The folder is always safe to delete. Other kinds of derived per-file facts can share the store (the kind byte of a record).

## Setup map list facts (kind 2)

The skirmish setup list (`skirmish::setup_maps`, `land_setup_maps`; the screens reach it through `SetupMapCache`) decodes every
eligible map to build one entry each: its name, the teams and factions its start markers give, whether it can start, and its
preview. With the store configured the list keeps two kinds of record per map in the same folder (kind byte 2), so a second
launch opens and decodes no map (legacy EAWR-6948):

- **Header facts** (`map-header`): the map kind and the authored lobby metadata of the file's root header. Key: the map's
  canonical path. Stamp: the map file's stamp, as above. The picker's eligibility (kind, capacity, levels, custom, owner, terrain,
  game types, new markers) is answered from this record alone, so a map the picker rejects is not opened at all.
- **Entry** (`space-entry`, `land-entry`): the finished list entry, without its preview file name (the list looks that up in the
  view every time). Key: the entry kind, the context fingerprint and the path. Stamp: the map file's stamp.

An entry also depends on content other than the map file (the catalog's objects and factions, the text database, what the view
holds), so its key carries a **context fingerprint** (`setup_facts_context`): a SHA-256 over the format tag, the mounted view
(`Vfs::view_identity`: every asset's path, layer, source, size and offset in mount order, and the size and time of every archive
container), the catalog's registry input hashes, and the size and modification time of every XML and text file of the view. Any
change to those is a different key and a miss. A map file whose stamp differs is a miss too. What the fingerprint does not see:
a loose file outside `data/xml` and `data/text` replaced by one of the same size (a model file, for instance) leaves a stored
land entry's availability text as it was; the match start never uses a list entry and reads everything again.

A record this build cannot decode, a stamp or context that differs, or a missing record is a miss: the entry is built exactly as
before and stored again, so the store only saves work. `SetupMapFactsStore` (include/eawr/skirmish/setup_facts.hpp) is the
interface the list sees; `platform::make_setup_map_facts_store` implements it over a `DerivedStore` (the viewer passes the
process-wide model facts store). The land list decodes its maps on the same worker pool as the space list and merges the results
in record order, so the list does not depend on the number of workers.

