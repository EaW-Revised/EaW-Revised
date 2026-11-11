# Shader Translation Behaviour Contract

Scope: `MeshGloss.fx`, `MeshBumpColorize.fx`, `RSkinGlossColorize.fx`, `MeshShield.fx`, and `Engine/PrimAlpha.fx`.

## Translation-wide rules

| Rule | Behaviour |
|---|---|
| S-01 | Resolve includes depth-first, with case-insensitive logical lookup and normalized separators. Diagnose an include cycle. Record every resolved relative path and hash in the output manifest. |
| S-02 | Lex comments, strings, and nested blocks before recognizing declarations, techniques, and passes. A line- or brace-oriented regular expression is not an adequate parser contract. |
| S-03 | Preserve parameter spelling, scalar/vector/matrix shape, array length, semantic, annotation, default value, and resource relationship in an intermediate descriptor. Unsupported metadata remains explicit rather than being discarded. |
| S-04 | Compatibility is decided before preference. For these five effects use the explicit per-effect ordered lists below; no generic lexical or source-enumeration tie-break is permitted. Maximum-viewport techniques are editor-only and never enter runtime selection. |
| S-05 | Source vectors are left operands. A source `float4x3` skin transform is three consecutive four-component vertex-constant vectors: output position component *j* is the dot product of input `(x,y,z,w)` with constant *j*, and output normal component *j* uses the first three elements of that same constant. Translation therefore maps the fourth element of each constant to translation and must preserve the source's vector-times-matrix order. |
| S-06 | Map `POSITION`, `NORMAL`, `COLORn`, `TEXCOORDn`, and `FOG` stage interfaces explicitly. Preserve integer-like palette data carried in floating channels without introducing interpolation or rounding changes. |
| S-07 | Resolve a sampler to its texture plus address and filtering states. Assign modern bindings deterministically by descriptor order, never by incidental map iteration. |
| S-08 | Parse render-state names and enum values case-insensitively. When a state is genuinely absent, use the documented D3D9 default: depth testing follows whether the swap chain has a depth stencil, depth write is on, comparison is less-or-equal, counter-clockwise faces are culled, and alpha blending is off. An explicit or dynamic state overrides the baseline. |
| S-09 | Preserve a dynamic state as a runtime predicate. Do not freeze it using an arbitrary translation-time parameter value. |
| S-10 | Lower each fixed-function texture-stage chain to an explicit programmable equivalent or fail with a named unsupported feature. Never silently discard a stage. Cleanup passes that only restore device state are metadata/state restoration, not geometry draw passes. |
| S-11 | Successful source translation, SPIR-V validation, and ingestion by the target renderer are three separate results. A manifest must report each result and exact tool version independently. |
| S-12 | Fixed-function color and alpha cascades are independent. `CURRENT` is the preceding stage result (and diffuse at stage zero); disabling color at a stage ends that stage and all later stages. The equations below use the component-wise Direct3D 9 operations exactly as documented; backend precision and final framebuffer representation are validated separately. |
| S-13 | Project binding policy: build an activation-time plan in CTAB order. Resolve each used constant to its exact effect parameter name and numeric shape, or an explicit compiler register default. Preserve boolean, integer and floating register sets; reject overlapping ranges, ambiguous names, unsupported conversions and unknown used semantics. Numeric material defaults use the effect's packed logical values and the reflected register orientation, including array-element padding. |
| S-14 | Project update ownership: material values retain their own defaults; engine semantics retain scene or instance providers and never freeze their effect defaults. Light and SH providers follow R-LIT-03..07 of `p1-effective-environment.md`. A program with preshaders or initialized numeric expressions requires the expression evaluation route before admission; collecting its dependencies does not execute them. |
| S-15 | Project stage-interface policy: declarations retain semantic index, register and component mask. Vertex inputs, including integer-like palette selectors, are vertex-only; linked varyings retain perspective interpolation unless public backend reflection specifies another supported mode. Missing legacy output declarations require reflection before linking. Unknown semantics fail rather than inheriting a gate register manifest. |
| S-16 | Project skin policy: Godot-pre-skinned input cannot consume a source skin palette. Source-palette input is a separate explicit prerequisite; declaring it is not proof that arbitrary shader-dependent skinning is supported. Matrix/coordinate and GPU fidelity admission remains subject to the controlled opaque and normal/skin cases. |
| S-17 | Independently authored native-compiler controls establish the sampler texture-state identifier as 164 and class-two matrix defaults as packed logical rows, with successive array elements adjacent. A Texture-only sampler resolves its one state to the named texture parameter. Distinct non-square matrices authored with both matrix qualifiers retain their initializer row order; this is independent of CTAB register orientation. |
| S-18 | Project native lowering core: accept a finite typed straight-line stage language, resolve all symbols and component reads/writes, and produce an owned function plus explicit port/uniform recipes. Check source, token, symbol, uniform, node, nesting, work and emitted-byte budgets. Refuse unknown syntax, conditional preprocessing, illegal writes, undefined component reads and dynamic indexing; never pass unresolved source identifiers into emitted code. Preserve expression structure, select guards, stage identities and interface metadata. A lowered function is not a complete or admitted material. |
| S-19 | Project state normalization: retain explicit pass/sampler assignment order and duplicates, decode established serialized identifiers into typed fields, and validate static scalar domains. Preserve dynamic parameter/expression identities as evaluation and conversion prerequisites. Shader and texture assignments remain explicit program/resource prerequisites. A normalized plan does not establish device capabilities, renderer representation, technique compatibility or multipass execution. |
| S-20 | Project metadata core: apply R-08's candidate gates with explicit mode-sensitive validation results and classify R-11's trailing cleanup annotation. R-10's load-failure report and R-11's texture-use record belong to integration. Proposed one-draw/cleanup state flow is project policy; general retail no-geometry cleanup and interpass carry remain unverified. Plans retain qualification prerequisites and do not establish admission or execute draws. |
| S-22 | Project spatial wrapper: reconcile lowered ports with typed reflection, link varyings by location/type/component/interpolation and require a qualified source semantic for vertex attributes. Restore asset-basis vertex inputs after the renderer's axis conversion, retaining the renderer's authored-binormal reconstruction. With the backend preserving source clip depth and vertex Y, map depth to the active renderer's clip interval. Resolve constant-bank offsets through typed P3/P5 plans, never through generated register names. Scene registers use material uniforms; object registers use independent instance uniforms (at most sixteen slots). Current providers are required on every update. GPU coordinate fidelity remains unqualified until the installed lit controls pass; activation remains off. |
| S-23 | Project wrapper compatibility is fail-closed: the first material wrapper admits only the canonical opaque render-state baseline with output alpha proven exactly one, and statically qualified two-dimensional sampling (matching point/linear min/mag, absent/linear mip filter, matching wrap/clamp U/V, no extra LOD or color conversion). P4's conservative component proof follows literal one, constructors, copies, swizzles and agreeing select alternatives. Unknown arithmetic, sampled values and parameter defaults prove nothing; overwrites invalidate the affected components. Other alpha values, states, dynamic predicates, cleanup and resources remain explicit refusals. Samplers retain their actual texture parameter, engine provider and default resource; no visible replacement texture is invented. An assembled program is still subject to renderer admission and prewarm. |
| S-24 | Activation requires explicit applied selection settings from its caller; zero-initialized P2 settings retain their fixed-function meaning. The native invented control uses desktop shader tier 3, water tier 2 and a synthetic recognized vendor, without changing its source or the selector's low/high gates. Activation owns the source compiler, stage cache and assembled programs. A validated warm stage payload skips translation; the current verified package, public ABI, capability/options identities and frozen input closure still gate reuse. Source-stage reuse also requires the frozen source/compiler identity. Cancelled work cannot publish. A resident retains a real renderer prewarm pin; a CPU descriptor alone cannot become resident. Unknown battle-time effects are pure lookups and schedule no work. The implementation limits retained stage payloads and prepared programs separately to 32 MiB per effect and resident CPU program payloads to 64 MiB across generations sharing a compiler. Allocator overhead, compiler internals and backend allocations are outside those logical payload budgets. |
| S-25 | Project route activation requires the explicit experimental shader-translation switch and remains off by default. Mod load completes eager admission before constructing the battle. Routes receive a pinned generation and prepare actual numeric overrides, sampler resources and current providers before upload; no source compiler, native translator or disk cache is reachable from frame submission. Compatible admitted arithmetic precedes the existing same-name adapter. A refused existing effect uses that adapter with approximation reporting; an unknown effect remains omitted, and a missing file keeps the explicit green material. Native environment preparation qualifies geometry separately from adapter-only material requirements; it never bypasses triangle, bone-chain or supported billboard checks. |
| S-26 | The initial renderer provider scope is current camera/object matrices and eye position, plus typed numeric material overrides supplied at upload. Matrices preserve source row-vector orientation through P5's CTAB packing and the source left-handed projection's [0,w] depth before S-22's target conversion. Required lighting, fog, clock, skin-palette and engine-texture providers remain unavailable and therefore refuse; no zero, white texture or effect default substitutes for them. Fog-enabled renderer contexts and used Colorization parameters also refuse native preparation: their current fog variant and per-entity team-colour override integration are unqualified. Actually read authored normal/tangent/binormal, float-colour and secondary-UV inputs retain a typed payload requirement and refuse native preparation while the shared renderer normalizes, packs or omits those channels; no missing attribute is silently replaced. Palette-skinned geometry also refuses until its source skinning and vertex-arithmetic order are qualified, even when a program reads no palette uniform. Only successfully prepared plain rigid environment geometry gains a native route. P5 updates reuse owned numeric buffers and interned names; missing current input hides that surface until a valid update. GPU fidelity and installed memory qualification remain pending before activation can default on. |
| S-27 | Project vertex builtin policy additionally admits only an unread scalar floating point-size output whose final component is proven exactly one. It maps to the target POINT_SIZE builtin. Other point sizes, reads, fragment-depth outputs and unqualified builtin shapes refuse with the exact stage, port, type and read/write masks in the diagnostic. The pinned producer emits this initialization even for the invented triangle-stage control; the fixture is unchanged. This narrow policy does not infer D3D semantics from generated identifiers or enable point/line primitive routes. |
| S-29 | The loading owner remains disabled unless the experimental shader-translation switch is supplied. It freezes the effective mounted chain and contributor policies, completes eager admission before creating battle content, and retains only an immutable generation in that content. Surface preparation owns used authored numeric overrides and actual sampler identities; its numeric update reuses existing buffers and cannot call the compiler or disk cache. |
| S-28 | The opt-in loading owner explicitly uses desktop shader detail 3 and water detail 2 for player and evidence runs while ST-O1 keeps the Options detail controls disabled and stored-only. It does not make those stored fields live. Actual adapter identity is mapped from recognized exact vendor strings; an unknown vendor refuses activation with a located diagnostic instead of using vendor zero. The settings and actual PCI vendor enter the activation cache identity, and the completed bounded report records the same policy and vendor. |

The complete CPU wrapper is `shader_material::assemble`; it owns both stage
functions, spatial entry points, bindings and numeric update plans. Per-instance
numeric buffers are allocated before frame submission and reused. A missing
provider invalidates the entire update. The input-axis inverse and clip-depth
conversion are project integration policies, not a new claim of retail GPU
equivalence. Unsupported paths retain the established fallback policy.

The first wrapper requires explicit GLSL binding and descriptor-set qualifiers
when matching numeric uniform banks. Desktop GLSL can omit those qualifiers;
that case is a known safe over-refusal, pending the admission slice's native
uniform fixture and optional-qualifier reconciliation. An empty texture default
without a typed engine provider is a resource refusal, including object slot zero.

The clip-depth input is D3D `[0,w]`, because the wrapper requires the bridge's
`preserve_clip_depth` variant. The pinned SPIRV-Cross Vulkan SDK 1.3.290.0
`spirv_glsl.hpp:169-172` defines its optional clip fixup as `[0,w]` to `[-w,w]`;
the bridge disables that fixup in `shader_stage/translate.cpp:125-127`.
Godot 4.7.2 `drivers/gles3/rasterizer_scene_gles3.cpp:890` selects reverse depth
without remapping; `core/math/projection.cpp:787-811` therefore reverses a GL
projection's Z. Starting from preserved D3D Z, first form GL Z `2z-w`, then
reverse it: `w-2z`. RenderingDevice's reversed `[0,w]` target instead uses `w-z`.
Thus source near/mid/far map to GL `1,0,-1` or RenderingDevice `1,0.5,0` after
division by W. Negating preserved D3D Z alone would incorrectly map near to
zero. Numeric CPU boundary controls cover both intervals; GPU qualification
remains pending.

S-18's first core supports floating, signed/unsigned integer and boolean scalars
and vectors, static numeric uniform arrays/blocks, one two-dimensional sampler
type, explicit constructors and a finite intrinsic set. Only a leading version
330 directive (optionally `core`) is accepted; absence selects the project's
330 subset, rather than inferring another language version. Other directives are refusals,
including conditional branches. Matrix syntax, function/control-flow expansion,
non-uniform or dynamic indexing, sampler/interface arrays and additional
resources remain named unsupported features. A terminal bare return is allowed.

Static uniform elements retain their block/member/element and layout identities
in declaration order and receive distinct generated names for each shader stage.
Function ports retain preliminary locations, components, interpolation qualifiers
and actual read/write masks. This information does not establish a D3D semantic
association: the consumer must reconcile the typed backend reflection, metadata
and vertex preprocessing, link both stages, declare qualified uniforms, apply
coordinate/sampler/output policy and pass renderer admission before drawing.
No wrapper, technique/state selection, expression execution, activation or
renderer call site is added by this core.

Input text is borrowed only for the lowering call. Returned function text and
recipes own their storage; intermediate nodes use a flat arena and recursive
walks enforce the same depth bound. Diagnostics contain bounded reasons and
locations rather than source excerpts. Resource limits are project policies,
independent of observed retail values and separate from renderer limits. This
is a restricted lowering component, not a complete GLSL conformance validator.

Declaration names follow the keyword and identifier restrictions in sections
3.6/3.7 of the [GLSL 3.30 specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.3.30.pdf),
including reserved words and consecutive underscores anywhere in a name.
Constant declarations require constant expressions under sections 4.3.2/4.3.3;
the core tracks this property without evaluating arithmetic on the CPU. The
project additionally limits every global initializer to a constant expression.
Ordinary local initializers may depend on uniforms or stage inputs.

The depth policy covers expression-tree depth, including flat operand chains.
The initial subset also refuses truncating constructors, implicit numeric
conversions, empty statements and comments on the version-directive line.
These are subset restrictions, rather than claims that all such GLSL is invalid.

S-13 distinguishes effect-container value storage from CTAB register orientation.
The observed class-two effect defaults are packed in logical row order (S-17);
CTAB row/column classes then decide register packing. The reader's unestablished
effect-container class-three default layout remains an explicit refusal. A
source producer must supply the same established logical value representation,
not copy CTAB register-padded bytes into effect parameter defaults.

S-17's invented native control uses two 2-by-3 matrices initialized by distinct
rows and a two-element matrix array. Native output retains `(11,12,13,21,22,23)`
for the first matrix and the corresponding ordered values for the other matrix
and both array elements, including the `column_major` declaration. A separate
sampler with only `Texture = <ProbeImage>` emits one state: identifier 164,
texture kind, and a named initializer targeting `ProbeImage`. The system native
compiler succeeded and produced a 576-byte effect. Private metadata receipt
`native_binding_probe.json` records source hash
`70d26d2d515bd6f1fc07951f068559673ced12d4cfbfd5ac68cb32ed8e283f05`,
compiler hash `0f89c19165fd83a05e39bea601a06007e6a8814d55e2adf913902926231f843e`,
and output hash `eff1204d7d58ea9426876f5ebefc455f7f2c6e5b543c796782036f37187bdf53`.
No installed input or third-party reader supplied either observation.

S-19 uses independently authored native effect-compiler controls, one isolated
pass or sampler parameter for each field. Render states use selector 0; sampler
states use selector 256. Serialized identifiers are distinct from the public
device API enum ordinals:

| Fields in order | Serialized identifiers in the same order |
|---|---|
| Depth enable, depth write, depth comparison, cull | 0, 3, 9, 8 |
| Alpha test, alpha comparison, alpha reference, blend enable | 4, 11, 10, 13 |
| Source blend, destination blend, blend operation | 6, 7, 75 |
| Separate alpha, alpha source, alpha destination, alpha operation | 99, 100, 101, 102 |
| Color write mask, sRGB write, depth bias, slope bias, stencil enable | 73, 97, 98, 79, 22 |
| Address U/V/W, border color | 165, 166, 167, 168 |
| Minification, magnification, mip filtering | 170, 169, 171 |
| LOD bias, maximum mip, anisotropy, sRGB texture | 172, 173, 174, 175 |

The control's static numeric values have class two, one row, one column, no
array, and one four-byte little-endian word. Bias fields retain floating bits;
the other observed fields use integer kind. A separate dynamic alpha-reference
control has floating kind and an expression initializer: normalizing it must
retain its expression and byte-valued destination domain, never interpret its
placeholder zero as a static reference. S-17's texture identifier 164 remains a
resource prerequisite; typed shader assignments at 146/147 retain their stages.

The native compiler succeeded with a 4344-byte result. Private metadata receipt
`native_state_probe.json` records source hash
`98399ef0388d047db91cf334df639e451a0facdd58726474b530a8a5c86895be`,
compiler hash `0f89c19165fd83a05e39bea601a06007e6a8814d55e2adf913902926231f843e`,
and output hash `fd6e37aeeb272229a97d52bead0995b160d4fe15b6d43e604c1f81b698199404`.
This establishes encoding metadata from owned synthetic input, not successful
execution of the project's normalizer or any installed effect.

Initial baselines follow Microsoft's [render-state defaults](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype)
and [sampler defaults](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype).
They apply once at the start of a state flow, never as an implicit reset between
passes. Explicit assignments are sparse and ordered; later execution must carry
the established state through draws and cleanup. General state-flow composition
is outside this normalization slice.

Static literals retain public enum meanings for
[blending](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend),
[blend operations](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblendop),
[comparison](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcmpfunc),
[culling](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcull),
[depth modes](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dzbuffertype),
[addressing](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtextureaddress),
and [filtering](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtexturefiltertype).
The first subset requires canonical zero/one boolean-kind literals. Integer-kind
sRGB texture decode retains the documented nonzero-enable rule. Alpha reference is 0..255; write mask
is 0..15; anisotropy is positive, with the device maximum deferred to admission.
Mip filtering accepts none/point/linear; minification and magnification retain
the other defined non-none filter values without claiming device support.
Maximum mip and border color retain their DWORD values; actual texture bounds
remain a resource/admission question. Biases must be finite floating literals.
Unsupported static conversions refuse. Dynamic values retain source kind and
destination domain for later evaluated conversion and validation. W buffering,
constant/inverse-constant blend factors (which require the currently unnormalized
blend-constant state), legacy coupled factors, second-source factors, stencil, independent sampler
axes and distinct filter choices remain capability/representation questions.

Unknown IDs/selectors, non-scalar numeric shapes, malformed links, alias cycles,
and state/work budgets refuse with a bounded source/byte diagnostic. Texture
aliases, defaults and provider resolution belong to S-13's resource path. None of
these plans executes expressions, emits renderer state or enables translation.
The retained first parameter is an override root, not the full dependency list:
evaluation must walk the immutable alias graph with all reached overrides,
including terminal parameter overrides, rather than unconditionally executing a
collected terminal expression or using a default behind an authored override.

S-20 follows the [R-08 candidate-selection gates](terrainwater-technique-selection.md#technique-selection-all-effects).
The caller supplies applied detail levels, vendor identity, initial software
processing mode and an actual compatibility result for each candidate in both
processing modes. The core selects the first compatible candidate in declaration
order, retaining rejection causes and the selected validation mode separately
from the final SWVP mode. A failed SWVP validation leaves software processing
enabled for subsequent candidates; a successful SWVP candidate switches it off.
CPU-skin intent is retained without changing LOD rank or technique preference.
The core makes no device calls and cannot establish those compatibility results.
An empty selection signals that no candidate qualified; the activation owner must
report R-10's load/validation failure. R-11 coverage here is limited to its
trailing cleanup annotation; its per-texture-parameter use record is out of scope.
The complete validation table is checked before candidate selection.

Known annotations in this first subset must be static, scalar and unique when
consulted. Wrong types, dynamic references, duplicate recognized names and
unsupported text references refuse explicitly; these are conservative project
restrictions, not claims about native annotation conversion. Unknown annotations
are retained in the descriptor. Only LOD values are compared ASCII-insensitively;
the core does not extend that rule to every annotation name. Annotation/value
indices, names, text, candidate/pass counts and deterministic work are bounded.
Text lookup follows the descriptor's typed object-slot identity, so a source
producer can provide the same graph without fabricating serialized slot words.
The text value's raw `bytes` are deliberately unused; the payload must have
exactly one terminal zero, with no earlier zero or silently ignored suffix.
Refusals name the technique/pass or annotation lookup where available. A byte
column is present only for a known object/state offset; unavailable offsets
remain unset rather than being reported as byte one.

Project pass-flow policy proposes one programmable draw and an optional boundary
identified by a trailing true `AlamoCleanup` annotation. **Unverified:** R-11's
recording rule alone does not establish general retail no-geometry cleanup or
interpass state carry. Such a boundary sets `requires_cleanup_qualification`;
integration must qualify those behaviours before admitting it. The gap is on the
[Phase 2 fidelity list](../../plan/phase-2/README.md#fidelity-list).

An earlier cleanup marker, a second proposed draw, a missing explicit shader
stage, duplicate shader-stage assignments within one pass or a fixed-function
draw is a named project refusal. A pass name, null shader or FIXEDFUNCTION LOD
label never substitutes for the trailing annotation. The proposed numeric state
composition keeps serialized order and the last duplicate assignment, carrying
unassigned fields into the next boundary. Effective shader snapshots can carry
from the earlier pass; explicit assignments remain separately available. These
are plan policies, not original-execution evidence. Dynamic parameter/expression
roots remain evaluation prerequisites rather than frozen defaults.

The caller explicitly asserts the canonical initial device baseline and depth
attachment for this flow. It is neither a reset per material/pass nor proof of
unknown state inherited from another material. Other inherited-state contexts,
sampler/resource realization, actual render-state representation, alpha/output
policy and renderer admission still require qualified integration. These plans
perform no draw or expression execution and leave translation disabled.

The render-state baseline follows Microsoft's [Direct3D 9 defaults](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype); fixed stages use its [texture operations](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtextureop) and [arguments](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dta).

## Effect contracts

### MeshGloss.fx

Material class: opaque mesh. Vertex layout is `alD3dVertNU2`; tangent generation and shadow use are disabled.

Parameters are `Emissive` float3 default zero, `Diffuse` float3 default one, `Specular` float3 default one, `Shininess` float default 32, and `BaseTexture`. `BaseSampler` uses wrap in U and V, clamp in W, and linear minification, magnification, and mip filtering. Vertex input supplies position, normal, and one texture coordinate.

The runtime programmable choice is `sph_t0`: vertex entry `sph_vs_main` at shader model 1.1 and pixel entry `gloss_ps_main` at shader model 1.1. It writes depth. Its alpha blending is dynamic: source alpha and inverse-source-alpha blending becomes active when the light-scale alpha is below full opacity. `sph_t1` is a fixed-function fallback that combines diffuse and texture with a doubled modulation. The maximum-viewport technique is editor-oriented and is excluded from ordinary runtime selection.

### MeshBumpColorize.fx

Material class: opaque mesh. Vertex layout is `alD3dVertNU2U3U3`; tangent data is required.

Parameters are `Emissive`, `Diffuse`, `Specular`, and `Shininess` with the same shapes/defaults as MeshGloss; `Colorization` is float4 default `(0, 1, 0, 1)`; `UVOffset` is float4 default zero; resources are `BaseTexture` and a discardable-annotated `NormalTexture`. Both samplers use wrap in U/V and linear minification, magnification, and mip filtering; no W-address override is declared.

Prefer DX9 technique `sph_t2`, using vertex entry `sph_bump_spec_vs_main` at model 1.1 and pixel entry `bump_spec_colorize_ps_main` at model 2.0. The DX8 fallback `sph_t1` uses `sph_bump_vs_main` and `bump_colorize_ps_main`, both at model 1.1. Fixed fallback `t0` loses bump detail and uses base-texture alpha for colorization before doubled diffuse modulation; that loss must be visible in the descriptor. The maximum-viewport technique is editor-oriented.

### RSkinGlossColorize.fx

Material class: opaque rigid-skinned mesh. Vertex layout is `alD3dVertRSkinNU2` with one bone influence per vertex.

Parameters are `Emissive` float3 default zero, `Diffuse` float3 default one, `Specular` float3 default one, `Shininess` float default 32, `Colorization` float4 default `(0, 1, 0, 1)`, `BaseTexture`, and `GlossTexture`. Both samplers use wrap in U/V, clamp in W, and linear minification, magnification, and mip filtering. The skin palette is `m_skinMatrixArray`, type `float4x3[24]`, semantic `SKINMATRIXARRAY`, starting at vertex constant zero. Each valid bone occupies three consecutive float4 constants, for 72 constants total. The value carried in the normal's fourth channel is converted directly to an integer and indexes one of the 24 matrices; it is vertex input and is not interpolated. Valid selected-content data must be in `[0, 23]`; the source performs no bounds check, so out-of-range behavior is not a compatibility oracle.

DX8 technique `sph_t0` uses `sph_vs_main` at model 1.1 with `gloss_colorize_ps_main` at model 1.1. Fixed fallback `sph_t1` requires CPU-skinned vertex input and sets `CPUSKIN` true; the modern descriptor must state that prerequisite. The maximum-viewport technique remains editor-oriented.

### MeshShield.fx

Material class: transparent mesh. Vertex layout is `alD3dVertNU2C`; depth sorting is requested.

Parameters are `Color` float4 default white; `EdgeBrightness` float default 0.5 with UI range 0–1; `BaseUVScale` float default 20, `WaveUVScale` float default 1, and `DistortUVScale` float default 1, each with UI range 0–100; and `BaseUVScrollRate`, `WaveUVScrollRate`, and `DistortUVScrollRate`, each float default 1 with UI range −10–10. Resources are `BaseTexture`, `WaveTexture`, and `DistortionTexture`. `BaseSampler`, `WaveSampler`, and `DistortionSampler` use wrap in U/V, clamp in W, and linear minification, magnification, and mip filtering. Time-dependent texture transforms drive scrolling.

The DX9 runtime technique `t0` uses vertex entry `sph_vs_main` at model 1.1 and pixel entry `distort_ps_main` at model 2.0. It disables depth writes, alpha test, fog, and culling, and uses additive one-plus-one blending. The maximum-viewport programmable technique is editor-only.

Fixed fallbacks are `t1` using base plus wave textures and `t2` using base texture only. Both omit programmable distortion, the vertex-color factor, and the normal-based edge-brightness term. Their additional cleanup passes restore texture-coordinate/device state and must not become additional geometry draws.

### Engine/PrimAlpha.fx

Material class: transparent primitive. Vertex input supplies position, diffuse color, and one texture coordinate. The effect has no material texture declaration; its sampler is explicitly bound to sampler slot zero.

The only live runtime technique, `t1`, is fixed-function. It disables depth writes, alpha testing, and lighting; blends source alpha against inverse source alpha; and modulates vertex diffuse and texture for both color and alpha. A programmable `t0` text exists only inside a block comment, so it is not selectable evidence. A fresh translator must synthesize an equivalent programmable form from the live fixed states or emit a precise unsupported diagnostic.

Distance fading appears only in the commented programmable material. It must not be added to the live fixed path without independent observation.

## Deterministic runtime pass compatibility

A technique is compatible only when its live draw pass can consume the declared vertex preprocessing/layout, every required texture is bound, every compiled shader profile fits the backend, reflected constant/sampler use fits backend limits, and every dynamic render state can be represented. A fixed-function technique additionally requires fixed vertex transform/lighting where enabled and at least the stated texture-blend-stage capacity. A cleanup pass is never considered a separate compatible draw.

For the bounded five effects, choose the first compatible entry in exactly these lists:

| Effect | Runtime order and exact additional conditions |
|---|---|
| `MeshGloss` | `sph_t0` (VS 1.1 + PS 1.1, one base texture), then `sph_t1` (fixed transform/lighting, one texture and one active blend stage). |
| `MeshBumpColorize` | `sph_t2` (VS 1.1 + PS 2.0, tangent layout, base + normal textures), `sph_t1` (VS 1.1 + PS 1.1, same layout/resources), then `t0` (fixed transform/lighting, base texture, two active blend stages). |
| `RSkinGlossColorize` | `sph_t0` (VS 1.1 + PS 1.1, base + gloss textures, the 72-constant palette beginning at c0 plus all reflected non-palette constants), then `sph_t1` only when CPU skinning has already produced compatible fixed-function position/normal input (base texture, two active blend stages). |
| `MeshShield` | `t0` (VS 1.1 + PS 2.0, base + wave + distortion textures), then `t1` (fixed transform, base + wave textures, two transformed coordinates and three active blend stages), then `t2` (fixed transform, base texture, one transformed coordinate and two active blend stages). |
| `Engine/PrimAlpha` | `t1` only (fixed transform, caller-supplied sampler-zero texture, one active blend stage). |

This table resolves the only same-LOD tie in the bounded set: shield `t1` precedes the intentionally reduced `t2`. An unavailable required texture makes that entry incompatible rather than silently substituting white. If no entry is compatible, translation reports the first unmet condition for every entry and fails; it never selects `max_viewport` or the commented PrimAlpha technique.

## Complete live fixed-function cascades

The equations below are original mathematical descriptions of the live Direct3D 9 stage states. `B` is the base sample, `W` the wave sample, `D` the iterated diffuse value after fixed vertex processing, and `F` the texture factor. RGB and alpha cascades are evaluated separately.

For the three lit mesh fallbacks, fixed vertex lighting supplies `D`. Their material ambient is `Diffuse`; material diffuse is `Diffuse.rgb × m_lightScale.rgb` with alpha `m_lightScale.a`; material specular is `Specular`; material emissive is `Emissive`; and material power is the literal 32, so the exposed `Shininess` value does not alter the fixed pass. Engine light and global-ambient values are external draw inputs, not constants supplied by these effects.

| Live draw pass | Texture coordinates and stage equations | Final relevant state |
|---|---|---|
| MeshGloss `sph_t1_p0` | Stage 0 samples `B` at input `TEXCOORD0`. `rgb = 2 × D.rgb × B.rgb`; `a = D.a`. Stage 1 is disabled. | Fixed lighting on; depth write on; less-or-equal depth test; blending is enabled exactly when `m_lightScale.a < 1`, using source alpha and inverse source alpha. Texture alpha does not supply output alpha. |
| MeshBumpColorize `t0_p0` | Stage 0 samples `B` at `TEXCOORD0`: `C0.rgb = B.a × F.rgb + (1 − B.a) × B.rgb` with `F = Colorization`, and `C0.a = B.a`. Stage 1 has no texture sample: `rgb = 2 × D.rgb × C0.rgb` and `a = D.a`. Stage 2 is disabled. | Fixed lighting on; base sampling is linear with wrap U/V; same depth and dynamic blend rule as MeshGloss. The normal texture and bump/gloss contribution are absent. |
| RSkinGlossColorize `sph_t1_p0` | Identical colorization and doubled-lighting cascade to MeshBumpColorize: stage 0 blends `Colorization.rgb` against `B.rgb` by `B.a`, stage 1 doubles the product with `D.rgb`, and final alpha is `D.a`. | Requires CPU-skinned input; fixed lighting on; same depth and dynamic blend rule. `GlossTexture` is unused in this fallback. |
| MeshShield `t1_p0` | Both stages use source `TEXCOORD0`. Base coordinates are `(BaseUVScale × u, BaseUVScale × v + BaseUVScrollRate × time)`; wave coordinates substitute the wave scale/rate. Stage 0 gives `C0.rgb=B.rgb`, `C0.a=B.a`; stage 1 gives `C1.rgb=C0.rgb × W.rgb`, `C1.a=C0.a`; stage 2 gives `rgb=C1.rgb × Color.rgb`, `a=C1.a`. Stage 3 is disabled. | Lighting and fog off; no culling; no depth write; alpha test off; additive one/one blending. Cleanup only restores coordinate indices and disables both transforms. |
| MeshShield `t2_p0` | Base coordinates use the same scale/vertical-scroll mapping. Stage 0 gives `C0=B`; stage 1 uses `F=0.25 × Color`, giving `rgb=C0.rgb × F.rgb` and `a=C0.a`. Stage 2 is disabled. | Same fixed render states as shield `t1`; cleanup only disables the base transform. |
| PrimAlpha `t1_p0` | Stage 0 samples caller-bound sampler zero at `TEXCOORD0`: `rgb=D.rgb × B.rgb` and `a=D.a × B.a`. Stage 1 is disabled. Here lighting is off, so `D` is vertex `COLOR0`. | No depth write; alpha test off; source-alpha/inverse-source-alpha blending. Fog, culling, depth comparison, and sampler filtering/addressing are not assigned by this effect and therefore follow the surrounding documented baseline rather than the commented shader text. |

The fixed shield paths never read vertex diffuse, so they do not reproduce the programmable vertex-color or edge-brightness multiplication. The equations make that degradation explicit instead of treating a fixed fallback as visually equivalent.

## Parameter/pass acceptance cases

### C-01: dynamic MeshGloss blend

- Given two MeshGloss material instances whose light-scale alpha is full opacity `1.0` and half opacity `0.5`.
- When the same selected programmable pass is described for drawing.
- Then blending is disabled for the first and source-alpha/inverse-source-alpha blending is enabled for the second. The translator does not bake either state globally. Covers S-08 and S-09.

### C-02: bump fallback loss is explicit

- Given a backend limited to the fixed MeshBumpColorize fallback.
- When technique selection completes.
- Then the descriptor selects `t0`, states that normal-map bump detail is not represented, and retains base-alpha colorization and doubled modulation. Covers S-04 and S-10.

### C-03: skin palette orientation and selector

- Given selector 23, position `(2,3,4,1)`, normal `(0,1,0)`, and the last bone's three constants `(1,0,0,10)`, `(0,1,0,20)`, `(0,0,1,30)`.
- When the source 4-by-3 palette operation is described.
- Then position is `(12,23,34)` and the normalized normal is `(0,1,0)`. Selecting 22 instead must use the preceding three constants; no interpolation or selector rescaling is permitted. Covers S-05 and S-06.

### C-04: shield cleanup does not draw

- Given the fixed shield base-plus-wave technique.
- When a draw plan is produced.
- Then exactly the material draw pass emits geometry; cleanup operations restore state afterward and do not increment draw count. Covers S-10.

### C-05: commented PrimAlpha technique is unavailable

- Given a request for a programmable source technique from PrimAlpha.
- When live techniques are enumerated.
- Then the commented technique is absent. The translator either lowers live `t1` to a declared programmable equivalent or fails explicitly. Covers S-02, S-04, and S-10.

### C-06: deterministic include and binding manifest

- Given identical input files whose directory enumeration order differs.
- When two translations run.
- Then include resolution, resource bindings, selected technique/pass, and manifest hashes are identical. Covers S-01 and S-07.

### C-07: fixed colorization arithmetic

- Given the MeshBumpColorize fixed pass with base sample RGB `(0.2,0.4,0.6)`, base alpha `0.25`, colorization RGB `(1,0,0)`, lit diffuse RGB `(0.5,0.5,0.5)`, and diffuse alpha `0.4`.
- When both fixed stages run.
- Then stage-zero RGB is `(0.4,0.3,0.45)` and the doubled second stage leaves final RGB `(0.4,0.3,0.45)` with alpha `0.4`. The normal texture cannot affect the result. Covers S-10 and S-12.

### C-08: fixed shield arithmetic and cleanup

- Given shield `t1` with base RGBA `(0.2,0.4,0.8,0.5)`, wave RGB `(0.5,0.25,0.5)`, and color RGB `(0.5,1,0.25)`.
- When its draw pass and cleanup metadata are evaluated.
- Then the draw output before framebuffer blending is RGB `(0.05,0.1,0.1)` and alpha `0.5`, exactly one geometry draw is emitted, and the two texture transforms are then disabled. Covers S-10 and S-12.

### C-09: missing programmable resource selects declared fallback

- Given MeshBumpColorize on a backend that supports PS 2.0 but has no bound normal texture, while fixed transform/lighting and two blend stages are available with a base texture.
- When compatibility and selection run.
- Then neither programmable entry is compatible and `t0` is selected; a white normal texture is not synthesized. Covers S-04.

### C-10: PrimAlpha live equation

- Given vertex diffuse RGBA `(0.5,0.25,1,0.4)` and sampler-zero RGBA `(0.2,0.8,0.5,0.5)`.
- When live `t1` is evaluated.
- Then the source color before framebuffer blending is `(0.1,0.2,0.5,0.2)`, and no distance fade is applied. Covers S-10 and S-12.

S-24 qualification: source-stage cache partition version 3 includes the verified package lock, capability and public ABI manifests plus result/options schema versions. CPU contracts seed valid source bytes under a changed package identity and require measured fresh source compilation/backend translation; each identity field also changes the key. Resource-policy controls can only tighten the 32 MiB stage/program/prepared and 64 MiB resident maxima, exercising each refusal branch without large synthetic allocations. Cancellation inside the prewarm callback must release its resident charge and return no published artifact. Compiler operations run on one activation thread and borrow a compiler that outlives its jobs.

The owner vendor mapping follows [Godot's public PCI vendor identities](https://github.com/godotengine/godot/blob/4.7/servers/rendering/rendering_context_driver.h) and [reported vendor names](https://github.com/godotengine/godot/blob/4.7/servers/rendering/rendering_device.cpp), with exact recognized OpenGL vendor spellings. Unknown strings are refused; substring guessing is not used.
