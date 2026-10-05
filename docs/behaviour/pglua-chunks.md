# PGLua Compiled-chunk Contract

Scope: the pinned 32-bit retail PGLua chunks and conversion to a native-width Lua 5.0.2 chunk. Hexadecimal fixtures here are synthetic.

Applicability: the pinned members are the compiled scripts of the older archives. The installed x64 game does not load them. Its chunk loader accepts only a `size_t` width byte of 8, with 8-byte string lengths, and every script has a `64Patch.meg` text source that shadows the compiled member in both the EaW and FoC profiles ([audit](debug-build-audit.md#pglua-compiled-chunks)). This contract serves the Phase 0 smoke fixture and corpus tooling; the FoC runtime compiles text sources.

## Header

All 584 pinned members have the same 22-byte header.

| Offset | Bytes/value | Meaning | Difference from upstream Lua 5.0.2 |
|---:|---|---|---|
| 0 | `1b 4c 75 70` | PGLua signature: escape followed by `Lup` | Upstream is `1b 4c 75 61`, escape followed by `Lua`. |
| 4 | `51` | PGLua format version marker | Upstream 5.0.2 is `50`. This does not mean the body is a Lua 5.1 chunk. |
| 5 | `01` | little-endian producer | Same value in the validated upstream x86 build. |
| 6 | `04` | byte width of C `int` | Same. |
| 7 | `04` | byte width of `size_t` | Same for x86; a native x64 re-encoding uses `08`. The FoC x64 loader accepts only `08`. |
| 8 | `04` | byte width of an instruction word | Same. |
| 9–12 | `06 08 09 09` | opcode, A, B, and C field widths in bits | Same. |
| 13 | `08` | byte width of a Lua number | Same: binary64. |
| 14–21 | `b6 09 93 68 e7 f5 7d 41` | little-endian binary64 format sentinel, approximately 31,415,926.535897933 | Same. |

A reader for this contract accepts this pinned header exactly. It must not interpret byte 4 as authority to use the Lua 5.1 prototype layout or opcode table.

The FoC loader checks the same fields with three differences: the version byte must equal `51` exactly, an endianness byte other than `01` makes it swap multi-byte values, and it compares only the integer part of the sentinel ([audit](debug-build-audit.md#pglua-compiled-chunks)).

## Scalars, strings, and counts

- Multi-byte retail values are little-endian.
- Serialized `int` fields are signed 32-bit values. Counts and line positions must be non-negative before allocation or multiplication.
- Instruction words are unsigned 32-bit values.
- Numbers are IEEE-754 binary64 values in the pinned corpus.
- A string starts with a 32-bit unsigned byte count. Zero means no stored string. A nonzero count includes the required trailing zero byte; the preceding bytes are length-bearing data and are not assumed to be UTF-8.
- For a nested prototype, a zero source-string count inherits the enclosing prototype's source name.
- The original format stores no quota. Project policy requires a 64 MiB input limit, 16 MiB string limit, prototype depth 128, and at most 1,000,000 aggregate entries in each instruction, constant, prototype, local-variable, upvalue-name, and line-info category. These are clean-tool rejection limits, not claims about the original loader.

## Prototype record order

Each file contains one root prototype. Nested prototypes recursively use the same record in depth-first order.

| Order | Field | Encoding |
|---:|---|---|
| 1 | source name | PGLua string; zero inherits parent |
| 2 | first source line | signed 32-bit integer |
| 3 | function persistence identifier | signed 32-bit integer; PGLua addition absent from upstream 5.0.2 |
| 4 | upvalue count | one byte |
| 5 | fixed parameter count | one byte |
| 6 | variable-argument flag | one byte |
| 7 | maximum stack slots | one byte |
| 8 | source-line vector | signed 32-bit count, then that many signed 32-bit entries |
| 9 | local-variable vector | signed 32-bit count; each entry is a string, start program counter, and end program counter |
| 10 | upvalue-name vector | signed 32-bit count, then that many strings |
| 11 | constant vector | signed 32-bit count, then tagged constants described below |
| 12 | nested prototypes | signed 32-bit count, then recursive records |
| 13 | instruction vector | signed 32-bit count, then that many 32-bit words |

This is the Lua 5.0.2 section order, not the later Lua 5.1 order. Retail members are stripped: across the pinned corpus, the source-line, local-variable, and upvalue-name vectors are empty, while first-line values remain. A conforming reader must still implement all three vectors because synthetic or other same-format inputs can contain them.

### Persistence identifier

The additional field is not a last-line number. In every pinned member, identifiers are positive and equal `1, 2, …, N` in prototype depth-first order, with the root equal to 1. The FoC loader reads the field but ignores it on an ordinary load. Each new prototype takes the first free slot of its state's prototype list, numbered from 1 in creation order; only a whole-state load uses the file values ([audit](debug-build-audit.md#pglua-compiled-chunks)). The file sequence therefore matches the ordinary numbering only for the first chunk loaded into a fresh state.

For the Phase 0 smoke path, retain the value in metadata for diagnostics, validate the observed ordering, then omit it from a standard Lua 5.0.2 chunk. Save-game/state restoration is out of scope and must report unsupported. A later persistence implementation must preserve the mapping and separately implement the whole-state format; ordinary chunk conversion alone cannot provide it.

## Constants

Each constant begins with a one-byte Lua type tag.

| Tag | Payload | Pinned-corpus count |
|---:|---|---:|
| 0 | none; value is nil | 168 |
| 3 | one binary64 number | 7,370 |
| 4 | one PGLua string | 63,469 |

No other constant tag occurs in the pinned 584 members. Boolean literals are represented by instructions, not constant-table entries. A reader rejects any other tag rather than guessing its payload size.

## Instruction encoding and opcode evidence

The 32-bit word uses the upstream Lua 5.0.2 layout: opcode bits 0–5, C bits 6–14, B bits 15–23, and A bits 24–31. `Bx` occupies bits 6–23; signed `sBx` is `Bx - 131071`. An RK operand below 250 denotes a register and an RK operand at least 250 denotes constant index `operand - 250`.

In the pinned corpus, IDs 4, 8, 11, 16 and 33 are absent. IDs 28 (`FORLOOP`, three FoC occurrences) and 32 (`SETLISTO`, eleven EaW occurrences) occur without an exact paired upstream body. The FoC VM runs them as the upstream `FORLOOP` and `SETLISTO` ([audit](debug-build-audit.md#pglua-compiled-chunks)). Every other listed ID has at least one exact paired prototype body.

| ID | Upstream 5.0.2 name | Form |
| ---: | --- | --- |
| 0 | MOVE | ABC |
| 1 | LOADK | ABx |
| 2 | LOADBOOL | ABC |
| 3 | LOADNIL | ABC |
| 4 | GETUPVAL | ABC |
| 5 | GETGLOBAL | ABx |
| 6 | GETTABLE | ABC |
| 7 | SETGLOBAL | ABx |
| 8 | SETUPVAL | ABC |
| 9 | SETTABLE | ABC |
| 10 | NEWTABLE | ABC |
| 11 | SELF | ABC |
| 12 | ADD | ABC |
| 13 | SUB | ABC |
| 14 | MUL | ABC |
| 15 | DIV | ABC |
| 16 | POW | ABC |
| 17 | UNM | ABC |
| 18 | NOT | ABC |
| 19 | CONCAT | ABC |
| 20 | JMP | AsBx |
| 21 | EQ | ABC |
| 22 | LT | ABC |
| 23 | LE | ABC |
| 24 | TEST | ABC |
| 25 | CALL | ABC |
| 26 | TAILCALL | ABC |
| 27 | RETURN | ABC |
| 28 | FORLOOP | AsBx |
| 29 | TFORLOOP | ABC |
| 30 | TFORPREP | AsBx |
| 31 | SETLIST | ABx |
| 32 | SETLISTO | ABx |
| 33 | CLOSE | ABC |
| 34 | CLOSURE | ABx |

No opcode greater than 34 occurs. The nine-file required smoke closure uses only paired opcodes. Consequently the smoke loader does not depend on IDs 28 or 32, and no absent opcode needs to be invented.

## Conversion contract

### Direct 32-bit conversion

For a validated pinned PGLua chunk, emit the standard Lua 5.0.2 signature and version, retain the remaining header ABI fields, and copy every prototype field in order except the persistence identifier. This shortens the file by exactly four bytes per prototype. Do not renumber constants, prototypes, registers, or instructions.

All 584 converted retail chunks were accepted with binary validation enabled by an upstream 32-bit Lua 5.0.2 build. This is a structural and bytecode-validity result, not a claim that every engine API exists.

### Native-width conversion

A 64-bit upstream Lua build rejects a raw 32-bit chunk because the `size_t` width differs. The viable cross-platform path is to decode the fixed retail representation, then emit a standard Lua 5.0.2 chunk in the target VM's native ABI:

1. write the standard signature/version and target endianness/width header;
2. encode every string length using the target `size_t` width;
3. encode integers, instructions, and binary64 numbers in the target format;
4. omit the persistence identifier but preserve all other values and vector order;
5. load through upstream `lua_load` with bytecode validation enabled.

Re-encoding all 584 members with 8-byte `size_t` produced 584/584 successful parses in a 64-bit upstream Lua 5.0.2 validator. Fresh code must still test actual execution through the Lua host's surface.

## Pinned upstream build contract

Use the exact PG-UP-01 archive. For the validated Phase 0 configuration:

- use the default `lua_Number` (`double`); do not define `LUA_NUMBER` or `LUA_USER_H`;
- keep `int` and `Instruction` at 32 bits;
- do not define `TRUST_BINARIES`; the upstream code validator must run;
- leave `LUA_COMPATUPSYNTAX` and `LUA_COMPATUPVALUES` disabled;
- disable dynamic loading, pipes, and temporary-name support in the embedded build (`USE_DLL=0`, `USE_POPEN=0`, `USE_TMPNAM=0`);
- open only the host-approved base, string, and table libraries described in the Lua host contract;
- use an IEEE-754 binary64 toolchain without fast-math transformations.

## Rejection contract

Reject before handing bytes to the VM when any of the following is true:

- header signature, version marker, endianness, widths, field-bit sizes, number width, or numeric sentinel differs from the pinned header;
- an integer count is negative, exceeds a configured quota, overflows a byte calculation, or extends beyond the input;
- a nonzero string is missing its trailing zero or exceeds the input/quota;
- a constant tag is not 0, 3, or 4;
- nesting exceeds the configured limit;
- a persistence identifier is nonpositive, duplicated, or not the observed depth-first sequence;
- an RK constant index, prototype index, jump target, register, upvalue, or instruction violates upstream Lua 5.0.2 validation;
- bytes remain after the root prototype;
- a retail-profile load or corpus run lacks a manifest profile/archive hash, or the requested identity does not match it.

Diagnostics report the logical source identifier, byte offset, prototype path, field, and reason. They never echo retail string or code bytes.

Retail identity validation does not apply to standalone original synthetic chunks. S-01 through S-11 intentionally carry no retail profile or archive identity and are accepted or rejected solely by their declared synthetic bytes, format rules, and configured safety limits. Supplying a retail profile switches the load into the manifest-verified path; omitting one must not let bytes claim membership in a retail corpus.

## Original synthetic byte fixtures

### S-01: minimal valid PGLua chunk

This 78-byte fixture was created by compiling the original source `return` with permitted upstream Lua 5.0.2, stripping debug data, substituting the PGLua header, and inserting root persistence ID 1. SHA-256 is `cb38e204cf10865666e03cf7a23242629499dff189cd213e41906ba2cf4f06c7`.

```text
1b 4c 75 70 51 01 04 04 04 06 08 09 09 08 b6 09
93 68 e7 f5 7d 41 08 00 00 00 3d 28 6e 6f 6e 65
29 00 00 00 00 00 01 00 00 00 00 00 00 02 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 02 00 00 00 1b 80 00 00 1b 80 00 00
```

Expected decode: source `=(none)`, first line 0, persistence ID 1, zero upvalues/parameters, non-vararg, maximum stack 2, empty debug/constant/prototype vectors, and two instruction words whose opcode ID is 27 (`RETURN`). Conversion yields a 74-byte standard x86 Lua 5.0.2 chunk by changing the header and omitting the four identifier bytes.

### S-02: direct and member call-analysis vector

This original synthetic program calls a global with number 7, calls method `Move_To` on global `unit` with string `A`, copies global `Dynamic` into a local, and calls the local. Its PGLua-form fixture is 181 bytes, SHA-256 `143477bb20d0e90d322c2558853dfc4f3c1eaf183a46ec0b957f22d53f8efb7a`. The constant vector is, in order: `Engine_Do`, number 7, `unit`, `Move_To`, `A`, `Dynamic`.

The instruction-vector bytes, including its count, are:

```text
0b 00 00 00
05 00 00 00 41 00 00 01 59 00 01 00 85 00 00 00
4b 3f 00 00 01 01 00 02 59 80 01 00 45 01 00 00
00 00 00 01 59 80 00 01 1b 80 00 00
```

Expected analysis, in instruction order:

1. direct global call `Engine_Do`, one explicit argument;
2. method call `Move_To`, receiver provenance `global unit`, one explicit argument;
3. direct global call `Dynamic`, reached through a register copy, zero explicit arguments.

`unit`, `A`, and other string constants are not calls. An analyzer that scans constants alone fails this fixture.

### S-03 through S-11: required rejection mutations

Apply each mutation independently to S-01 unless stated otherwise: change signature byte 3 to `61`; change version byte 4 to `52`; change endian byte 5 to `00`; change `size_t` width byte 7 to `08`; change number-width byte 13 to `04`; delete the final byte; append one zero byte; replace bytes 38–41 with zero; or replace the source terminator at byte 33 with a nonzero byte. Each mutation must be rejected for its specific field, truncation, trailing-data, identifier, or string reason. For S-02, replacing constant tag byte 62 with `01` must be rejected as an unknown constant tag.

<a id="static-compiled-script-call-analysis-for-p0-08"></a>

## Static compiled-script call analysis for the Lua call inventory

Constants alone cannot satisfy vanilla coverage. A clean analyzer can use the validated decoder and the official Lua 5.0.2 instruction contract without executing scripts:

1. build a control-flow graph per prototype using validated jump/test behavior;
2. propagate abstract register provenance such as unknown, literal, global name, table field, or nested closure, merging to unknown when paths disagree;
3. apply the official effects of MOVE, GETGLOBAL, GETTABLE, SELF, CLOSURE, and assignments to provenance;
4. only emit a call site at CALL or TAILCALL, using the callable register's current provenance and the instruction's argument-count convention;
5. recognize script-defined globals when a closure/literal is assigned to a global, but retain conditional or cross-module ambiguity as unresolved;
6. emit logical member path, prototype persistence ID, program counter, opcode, classification basis, and unresolved reason; stripped retail chunks have no exact source line/column, so never fabricate them;
7. keep dynamic table lookup, merged register state, vararg/open calls, and overwritten globals explicitly unresolved.

This route accounts for every compiled vanilla member. It complements, rather than replaces, source parsing for loose scripts. The approved inventory schema version 2 records compiled locations as persistence `prototype_id` plus zero-based instruction `pc`, with line and column null; claiming source coordinates for stripped vanilla chunks would be false precision. The exact row shape and deterministic JSON policy are project policy, not format facts.

## Required smoke subset and optional persistence

The pinned smoke script and eight module dependencies in [lua-script-model.md](lua-script-model.md) use constant tags 0, 3, and 4 and opcode IDs `0, 1, 2, 3, 5, 6, 7, 9, 10, 12, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 34`. Every one has paired opcode evidence. A fresh Lua host implementation may therefore make ordinary compiled-chunk loading required while explicitly leaving these features optional and unsupported:

- whole-state dump/undump;
- save-game restoration of coroutine stacks, userdata, and event state;
- resolving saved function references through persistence IDs.

Opcode IDs 28 and 32 need no special support: the FoC VM runs them with the upstream semantics (PG-G02).

## Remaining gates

| Gate | Unknown |
| --- | --- |
| PG-G01 | Runtime execution equivalence after native-width conversion |
| PG-G02 | Resolved: IDs 28 and 32 run the upstream semantics |
| PG-G03 | Whole-state serialization and function-reference restoration |
| PG-G04 | Fresh implementation qualification |

Readiness: the compiled-module structure, resource-policy boundary, version-2 compiled location shape, and required smoke opcode subset are ready for clean implementation. The former “unknown PGLua loader” gate is narrowed to execution conformance and optional persistence—not corpus decoding. Compiler choice, quotas, and diagnostic serialization are coordinator policy rather than original-engine research questions.
