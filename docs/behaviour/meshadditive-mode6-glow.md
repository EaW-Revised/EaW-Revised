# FoC mode-6 sunlight glow

The FoC billboard path places a mode-6 bone at its **parent's** origin, turns
its quad toward the pass camera, and shifts it toward directional light 0 in
the quad's plane. The shift is the light vector with its facing-axis component
removed, multiplied by the length of the bone's authored relative translation
and the quad's retained facing-axis scale. The projected vector is not
normalised again. Mode 7 follows a different rule: it places the sun relative
to the camera along light 0. The common reference implementation that groups
modes 6 and 7 does not describe the FoC behaviour.

For `w_planet_urban00.alo`, the Glow bone's authored offset has length
0.656168. The Glow mesh is a square with half-width 6.367527; the opaque
planet's radius is 4.921260. Its material is `MeshAdditive.fx`, with
`BaseTexture=w_glow01`, `UVScrollRate=(0,0,0,0)`, and authored
`Color=(0.623529,0.494118,0.705882,1)`. The texture's alpha is fully opaque,
while its RGB is white in the centre and fades to black at the edge.

The FoC `MeshAdditive.fx` preferred pass uses ONE/ONE additive blending. Its
source RGB is texture RGB multiplied by `Color.rgb` and light-scale RGB and
alpha; texture alpha and authored Color alpha do not attenuate source RGB.
The existing viewer's additive route already follows this product and blend
rule. A prior 1.18 scale multiplier exposed too much of the square's radial
falloff beyond every limb. The viewer now retains the authored mesh dimensions
and uses the environment's light-0 direction for the mode-6 shift.

Evidence: the FoC debug executable's billboard dispatch and light-vector
path, the shipped FoC effect source, and the installed model and texture. The
mode-7 retail distinction is also recorded in
[meshadditive-sun-mode7-retail.md](meshadditive-sun-mode7-retail.md). The debug
path names its input a view-space light vector; the viewer applies the
environment's toward-light direction in object space. An exact match across
all camera orientations remains unverified without a retail frame trace.
