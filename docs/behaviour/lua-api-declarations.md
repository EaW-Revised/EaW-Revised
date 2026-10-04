# Lua-visible Declaration Inventory Contract

The machine-readable schema-v1 index is [lua-declarations.json](../../plan/inventories/lua-declarations.json).

## Purpose

The inventory answers one bounded question: which literal Lua-visible names are declared by the selected registration sites in GlyphX reference revision `9541b8c183e540d05bbcf199921f6d71c33181e9`?

It is metadata, not runtime code and not a claim about behavior. It supports stub planning, missing-API diagnostics, coverage reports, and later per-API research. It does not authorize generating implementation bodies from the reference.

## Declared search scope

Paths are relative to `reference/GlyphX-Reference/Project1/Code`:

- `PGLua/*.cpp`
- `RTS/AI/LuaScript/*Wrapper.cpp`
- `RTS/AI/LuaScript/Commands/GlobalCommands.cpp`

The generated index contains 376 deterministic records. A record contains:

- `exposed_name`: exact visible spelling;
- `call_kind`: `global` or `method`;
- `receiver_type`: the declared receiver for a method, otherwise null. Receivers carry neutral names (`taskforce`, `story_event`, `wide_string`, ...); only one the game's own Lua scripts spell keeps its name (`GameObjectWrapper`, `GameObjectTypeWrapper`, `PlayerWrapper`). The FoC index uses the same names.

The committed index keeps only the visible Lua names, which are the mods' public API. The reference release's internal registration symbols and its file and line locations stay in the private extraction under ignored `out/research` (fresh public-repository release plan, audit A-4); a record that once carried them has neither key now.

The top-level object pins the schema version, reference name/revision/code root, declared glob scope, and limitations. Records sort case-insensitively by visible name and then by stable metadata fields.

This declaration index is schema version 1 and remains a metadata input. It is not the consumer/static Lua call inventory: that output is schema version 2 with text locations expressed as line/column and compiled PGLua locations expressed as persistence `prototype_id` plus zero-based `pc`. The [compiled-chunk contract](pglua-chunks.md) defines how those compiled locations are obtained without fabricating stripped source coordinates.

## Interpretation rules

| Rule | Behaviour |
|---|---|
| D-01 | A record means a literal binding was found in the declared scope at the pinned revision. |
| D-02 | The same visible name may have more than one record when it has different receivers, call kinds, or registration symbols. Consumers must not collapse those records into one behavioral API. |
| D-03 | No record means only “not found by this literal-name scan in this scope.” It does not prove the engine lacks the name; dynamic names, other registration areas, mode-gated setup, aliases, and native VM functions may exist. |
| D-04 | A registration does not establish signature, accepted value types, return count, side effects, determinism, game-mode availability, or failure behavior. Those require a separate reviewed behavior note. |
| D-05 | A consumer may use the inventory to emit “declared but not implemented,” “implemented,” or “outside indexed scope.” It must not label an inventory miss “unsupported by the original engine.” |

## Relation to FoC

The GlyphX index describes its reference revision, not FoC. The separate names-only [FoC registration index](../../plan/inventories/foc-lua-registrations.json) records 472 debug-build registrations: 145 globals and 327 receiver-specific members. The FoC debug build was compared with GlyphX in the [behaviour audit](debug-build-audit.md#lua-api-declarations) (debug-build behaviour audit):

- every global in the index is registered in FoC;
- the 22 wide-string methods (`append` through `substr` on the `wide_string` receiver) are not; FoC has no wide-string receiver;
- FoC registers 20 globals and 61 member names that the index lacks, among them the context globals `AITarget`, `Budget`, `FreeStore`, `Object`, `PlayerObject` and `Target`, the `GUI_*` functions, and every TaskForce method.

The FoC and Remake profiles of `tools/inventory/lua_inventory.py` use the FoC index for stub planning and missing-API reports; the base EaW profile continues to use GlyphX. A FoC index entry establishes a visible name on a receiver, not a signature or mode-independent implementation. A miss remains an index miss, not proof that an original engine lacks an API.

To regenerate the FoC index, run [FocLuaRegistrationDump.java](../../tools/ghidra/FocLuaRegistrationDump.java) in Ghidra against the FoC debug `StarWarsI.exe` with two script arguments: an ignored TSV output path and a names file under ignored `out/research/`. The names file gives the debug build's member and global registration functions and their namespaces as `member_function=`, `member_namespace=`, `global_function=` and `global_namespace=` lines, so the engine's names stay out of the committed script (runtime-only research symbol lookup); read them off the debug build's symbol tree. Headless: `analyzeHeadless <project dir> <project> -process StarWarsI.exe -noanalysis -scriptPath tools/ghidra -postScript FocLuaRegistrationDump.java out/research/foc-lua-registrations.tsv out/research/lua-registration-names.txt`. Then run `python tools/generate_foc_lua_index.py <TSV path> --receiver-names out/research/lua-receiver-names.tsv`; that ignored table maps each debug-build receiver to its neutral name, so the engine's class names stay out of the committed index. The extractor follows the registration calls and records literal visible names with receiver classes, including the two globals installed during Lua state setup (`LUA_PATH`, `Script`). The Python step checks the 20/61/22 visible-name differences against GlyphX before writing the names-only JSON. Keep the TSV and any decompiler output under ignored `out/research`.

`DECL-SCAN-01` is the private, reproducible literal-registration extraction performed against the pinned revision. The extractor and any inspection fragments remain under ignored `out/research`; only metadata is distributed.

## Original expected-outcome cases

### C-01: exact lookup

Given one metadata record named `Synthetic_Action` and a query with the identical spelling.
Expected: it reports the record and its location; it does not infer a signature. Covers D-01 and D-04.

### C-02: absent name

Given a query `Synthetic_Missing` with no record.
Expected: it reports “outside indexed scope” and names the pinned revision and scope. It does not claim the original engine lacks the name. Covers D-03 and D-05.

### C-03: same spelling on two receivers

Given two method records with the same visible name but distinct synthetic receiver types.
Expected: both receiver-specific records remain independently addressable. Covers D-02.

### C-04: repeatability

Given an unchanged pinned reference tree and extractor.
Expected: canonical JSON serialization and record count are identical. Covers D-01.
