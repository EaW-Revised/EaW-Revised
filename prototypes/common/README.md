# Common renderer-prototype scene

`scene.json` is the single input contract for both Phase 0 renderer prototypes. It uses
the real Remake `Rebel_Mon_Calamari_MC_50.alo` loose asset and its real `MeshGloss.fx`
Hangar submesh. The model and `Hangar_3.dds` hashes were measured read-only from workshop
item 1770851727 on 2026-09-21; neither asset is copied into the repository.

Mount the accepted `remake` VFS profile, open both logical paths through `eawr::vfs`, and
load them through `eawr::assets`. A missing path, hash mismatch, missing Hangar mesh,
missing MeshGloss submesh, unexpected `BaseTexture`, or parser diagnostic is a hard scene
failure. Matching shader and parameter names is ASCII-case-insensitive because asset
lookup is case-insensitive; values and ordering remain those returned by the CPU loader.

Both renderers must apply the declared axis conversion exactly once. The right-handed
Z-up to right-handed Y-up map is a proper rotation, so triangle winding and tangent
handedness are preserved; normals/tangents receive the same axis map. Do not transpose in
the asset loader and then transpose again in a backend adapter. DDS is top-left and
`BaseTexture` is sampled as sRGB colour; material scalar/vector values are not
gamma-converted.

Both backends use `material.runtime_technique` (`sph_t0`) and
`material.runtime_pass` (`sph_t0_p0`): the preferred programmable MeshGloss path
from the accepted shader contract, with `sph_vs_main` and `gloss_ps_main`.
The fixed-function `sph_t1` path is not a substitute for these comparison runs.
Any demonstrated backend incompatibility requires a recorded shared-contract
decision before changing the selected technique. Preserve the accepted translated
stage arithmetic and descriptor render state; standard backend PBR lighting is
not equivalent. Generated shader bodies remain private under ignored `out/`.

`lighting.programmable_uniform_policy` is `meshgloss-hemisphere-v1`, defined in
[`meshgloss-programmable.md`](../../docs/behaviour/meshgloss-programmable.md).
It fixes the previously unspecified SH coefficient encoding, eye/light uniforms
and light scale for both backends. The note distinguishes source-supported shader
arithmetic from this original prototype illumination policy and supplies numeric
cases. Preserve that distinction in reports; it does not recover the original
engine's environmental-light encoder.

The fixed camera, light, clear colour, resolution, VSync setting, warm-up and measurement
window are comparison inputs, not backend defaults. Orbit/zoom controls may change the
interactive view but fixed capture resets to these values. Reports must include the exact
scene-file hash plus the two declared asset hashes so a different effective winner cannot
silently enter one prototype.

`team_colour.linear_rgba` is also a comparison input. Its four values are linear,
straight-alpha channels supplied unchanged to the common team-colour shader binding; it
is not an sRGB UI colour and must not be gamma-converted by either renderer.
