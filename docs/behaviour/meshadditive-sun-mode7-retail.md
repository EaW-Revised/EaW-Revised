<a id="retail-mode-7-sun-billboard-placement-p1-27-gate-g-01"></a>

# Retail mode-7 (`sun`) billboard placement (P1 rendering, gate G-01)

## Interface contract (retail rule, asset basis)

The asset basis and the engine world basis are the same here: right-handed, Z up, with column
vectors. A 3×4 record maps (x, y, z) to its three rows applied to (x, y, z, 1).

- Inputs:
  - the camera frame of the current render pass: eye E, and the unit world axes R, U and B
    (right, up and backward, with R × U = B);
  - **L**, the world vector of directional light 0, pointing toward the light (unit when it is
    built from angles);
  - **d**, the length of the mode-7 bone's own authored relative translation. That is the
    Euclidean norm of the record's translation elements, taken before load zeroes them;
  - **s**, the length of the third rotation column of the bone's stored matrix immediately
    before this pass's billboard rule. A dirty pose rebuilds it from render-object world,
    parent chain and the bone's own rotation; otherwise it can retain the prior billboard;
  - an object-space mesh vertex (x, y, z).
- Output: the world position of that vertex, for this pass, when the attached object's local
  offset is identity (see G-01a).
- Cadence: each pass that runs the rule reads its camera and the current light-0 vector.
- Retained state: the stored distance d from load and the bone's current matrix. The rule reads
  s from that matrix. The pose is re-established only when dirty, so a degenerate pass can leave
  s = 0 for later passes, collapsing the mesh even after the light direction becomes valid.

## Ordered behaviour rules (retail)

| Rule | Behaviour |
|---|---|
| R-M7-01 | The draw-time mode is the version-2 record's low four bits: a raw 23 behaves as 7 and a raw 16 as 0 (not a billboard). The load-time translation is zeroed only for authored modes exactly 6 or 7, so raw 23 retains its rest translation for descendants. Values 8–15 count as billboard bones, but no rule changes their pose. Earlier bone-record versions have mode 0. |
| R-M7-02 | **Position.** The billboarded bone origin is placed at E + d·L. It is independent of the model or sky-object world translation and rotation, of every parent translation, and of the direction of the bone's own translation. Only that translation's length d is used. An attached object's local offset can move its mesh origin (G-01a). |
| R-M7-03 | **Facing axis.** Local +Y maps to n, the unit vector from the placed origin toward the eye. When d > 0 and L is nonzero, n = −L/\|L\|. Local −Y (the authored face normal of the pinned sun quads, EV-OBS-03) therefore points away from the eye. |
| R-M7-04 | **Roll.** Local +X maps to the unit vector along U × n. Local +Z maps to the unit vector along n × X. The quad plane is therefore perpendicular to the sun direction, not to the view direction. Its roll comes from the camera's up axis U. The eye position does not affect orientation. |
| R-M7-05 | **Size.** All three mapped axes are multiplied by s, read from the bone's current stored matrix. The mesh is otherwise sized only by its vertex coordinates. With a unit-scale world and a newly posed rigid chain, s = 1; after a degenerate pass, it can remain 0 until the pose is dirty. |
| R-M7-06 | **World vertex with identity local offset.** Rules R-M7-02…05 combine into E + d·L + s·(x·X + y·n + z·Z). The frame (X, n, Z) is a reflection (determinant −1). Positions in the local XZ plane are not affected by this. For y ≠ 0 the sign is the opposite of B-02. |
| R-M7-07 | **Degenerate inputs are not guarded.** Two cases collapse the drawn mesh: d = 0 or L = 0 zeroes every axis, and L parallel to U zeroes X and Z. L is not normalised in position, so a non-unit L scales the distance. No special case exists for a vertical L. |
| R-M7-08 | **Pass gating.** The rule is skipped for a pass camera inferred to be the shadow-projection camera; that skipped pass pushes no new transform, so the mesh keeps its last transform, which need not be the pre-billboard one. The rule runs only when the model itself is drawn in that pass and holds at least one bone with a nonzero mode. In the main scene pass the camera is the scene's active camera. Any other pass uses its own pass camera; those passes were not enumerated individually. |
| R-M7-09 | **Animation.** Position and orientation never read the animated pose. d comes from the bind record, and animation can at most change s. The billboarded absolute bone matrix passes to the attached mesh's set-transform step, with no inverse-rest product; an attached object's own local offset may also be composed (R-M7-10). |
| R-M7-10 | **Hand-off.** The billboarded bone matrix is passed to each attached mesh's set-transform step, which composes the attached object's own local offset when present. Whether the resulting transform is the `WORLD` value bound for the deferred draw remains G-01a. |
| R-M7-11 | **Light source.** L is directional light 0 of the active lighting environment. It is the same vector as `DIR_LIGHT_VEC_0` and points toward the light. In the environment it is built as (sin a·cos e, −cos a·cos e, sin e), so it is unit length whenever it comes from the angles. |
| R-M7-12 | **Comparison only.** Forces of Corruption has the same mode-7 structure. In both executables, mode 6 uses a different rule from mode 7. |

### Comparison with the reference-derived rules B-01…B-09

| Rule | Retail verdict | Exact difference |
|---|---|---|
| B-01 | Partly confirmed | Numeric mode 7 and the version-2 record are confirmed. The label names do not appear in the retail executable. Retail keeps only the low four bits (R-M7-01). Retail does **not** share one rule between modes 6 and 7 (R-M7-12). |
| B-02 | **Rejected** | Retail puts the origin at E + d·L, not S(o) in model space. Its quad plane is perpendicular to L, not to B. Local +Y maps toward the eye, where B-02 maps local −Y toward the eye. The two agree only when the sun is exactly at the screen centre (L = −B) and E = 0. Even then, the rest-origin length and parent translation can still make d differ from \|o\|. |
| B-03 | **Rejected** | The eye position sets placement (R-M7-02). Orientation depends on L and the camera up axis U, not on the view orientation alone, and the quad is not screen-aligned (R-M7-04). |
| B-04 | Partly confirmed, partly rejected | Confirmed: bone and ancestor rotations do not orient the mesh. Rejected: the parent chain does **not** move the origin. The scale of the third rotation column does size the mesh (R-M7-05). |
| B-05 | **Rejected** | There is no product of inverse rest and animated pose. The pose cannot move or turn the mesh (R-M7-09). |
| B-06 | **Superseded** | The mesh is camera-centred. The world transform affects only s (R-M7-02, R-M7-05). |
| B-07 | **Rejected** | Retail uses no angle decomposition, so a vertical L is not discontinuous. The magnitude of L scales the distance. The retail degeneracies are d = 0, L = 0 and L ∥ U (R-M7-07). |
| B-08 | Open (G-06) | No cull state was recovered. For every sampled sun direction in front of the camera, the screen winding of an XZ-plane quad under the retail rule matches B-02's (RT-08). The EV-OBS-03 conclusions about which states draw the quad therefore carry over. |
| B-09 | Not applicable | The retail mode-7 path uses no matrix inverse. Singular chains could affect only s. |

## Failure and diagnostic behaviour

The retail engine reports nothing and does not fall back. The degenerate cases in R-M7-07 draw
a zero-area or unstable mesh. A clean implementation must fail closed on them with named statuses
(see the handoff) and must not reproduce the collapse.

## Synthetic expected-outcome cases (retail rule)

All cases are synthetic and use the asset basis with camera up hint (0, 0, 1). The camera frame
is built as the existing note does:

- f = normalise(target − eye);
- R = normalise(f × up);
- U = R × f;
- B = −f.

This matches the retail camera frame (R-M7-04 and EV-RET-09). The cases take s = 1 and a unit L,
and use a position tolerance of 1e-3. The values were checked with the private case script.

- **RT-01 Centred sun.**
  - Given: eye (0, −10, 0) → target (0, 0, 0), so R = +X, U = +Z and B = −Y. L = (0, 1, 0) and
    d = 1000.
  - Then:
    - the origin is at (0, 990, 0), with X = +X, n = (0, −1, 0) and Z = +Z;
    - local (1, 0, 1) maps to (1, 990, 1) and (1, 0, −1) to (1, 990, −1);
    - local (0, 1, 0) maps to (0, 989, 0), toward the eye, and (0, −1, 0) to (0, 991, 0).
  - The B-02 policy gives (1, 1000, 1) for local (1, 0, 1) with o = (1000, 0, 0).
  - Covers: R-M7-02, 03, 04, 06.
- **RT-02 Camera translation.**
  - Given: the RT-01 bone and L, with eye (500, −300, 40) → target (500, −290, 40).
  - Then: the origin is at (500, 700, 40), and local (1, 0, 1) maps to (501, 700, 41). The offset
    from the eye is the same as in RT-01.
  - B-02 gives (1, 1000, 1) for both cameras.
  - Covers: R-M7-02.
- **RT-03 Off-centre sun faces the eye.**
  - Given: eye at the origin → target (0, 1, 0), L = (0.70711, 0.70711, 0) and d = 1000.
  - Then:
    - the origin is at (707.107, 707.107, 0), with X = (0.70711, −0.70711, 0), n = (−0.70711,
      −0.70711, 0) and Z = (0, 0, 1);
    - local (1, 0, 1) maps to (707.814, 706.400, 1), and local (0, 1, 0) to (706.400, 706.400, 0).
  - A screen-aligned quad at the same origin would give (708.107, 707.107, 1).
  - Covers: R-M7-03, R-M7-04.
- **RT-04 Parent chain and translation direction are ignored.** The bones are given as 12 floats
  in the `Bone::relative_transform` layout.
  - Given:
    - parent (root) {0, −1, 0, 0, 1, 0, 0, 50, 0, 0, 1, 0};
    - a mode-7 child {1, 0, 0, 0, 0, 0.8660254, −0.5, 600, 0, 0.5, 0.8660254, 800}, whose own
      translation (0, 600, 800) has length 1000;
    - the RT-01 camera and L.
  - Then: the result is identical to RT-01. It stays identical when the child translation is
    (1000, 0, 0) or (−1000, 0, 0) (compare EV-OBS-02's cinematic model), and when the parent's
    translation or rotation changes.
  - Covers: R-M7-02, R-M7-05.
- **RT-05 Vertical light is accepted.**
  - Given: eye at the origin → target (1, 0, 1), so R = (0, −1, 0), U = (−0.70711, 0, 0.70711)
    and B = (−0.70711, 0, −0.70711). L = (0, 0, 1) and d = 1000.
  - Then:
    - the origin is at (0, 0, 1000), with X = (0, −1, 0), n = (0, 0, −1) and Z = (−1, 0, 0);
    - local (1, 0, 1) maps to (−1, −1, 1000), and local (0, 1, 0) to (0, 0, 999).
  - The current evaluator rejects this input as `sun_direction_vertical`.
  - Covers: R-M7-04, R-M7-07.
- **RT-06 Degeneracies (reject; retail collapses the mesh).**
  - (a) The RT-01 camera with L = (0, 0, 1): U ∥ n and \|U × n\| = 0.
  - (b) d = 0: the bone's own translation is zero.
  - (c) L = (0, 0, 0).
  - (d) Boundary: with the RT-01 camera, L = normalise(0, 1e-3, 1) gives \|U × n\| ≈ 1.0e-3 and
    is placed. Its origin is (0, −9.000, 999.9995), and local (1, 0, 1) maps to
    (1, −10.000, 1000.0005). L = normalise(0, 1e-5, 1) gives \|U × n\| ≈ 1.0e-5 and is rejected.
  - Covers: R-M7-07.
- **RT-07 General case.**
  - Given:
    - the bones of the existing note's T-10: parent {0, −1, 0, 20, 1, 0, 0, −40, 0, 0, 1, 5} and
      mode-7 child {1, 0, 0, −300, 0, 0.8660254, −0.5, −100, 0, 0.5, 0.8660254, 50};
    - eye (10, 20, 5) → target (−30, 70, −15);
    - L = normalise(−2, 3, 1.5) = (−0.51215, 0.76822, 0.38411).
  - Then:
    - d = \|(−300, −100, 50)\| = 320.15621;
    - the origin is at (−153.96721, 265.95081, 127.97541);
    - X = (0.83875, 0.54363, 0.03106), n = (0.51215, −0.76822, −0.38411) and Z = (0.18495,
      −0.33808, 0.92276);
    - local (2, −3, 4) maps to (−153.08635, 267.99042, 132.88093).
  - Covers: R-M7-02…06.
- **RT-08 Winding.**
  - Given: the triangle (−1, 0, −1), (1, 0, −1), (1, 0, 1) in the local XZ plane.
  - Then: for every sampled camera and unit L with the sun in front of the camera (L·(−B) ≥ 0.2)
    and away from U, the triangle projects counter-clockwise on screen under the retail rule. This
    held in 7606 of 7606 random samples, the same winding as B-02 at the same origin.
  - Covers: R-M7-06, B-08.
- **RT-09 Non-unit L (documented only).** With the RT-01 camera, L = (0, 2, 0) puts the retail
  origin at (0, 1990, 0). This is not an oracle, because the recommended policy rejects a non-unit
  L.

## Uncertainties and gates

| Gate | Unknown |
| --- | --- |
| G-01 | **Partially established.** R-M7-01…09 and R-M7-11 are established statically, with caller, callee and parameter provenance followed. Only G-01a is still open. No dynamic confirmation exists. |
| G-01a | The set-transform step composes an attached object's own local offset when present. The final hop from that resulting mesh transform to the `WORLD` value bound for its deferred draw was inferred, not followed (R-M7-10); the synthetic formula assumes an identity local offset. |
| G-02 | Far-plane handling of the camera-centred origin, draw order against the star sphere, and depth state. The private EV-OBS scan reports that d is of order 10³–10⁴ in the scanned suns; this note did not revalidate that. |
| G-03 (narrowed) | [TED environment rules](p1-effective-environment.md) establish the light-0 angle units and mapping to L. Runtime selection of the current environment still prevents prediction from map bytes alone. |
| G-06 | Ambient cull, depth, blend and fog states. |
| G-12 | Behaviour of s when the sky object has a non-unit world scale. |

## EAWR evaluator policy

Keep retail and reference evaluators under separate identities. The retail policy
uses the mesh bone's own translation length, the pass eye and camera up, and a
unit toward-light vector. Proper rigid chains, no billboard ancestors and exact
mode 7 are project restrictions. Reject nonfinite/zero L, unit-length error above
1e-4, distance at or below 1e-6, and camera-up cross-product length below 1e-4;
vertical L alone is not a rejection. Report origin and axes, not reference tilt
or azimuth. RT cases are the retail transform oracles; reference T cases must
not silently acquire new meanings. Final draw binding, composition/state and
runtime environment selection remain outside the pure evaluator's claim.
