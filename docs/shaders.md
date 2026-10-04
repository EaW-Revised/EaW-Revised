# Shader translation spike

The shader translation spike provides a clean-room, local-only route from the five approved Direct3D 9
effects to Vulkan 1.1 SPIR-V. The translator verifies the protected archive and
effect/include closure before it reads an effect, expands macros with a pinned
preprocessor, parses effect structure with a lexer and nested-block parser, lowers
programmable and fixed-function draw passes, compiles every emitted stage and runs
an independent SPIR-V validator. Compiler success is not renderer success.

The implementation never embeds an original effect body. Original effects,
preprocessed text, lowered HLSL, SPIR-V, rich descriptors and logs stay below
ignored `out/shaders/`. Checked-in fixtures under `tests/shaders/fixtures/` are
original synthetic inputs.

## Toolchain

The spike pins glslang 16.5.0 and SPIRV-Tools v2026.1. The Windows glslang release
archive is pinned by SHA-256 in `tools/shaders/toolchain.json`; `spirv-val` is built
from SPIRV-Tools commit `fbe4f3ad913c44fe8700545f8ffe35d1382b7093` with
SPIRV-Headers commit `04f10f650d514df88b76d25e83db360142c7b174`.
On Windows x86-64, both resulting executable hashes are pinned as well. On other
platforms, where source-built bytes can differ, exact semantic version and source
commit identity remain mandatory. Explicit executable paths, toolchain-root paths,
and PATH defaults all pass the same preflight; a swapped, missing, wrong-version,
wrong-commit or wrong-hash binary fails before any protected source is read.

glslang is the Khronos reference GLSL front end and includes an HLSL-to-SPIR-V
front end. Its upstream project now marks HLSL support deprecated, so the pin is a
deliberate bounded-spike dependency rather than a permanent renderer choice:
<https://github.com/KhronosGroup/glslang>. SPIRV-Tools supplies the independent
validator: <https://github.com/KhronosGroup/SPIRV-Tools>.

The exact compiler flags are:

```text
-D -V --target-env vulkan1.1 --hlsl-dx9-compatible
--auto-map-bindings --auto-map-locations -S <vert|frag> -e <entry>
```

Validation uses `spirv-val --target-env vulkan1.1`. Install the pinned Windows
tools below the ignored output tree:

```powershell
powershell -ExecutionPolicy Bypass -File tools/shaders/install_tools.ps1
```

The script checks the downloaded archive hash, checks exact source commits, verifies
both executable identities and writes an ignored `install-provenance.json` receipt.
It writes only below `out/tools/`. `tools/shaders/toolchain.json` is the portable
manifest for reproducing an equivalent build on Linux. Stable policy diagnostics are
`SHD_TOOL_NOT_FOUND`, `SHD_TOOL_IDENTITY_MISMATCH`, and
`SHD_TOOLCHAIN_CONTRACT_INVALID`.

## Translation contract

`tools/shaders/fx_parser.py` provides:

- comment/string-aware tokenization with original line and column diagnostics;
- balanced nested blocks without regular-expression brace deletion;
- active, depth-first, case-insensitive include resolution with cycle detection;
- root and include SHA-256 recording in deterministic resolution order;
- parameter shape, arrays, semantics, annotations, defaults and registers;
- sampler-to-texture relationships plus address/filter state;
- technique, pass, shader compile expression and case-insensitive render state IR;
- whole-function call-graph pruning, retaining the selected entry's executable
  dependency closure unchanged; and
- explicit diagnostics for unknown essential fixed-function operations or arguments.

`tools/shaders/translate.py` removes only parsed effect wrappers. It never performs
a regex brace sweep. Named precompiled-shader variables resolve through their parsed
`compile <profile> <entry>()` declarations. Modern bindings follow descriptor order,
while an explicit legacy register such as PrimAlpha's `s0` is retained separately.
Missing includes retain the actual including logical file and directive line/column,
including failures reached through nested include resolution.

Every API and CLI invocation resolves its output before tool execution or source
processing. The resolved path must be a strict descendant of this repository's
ignored `out/shaders`; lexical `..`, an absolute escape, and an existing symlink or
junction parent that resolves outside all fail with
`SHD_OUTPUT_OUTSIDE_CONFINEMENT` before any directory or partial artifact is created.
`--allow-unpinned-synthetic` only allows an original synthetic effect outside the
five-effect hash contract. It never relaxes output confinement or tool identity.

Programmable stages retain the source arithmetic and vector-left `mul` order. The
fixed-function lowering emits programmable vertex/fragment pairs from the parsed
stage cascade. RGB and alpha cascades are independent; cleanup passes restore state
without drawing. Fixed lighting uses explicit material, ambient, eye and three-light
uniforms, and skin palette tests preserve three consecutive `float4` constants per
bone with translation in their fourth elements. The descriptor records CPU-skin,
fixed-transform/lighting, texture-stage and degradation prerequisites.

Dynamic render-state expressions remain predicates in the descriptor. Absent state
is tagged `renderer_baseline`, never source behavior. The current renderer baseline
uses the reviewed D3D9 defaults (less-or-equal depth comparison, depth write enabled,
counter-clockwise culling and blending disabled; fog disabled). PrimAlpha therefore
does not acquire fog, cull, sampler or distance-fade behavior from its commented
programmable text.

## Regeneration

The archive must be `shaders/foc_shaders.zip` with SHA-256
`88b9cb03322aab9be7451968ca514e9d2c810973d83f65459fd8cb52e7f96f8d`.
An explicit `--source-archive` is recommended even though the CLI can find that file
in a source-root ancestor.

```powershell
$effects = @('MESHGLOSS','MESHBUMPCOLORIZE','RSKINGLOSSCOLORIZE','MESHSHIELD','PRIMALPHA')
foreach ($effect in $effects) {
  python tools/shaders/translate.py `
    --source-root shaders/petroglyph-foc/Shaders `
    --source-archive shaders/foc_shaders.zip `
    --effect $effect `
    --out ("out/shaders/" + $effect) `
    --toolchain-root out/tools/glslang-16.5.0
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Every per-effect `manifest.json` is canonical UTF-8 JSON with sorted keys, two-space
indentation, LF newlines, no timestamp and no private absolute path. It records each
compiler and validator command, exit code and artifact hash separately. A second run
to a different output root must produce byte-identical manifests and stage hashes.

## Rendering consumer

[Godot presentation decision](architecture-decisions.md#adr-011-godot-presentation) selects Godot RenderingServer through a C++ GDExtension. RenderingServer
materials do not ingest the spike's SPIR-V directly: the retained prototype expresses
the selected MESHGLOSS semantics as reviewed Godot shader-language source, installs it
with `shader_set_code`, binds parameters with `material_set_param`, and assigns the
material RID to the mesh. Each additional legacy effect still needs a separately
reviewed adaptation and draw qualification; compiler or `spirv-val` success alone is
not renderer acceptance.

The SPIR-V translator, validator, descriptors, tests and `adapt_wgpu.py` remain
shared clean-room/reproduction tooling. The latter records the historical losing
backend's mechanical interface adaptation and is not a selected runtime dependency.
If a future Godot path requires raw SPIR-V or compute, that is a separately scoped
RenderingDevice integration with its own shader/pipeline/resource proof; it must not
be inferred from the current RenderingServer material route.

## TerrainWater t1 and ps11

The opt-in tools/shaders/terrainwater_t1.py path compiles vs_1_1 and lowers
ps.1.1 through ps11.py. It emits SPIR-V plus a binding contract; it does not select
a real map or imply Godot material ingestion. The default descriptor still lists
t1/t1_p0 as legacy_shader_assembly. Binding requires explicit external state and
provenance, failing with EAWR-SHD-T1-* or EAWR-SHD-PS11-* diagnostics.

texbem reads signed bump R/G, with V8U8 decoded as c/127; unsigned textures are
rejected, not remapped as normal maps. Its bump matrix has no zero/identity
default. The documented D3D9 formula and the inspected native driver disagree
on off-diagonal index order. Require a declared convention even for symmetric
matrices; the translator accepts neutral coefficients instead of guessing the
game's layout. Require single-level bump sampling because perturbed LOD is
implementation-defined.

The bounded subset is tex, texbem, mov, add, mul and mad with rgba/rgb/a masks.
Texture instructions precede arithmetic; no register is read after texbem consumes
it. Modifiers, swizzles, constants, co-issue and other instructions fail explicitly.
Masked writes preserve the untouched components. The FOW multiply is RGB-only;
output alpha retains vertex alpha times reflection alpha plus the secondary
vertex alpha. Clamp vertex colours before interpolation; reflection UV divides
by w per vertex, then interpolates perspective-correctly.

Require PixelShader1xMaxValue wherever an intermediate can leave [-1,1]. Stage-1
reflection addressing is effect-owned CLAMP; cleanup restores WRAP, not texture,
blend or depth state. The pass uses SRCALPHA/INVSRCALPHA, LESSEQUAL and depth
writes. Declare blend operation, separate-alpha policy and culling explicitly;
only declared fog.enabled=false is supported. No absent state becomes an inferred
renderer default. [Technique selection](behaviour/terrainwater-technique-selection.md)
is a separate original-game question.
