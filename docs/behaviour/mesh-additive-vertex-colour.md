# Additive mesh vertex colour

This contract describes the preferred programmable `MeshAdditiveVColor.fx`
technique used by the black-hole model. Evidence is the published effect source,
its shared additive include, the effective model data and the debug build.
Private research receipts stay under ignored `out/research/`.

| Rule | Observable behaviour | Evidence |
|---|---|---|
| AVC-01 | Select `t0`, pass `t0_p0`, in the transparent phase. Position follows the object's world-view-projection matrix. The pass consumes position, first UV and authored vertex colour; normals and scene lighting do not contribute. | shader source |
| AVC-02 | Add elapsed effect time times `UVScrollRate.xy` to the first UV before interpolation. The rate defaults to zero. Sampling repeats in U/V with linear minification, magnification and mip filtering. | shader source |
| AVC-03 | Vertex RGB is authored vertex RGB times object light-scale RGB times light-scale alpha, saturated before interpolation as a vertex colour output. Vertex alpha is one; authored vertex alpha and a material `Color` do not affect this pass. | shader source |
| AVC-04 | Multiply sampled texture RGB by interpolated vertex RGB and add it to destination RGB with source ONE and destination ONE. Texture alpha does not attenuate RGB. Disable depth writes and alpha testing; keep the declared less-or-equal depth comparison. The pass writes a constant full fog factor. | shader source |
| AVC-05 | Effect engine parameters are applied to each draw from its render context and render task. The remake supplies the existing map presentation clock to this adapter, with a held clock for repeatable captures, and the existing object light-scale input. Clock origin and ambient cull/depth-enable state are not established by this source reading; the established legacy renderer baseline supplies them. | debug build; project presentation policy |
| AVC-06 | The black-hole mesh authors `UVScrollRate` as `(0, 0.04, 0, 0)` and carries the `p_black_hole_inner` particle proxy on its matching bone. Its effective data has no matching idle animation, so motion comes from UV scrolling and the particle system; do not invent a model rotation. Ordinary background mesh models must reach population unless the environment path has claimed their record. | effective model and object data; project routing policy |
| AVC-07 | `Sort_Order_Adjust` has no FoC tag consumer (`DB-NOTAG`); its authored value does not override backdrop ordering. The observed transparent comparator orders render tasks by descending squared distance from their bounds centre to the eye. The remake retains its established transparent sorting policy. | reviewed tag registry; debug build |

Space props take their authored `Layer_Z_Adjust` in the presentation placement
translation (space-movement LZ-01). Their simulation input and heading stay
unchanged. `Sort_Order_Adjust` stays ignored under AVC-07, matching the reviewed
tag registry's `foc-ignores` disposition.

Geonosis's backdrop is already claimed once by the environment path for its
`Planet.fx` sphere. That model's `MeshAlpha.fx` ring and additive glow remain
companions of that same record, with their ordinary scene selectors and shared
placement matrix. Its authored idle clip uses the existing playback rules and
renderer bone palette on the same held/live clock as the effect shaders.
The existing Planet fixed-function route remains in use;
this change does not introduce a second sphere or a new Planet shading policy.

R-ROT-01..03 place the model and its particle bone using the same full placement
matrix. R-ROT-04 keeps simulation yaw unchanged. The additive route uses the
existing stored-value output policy and receives no directional shadows.
The fixed-function fallback is outside this contract. Unread engine lighting
declarations do not imply illumination; `Color` is unread by this preferred pass.
