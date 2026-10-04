# FoC mod loading and data overrides

This walk supplies the loading contract for the real-mod scope check (legacy EAWR-1286),
tracked by the mod-loading walk (legacy EAWR-1427). It covers startup and demand-driven
file lookup, rather than a per-frame simulation service. Activation establishes the
search roots; archives and loose files supply bytes; XML registries admit definitions;
unit tables, scripts and asset caches consume those definitions.

Evidence is build-specific. A debug-build reading establishes the inspected path,
not every Steam release or the process-wide outcome after an assertion. Original
symbols, addresses and raw output remain in ignored research files. Evidence IDs
below identify plain-language source descriptions. `same` applies only to the
stated boundary; it never means that a whole mod is compatible.
Current-code comparisons were read at revision `4f2b47eb`; later changes need
their own qualification against these rules.

## Activation and file selection

Evaluate these stages at startup, then use the resulting file service for logical
reads. Root order and archive registration order are separate inputs.

| ID | FoC rule | Source | Ours today and module |
|---|---|---|---|
| ML-01 | The inspected command-line parser recognizes `MODPATH=` and `STEAMMOD=` without case sensitivity in the option name; the original value is retained. Its token delimiters are space and double quote. This is not a general quoted-path parser. | Debug build EML-01. | **differs**, mod root: scanner/viewer options accept a supplied filesystem root or chain; they do not parse FoC's launch string. Normalize launch intent before a scope-check comparison. |
| ML-02 | A nonempty `MODPATH` value is appended to the registered root vector without deduplication. Lookup tries roots in insertion order; the first root containing a requested loose file wins. Repeated options can register multiple roots. | Debug build EML-01/EML-02. | **same** for ordered distinct roots, VFS/mod root: `src/vfs/vfs.cpp` `mod_chain_roots` supplies leaf, then parent roots. Its semicolon option and acceptance of a `Data` directory are project interfaces, not FoC command-line syntax. UI gallery still accepts a single root. |
| ML-03 | `STEAMMOD` activation requires the Steam-build path, a nonzero user ID and a matching queried Workshop item. It services callbacks and sleeps 5 ms while awaiting query completion, with no timeout in this helper, then compares the parsed published ID and registers the matching item's folder. No match adds no root. | Debug build EML-03. | **missing**, mod root: there is no native published-ID-to-installed-folder adapter. The scope check can supply a legally installed folder directly; subscription/download behavior is a separate setup stage. |
| ML-04 | Alternate-root lookup recognizes exactly `./Data/`, `.\Data\`, `Data/` and `Data\`, ignoring case in that prefix. It removes the prefix and probes `<root>\Data\<suffix>` in root order. Other names are not automatically redirected into each mod. | Debug build EML-02, including the four-entry prefix table. | **differs**, VFS: canonical logical names and mounted `data` prefixes replace native prefix routing. Audit callers that use bare or absolute paths separately from normal `data/...` requests. |
| ML-05 | An ordinary read tries registered mod loose files, then the requested native disk path, then all mounted public archives, then registered fallback loose files. First successful open wins; exhaustion returns false. Therefore a parent-mod loose file or a current-game loose file can beat a mod archive. | Debug build EML-02/EML-04. | **differs**, VFS: `Vfs::mount` indexes each layer's loose files and then its reversed archives before visiting weaker layers. A leaf archive beats parent/current-game loose files in ours. |
| ML-06 | An archives-only read skips both mod/current disk lookup and fallback disk lookup. It succeeds only from a mounted public archive. Ordinary read overloads inspected here pass false for this flag. | Debug build EML-04. | **missing** as a caller-selectable read policy, VFS: `Vfs::open` uses the mounted effective winner. No FoC caller requiring this policy is qualified by this walk. |
| ML-07 | Startup loads the fallback game's manifest and its `Patch.meg`, `Patch2.meg`, `64Patch.meg`, then the current manifest and the same three patch slots. Each manifest contributes nonempty declaration values in XML order. There is no archive-directory glob on this path. | Debug build EML-05/EML-06. | **same** for reverse declaration/patch-slot ordering within each mounted base/expansion layer, VFS: `resolve_manifest_mount`. Ours additionally mounts conventional SFX archives; ML-48 qualifies their original audio-specific registration separately. |
| ML-08 | Every successfully added archive is inserted at the front of one archive list. Public lookup walks that list from the front and returns the first matching entry. Registration names compare case insensitively; re-registering that name does not move it. An unavailable archive is diagnosed and not inserted. | Debug build EML-06. | **differs**, VFS: later archives win within a layer, but archives remain grouped by layer. Registration identity and priority are not represented by one global history. |
| ML-09 | The current `./Data/MegaFiles.xml` is itself opened through the file service. Its winning file supplies the whole declaration list. Declared archive paths are then opened through that service too. Startup does not independently load a manifest from every registered mod root; a loose-only mod need not supply its own manifest on this path. A failed manifest read contributes no declarations in the inspected helper. | Debug build EML-02/EML-05/EML-06. | **differs**, VFS/mod root: `resolve_manifest_mount` requires a nonempty manifest per root, and `resolve_manifest_chain` combines independently parsed root manifests while resolving missing mod declarations against weaker roots. |
| ML-10 | Archive lookup trims surrounding whitespace, uppercases the name, converts `/` to `\`, splits folder/leaf and removes a leading `.` and leading separator at the inspected split boundary. It searches the CRC table for that reconstructed name without a full-name comparison after a hit. Internal `.`/`..` components are not collapsed by this helper. | Debug build EML-07. | **differs**, VFS: `canonicalize` uses ASCII lowercase, resolves internal dot components and rejects escape/absolute/drive paths; archive indexing compares canonical names rather than using CRC as sole identity. These validation policies must remain explicit. |
| ML-11 | Loose-file opens use the native Windows file API; the alternate-root prefix test is case insensitive. That is separate from the archive name transformation in ML-10. | Debug build EML-02/EML-04. | **differs**, VFS: loose trees are indexed by portable canonical names and case collisions are rejected deterministically. Native case-sensitive-directory settings, non-ASCII folding and ambiguous collisions remain unverified. |
| ML-12 | Failure to find bytes returns a failed open; the file service does not synthesize a replacement asset. Assertions, warnings, omitted definitions and visual fallback are decisions of the caller. | Debug build EML-04/EML-06; XML and text callers below. | **same** at the VFS boundary: `Vfs::open` returns a not-found diagnostic; loaders and renderer routes decide the consequence. Successful fallback at a later stage must not hide this outcome. |
| ML-13 | Both launch activation paths derive shader and terrain-shader directories from each supplied mod root. The effect path manager appends supplied directory strings to its vector without deduplication. Registration alone does not qualify effect search or include order. | Debug build EML-01/EML-03/EML-14. | **missing**, asset caches/material admission: current viewer adapters and `docs/shaders.md` qualify known material families and bounded translation routes, without equivalent dynamic mod effect-root registration (legacy EAWR-1483). |
| ML-14 | The Workshop query callback marks completion even on query failure. Item loading requires state bit `0x04` present, bit `0x02` absent, a successful folder query with a 1024-byte buffer and a nonempty returned path. The retained path is uppercased, `/` becomes `\` and a trailing `\` is appended if absent. The item must then be recognized as FoC-tagged; other results are removed before published-ID activation. The meanings of the service state bits are not inferred here. | Debug build EML-13/EML-16. | **missing**, Workshop/mod-root setup: no queried-item activation/filter adapter (legacy EAWR-1444). A directory supplied directly is a separate setup contract; query failure and unavailable items need distinct setup outcomes. |
| ML-15 | The inspected manifest helper consumes each root child's nonempty value without checking the root or child tag name. Its XML reader trims value whitespace, rejects a root without children and rejects multiple roots. This is a parser boundary, not permission to invent archive names. | Debug build EML-06. | **differs**, VFS: `resolve_manifest_mount` requires the expected manifest root and `File` child shape. Keep stricter admission policy explicit in compatibility diagnostics (legacy EAWR-1441). |

The VFS differences in ML-04 through ML-11 are tracked together (legacy EAWR-1441).
Native launch/Workshop resolution is separate setup work (legacy EAWR-1444).

Workshop folder handling is qualified by ML-14. Neither the root
vector nor a command-line token establishes a Steam dependency ordering rule.
The meaning of a supplied leaf/parent chain in our tools should be recorded in
the report, along with each root's effective provenance.

## XML admission and inheritance

| ID | FoC rule | Source | Ours today and module |
|---|---|---|---|
| ML-16 | A GameObject registry is an ordered list of files. Only admitted files contribute definitions; discovering an unrelated XML file does not activate its objects. | Debug build; XML registry audit AU-133, R-07/R-09. | **same**, XML loaders: `src/data/xml.cpp` `load_catalog`; physical inventory is separate from activation. |
| ML-17 | A registry or included XML path selects one winning file through the file service. A mod replaces that entire file; this path does not merge corresponding nodes from weaker versions. An omitted stock include in a replacement registry is not automatically reintroduced. | Debug build EML-04/EML-08; R-07. | **same** after file selection, XML loaders: `load_catalog` opens one VFS winner per registry and include. The winner itself can differ under ML-05/ML-09. |
| ML-18 | GameObject and hardpoint registry entries use `File` and resolve relative to the XML data directory. Hardpoints first store admitted include names by filename CRC, then parse in ascending map-key order; that is not GameObject list order. The hardpoint startup path assigns each uppercase-name CRC binding, replacing an earlier binding. | Debug build EML-08; R-07. | **differs**, XML loaders: `load_catalog` reads all five registries in document order and `index_winners` applies a generic strongest/later policy. Category-specific hardpoint order is absent. |
| ML-19 | A missing object include produces a diagnostic and assertion in the debug build. If that assertion returns, the include is omitted and later entries are attempted. A failed registry read returns failure. | Debug build; R-07, AU-133. | **differs**, XML loaders: `load_catalog` retains outcomes but returns a catalog with diagnostics for a bad registry; runtime diagnostic enforcement is incomplete (legacy EAWR-793). An omitted missing include contributes no objects in both. |
| ML-20 | Inheritance is attempted before registering a new name. It requires a previously completed base. An earlier completed object with the same name may supply that base. | Debug build R-08; FoC retail R-11. | **differs**, XML loaders: `src/data/xml_merge.cpp` resolves the selected catalog definitions on demand, without FoC's completion state and registration sequence (legacy EAWR-1431). |
| ML-21 | Registering a duplicate object name keeps the earlier public binding; the debug build asserts. File precedence does not turn this into last-object-wins. | Debug build R-08; FoC retail R-11. | **differs**, XML loaders: `src/data/xml_registry.cpp` `index_winners` sorts layer, include and definition order descending and selects the first (legacy EAWR-792). Its `layer_rank` recognizes `mod[N]`, while `mod_chain_roots` emits `mod-parent-N`; parent definitions therefore get rank zero. |
| ML-22 | An isolated self-base cannot complete itself. At most ten parse passes run; after the first pass, only files owning incomplete types are parsed again. Successful loader return alone does not prove every type completed. | Debug build R-09, AU-133; FoC retail R-11. | **differs**, XML loaders: `Catalog::resolve` rejects a cycle or missing base directly; no ten-pass admission/completion lifecycle (legacy EAWR-1431). |
| ML-23 | A variant starts with a full copy of its base and applies its own tags in document order. For a scalar, the last effective authored occurrence wins. | Debug build AU-80; unit-data retail parse facts. | **same** for ordinary nonempty scalar overrides, XML loaders/unit tables: `Catalog::resolve`, `EffectiveObject::value`, `src/units/unit_support.cpp` `Object::single`. Copy-dependent and list exceptions follow below. |
| ML-24 | Empty tag values and `TBD` leave the previous or inherited value unchanged. | Debug build AU-80. | **differs**, XML loaders/unit tables: generic replacement retains an empty authored occurrence; `Object::fixed` reports an invalid number instead of preserving the inherited value (legacy EAWR-273). |
| ML-25 | A boolean is true for `1` or a value starting with Y or T, ignoring case; other values are false. Numeric parsers accept a leading numeric value and ignore trailing text. | Debug build AU-80/AU-81; unit-data retail parse facts. | **differs**, unit tables: `boolean` accepts only yes/true/1 and no/false/0; `number` requires a valid fixed-point decimal with an optional float suffix (legacy EAWR-273). |
| ML-26 | `Squadron_Units`, `Starting_Spawned_Units_Tech_0` and `Reserve_Spawned_Units_Tech_0` append per occurrence and onto inherited lists. A spawned-unit occurrence consumes its first type/count pair. | Debug build AU-82. | **differs**, unit tables: `Object::list` selects every occurrence from the most-derived authoring layer and omits base-layer lists (legacy EAWR-273). |
| ML-27 | `Squadron_Offsets` and `Fire_Inaccuracy_Distance` append in a base type; each authored occurrence in a variant replaces that list, leaving the variant's last occurrence. | Debug build AU-82. | **differs**, unit tables: `Object::list` returns every occurrence from the selected layer, including every variant occurrence (legacy EAWR-273). |
| ML-28 | The inspected GameObject leaf-tag mapper uppercases the key for CRC-table lookup. Empty values return success without mutation; a nonempty unknown key returns false. The ordinary leaf caller does not log that false result; the nested `SubObjectList="Yes"` path does warn on a failed mapping. Unknown tags therefore do not all share one warning policy. | Debug build EML-09; AU-80. | **differs**, XML loaders/tag registry: `src/data/xml_registry.cpp` preserves unknown nodes with diagnostics rather than applying this original dispatch policy. The scope census should keep those useful diagnostics and separate them from FoC acceptance. |
| ML-29 | The inspected GameObject parser asserts that a name fits below 128 bytes, and the ordinary mapped tag key below 256 bytes. Hardpoint definition names must fit below 256 bytes. These are debug assertions before fixed-buffer copies, not verified release-build recovery rules. | Debug build EML-08/EML-09. | **differs**, XML loaders: strings are dynamically stored and are not rejected at those original name thresholds. Qualification needs threshold probes; do not copy unsafe fixed-buffer behavior. |
| ML-30 | The shared inspected XML reader rejects a document with a root but no child nodes, multiple roots or a reported element syntax error. Those are read/parse failures, distinct from an absent include. | Debug build EML-06/EML-08. | **differs**, XML loaders: `load_catalog` accepts an empty correctly named registry, while `src/data/xml_registry.cpp` `parse_document` handles malformed XML separately. Preserve the distinction and qualify empty-root compatibility: diagnostic enforcement (legacy EAWR-793), regression coverage (legacy EAWR-799). |

Hardpoint category policy is tracked separately (legacy EAWR-1442); the original
name thresholds need safe compatibility validation (legacy EAWR-1443). Useful unknown
tag diagnostics should remain in the scope census (legacy EAWR-793).

## Lua lookup

| ID | FoC rule | Source | Ours today and module |
|---|---|---|---|
| ML-31 | General startup registers `Data/Scripts/Library/`, `GameObject/`, `FreeStore/`, `Miscellaneous/` and `Story/`, in that order under `Data/Scripts/`. These roots feed logical file lookup, so mod precedence comes from ML-05, rather than a second independent Lua mod-root order. Mode-specific additions are separate. | Debug build EML-11; L-03/L-07. | **differs**, Lua host integration: authoritative space AI admission/configuration is a bounded stock closure and does not admit every general/story root. The P0 host's configured paths do not establish space-session admission (legacy EAWR-1429). |
| ML-32 | Each script instance owns an isolated state and loaded-module set. Loading a module in another instance does not populate this instance's cache. | Debug-build Lua audit; L-01. | **same**, Lua host: `src/script/authoritative/scheduler.cpp` creates a state per instance. |
| ML-33 | `LUA_PATH` begins `./?.lua;./?.lc`; configured directories follow in insertion order, with `.lua` before `.lc` for each. A trailing slash is added only when neither slash kind ends the directory; duplicate directories compare exactly. A state copies the path at creation. | Debug build; L-03. | **differs**, Lua host: authoritative `bindings.cpp` `find_module` searches that pattern order but concatenates raw directories without normalization and does not publish `LUA_PATH` (legacy EAWR-817). |
| ML-34 | `require` uses the module name verbatim as the `_LOADED` key and substitutes it verbatim for each `?`. A truthy cached value returns immediately. The first loadable module executes; nil/no result becomes true, false is stored but does not count as a cache hit, and failure is not cached. | Debug build; L-03a. | **same** for these cache/execution rules, Lua host: authoritative `require_module`; available module bytes are restricted to a preloaded session manifest. |
| ML-35 | A found script that fails compilation stops module search with that error. Module execution temporarily publishes `_REQUIREDNAME`; no automatic in-progress cache entry breaks recursive same-name loads. | Debug build; L-03a. | **same** at this boundary, Lua host: authoritative `require_module` loads the first manifest hit, propagates failure, and sets/restores `_REQUIREDNAME`. |
| ML-36 | Lua file opens, including `loadfile` and `dofile`, use the logical file callback. `loadstring` reads supplied bytes. There is no separate native-filesystem authority implied by the Lua API. | Debug build; L-03b/L-07. | **differs**, Lua host: the P0 host routes file functions through VFS, while the authoritative sandbox provides `require` from a session manifest and removes `loadfile`/`dofile` (legacy EAWR-1429). |
| ML-37 | The inspected x64 installation selects text-source scripts from `64Patch.meg`; old compiled chunks fail its required 8-byte `size_t` ABI. | Debug-build chunk audit L-06 plus installed data; build-specific. | **same** for text modules admitted to the authoritative host. Older chunk conversion in `src/script/pglua.cpp` is a separate compatibility route, not evidence that FoC executes the old chunks. |
| ML-38 | A module dependency is looked up when the script requests it through the configured logical file interface. An otherwise valid dependency is not restricted to a fixed list of stock AI plan names. | Debug-build Lua audit; L-03a/L-03b/L-07. | **missing**, Lua host integration: `src/script/foc/tactical_ai_loading.cpp` `required_modules`/`selected_plans` and `src/skirmish/ai.cpp` admit a stock closure to the authoritative manifest; a mod's new dependency needs admission before execution. Preserve bounded, deterministic loading when extending that closure (legacy EAWR-1429). |

## Asset, text and audio admission

These rules end at asset acquisition. Rendering, voice allocation and runtime
event selection are owned by their behavior contracts.

| ID | FoC rule | Source | Ours today and module |
|---|---|---|---|
| ML-39 | A model request first uses a normalized name to search cached renderables, then emitters, with case-insensitive first-match selection. Only a miss with demand loading enabled attempts acquisition. The file loader tries the supplied name, then registered search-directory candidates in insertion order, using ordinary logical reads; a failed acquisition contributes no decoded resource. The request has a second constructed-name retry whose exact suffix is not established by this decompile. | Debug build EML-17. | **differs**, asset caches: `src/scene/scene_assets.cpp` probes a fixed model directory and caches successful and failed model loads by supplied logical path. There is no equivalent demand-loading gate or original normalized renderable/emitter lookup contract (legacy EAWR-1497). Decoder/render behavior remains separate. |
| ML-40 | The name-based texture loader tries the supplied name, then a `.dds` candidate constructed by its dot-token operation. A final lookup miss logs a warning and returns a lazily created shared default texture only when the caller requested fallback; otherwise it returns no texture. Decode failure is a separate boundary. | Debug build EML-15. | **differs**, asset caches: `src/assets/texture.cpp` opens exactly the supplied VFS path. Scene and viewer probes have separate policies: `src/scene/scene_assets.cpp` and `apps/viewer/src/unit_mode.cpp` add TGA then DDS candidates, while `world_ui_prepare.cpp` tries DDS first. There is no single qualified original name/default policy across these routes (legacy EAWR-1482). |
| ML-41 | The shared asset-name helper lowercases the name and removes dot suffixes within its leaf, stopping at a directory separator and retaining the directory portion. Texture requests check that normalized cache key before file loading and insert a returned resource; no resource means no insertion on this path. The subordinate texture finder tries its current candidate, then registered texture paths in order through ordinary reads. Its exact concatenation treatment needs a separate trace because call arguments are incomplete in the decompile. | Debug build EML-18. | **differs**, asset caches: scene caches and viewer texture caches use supplied or resolved logical paths, often including the extension, with route-specific failed-load retention. A correct VFS winner does not establish equivalent cache identity or miss behavior (legacy EAWR-1497). |
| ML-42 | A successful 3D asset acquisition reads one winning file into a memory-backed chunk reader. That stream dispatches model, animation and emitter chunks; the inspected path does not combine matching chunks from weaker copies of the file. Cached resource selection precedes acquisition under ML-39. | Debug build EML-17/EML-19. | **same** for whole-file byte acquisition, asset loaders: `src/assets/model.cpp` reads one VFS winner, and `apps/viewer/src/map_mode_particles.cpp` opens one selected file before `particles::load_alo`. Shared original cache dispatch, emitter/renderable identity and decode/application parity are not established by this bounded match. |
| ML-43 | Startup tries the requested `MasterTextFile_<language>.DAT`; if missing and the request is not English, it tries English, then the eleven language slots in enum order. If none opens, the debug path diagnoses/asserts and restores the original requested language. | Debug build EML-10. | **missing**, text asset loading: `src/data/ui/text_database.cpp` `text_database_path` and callers use a supplied language; there is no availability-selection helper (legacy EAWR-1443). |
| ML-44 | The chosen language database is opened and loaded as one complete file: count, record headers, UTF-16 text and ID bytes. The inspected loader does not merge missing mod keys from weaker database files. | Debug build EML-10. | **same** after selection, text assets: `load_text_database` reads one VFS winner and `TextLookup` reports an absent key. Missing-key presentation and collision selection are separate qualification questions. |
| ML-45 | The inspected map-loading entry opens the supplied map filename through an ordinary logical read, copies the selected file into a memory-backed chunk reader and loads that single stream. A failed open diagnoses/asserts and returns false if the assertion returns. This entry does not merge weaker maps or select another map on an open failure. | Debug build EML-20. | **same** at file acquisition, map assets: `src/assets/map.cpp` `load_map` opens the supplied VFS path and reports failure; map discovery, selected-name construction, chunk interpretation and battle startup need their own outcomes. |
| ML-47 | SFX event admission reads one winning registry and opens its `File` entries through the file service. The inspected startup keeps a list in admission order for parsing; it does not merge weaker copies of the same XML file. Preset application occurs during event parsing. | Debug build EML-12; audio audit BA-01/BA-02 (AU-01/AU-02). | **same** for whole-file selection and admitted-file order, XML/audio loading: `src/data/xml.cpp`, `src/presentation/audio/sfx.cpp`. Duplicate event binding, localized sample mounts and sample-cache fallback are not qualified by this row. |
| ML-48 | 2D audio startup adds fallback nonlocalized, current nonlocalized, fallback selected-language and current selected-language archives in that order as public archives. 3D startup adds fallback then current nonlocalized archives. A missing required registration diagnoses/asserts and returns false on the inspected path; optional archive validation has separate failure cleanup. These registrations feed the global archive history, rather than an independent per-layer audio winner. | Debug build EML-21. | **differs**, VFS/audio setup: `src/vfs/vfs.cpp` optionally mounts three conventional English/nonlocalized SFX archives per layer before its patch slots. There is no selected-language audio registration stage or corresponding required-registration outcome (legacy EAWR-1498). |
| ML-49 | The inspected audio-driver file callback constructs a logical file and performs its read-open. A miss disposes the file and returns failure; a successful open returns that file handle. It does not invent substitute sample bytes. Event/sample-cache decisions happen outside this callback. | Debug build EML-22. | **same** for byte-open success/failure, audio assets: `apps/viewer/src/battle_audio_prepare.cpp` `sample` opens VFS bytes before WAV decoding and records a missing or undecodable sample. Original event-to-sample paths and cache/reload parity remain unverified. |

## Capacity qualification

| ID | FoC rule | Source | Ours today and module |
|---|---|---|---|
| ML-50 | The inspected text loader requires a positive 32-bit record count and dynamically allocates record/text/ID arrays. Each text length must be below 4096 UTF-16 units and each ID length below 4096 bytes; the debug build asserts before reads into fixed temporary buffers. No universal maximum record count is established here. | Debug build EML-10. | **differs**, text assets: `load_text_database` accepts a valid zero-record layout and uses dynamic strings within a 512 MiB safety bound, without those per-record original thresholds (legacy EAWR-1443). |

A collection's initial reserve, an allocator's block size, an on-disk integer
width and a rejection threshold are different facts. Do not report a pool's
initial allocation as a maximum object count. The following are current project
bounds to measure independently in the scope check; they do not establish FoC
limits:

| Boundary | Current bound and code |
|---|---|
| Active archive tables | 4,000,000 filename/entry records; 256 MiB cumulative name bytes, `src/vfs/vfs_archive.cpp` |
| Archive manifest | 4 MiB, `src/vfs/vfs.cpp` `resolve_manifest_mount` |
| VFS asset read | 2 GiB, `src/vfs/vfs.cpp` |
| Portable model/animation/texture readers | 512 MiB file, 16,777,216 collection entries and 256 nesting levels, `src/assets/asset_internal.hpp` and the [asset contract](../../asset-formats.md) |
| Text database | 512 MiB, `src/data/ui/text_database.cpp` `load_text_database` |
| Hardpoints per tactical type | 255, `include/eawr/sim/tactical/durability.hpp`; validated in `src/sim/tactical/durability.cpp` |
| Ordinary abilities per tactical type | 2, `include/eawr/sim/tactical/abilities.hpp`; validated by unit and tactical loaders |
| Replay initial entities | 1,000,000, `include/eawr/sim/replay.hpp`; this is a replay bound, not an XML type-count limit |

No universal supported-mod size follows from these separate bounds. Measure
definition count, admitted script bytes, decoded assets, live entity/hardpoint
counts and peak memory separately.

## Evidence and unresolved qualification

Fresh evidence IDs describe these debug-build paths. The private evidence index
records the read-only query receipts and exact inspected sources; it is ignored.

| Evidence | Inspected boundary |
|---|---|
| EML-01 | Command-line token processing and mod-root registration |
| EML-02 | Alternate loose-root opener, registered roots and accepted prefix table |
| EML-03 | Steam published-ID activation and queried item selection |
| EML-04 | Ordinary/archives-only logical read dispatch and failure return |
| EML-05 | Startup manifest and patch registration sequence |
| EML-06 | Manifest value traversal, archive registration and public archive lookup |
| EML-07 | Archive path transformation and CRC-table lookup |
| EML-08 | Hardpoint master list admission, CRC file order and definition binding |
| EML-09 | GameObject leaf/nested tag dispatch and name/key assertions |
| EML-10 | Startup language availability selection and complete text database loading |
| EML-11 | General startup Lua directories and script path registration |
| EML-12 | SFX registry admission and ordered parsing list |
| EML-13 | Workshop result completion and FoC classification filter |
| EML-14 | Effect directory append/clear |
| EML-15 | Name/file texture entry points, DDS retry and optional shared default |
| EML-16 | Workshop item state gates, folder query and returned-path normalization |
| EML-17 | Cache-first model request, demand-load gate and ordered 3D file acquisition |
| EML-18 | Shared asset name normalization, texture cache and ordered texture finder |
| EML-19 | One-stream model/animation/emitter chunk dispatch |
| EML-20 | Supplied map file open and one-stream handoff |
| EML-21 | 2D/3D public audio archive registration and required-registration failures |
| EML-22 | Audio-driver file-open callback and byte-open failure |

Older AU/R/L citations refer to the named XML/unit/Lua/audio contracts and audits
linked below. Their retail observations qualify only the build and scenario
recorded there. No new retail run or real-mod install was performed for this walk.

The following are **unverified**, not claims that FoC follows our current policy.
The source for each is an unresolved query/capture boundary. Do not assign a
same/differs verdict until the original side is established.

| Question | Current module boundary | Evidence needed to settle it |
|---|---|---|
| Workshop service-state interpretation, download/subscription behavior and published dependency order | VFS/mod-root setup; direct paths are supported; ML-14 qualifies the inspected folder/state branches | Match the virtual service calls/state masks to their build-specific interface, then capture repeated `STEAMMOD`/`MODPATH` options with synthetic loose markers on a Steam-enabled installation. Folder acquisition and query selection alone do not establish dependency ordering. |
| Mode-specific Lua directory additions and the complete AI/story host capability surface | Lua host; authoritative space AI admits a bounded closure | Trace mode entry and script-root admission; synthetic same-name modules in general, space-AI and story roots establish the additional order. An `include` helper defined by a script must be audited as script code; it is not automatically a distinct host file API. |
| Model retry suffix, registered model/texture directory construction, particle-group acquisition, map discovery and runtime invalidation | ML-39/ML-41/ML-42/ML-45 qualify inspected acquisition/cache boundaries; `src/scene/scene_assets.cpp` caches existence, successful models and failed model loads by supplied path | Trace the remaining argument/registration consumers and particle-group opener, then replace/remove one synthetic asset per category. Record cache hits, requested candidates and winning bytes before decoding; cover dot/separator and extension collisions. A capped call tree and an incomplete C-library argument list do not establish the remaining filename construction. |
| Shader/effect search priority, include-relative paths and failed-compilation fallback | `docs/shaders.md`, renderer adapters and effect routes | Trace the effect path manager and compiler/include callback; use two distinct synthetic effects and an include collision. Activation registers mod shader/terrain directories, but registration alone does not prove the winning effect or archive visibility. |
| Other XML registry duplicate/inheritance policies, including audio presets/events beyond the cited audit | XML loaders/unit tables/audio | Trace each category's registration and inherited-data parser; collide two definitions and exercise earlier/later bases. ML-20 through ML-27 qualify GameObjects, not a universal XML inheritance engine. |
| Audio event-to-sample localized path selection, sample caching/reload and language-change registration | ML-48/ML-49 qualify startup archive registration and the file callback; `src/presentation/audio/` and viewer audio resources own later stages | Trace the sample selector/cache and language-change consumers, then collide/remove a synthetic WAV and record event resolution separately from lookup/decode. Startup registration does not establish language-change or missing-sample fallback behavior. |
| Text duplicate/CRC-collision identity, absent-key display and non-ASCII key folding | `src/data/ui/text_database.cpp` uses exact keys after CRC ordering and renders a missing key | Trace original text key lookup and capture synthetic duplicate/colliding/absent keys. Do not equate whole-file replacement with first/last duplicate record selection. |
| Universal object-type count, hardpoints per unit and allocator/pool exhaustion | XML/unit tables, tactical durability, asset/particle caches | Follow allocation/growth/exhaustion consumers, then run escalating synthetic counts while recording failures and peak memory. The inspected dynamic registries do not prove unlimited capacity; reserve/block sizes do not establish a cap. |
| Native path corner cases and release-build assertion consequences | VFS/XML/text diagnostics | Case-sensitive directories, non-ASCII names, internal dot components and duplicate/colliding archive names need isolated probes. Capture malformed and threshold inputs in a disposable test setup, recording whether the release build rejects, skips, continues or aborts. |

## Existing documentation and registry scope

The [VFS contract](../../vfs.md) states the current project ordering policy; its
third-party evidence is not by itself a debug-build qualification. The
[XML model](../../xml-model.md) correctly distinguishes physical inventory from
registry activation, but its later-definition policy differs from ML-21. The
[XML edge cases](../xml-registry-edge-cases.md) and
[debug-build audit](../debug-build-audit.md#xml-registry) agree with ML-19 through
ML-22. EaW-only admission and file-service addenda do not qualify uninspected FoC
paths. The [unit-data note](../../unit-data.md#value-rules) already records the
differences in ML-24 through ML-27. The [Lua contract](../lua-script-model.md)
agrees with ML-32 through ML-37; its P0 claims must not be applied to the
authoritative sandbox without checking that host.

The [asset format contract](../../asset-formats.md) establishes our VFS acquisition
and decoder bounds, with some format/presentation observations; it does not
establish every original asset-cache search path. The [shader contract](../../shaders.md)
and [shader behavior](../shader-translation.md) cover translation and approved
effect behavior, with original mod effect search priority missing there. The
[audio contract](../battle-audio.md) agrees with ML-47's admitted-file/preset
boundary. ML-48 adds startup audio archive registration missing there; original
sample-cache and language-change behavior remain unverified until the probes above.

The tag registry describes application coverage, not parser or override parity.
At the inspected main revision, `Variant_Of_Existing_Type` is applied for selected
space object classes, but that label does not qualify ML-20/ML-22. For the tags this
walk reads, unresolved application rows are:

| Tag and classes | Registry status | Existing owner |
|---|---|---|
| `Reserve_Spawned_Units_Tech_0`: SecondaryStructure, SpaceUnit, SpecialStructure, StarBase, UniqueUnit | todo: parsed without reserve application | fighter coverage (legacy EAWR-651) |
| `Squadron_Offsets`: Container | todo: destination not traced | fighter coverage (legacy EAWR-651) |
| `Starting_Spawned_Units_Tech_0`: SecondaryStructure | todo: not reached for this class in the M2 scene | fighter coverage (legacy EAWR-651) |
| `Variant_Of_Existing_Type`: TransportUnit | todo: carried hero/unselected transport metadata is touched without deploying its body | combat coverage (legacy EAWR-650) |

`Squadron_Offsets` and `Squadron_Units` are applied for Squadron, and
`Fire_Inaccuracy_Distance` is applied for HardPoint. Cinematic and land classes
remain outside this walk's implementation scope. A real-mod census must compare
every (class, tag path), including a known tag newly authored on another class;
the existing name-level inventory gate cannot establish that coverage.

## Implications for the real-mod scope check

Use a synthetic loading contract first, then legally installed real mods on a
test host. Keep game/mod bytes, asset names beyond the established documentation,
and rich provenance reports out of the repository. Loading a catalog successfully
does not prove registry completion, simulation application, script closure or
successful rendering.

Start each source-collision case with a fresh original process and a fresh project
VFS/session/cache. Record a cache hit separately from a file open. Removing or
replacing a file while an asset remains cached does not measure startup precedence;
runtime reload/invalidation needs its own qualified case.

Record independent outcomes for activation, effective file selection, registry
admission, object resolution, applied tags, script loading/execution, asset decoding
and battle startup. The required multiplayer colour constants remain a separate
setup gate; classify a missing colour as setup failure, not archive precedence.

The XML probes should replace a whole registry, replace one included file, define
the same object in two different includes, derive from an earlier name, reference
a forward base, and author empty/TBD scalar and inherited/repeated list values.
Report missing includes separately from malformed admitted files and bad registry
roots. Record both a loader return and each definition's usability.

The Lua probes should collide a library name with a mode-specific name, replace
the same logical script across mod roots, provide `.lua` and `.lc`, and exercise
nil/false/truthy module results, a syntax error and a dependency outside the
current session manifest. A scripts-only failure is not an XML coverage failure.

Record a synthetic marker's winning source at each stage rather than judging
loading order from a model's appearance. For file-selection probes, put different
marker bytes at the same logical path in the leaf loose tree, leaf archives,
parent loose tree, parent archives and retail sources; remove winners one at a
time. Test archive declaration order separately from root order. Test case and
separator variants independently from path escape and malformed input.

For assets, replace the same logical model, texture, particle, map, language
database and sound sample independently. Then remove an asset referenced by a
valid object and record lookup, decode and fallback outcomes separately. A
replacement text database needs both a retained key and an omitted stock key to
distinguish whole-file replacement from key merging. A shader file being visible
in the VFS is not proof that the renderer executes its effect. Keep shader
selection, translation/admission and rendering as separate results.

The report taxonomy should follow the scope-check plan:

| Outcome | Evidence to record |
|---|---|
| Built and working | Winning logical source, admitted object/script/asset, applied tag or successful run stage |
| Built but broken by mod input | The first failed contract stage, diagnostic and smallest synthetic reproduction |
| Not built yet | The missing original behavior and its module/gap owner, independently of successful parsing |
| Outside M2 | The mode or presentation scope that excludes it; preserve the finding for later compatibility work |

Rank follow-ups by the number and styles of mods each unblocks, after separating
parser failure from behavior application. A common tag marked applied in the
stock registry can still be broken by a different inheritance pattern or a newly
authored object class. Run the headless phases before a viewer battle so an
earlier loading failure is not hidden by a later fallback.

The known differences are compatibility work after M2. Link existing issues
rather than implementing them in this walk: object binding (legacy EAWR-792),
diagnostic enforcement (legacy EAWR-793), scalar/list parsing (legacy EAWR-273), Lua
directory policy (legacy EAWR-817), and registry regression coverage (legacy EAWR-799).

New scoped gap owners are the Lua dependency closure (legacy EAWR-1429), bounded
inheritance completion (legacy EAWR-1431), global file/manifest policy (legacy EAWR-1441),
hardpoint registry policy (legacy EAWR-1442), language/threshold qualification
(legacy EAWR-1443), Workshop activation (legacy EAWR-1444) and gallery chain admission
(legacy EAWR-1446), texture candidate/default policy (legacy EAWR-1482), effect roots
(legacy EAWR-1483), cache identity/demand loading (legacy EAWR-1497) and audio archive
setup (legacy EAWR-1498). All are linked by the tracking issue (legacy EAWR-1427).

For space-battle mod compatibility, the five highest-impact follow-ups are:

| Rank | Gap | Why it precedes a viewer battle |
|---|---|---|
| 1 | Global source/manifest policy (legacy EAWR-1441) | A loose-only or parent-dependent mod can fail setup or select the wrong bytes before any object is parsed. |
| 2 | First GameObject binding (legacy EAWR-792) | Two admitted definitions can select the wrong public type despite correct whole-file lookup. |
| 3 | Inheritance completion (legacy EAWR-1431) | Forward bases and same-name variants can produce different usable fields or unresolved objects. |
| 4 | Scalar/list override semantics (legacy EAWR-273) | A valid type can silently lose inherited fighters, offsets or numeric/boolean values. |
| 5 | Script dependency admission (legacy EAWR-1429) | A valid replacement script can fail when it requires a new mod helper outside the stock closure. |

This ranking concerns mods loaded into the space battle. It does not make
post-M2 compatibility fixes prerequisites for the existing stock M2 milestone.
