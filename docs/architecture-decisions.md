# Architecture decisions

## Runtime and presentation

Use C++20 and CMake for the engine-independent simulation, VFS, loaders and Lua
host. Target Windows x64, Linux x64 and Linux ARM64; qualify MSVC/Clang on Windows
and GCC/Clang on Linux. See [build](build.md) and the maintainer CI policy.

ADR-011 selects Godot 4.7.2 with pinned godot-cpp 10.0.0 bindings through a C++
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

## Deterministic state

ADR-003/010 requires integer-only authoritative arithmetic: checked signed int64
Q24, nearest-even reductions and exact widened accumulation. There is no float
fallback. [Fixed-point](fixed-point.md) defines overflow, geometry and extended
finite-duration rules. Lua remains outside authoritative state until its numeric,
RNG, time, iteration and serialization policies are defined and qualified. The
selected [Lua profile](multiplayer-readiness.md) (owner decision EAWR-254) keeps
script numbers as IEEE binary64 computed by integer-only soft-float code, not
hardware floats, and converts them to and from Q24 with defined rounding at the
simulation boundary; Q24 stays the simulation's number type.

ADR-009 uses EnTT v3.15.0, commit
`d4014c74dc3793aba95ae354d6e23a026c2796db`, as private component storage.
Persistent nonzero uint64 EntityIds are separate from EnTT handles and are never
reused. Evaluation, commit, serialization and snapshots use ascending stable-ID
order. Workers read immutable copied inputs and write disjoint staging outputs;
ordered serial commit alone mutates storage. Per-entity work runs in named
phases over 64 fixed partitions on a persistent worker pool; the
[phase map](simulation.md#phase-map) says which steps are serial and why. Public
interfaces expose no ECS handles or references. EnTT avoids a second scheduler; this is not a benchmark
claim against Flecs.

ADR-005 makes headless replay hashes the cross-platform determinism oracle.
Compare actual per-tick outputs across targets and worker counts (CI evidence 1/2/4;
contract tests also 8 and the hardware count), including tests that perturb storage
order. See [simulation](simulation.md) and
[replay format](replay-format.md).

## Assets and compatibility

ADR-004 routes runtime asset I/O through a case-insensitive [VFS](vfs.md): mod
layers, expansion, base; loose files override active archives within each layer.
Keep original XML tag and Lua API names and semantics; extensions are additive.
Inventory declarations do not prove runtime behaviour. Keep exact numeric
lexemes and report unsupported features instead of inventing semantics.

Ship new code only; users supply their own game/mod installation. Original
assets, restricted source and translated private shaders are not redistributed.
ADR-008 requires the [clean-room boundary](clean-room.md); MIT reuse retains
attribution. Original behaviour belongs in clean notes with independent fixtures.

## Shader extensibility

Newly authored modern shaders must have a path independent of legacy effect names
and the FX parser. Describe supported languages, stages, resources and backend
constraints explicitly. Do not promise arbitrary HLSL/GLSL, compute, mesh shaders
or ray tracing can be dropped in unchanged. Legacy material semantics stay intact.
See [shader translation](shaders.md).
