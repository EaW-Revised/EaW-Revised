<a id="meshadditive-sun-billboard-p1-27-source-research"></a>

# MeshAdditive sun billboard (P1 rendering source research)

## Interface contract

- Geometry inputs:
  - one unskinned ALO submesh with vertex layout `alD3dVertNU2`. The pass reads position and the first UV and ignores the normal;
  - the submesh's bone, its bind-pose (rest) chain and its billboard mode.
- Material inputs:
  - `BaseTexture`;
  - `UVScrollRate` (effect type float2);
  - `Color` (effect type float3).

  EV-OBS-01 reports that both are authored as ALO float4 (`0x10106`) chunks in
  all 22 scanned sun surfaces; this review did not independently verify that corpus.
- External inputs:
  - the world-view-projection transform, including the billboard world placement;
  - a scalar time;
  - a light-scale RGBA;
  - the camera view orientation;
  - the sun (toward-light) direction.
- Coordinates: the asset basis (right-handed, Z-up, vector-left `float4x3` storage, as in
  [asset-formats.md](../asset-formats.md)). Every vector in the transform rules is in this
  basis, before any renderer basis conversion.
- Output: one additive draw with no depth write.
- Cadence: per frame and per camera. The reference rebuilds the view-derived matrix on
  every camera change and the sun matrix on every environment change.
- Retained state: none, apart from the clock.

## Ordered behaviour rules: MeshAdditive preferred pass

| Rule | Behaviour |
|---|---|
| A-01 | The preferred programmable source technique is `t0`, pass `t0_p0` (LOD `DX8`): vertex model 1.1, pixel model 1.1, one pass. Source order and LOD alone do not establish which technique the retail runtime selects. `t1` (`FIXEDFUNCTION`) is a fallback outside this preferred-pass contract. It produces the same colour product through a texture factor, but applies **no** UV scroll, and its factor is an 8-bit colour. The descriptor sets render phase `Transparent`, vertex processing `Mesh`, vertex type `alD3dVertNU2`, Z-sort on, and no tangent or shadow-volume use. |
| A-02 | Clip position is the object-space position extended with w = 1 and transformed by the supplied world-view-projection. The normal and all lighting uniforms are ignored. |
| A-03 | Each vertex's texture coordinate is the authored first UV plus time multiplied by the first two components of `UVScrollRate`. No scale and no other offset are applied. Wrapping is left to the sampler (A-09): the unwrapped coordinate is interpolated, then wrapped per sample. |
| A-04 | The vertex colour RGB is `Color.rgb` × light-scale RGB × light-scale alpha, per channel. The vertex colour alpha is exactly 1. Light-scale alpha therefore dims RGB, not coverage. |
| A-05 | Only the first three components of `Color` and the first two of `UVScrollRate` are declared, so the authored `Color` alpha and the third and fourth scroll components cannot affect any output. |
| A-06 | Before the pixel stage, each vertex-colour channel is saturated to [0, 1]. A product above 1 contributes 1 and a negative one contributes 0. |
| A-07 | The pixel shader multiplies the sampled texel component-wise by interpolated vertex colour. RGB = texel RGB × colour RGB, and alpha = texel alpha × 1. The effect writes a vertex fog factor of 1; this suppresses vertex-fog mixing where that factor is used, but does not establish the ambient fog or table-fog state of the retail pipeline (G-06). |
| A-08 | Blending is on, with source ONE, destination ONE, and the default ADD operation. Alpha test and depth writes are off. The declared depth comparison is less-or-equal: **if depth testing is enabled**, equal depth passes and farther depth fails. The effect does not declare depth enable, cull mode, separate alpha blending or sRGB states, so their actual pipeline values remain open (G-06). |
| A-09 | `BaseSampler` uses wrap in U and V, clamp in W, and linear minification, magnification and mip filtering. The effect declares neither sRGB texture decode nor sRGB target write; absence of those declarations does not prove their actual pipeline values. Arithmetic on stored texel values is a valid explicitly declared no-decode test policy, not a recovered retail state (G-06). |
| A-10 | Texture alpha reaches the shader output alpha but not its RGB. With source ONE and destination ONE, texture alpha does not scale the source RGB contribution. If separate alpha blending is off and alpha writes are enabled, it is added to destination alpha; the actual ambient states remain open (G-06). |
| A-11 | Material parameters are set by name. A float4 authored chunk is uploaded as a 4-vector, and the effect keeps only its declared components (A-05). The reference replaces a float4 `Color` with the object colourisation only when the mesh name starts with `FC_`; applicability of that exception to any retail sun mesh is not established here. |
| A-12 | In the reference, time is one float in seconds. It accumulates wall-clock milliseconds / 1000 × game speed from viewer start and is uploaded once per frame to every effect. EV-BIN-01 reports that retail `TIME` is bound from one engine-global float when effect parameters are applied; this review did not independently verify that binding. That clock's origin, unit, pause behaviour and wrap were not recovered. |
| A-13 | EV-BIN-02 reports that retail `LIGHT_SCALE` comes from the drawn object's own light-scale record when it has one, and is otherwise (1, 1, 1, 1); this review did not independently verify that binding. The reference never sets it, so it keeps the effect default (1, 1, 1, 1). Which value the game supplies for sky objects was not recovered. |

## Ordered behaviour rules: billboard mode 7 (`sun`)

B-01…B-09 describe the pinned MIT reference implementation only. They are
candidate EAWR policy for synthetic tests, not recovered retail mode-7 behaviour;
G-01 remains open even where the reference algebra is exact.

Definitions, all in the asset basis:

- **R, U, B** are the camera's world right, up and backward axes. B is the unit vector from the look target toward the eye. They are the rows of the rotation part of the inverse view matrix. The reference builds its view with a right-handed look-at and up (0, 0, 1) (`3DTypes.cpp:168-172`).
- **L** is the direction toward the sun, meaning the opposite of the direction the sun's light travels. In the reference it is the negated direction of its first directional light (`RenderEngine.cpp:373-376`). The researcher also states that this light feeds the effects' light-zero vector. The 2026-09-24 review did not verify that, and it does not matter here, because L's game source is gated (G-03).
- **t** is the elevation of L: atan2(Lz, √(Lx² + Ly²)).
- **a** is the azimuth of L: atan2(Ly, Lx).
- **S** is the proper rotation that first turns about −Y by t and then about +Z by a. Its axis images are:
  - S(+X) = (cos t·cos a, cos t·sin a, sin t), which is L normalised;
  - S(+Y) = (−sin a, cos a, 0);
  - S(+Z) = (−sin t·cos a, −sin t·sin a, cos t).
- **o** is the rest origin: the model-space point to which the bone's local origin maps under its bind-pose absolute transform. That transform composes the full parent chain, from the bone through each parent to the root.

Input schema (EAWR policy, all in the asset basis):

- **Bind records.** They are exactly the parsed `assets::Bone::relative_transform` values: twelve
  floats, three source columns of vector-left `float4x3`, with translation in elements 3, 7 and 11.
  A record maps (x, y, z) to (m0·x + m1·y + m2·z + m3, m4·x + m5·y + m6·z + m7,
  m8·x + m9·y + m10·z + m11). The chain composes parent after child, exactly as the planner's
  rigid bake does. So o is the translation of the planner's model-rigid transform for the mesh bone.
- **Camera.** It is a look-at eye, target and up, all in the asset basis. The reference hard-codes
  up (0, 0, 1); EAWR takes up as an input with that default. With f the unit vector from eye to
  target: R = normalise(f × up), U = R × f and B = −f.
- **Render-basis cameras.** A `FixedCamera` (Y up) converts to the asset basis by
  (X, Y, Z) → (X, −Z, Y). That is the exact inverse of the renderer's upload conversion
  (x, y, z) → (x, z, −y). Render up (0, 1, 0) is asset up (0, 0, 1).
- **L.** It is a caller input in the asset basis. Its game source, sign and basis are open (G-03).

| Rule | Behaviour |
|---|---|
| B-01 | Mode values follow the reference labels: 0 disable, 1 parallel, 2 face, 3 zaxis_view, 4 zaxis_light, 5 zaxis_wind, 6 sunlight_glow, 7 sun. The mode is read from the version-2 bone record. Modes 6 and 7 share one rule in the reference. Only mode 7 is in scope. |
| B-02 | For an unanimated model, a mode-7 mesh vertex with object-space offset (x, y, z) goes to world position x·R + z·U − y·B + S(o). In words: the local −Y axis faces the viewer, local +X points to screen right, local +Z points to screen up, and the mesh origin sits at the rest origin rotated by S. |
| B-03 | Orientation depends on the camera's **view orientation only**. The eye position is used neither to orient nor to place a mode-7 mesh, and the mesh is screen-aligned rather than turned toward the eye. (Mode 3 uses the eye position; mode 7 does not.) |
| B-04 | The parent chain affects only o. Parent rotations, translations and scales move the rest origin. The bone's own rotation and scale, and every ancestor's rotation and scale, have **no** effect on the mesh's orientation or size. The mesh size comes only from its vertex coordinates. |
| B-05 | The rest (bind) chain is used, not an animated pose. When an animation is bound, the reference applies the product inverse(rest) · animated absolute transform *after* the billboard placement. That is not a rigid follow of the animated bone. |
| B-06 | The reference draws a model at the model origin with no separate object/world transform. Any original sky-object world transform is outside this rule. |
| B-07 | The magnitude of L does not matter, because only its angles are used. When L is vertical, near vertical or zero, the reference result depends on the sign of zero components and is discontinuous, so it is **not** an oracle. Reference facts for o = (1000, 0, 10): L = (+0, +0, 1) gives a = 0 and (−10, 0, 1000). L = (−0, −0, 1), which is what negating a light direction (0, 0, −1) produces, gives a = atan2(−0, −0) = −π and (+10, 0, 1000). L = (±1e-9, 0, 1) jumps by 2·o_z = 20 horizontally between the two signs. Zero L gives the identity only as (+0, +0, +0); negating a zero light direction gives (−0, −0, −0), a = −π and S(+X) = −X. EAWR rejects all of these (`sun_direction_vertical` when hypot(Lx, Ly) ≤ 1e-6·\|L\|, `sun_direction_zero` when every component is ±0). |
| B-08 | The reference culls clockwise-on-screen triangles globally, so it keeps triangles that appear counter-clockwise on screen, whatever the handedness of its matrices. The effect declares no cull state. |
| B-09 | The reference forms the placement with a numeric matrix inverse of the rest transform. A singular rest transform therefore leaves the reference result undefined. |

## Failure and diagnostic behaviour

No original failure behaviour was recovered. A clean implementation must **fail closed**,
with a named diagnostic and no draw, and must not invent a fallback when any of these
holds:

- the effect identity is not exactly `MeshAdditive.fx`, compared case-insensitively
  like the existing route identity rule;
- `Color` is authored as anything other than a float3 or float4 chunk, or
  `UVScrollRate` as anything other than a float4 chunk. Other kinds were not observed;
- a value is nonfinite, or the texture is missing or undecodable;
- the bone is animated or skinned;
- any bone on the chain, the mesh bone included, is not a finite proper rigid transform. This
  is the planner's test, tolerance 1e-4: unit orthogonal axes and determinant +1. It rejects
  every singular, scaled, sheared, reflected or nonfinite chain;
- any ancestor of the mesh bone has a billboard mode other than 0;
- the chain is malformed: an out-of-range mesh-bone or parent index, a parent index below −1,
  or a chain that does not terminate;
- L is nonfinite, zero (every component ±0), or vertical or near vertical
  (hypot(Lx, Ly) ≤ 1e-6·|L|);
- the camera is nonfinite, its eye-to-target distance is ≤ 1e-6, its up has length ≤ 1e-6,
  or |f̂ × ûp| < 1e-4 (the existing `validate_camera` thresholds);
- the mesh bone's mode is anything but exactly 7: mode 6, modes 0–5, and every value ≥ 8
  (the parser keeps the raw u32).

A missing `Color` or `UVScrollRate` would fall back to the effect defaults, which are
white and zero. That is the effect's declared behaviour, but no pinned surface
exercises it, so whether to accept it is a qualification decision.

## EAWR reference evaluator policy (mode 7)

`eawr-sun-mode7-reference-av01-v1` is the policy of the pure CPU evaluator
`sun_reference_placement` / `sun_reference_vertex` in `eawr/presentation/space/space.hpp`.
It implements B-02…B-04 and B-07 from AV-01 as **EAWR policy**. It is not recovered retail
behaviour (G-01).

- **Inputs.** The inputs follow the schema above: the bind records, the mesh-bone index, the
  camera eye, target and up, the caller-supplied L, and a local vertex. There is no animation
  input, so the pose is the bind pose by construction (B-05, G-07). Bone visibility, skinning
  and material are the planner's concern and are not read.
- **Arithmetic.** All internal arithmetic is in double, and results are returned in double.
  This deviates from the reference's float, whose tilt √(x² + y²) overflows for |L| ≳ 1.8e19
  (T-09).
- **Status order.** The first failure wins, in this order:
  1. the mesh-bone index is out of range → `chain_invalid`, because its mode cannot be read;
  2. the mode is not exactly 7 → `mode_not_sun`;
  3. over the whole chain, first a malformed structure → `chain_invalid`, then a billboard
     ancestor → `chain_billboard_ancestor`, then a bone that is not proper rigid →
     `chain_not_proper_rigid`;
  4. the camera: nonfinite → `camera_nonfinite`, distance ≤ 1e-6 →
     `camera_direction_degenerate`, and up length ≤ 1e-6 or |f̂ × ûp| < 1e-4 →
     `camera_up_collinear`;
  5. L: nonfinite → `sun_direction_nonfinite`, every component ±0 → `sun_direction_zero`,
     and hypot(Lx, Ly) ≤ 1e-6·|L| → `sun_direction_vertical`.
- **Failures.** A failed placement carries a detail naming the bone or field, all-zero
  geometry, and no vertex.
- **Not wired.** The planner, `plan_surfaces`, `build_plan`, the material routes, the renderer
  and the viewer are unchanged. Every billboard bone, modes 1–7 and any other nonzero value,
  stays `hierarchy_unsupported`.

## Synthetic expected-outcome cases

All cases are synthetic. Vectors are in the asset basis and camera up is (0, 0, 1).
The transform cases exercise the reference policy, not retail mode 7. Test policy
uses tolerance 1e-3 for positions and ±1/255 for ideal arithmetic after A-06
and UNORM output quantisation, unless a case states otherwise. The colour tolerance
is not a claim about retail ps_1_1 precision (G-11). The position tolerance assumes
the double evaluator and |o| ≲ 1e6; a float path, if one is ever compared, would use
1e-3 + 1e-6·|o|. Unless a case says otherwise, the transform cases use the T-01 camera,
L = +X, and a mode-7 bone translated to o under an identity root.

### Transform

- **T-01 Basis check.**
  - Given: eye (0, −10, 0), target (0, 0, 0), L = +X, o = 0.
  - Then: R = +X, U = +Z and B = −Y. Local (1, 0, 1) maps to (1, 0, 1), local (1, 0, −1) maps to (1, 0, −1), and local −Y maps to −Y, toward the eye.
  - Covers: B-02.
- **T-02 Camera translation.**
  - Given: o = (1000, 0, 0), L = +X, local corner (1, 0, 1).
  - When: first eye (0, −10, 0) with target (0, 0, 0), then eye (500, −300, 40) with target (500, −290, 40).
  - Then: the corner is at (1001, 0, 1) in both.
  - An eye-facing billboard would instead turn its normal to about (−1, −0.01, 0) for the first camera and would change between the two cameras.
  - Covers: B-03.
- **T-03 Camera rotation.**
  - Eye (0, 0, 0), target (1, 0, 0): R = (0, −1, 0), U = (0, 0, 1), B = (−1, 0, 0). Local (1, 0, 1) maps to (0, −1, 1), and local −Y maps to (−1, 0, 0).
  - Eye (0, 0, 0), target (1, 1, 1): R = (0.70711, −0.70711, 0), U = (−0.40825, −0.40825, 0.81650), B = (−0.57735, −0.57735, −0.57735). Local (1, 0, 1) maps to (0.29886, −1.11536, 0.81650).
  - Covers: B-02, B-03.
- **T-04 Sun rotation.**
  - L = (0, 1, 1) or (0, 2, 2), which give the same S: S(+X) = (0, 0.7071, 0.7071), S(+Y) = (−1, 0, 0), S(+Z) = (0, −0.7071, 0.7071).
  - o = (1000, 0, 0) gives a mesh origin of (0, 707.107, 707.107).
  - o = (1000, 0, 10) gives (0, 700.036, 714.178). Placing the sun at |o|·L instead would give (0, 707.142, 707.142).
  - Covers: B-02, B-07.
- **T-05 Rotated and translated parent.** Each bone is given as its 12 floats in the
  `Bone::relative_transform` layout. L = +X, with the T-01 camera.
  - **T-05a (accepted).**
    - Parent (root): {0, −1, 0, 0, 1, 0, 0, 50, 0, 0, 1, 0}. It rotates +X onto +Y (90° about +Z) and translates by (0, 50, 0).
    - Child (mode 7): {1, 0, 0, 1000, 0, 0.8660254, −0.5, 0, 0, 0.5, 0.8660254, 0}. It rotates 30° about +X with unit scale and translates by (1000, 0, 0).
    - Then: o = (0, 1050, 0), and local (1, 0, 1) maps to (1, 1050, 1). Replacing the child's rotation with the identity changes nothing.
  - **T-05b (rejected).** The child's axes are scaled by 2: {2, 0, 0, 1000, 0, 1.7320508, −1, 0, 0, 1, 1.7320508, 0}. The result is `chain_not_proper_rigid`, naming the child. The reference value (1, 1050, 1) is documented only and is not an oracle.
  - **T-05c (rejected).** The parent is scaled by 2: {0, −2, 0, 0, 2, 0, 0, 50, 0, 0, 2, 0}, with the T-05a child. The result is `chain_not_proper_rigid`, naming the parent. The reference o = (0, 2050, 0) is documented only and is not an oracle.
  - Covers: B-04, B-09, G-10.
- **T-06 Opposite, vertical and zero L.**
  - **T-06a.** L = (−1, 0, 0) with o = (−1000, 0, 0) gives (1000, 0, 0).
  - **T-06b (rejected, `sun_direction_vertical`).** L = (0, 0, 1), (−0, −0, 1), (±1e-9, 0, 1), (0, 0, −1), (0, 1e-6, 1) or (0, 0, 1e-30). The B-07 reference values are not oracles. L = (2e-6, 0, 1), just past the threshold, is placed: o = (1000, 0, 10) gives (−9.998, 0, 1000.00002).
  - **T-06c (rejected, `sun_direction_zero`).** L = (0, 0, 0), (−0, −0, −0) or (0, −0, 0).
  - Covers: B-07, G-10.
- **T-07 Cross terms.**
  - Given: camera eye (0, 0, 0) → target (1, 1, 1); L = (0, 1, 1); o = (1000, 0, 10).
  - Then: local (1, 0, 1) maps to (0.29886, 698.92036, 714.99435), and local (0, −1, 0) maps to (−0.57735, 699.45836, 713.60050).
  - A transposed S would give S^T(o) = (7.071, −1000, 7.071) instead.
  - Covers: B-02, B-03, B-07.
- **T-08 S(+Y).** L = (0, 1, 1) with o = (0, 100, 0) gives S(o) = (−100, 0, 0). Covers: B-02.
- **T-09 Double policy.** L = (3e38, 0, 3e38) with o = (1000, 0, 0) gives (707.10678, 0, 707.10678). The reference float path overflows √(x² + y²) and would give (1000, 0, 0). Covers: B-07 (EAWR numeric policy).
- **T-10 General case.**
  - Given:
    - parent (root) {0, −1, 0, 20, 1, 0, 0, −40, 0, 0, 1, 5};
    - child (mode 7) {1, 0, 0, −300, 0, 0.8660254, −0.5, −100, 0, 0.5, 0.8660254, 50};
    - camera eye (10, 20, 5) → target (−30, 70, −15), up (0, 0, 1);
    - L = (−2, 3, 1.5).
  - Then:
    - o = (120, −340, 55);
    - tilt 0.39424 and azimuth 2.15880;
    - S(+X) = (−0.51215, 0.76822, 0.38411), S(+Y) = (−0.83205, −0.55470, 0), S(+Z) = (0.21307, −0.31960, 0.92329);
    - S(o) = (233.15804, 263.20665, 96.87407);
    - R = (0.78087, 0.62470, 0), U = (−0.18625, 0.23281, 0.95452), B = (0.59628, −0.74536, 0.29814);
    - local (2, −3, 4) maps to (235.76364, 263.15122, 101.58658).
  - Covers: B-02…B-04.

### Transform rejection cases (EAWR policy, G-10)

- **R-01.** A mesh-bone mode of 0, 1, 5, 6, 8 or 0xFFFFFFFF gives `mode_not_sun`.
- **R-02.** A parent with mode 7, 1 or 6, or a grandparent with mode 6, gives `chain_billboard_ancestor`, naming that ancestor.
- **R-03.** Each of these gives `chain_invalid`:
  - a mesh-bone index of −1, −5 or one past the end, or an empty bone list;
  - a self-parented bone or a two-bone cycle;
  - a parent index out of range, or a parent index of −2 on the mesh bone or on the root.
- **R-04.**
  - A NaN or ±infinite component of L gives `sun_direction_nonfinite`.
  - A NaN or infinite eye, target or up gives `camera_nonfinite`.
- **R-05.** All thresholds are those of `validate_camera`.
  - Eye equal to the target, or 5e-7 away from it, gives `camera_direction_degenerate`.
  - Each of these gives `camera_up_collinear`:
    - eye (0, 0, 10) looking at the origin with up +Z;
    - up (0, 0, 0), or up of length 5e-7;
    - up (0, 1, 1e-5) with the T-01 camera, where |f̂ × ûp| ≈ 1e-5.
  - Up (0, 1, 1e-3) is placed, with U = +Z.
- **R-06.** A local vertex with a NaN or infinite component gives no vertex. A failed placement never gives a vertex.
- **Precedence.** Each check wins over every later one: an unreadable mesh-bone index, then mode, chain structure, billboard ancestor, rigidity, a nonfinite camera, a degenerate camera, a collinear up, and finally L (nonfinite before zero before vertical).

### Shader arithmetic and state

- **P-01 Colour and alpha.**
  - Given: Color (0.5, 0.25, 1.0) with authored alpha 0.2, light scale (1, 1, 1, 1), texel (0.8, 0.4, 0.6, 0.3).
  - Then: the fragment is (0.4, 0.1, 0.6, 0.3). Changing the authored alpha to 0.9 changes nothing.
  - Covers: A-04, A-05, A-07.
- **P-02 Light scale.**
  - Given: light scale (0.5, 1, 2, 0.5) with Color (0.5, 0.25, 0.8).
  - Then: the vertex colour is (0.125, 0.125, 0.8). With the P-01 texel the fragment is (0.1, 0.05, 0.48, 0.3). Light-scale alpha 0 gives RGB 0 and alpha 0.3.
  - Covers: A-04.
- **P-03 Saturation.**
  - Given: Color (1, 1, 1), light scale (2, 2, 2, 1), texel (0.5, 0.5, 0.5, 1).
  - Then: the fragment is (0.5, 0.5, 0.5, 1), not 1.0. A negative Color channel contributes 0.
  - Covers: A-06.
- **P-04 Additive blend.**
  - Given: an RGBA UNORM target with alpha writes enabled, separate alpha blending off, and sRGB decode/write off as explicit test policy (G-06).
  - Destination (0.1, 0.2, 0.3, 0.4) plus the P-01 fragment gives (0.5, 0.3, 0.9, 0.7).
  - Destination (0.7, 0.95, 0.2, 0.9) gives (1, 1, 0.8, 1) in a UNORM target.
  - Texel alpha 0 gives the same RGB.
  - Covers: A-08, A-10.
- **P-05 Depth.**
  - Given: depth testing enabled as an explicit test policy.
  - Then: a fragment at a depth equal to the stored value passes, while a farther fragment is rejected; after either draw the depth buffer is unchanged.
  - With depth testing disabled, neither fragment is rejected by depth, and the depth buffer remains unchanged.
  - Covers: A-08, G-06.

### UV scroll and orientation

- **U-01 Scroll at two times.**
  - Given: authored rate (0.25, −0.5, 7, 9) and vertex UV (0.1, 0.8).
  - Then:
    - t = 0 gives (0.1, 0.8);
    - t = 2 gives unwrapped (0.6, −0.2), sampled as (0.6, 0.8);
    - t = 3 gives (0.85, −0.7), sampled as (0.85, 0.3).
  - The third and fourth components have no effect.
  - Covers: A-03, A-05, A-09.
- **U-02 Asymmetric texture.**
  - Given:
    - a texture with four distinct quadrant colours, where v = 0 is the top row;
    - a quad with vertices (±h, 0, ±h), with u = (x/h + 1)/2 and v = (1 − z/h)/2;
    - the T-01 camera, with o = 0.
  - Then: the (+x, +z) corner appears at screen top-right and samples the top-right quadrant.
  - The point at local (−0.6h, 0, 0.6h) samples the top-left quadrant at t = 0 and at t = 1, and the top-right quadrant at t = 2, with rate (0.25, 0).
  - A mirrored basis or swapped U/V fails this case.
  - Covers: A-03, B-02.
- **U-03 Winding.**
  - The EV-OBS-03 winding is visible both with culling disabled and under B-08.
  - The reversed winding is visible with culling disabled and hidden under B-08.
  - The original result is unknown (G-06).
  - Covers: B-08.

## Uncertainties and gates

| Gate | Unknown |
| --- | --- |
| G-01 | B-01…B-09 remain reference-only policy. The [retail mode-7 rules](meshadditive-sun-mode7-retail.md) establish a different placement statically; final deferred-draw world binding and dynamic confirmation remain open there. |
| G-02 | Whether the original sky object has a world transform or is camera-centred; its far-plane handling and draw order against the star sphere; and whether the eye lies inside that sphere. |
| G-03 | [TED environment rules](p1-effective-environment.md) establish that retail L is the current environment's light-0 toward-light vector. The effective environment can change at runtime, so a map alone cannot identify L for a frame. The reference evaluator still takes L as an explicit caller input. |
| G-04 | The TIME clock: origin, unit, pause, game speed and wrap (A-12). |
| G-05 | The LIGHT_SCALE value the game supplies for sky objects (A-13). |
| G-06 | The ambient cull, depth-enable, fog and table-fog, separate-alpha-blend, alpha-write, and sRGB decode/write states of the original pipeline. The effect does not set them. |
| G-07 | Animated mode-7 bones (B-05). |
| G-08 | The researcher reports that the 18 Remake-profile sun billboards, including mod-layer models, were not scanned for authored values or chains. |
| G-09 | Modes 1–6 and `Lensflare0`. |
| G-10 | L zero, vertical or near vertical; a singular, collapsed, scaled or otherwise non-rigid rest chain; a billboard ancestor; nonfinite input; or a degenerate camera basis. |
| G-11 | Precision of the ps_1_1 fixed-point path. |
