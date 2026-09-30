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
