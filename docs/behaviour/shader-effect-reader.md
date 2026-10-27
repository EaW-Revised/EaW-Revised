# Compiled-effect reader contract

This note covers reading a caller-supplied compiled Direct3D 9 effect into metadata.
It does not choose a technique, execute a state, evaluate an expression, translate a
program, or activate a material. The input is binary effect or stage data, never shader source.

## Evidence and scope

Microsoft documents the effect's logical interface and parameter kinds, but not the
legacy binary container layout. The layout evidence for this reader must therefore
come from independently authored effects compiled by Microsoft's native tools and
compared with their documented metadata API. Installed-file checks supply private
metadata/count receipts only. No game/mod bytes or generated installed source are
fixtures, and no third-party effect-reader implementation is an input to this note.

Public interface references:


## Required observable behaviour

| Rule | Contract |
|---|---|
| EFX-01 | Read a bounded byte span. Validate each offset, length and count before accessing memory or allocating its result. Arithmetic must not wrap. Source-provenance size disagreement is diagnosed. |
| EFX-02 | Preserve file order, parameter spelling, semantic, type/shape/array information, defaults and annotations. A shared descriptor is not permission to duplicate an unbounded subtree. |
| EFX-03 | Preserve techniques, passes, state assignments and object references as metadata. Do not choose a technique, discard cleanup/editor metadata, fill absent states with defaults, or execute state changes. |
| EFX-04 | Preserve program and expression byte spans without interpreting arithmetic or evaluating them. References must resolve to a structurally supported object or yield a precise unsupported/malformed diagnostic. |
| EFX-05 | Validate referenced graphs without recursive host-stack growth. Diagnose cycles, invalid references, unsupported forms and truncated records explicitly. Limits derive from the available bytes and representation; any extra safety budget has a named diagnostic and a documented resource reason. |
| EFX-06 | Failure reports identify the category and byte location without echoing payload or shader text. The caller's supplied source provenance is retained by the standard asset diagnostic. No partial graph is exposed as a successful parse. |
| EFX-07 | Tests build invented containers and mutate them deterministically. Truncation, overflowing offsets/counts, excessive counts, cycles, invalid references and unknown objects must terminate with a diagnostic. No installed-file literal or profile corpus is committed. |
| EFX-08 | Frame typed vertex/pixel programs from their profile through a terminal END instruction. Skip comments and complete instruction operands, including END-shaped literal operands. Preserve declarations and unknown semantic usage identifiers. Framing is not executable shader validation or a promise of backend support. |
| EFX-09 | Read public constant tables within their enclosing comment. Preserve ordered constants, register sets/ranges, type/member graphs, arrays and register defaults. Share repeated type descriptors and diagnose cycles without recursive host-stack growth. Link exact effect parameter names when present; retain unmatched compiler constants without guessing a binding. |
| EFX-10 | Resolve named objects to terminal objects and share program descriptors across aliases. Inline shader initializers and separate program objects use the same descriptor. Empty or one-zero-word shader defaults are null only in typed shader contexts; numeric zero remains numeric data. Keep dynamic shader expressions distinct from fixed program assignments. |
| EFX-11 | Preserve state-expression and preshader envelopes, their ordered tagged sections and constant inputs, without evaluating arithmetic. Validate enclosing version/comment/END framing; retain unknown section bodies. Diagnostics address the original byte span and retain logical provenance without printing shader bodies. |

## Independently observed layout

Six invented effects were compiled by the native Microsoft compiler and inspected
through its metadata API: empty, scalar, annotated vector, texture/sampler/program,
struct/array, and parameter-expression cases. Their binary sizes were respectively
96, 256, 476, 720, 508 and 948 bytes. These are observations of our own effects,
not installed-effect fixture sizes or support limits.

All observed words are little-endian and records are word aligned. The first word
is `0xfeff0901`; the second gives the size of a data pool beginning at byte 8.
The ordered record stream begins immediately after that pool. Offsets in the
records below refer to the beginning of the pool, not to the file or the record.
Pool offset zero is a zero sentinel. Each referenced range must fit within the
pool; a string reference must not spill into the ordered records.

The ordered stream starts with four words: parameter count, technique count, an
auxiliary field, and object-slot count. The auxiliary field's meaning is not yet
established and must be preserved without driving allocations or selection. The
object slots include slot zero, the null sentinel. In the empty case there is one
technique, one pass and only that sentinel object slot.

Parameter records contain type offset, default offset, flags and annotation count,
followed by that many annotation pairs. An annotation pair contains type and value
offsets. A technique contains name offset, annotation count and pass count, then
its annotation pairs and ordered passes. A pass contains name offset, annotation
count and state count, then annotation pairs and ordered states. Each state has
four words: state identifier, index/selector, type offset and value offset. This
reader records these identifiers; it does not map them to rendering operations.

A type begins with kind, class, name offset, semantic offset and array count.
The observed scalar/vector classes append column count and row count, in that
order. The struct class appends member count followed by inline member types;
the object class has no numeric shape suffix. A zero array count denotes one
value rather than an empty array. Numeric defaults are packed four-byte values;
the observed struct default concatenates member defaults, without padding a
three-component vector to four. Object defaults contain object-slot identifiers.
Sampler defaults instead begin with a state count and inline state records.
Additional invented native probes confirmed inline nested struct types,
packed matrix/struct defaults, string arrays,
and arrays of sampler values. A sampler array contains one count/state-list pair
per element. Missing numeric initializers were emitted as explicitly stored zero
values, not inferred renderer defaults. The reader preserves the serialized value.

The independently compiled non-square matrices establish a different dimension
order for matrix class two: row count precedes column count. The numeric byte
length is their product, so checking only packed byte counts cannot detect a
dimension swap. Both authored matrix orientations in these probes were reported
as class two by the native metadata API. Class three's serialized form has not
been independently established here and receives an explicit unsupported
diagnostic; the reader does not infer its layout from the enum name.

Pool strings have a byte-count word followed by that many bytes, including the
terminating zero, then padding to the next word. The byte count, terminator and
padded range must fit the pool. A zero name/semantic reference represents absence.

After the techniques, the stream contains raw-object and initializer counts. Raw
objects carry slot identifier, byte length, payload and word padding. Payloads can
be strings, programs or empty resource defaults, so their interpretation comes
from the typed references, not from searching for shader version words. An
initializer has six header words and a length-delimited, word-padded payload. In
the observed cases it attaches either a named object reference or an expression
payload to a pass/sampler state. Its first four words select technique, pass,
sampler element and state. For a pass state the sampler-element word is all ones.
When the technique word is all ones, the next words select a parameter, sampler
array element and sampler state instead. Multiple techniques/passes and two
sampler-array elements were independently exercised to disambiguate these paths.
The last two header words are initializer kind and payload byte length: observed
kind zero holds an opaque expression, and kind one a zero-terminated parameter
name. Named references must resolve without ambiguity to a compatible supported
parameter shape; other initializer kinds are explicitly unsupported in this slice.
Initializer arithmetic is not executed by this reader.

These relationships follow from matching each independently authored declaration
and API metadata result to the resulting offsets, lengths and values, including
the changes between cases. Unknown fields are not assigned guessed semantics.
Private research receipts record exact tool/source/binary identities and native
API results; the SDK and compiled outputs are not shipped with the project.

## Ownership, graph checks and resource bounds

The result is a view: byte spans and text views borrow the supplied immutable
input. The caller must retain it while using the result. Types and values use
indices into flat metadata arrays, so adversarial nesting cannot cause recursive
parsing or recursive tree destruction. All records and referenced defaults are
validated before a successful result is returned.

Object slot zero is null. Every other referenced slot must have an initializer or
raw payload consistent with its metadata. A named reference links the target
state's object slot to the referenced parameter's default object slot. Detect
cycles in those structural edges; expression/program payloads remain opaque,
and their arithmetic dependency analysis belongs to the next slice.

## Program and expression descriptors

`effect_descriptor_version` identifies the shared metadata contract for compiled
effects and independently compiled source stages. A source-stage compiler calls
`read_shader_program` and retains its source-line, entry/profile and compilation
provenance in its outer activation record. The supplied `Source` identifies the
diagnostic origin; program offsets address the compiled stage bytes. Within a
compiled effect those offsets instead address the enclosing effect bytes.
All byte and text views borrow immutable caller-owned storage, including constant
names, defaults and nested expression sections. The activation/cache owner must
retain that storage until all descriptors and consumers of their views are gone.

The stage reader supports vertex profiles 1.1, 2.0 and 3.0 and pixel profiles
1.1 through 1.4, 2.0 and 3.0 for metadata framing. The earlier profiles use
documented opcode operand counts; later profiles carry the operand count in each
instruction. Unsupported versions/opcodes receive located diagnostics. These
rules follow Microsoft's [shader code format](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/shader-code-format),
[instruction token](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/instruction-token)
and [comment token](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/comment-token)
contracts. A successful metadata parse never substitutes for the backend's full
shader validation.

Constant-table offsets are relative to the table header following its `CTAB`
tag. The [table header](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-constanttable),
[constant record](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-constantinfo)
and [type record](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-typeinfo)
are public interfaces. Numeric defaults in these records use register storage
(four bytes per boolean register, sixteen per integer/float register); effect
parameter defaults remain their separate packed typed values. Sampler records
retain sampler register/type metadata; texture resources and sampler states
remain linked through the ordered effect values and named references. Unknown
semantic strings and declaration usage identifiers remain data for later binding
compatibility decisions.

Named sampler assignments retain their immediate `initializer.parameter` link
and original typed default; consumers follow the link to the parameter's sampler
states. Kind and array shape must agree. Default/member/sampler/reference edges
are checked together for cycles using a bounded iterative traversal.

The independently authored native reset pass establishes shader-state identifiers
146 (vertex) and 147 (pixel). Its `NULL` assignments are serialized as integer
zero, as is an ordinary numeric zero in a different state. `EffectState::shader`
therefore exposes the assignment's stage and null/program/expression descriptor
using the shader-state identifier as context. It retains the original typed value
and any immediate named reference. Integer zero in other states remains numeric;
an absent shader state does not acquire a synthesized null assignment.

An optional Windows contract probe compiles an independently authored effect in
memory using the system native compiler, with separate/inline programs, a named
program reference, a null reset, sampler metadata, a state expression and a
preshader. It loads only the system compiler and never compiles installed source.
The expression envelopes use version words `0x46580200` and `0x46580201`, followed
by length-delimited comment sections and END. `CTAB` describes inputs, `CLIT`
retains literal data and `FXLC` retains expression code. A program's `PRES` comment
contains a preshader envelope when it has a body. A tag-only `PRES` comment is an
empty marker retained in comment order, with no expression descriptor; any
nonempty body still requires its version, complete section framing and END.
Bodies remain opaque in this reader: instruction
evaluation, literal interpretation and arithmetic dependency analysis belong to
the evaluator. Unknown sections remain visible rather than being discarded.

The test executable's `--inventory data-root ...` mode reads a caller-supplied
effective VFS chain and prints counts and located failures only. It uses the
production manifest/loose-file mounting policy and writes no asset or generated
shader files. It is a private acceptance tool, not a corpus-derived capability
list or a runtime effect-name allowlist.

Counts are bounded by the minimum serialized record sizes and bytes remaining.
The parser also bounds its metadata visits and string work relative to the input
size, with a named resource-limit diagnostic for excessive repeated/overlapping
references. This is a denial-of-service bound, not a limit derived from game
values. No supported effect-name list, technique count or constant limit is taken
from the installed corpus.

The default logical metadata budget is 64 MiB and can be changed by the caller.
It accounts for result records and auxiliary traversal/cache storage to bound
allocation amplification independently of opaque program size. A separate work
budget allows 32 visits/bytes of string work per input byte, with saturating
arithmetic. Exceeding either budget returns the named asset resource-limit
diagnostic rather than a partial result. Neither budget is a format capability
limit or a count inferred from installed effects.
