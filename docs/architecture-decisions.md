# Architecture decisions

Each decision below has a stable reference and a plain name. Accepted decisions
remain in force except for the parts explicitly superseded by a later decision.

## ADR-001: Engine-independent core before backend selection

Status: superseded by the [Godot presentation decision](#adr-011-godot-presentation).

Build the simulation, loaders, virtual file system and Lua host independently of
the presentation engine. Compare an SDL3/wgpu backend with Godot RenderingServer
through GDExtension against loaded assets before choosing. The lasting boundary
is a read-only simulation snapshot; simulation code includes no engine headers.

## ADR-002: Windows and Linux targets

Status: accepted.

Target Windows x64, Linux x64 and Linux ARM64. Qualify MSVC/Clang on Windows and
GCC/Clang on Linux. Cross-target replay comparison checks deterministic state;
cross-built packages alone do not establish native runtime or GPU support.
See [build](build.md) and the maintainer CI policy.

## ADR-003: Integer-only authoritative simulation

Status: accepted; refined by the [checked Q24 math decision](#adr-010-checked-q24-math-and-finite-durations).

Simulation state and arithmetic use fixed point. Floating values belong to
rendering, audio and UI, which convert at the snapshot boundary. Deterministic
geometry uses integers. The checked Q24 contract settles the binary point and
removes the earlier proposed strict-float fallback.

## ADR-004: Case-insensitive layered asset access

Status: accepted.

Route runtime asset I/O through a case-insensitive [VFS](vfs.md): mod
layers, expansion, base; loose files override active archives within each layer.

## ADR-005: Headless replay determinism

Status: accepted.

Use headless replay hashes as the cross-platform determinism oracle.
Compare actual per-tick outputs across targets and worker counts (CI evidence 1/2/4;
contract tests also 8 and the hardware count), including tests that perturb storage
order. See [simulation](simulation.md) and
[replay format](replay-format.md).

## ADR-006: C++20 and composed libraries

Status: accepted; the presentation composition is superseded by the
[Godot presentation decision](#adr-011-godot-presentation), and the entity-library
choice is settled by the [EnTT storage decision](#adr-009-entt-storage-and-stable-simulation-ids).

Use C++20 and CMake for the engine-independent simulation, VFS, loaders and Lua
host. The original library composition proposed SDL3, RmlUi, Dear ImGui and
miniaudio for presentation; Godot now owns that integration. Recast/Detour land
navigation remains subject to behaviour matching; GameNetworkingSockets and
Steamworks were proposed for networking. These proposals do not establish a
completed navigation or networking implementation. MIT reuse retains attribution.

## ADR-007: Original XML and Lua compatibility

Status: accepted.

Keep original XML tag and Lua API names and semantics; extensions are additive.
Inventory declarations do not prove runtime behaviour. Keep exact numeric
lexemes and report unsupported features instead of inventing semantics.

## ADR-008: Clean-room implementation and distribution

Status: accepted.

Ship new code only; users supply their own game/mod installation. Original
assets, restricted source and translated private shaders are not redistributed.
Require the [clean-room boundary](clean-room.md); MIT reuse retains
attribution. Original behaviour belongs in clean notes with independent fixtures.

## ADR-009: EnTT storage and stable simulation IDs

Status: accepted.

Use EnTT v3.15.0, commit
`d4014c74dc3793aba95ae354d6e23a026c2796db`, as private component storage.
Persistent nonzero uint64 EntityIds are separate from EnTT handles and are never
reused. Evaluation, commit, serialization and snapshots use ascending stable-ID
order. Workers read immutable copied inputs and write disjoint staging outputs;
ordered serial commit alone mutates storage. Per-entity work runs in named
phases over 64 fixed partitions on a persistent worker pool; the
[phase map](simulation.md#phase-map) says which steps are serial and why. Public
interfaces expose no ECS handles or references. EnTT avoids a second scheduler; this is not a benchmark
claim against Flecs.

## ADR-010: Checked Q24 math and finite durations

Status: accepted; refines the [integer-only simulation decision](#adr-003-integer-only-authoritative-simulation).

Require integer-only authoritative arithmetic: checked signed int64
Q24, nearest-even reductions and exact widened accumulation. There is no float
fallback. [Fixed-point](fixed-point.md) defines overflow, geometry and extended
finite-duration rules. Lua remains outside authoritative state until its numeric,
RNG, time, iteration and serialization policies are defined and qualified. The
selected [Lua profile](multiplayer-readiness.md) (owner decision on the Lua numeric policy) keeps
script numbers as IEEE binary64 computed by integer-only soft-float code, not
hardware floats, and converts them to and from Q24 with defined rounding at the
simulation boundary; Q24 stays the simulation's number type.

Retain exact authored large finite durations rather than treating them as unlimited.
The [fixed-point contract](fixed-point.md) specifies a separate whole-seconds and
fraction integer representation for duration consumers after conformance tests.
Unknown sentinel meanings remain unresolved; ordinary Fixed conversion fails
explicitly outside its domain.

## ADR-011: Godot presentation

Status: accepted; supersedes the [deferred backend choice](#adr-001-engine-independent-core-before-backend-selection)
and the presentation portion of the [composed-library stack](#adr-006-c20-and-composed-libraries).

Select Godot 4.7.2 with pinned godot-cpp 10.0.0 bindings through a C++
GDExtension and RenderingServer resource ownership. A bootstrap node is allowed;
simulation entities do not become Godot nodes. The choice reduces editor,
rendering, UI, audio and platform integration work; it is not an FPS ranking.
It supersedes the deferred backend choice and proposed SDL/RmlUi/ImGui/miniaudio
presentation stack. Prefer Godot UI/audio behind project-owned adapters for
original XML, atlas and sound-event data; this is not a completed UI/audio port.

Immutable snapshots are the only simulation-to-renderer state boundary. Godot
types, callbacks and floating presentation state stay outside authoritative
simulation. RenderingServer materials require Godot-language adaptation; raw
SPIR-V is not a drop-in material. RenderingDevice/compute needs a separate
integration. Preserve dependency caches and keep packages free of original
assets and translated private shaders. Cross-built ARM packages do not establish
native runtime or GPU support. Revisit the backend if material fidelity, platform
viability, representative RTS performance or maintenance cost requires it.

## Shader extensibility

Newly authored modern shaders must have a path independent of legacy effect names
and the FX parser. Describe supported languages, stages, resources and backend
constraints explicitly. Do not promise arbitrary HLSL/GLSL, compute, mesh shaders
or ray tracing can be dropped in unchanged. Legacy material semantics stay intact.
See [shader translation](shaders.md).
