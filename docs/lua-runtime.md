# Lua 5.0.2 bounded runtime

Phase 0 embeds the exact upstream Lua 5.0.2 archive with SHA-256
`a6c85d85f912e1c321723084389d63dee7660b81b8292452b190ea7190dd73bc`.
The sources are under `third_party/lua/`, with an independent CMake integration
file and one documented portability patch: `src/llimits.h` uses `unsigned int`
for the 32-bit instruction word on all supported targets. Upstream's
`unsigned long` becomes 64-bit on Linux LP64 and fails the converter's four-byte
instruction ABI. The loader's header validation remains enabled. Configuration
is fully offline; attribution is in `THIRD_PARTY_NOTICES.md`.

## Build and library surface

The C runtime uses upstream `double` Lua numbers, 32-bit `int` and instruction
words, and target-native `size_t`. `TRUST_BINARIES`, `LUA_USER_H`,
`LUA_NUMBER`, `LUA_COMPATUPSYNTAX`, `LUA_COMPATUPVALUES`, and
`LUA_USE_APICHECK` are not defined. The initial profile is `/Od /fp:strict` on
MSVC-compatible compilers and `-O0 -fno-strict-aliasing -ffp-contract=off
-fno-fast-math` on GCC/Clang. Upstream binary validation remains enabled.
The C++ PGLua converter directly includes the standard `<bit>` header used by
`std::bit_cast`; clean Clang 18/libc++ qualification must not depend on a
command-line forced include.

Only the VM, auxiliary library, base/coroutine library, string library, and
table library are linked. The I/O, OS, math, debug, and dynamic-loader library
translation units are not linked, so their `USE_DLL`, pipe, and temporary-name
paths are absent. After opening base, the host replaces `require`, `loadfile`,
and `dofile` with VFS-only implementations. `loadstring` remains the upstream
byte-string operation and performs no file access. No `package` table or
fork-security members are synthesized.

## Script host boundary

`ScriptHost` exposes `load`, `start`, `resume`, `dispatch`, `destroy`, and
`register_api`. Every loaded instance owns a separate Lua state, globals,
`_LOADED` table, retained coroutine slots, event queues, registrations, and
function references. Destroy is idempotent; references contain their owning
instance and are rejected after destroy or across instances.

Host values own byte strings and sequential tables. Lua binary64 numbers are
checked for finiteness and range, rounded once to binary32 at the C++ boundary,
and widened back to binary64 when returned to Lua. Associative/holey and cyclic
tables fail with `EAWR-SCRIPT-0002`; they are never flattened. Functions,
coroutines, and host callables use instance-tagged references.

Registered APIs are callable userdata values, which supports both ordinary
global calls and Lua-visible `Function_Call`. An empty registered callback is a
known-but-unimplemented binding and fails at invocation with
`EAWR-SCRIPT-0005`; it is not a nil-returning stub. Calls and arguments are
logged. Host-to-Lua lookup of an absent or nonfunction global instead produces
the specified no-result outcome.

The host exposes only infrastructure implementations for the reviewed thread,
event, and logical-loading contracts. Coroutine slots increase monotonically,
are retained through yields, and accept only exactly one yielded boolean as the
scheduling protocol. Event callback and parameter queues remain independent.
The observed mutation restart rule is used by `Register_Event`/`Cancel_Event`;
this does not claim that unrelated engine event families share that rule.

`WideString` stores `char16_t` units independently of platform `wchar_t`.
Length and slicing count UTF-16 units, including split surrogate pairs, and
mutators return a distinct wrapper of the post-mutation value. ASCII conversion
is implemented. Non-ASCII narrow conversion and the native-width numeric
not-found sentinel remain explicit unsupported boundaries.

## Logical loading and PGLua

All script and module bytes are obtained from the supplied `Vfs`. Module cache
keys and `?` substitutions preserve the request spelling exactly. Search begins
with `./?.lua;./?.lc`, then each configured directory's `.lua` and `.lc`
patterns in insertion order. Nil/no module results become boolean true; false
and failures do not become truthy cache hits. `loadfile` and `dofile` use the
same VFS callback and cannot fall back to a native path.

The clean PGLua decoder accepts the pinned 22-byte header, fixed little-endian
retail scalar widths, tags 0/3/4, the documented prototype order and positive
depth-first persistence IDs. It validates counts, strings, nesting, operands,
indices, jump targets, trailing bytes, and project quotas before calling Lua.
It then emits a standard Lua 5.0.2 chunk with target-native string lengths and
omits only the persistence ID. Opcode IDs 28 and 32, unknown opcodes, whole-state
persistence, suspended-state restoration, and saved-function fixups are not
executed and report `EAWR-SCRIPT-0006`.

Retail-profile conversion requires the pinned archive identity. Standalone
synthetic chunks deliberately omit that identity and cannot claim retail
membership. The host limits a chunk to 64 MiB, a string to 16 MiB, prototype
depth to 128, each aggregate vector category to 1,000,000 entries, an instance
to 4,096 coroutine slots, each event queue to 1,000,000 entries, and each
top-level operation to 10,000,000 VM instructions. These are new-host safety
limits, not claims about the original executable.

## Diagnostics and unsupported capabilities

Stable codes distinguish invalid instance/value, load/parse, ordinary Lua
execution, missing engine API, unsupported feature, and resource limit. Runtime
diagnostics add instance, coroutine, operation, API name, traceback, source kind,
and compiled prototype/zero-based PC when established. Stripped retail chunks
never receive fabricated lines.

Each VFS chunk load retains the winning `AssetRecord`. Load and parse failures
use its canonical logical path and available source ID, including when a higher-
precedence mod shadows a base module. Protected Lua calls capture errors before
stack unwinding and attribute runtime or missing-binding diagnostics to the first
mapped Lua frame, preserving that frame's line and traceback. If no live frame
can be matched to a loaded VFS asset, path/source fields stay absent rather than
being replaced with the root script's provenance. The pinned retail smoke keeps
its independently established PGLua prototype/PC context.

Fork security, state pooling, whole-state persistence, multi-value yield
selection, non-ASCII narrow/wide conversion, and the wide not-found sentinel are
unsupported. FoC can be selected by the CLI as a general loader profile, but no
FoC smoke script or oracle is qualified.

## Determinism boundary

Lua 5.0.2 remains outside authoritative simulation state. The authoritative
numeric profile is a separate build of the same sources with integer-only
binary64 numbers ([numeric profile](lua-numeric-profile.md)); it does not change
this runtime. Local coroutine and
event ordering does not make table iteration, binary floating-point behavior,
randomness, time, or engine collection order lockstep deterministic. Before any
gameplay integration, a separate reviewed strategy must define authoritative
state serialization, deterministic numeric/RNG/time replacements, iteration
ordering, cross-target conformance, and replay/hash participation. Until that
gate closes, the script host is a bounded compatibility and tooling runtime.

## Integration decisions

Reject callback re-entry once an instance is closing. Nonfinite boundary numbers,
cross-instance references and unsupported values fail explicitly; no generic opaque
pointer API exists without a registered signature. Only Suspend_AI(number=1) is a
gameplay smoke stub: it logs and returns zero values; Lock_Controls(1) is the next
named failure. Declaration presence does not authorize invented implementations.
Static PGLua analysis retains uncertain instructions/dataflow as unresolved with
counts and reasons. Use independent upstream-Lua or handwritten synthetic fixtures;
private researcher implementations and extracted bytecode are not test inputs.
