# Owned shader stage translation

The optional stage compiler translates a validated effect program into owned
SPIR-V, desktop GLSL 330 and typed reflection. It is an activation-worker API;
there is no renderer call, automatic package loading, battle-time compilation,
technique selection, Godot admission or persistent cache here. Developer builds
and the hand adapters do not need the optional package. See
[the dependency package](../shader-runtime-package.md) for its pins and licences.

`CompilerSession` consumes a runtime which attests the complete reviewed file
identity table. A deliberately replaced runtime is not accepted merely because
it reports the same versions. The session owns the module references; contexts,
compiler allocations and messages are released through their originating public
C interfaces. Only a successful compiler return transfers compiled-code
ownership: the pinned library can leave already freed output fields on failure.
Messages have their own ownership and are released on both outcomes. Neither
DLL-owned pointers nor raw messages escape the wrapper.

`prepare_stage` runs before cache lookup. It validates the stage envelope and
options, scans descriptors, resolves the complete binding plan and returns an
immutable request identity plus canonical options bytes. An empty plan assigns
bindings in source-register order, then assigns the first unused binding to each
sorted image/sampler pair. An explicit plan must cover the same descriptors and
pairs, without duplicate targets. Desktop GLSL uses descriptor set zero. Constant
register banks have typed float/integer/boolean kinds and bank index zero; they
are not individual `cN`, `iN` or `bN` parameters.

`translate_stage` receives those resolved options on a miss. Input bytes and
profile must agree with the effect reader. vkd3d emits SPIR-V using its measured
API-version option 1.0 and debug stripping disabled; its SPIR-V target is explicitly
Vulkan 1.0 with entry `main`, no optional extensions, parameters, swizzles or
dual-source blending. The independently verified library/public-header version
remains 2.1. These are separate identities. SPIRV-Cross uses its shared C API
0.61.0, GLSL version 330, ES and Vulkan semantics disabled, 420pack disabled,
uniform blocks retained and combined image samplers built explicitly. Depth and
vertex-Y changes are explicit variants supplied by the admission owner.

The finite capability IDs are 1 bytecode-to-SPIR-V, 2 GLSL 330 output, 3 typed
resources, 4 typed interfaces and 5 combined-sampler relationships. They describe
wrapper operations, not universal support for every profile or shader. Required
capabilities are sorted and unique. Variant IDs select exactly one in each group:
1 desktop GLSL 330; 10 preserve or 11 fix depth; 20 preserve or 21 flip vertex Y;
30 uniform blocks; 41 combined image samplers. Unknown, duplicate, conflicting or
incomplete choices are refused. Plain uniforms and preserved sampler form are
not accepted variants.

Reflection owns a type graph, all array dimensions, scalar widths, block/member
offsets and sizes, available array/matrix strides, row-major flags, image shapes,
resource bindings, flattened interface member paths, builtins, interpolation and
component masks. Absent metadata remains absent. Combined resources are
synthesized local table entries with explicit image/sampler relationships, not
pretend original SPIR-V IDs. Unsupported kinds are refusals. In particular,
ray-query/acceleration type instructions are rejected before Cross; its pinned
public C API does not implement the ray-query resource query.

Source register/semantic associations are explicitly unavailable unless a proven
public metadata association exists. The wrapper never derives them from emitted
GLSL names. The effect reader's constants/declarations remain the source authority;
the adapter owns documented implicit legacy output derivation and admission.
Source-interface policy, effect defaults and CTAB register storage remain separate.

Every reflected bound resource must match the resolved individual plan, and every
combined resource must match its combined target and original image/sampler pair.
Contradictory backend reflection is a `reflection_failure`. Fresh result validation
uses `reflection` / `backend_failure`; cache decoding always uses `cache_read`,
with `invalid_cached_result` for an invalid entry or the actual budget/cancellation
reason. P6 projects the wrapper's `compiler_failure` to P8's `compile_failure`;
these are typed enum mappings, never interpretations of raw compiler text.

The public compiler's `INVALID_SHADER` return (-4 in the pinned public header)
is a distinct `invalid_shader` reason. Other nonzero returns, including memory
exhaustion, remain `compiler_failure`. Both project to the same activation
`compile_failure`; neither changes the fallback or permits material admission.
The activation owner alone may persist the exact single error diagnostic for
`invalid_shader`. Its separate `material-stage-refusal/1` key binds the freshly
verified package, ABI, capability, stage, options, schemas and outer input identity.
The only accepted payload is the fixed canonical typed marker
`eawr-stage-refusal/1:stage_compile:invalid_shader:error`; it reconstructs the
same bounded wrapper detail and absent location, retaining no compiler text.
Malformed entries are misses. Cancellation, budgets, unavailable dependencies,
other compiler statuses and all other refusals are never persisted. A warm
refusal increments cache hits and performs no translation or source compilation.

All successful values are independent of session/context lifetime. To reconstruct
the borrowed effect-program view, call `view_stage_program` over the artifact's
immutable owned stage bytes and retain those bytes while using the view. The
decoder validates the same reader contract before publishing a result.

## Cache envelope

The cache stores owned bytes and supplies the freshly prepared current request
to `decode_stage_result`. Matching reported versions or a self-consistent old
payload is insufficient: full package/lock, declared ABI, capability, options and
input-stage identities must match the current request. The ABI digest includes
the actual declared C signatures, layouts, calling convention and ownership;
the package digest includes the matching headers and complete import closure.
The outer cache owner separately binds VFS/source/include/macro provenance and
admission/engine identity. The decoder does not resolve files or choose routes.

Version 1 has a 40-byte little-endian header: bytes 0..7 `EAWRSR01`, u32 version
at offset 8, u32 manifest length at 12, then u64 lengths for compiled stage at 16,
SPIR-V at 24 and UTF-8 GLSL at 32. SPIR-V length is divisible by four. The canonical
UTF-8 JSON manifest follows, then unchanged compiled-stage bytes, little-endian
SPIR-V words and GLSL bytes, without padding, terminators or trailing data.

Manifest keys have exactly this order: `schema` (`eawr-stage-result/1`),
`descriptor_version` (1), `stage`, `major`, `minor`, `identity`, `options`,
`reflection`, `sections`. Stage IDs are vertex 0 and pixel 1. `sections` contains
three `[byte_length, sha256]` pairs in payload order. Digests are 64 lowercase hex
characters. `options` is the canonical request manifest encoded as a JSON string;
it includes the typed options, every fixed backend option, selected coordinate
option values and the SPIR-V target. Struct records in identity/reflection are
fixed positional arrays in public declaration-field order; vectors retain their
semantic order and absent optionals are JSON null. Enums use their public numeric
IDs. Changing any record meaning/order requires a new envelope/schema version.

The parser accepts only this schema, unsigned integer/boolean/string/array values
and exact delimiters. It rejects unknown/duplicate fields, whitespace variants,
noncanonical escapes, invalid UTF-8, integer overflow and extra values. Lengths
and logical allocation charges are checked before payload/table allocation.
Reference, cycle, depth, layout, stage and SPIR-V framing checks precede publication.
The payload has no serialized spans, handles, pointers or live renderer objects.
Malformed or stale entries are recoverable misses; no partial artifact is returned.

Injected limits bound accepted stage/SPIR-V/GLSL sizes, metadata, table entries,
strings, nesting and envelopes. Cancellation is checked before/after backend calls
and during wrapper traversal. These limits bound wrapper work and accepted output;
they do not interrupt a foreign compiler call or impose its internal heap limit.
Activation-worker scheduling and renderer generation handover belong to the
activation owner.

## Dialect handover and tests

`summarize_dialect` records directive kinds, uniform/structure/member shapes,
function definitions and control-flow counts without retaining source snippets.
It reports the exact core-version spelling separately. The measured package emits
plain `#version 330`; output bytes remain unchanged. Disabling 420pack is a public
compiler option, not a directive rewrite. The adapter admits or refuses this
output using its own policy.

The independent wire fixture is authored test data and has a pinned digest.
Tests cover every single-byte corruption of it, current identity changes,
truncation/trailing bytes, type cycles/dangling references, typed metadata,
UTF-8/NUL, cancellation, budgets and ownership. The optional package test runs
invented position, constant-bank and textured pixel stages, checks the prelookup
identity, round-trips complete artifacts, records dialect shapes and proves that
owned results survive module/context destruction. Without an injected package,
that additional test is explicitly skipped; ordinary codec/refusal tests still run.
The native package controls also repeat the default late semantic failure through
`translate_stage`. Default and API21 late failures exercise the same C++ code owner
as production translation; API21 is solely a negative private ABI control and is
never an accepted production option. Failed code storage is neither dereferenced nor freed.
