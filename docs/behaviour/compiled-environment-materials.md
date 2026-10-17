# Compiled environment materials

Environment routing uses explicit material shader names, case-insensitively.
The suffix alone does not establish compatibility. The following rows were
checked against the installed mod's compiled effects with the author's porting
permission. Original files and disassembly remain outside the repository.

| Rule | Visible behavior and inputs | Adapter |
|---|---|---|
| CEM-01 | `MeshGlossForStarfields.fxo` and `MeshGloss.fxo` use the same programmable diffuse/specular family as the reviewed MeshGloss route: twice the base RGB times vertex diffuse, plus base alpha times vertex specular. Position, normal, UV, Emissive, Diffuse and Specular feed the existing vertex lighting; the specular exponent is 16. A sky uses the existing unlit sky policy. | Existing environment MeshGloss adapter; no new shader arithmetic. |
| CEM-02 | `MeshAdditive.fxo` scrolls UV by time times UVScrollRate.xy. Its vertex color is the clamped product of Color.rgb and light-scale RGB and alpha. Base RGB multiplies that color and adds to the destination; texture alpha does not attenuate RGB. | Existing environment MeshAdditive adapter. |
| CEM-03 | `MeshAdditive2x.fxo` has the same inputs and vertex stage as CEM-02. Its sampled fragment color is doubled and saturated before additive composition. Doubling Color before vertex saturation would give a different result and is not equivalent. | Separate doubled additive environment route, retaining the existing sampling and sky state policy. |
| CEM-04 | An effect with no validated technique contributes zero draw passes. The debug build records the unsupported technique and returns zero from effect begin; it does not require a replacement sky. | Preserve unsupported-surface diagnostics and continue the environment with no primary sky drawn. |
| CEM-05 | Ordinary unit and prop materials named `MeshGloss.fxo` or `MeshAdditive.fxo` use the corresponding qualified family from CEM-01 or CEM-02. | Return the existing canonical `.fx` selector, including its technique, pass, transparency and stored-value policy. Preserve the authored shader name in surface reports. Other compiled names are not admitted by suffix; the starfield route above remains environment-specific. |
| CEM-06 | Ordinary unit and prop materials named `MeshAdditive2x.fxo` use the sampled-product doubling and saturation from CEM-03. | Dedicated canonical `MeshAdditive2x.fx` family; shared additive bindings, light scale, stored-value output and scene effect clock, with transparent ONE/ONE blending and no depth writes or shadows. |
| CEM-07 | `MeshBumpSpecGlowColorize.fx` uses base RGB colorized by base alpha, tangent-space normal RGB, and vertex fill from Diffuse and spherical fill lighting without emission. Pixel RGB is twice that surface times sun diffuse plus vertex fill, plus light specular times SpecularTexture.rgb times the clamped normal/half-vector dot raised to `32 * SpecularTexture.a`, plus Emissive times NormalTexture.a. The material Specular vector is unread. UVOffset.xy shifts all three samplers. | Dedicated opaque `sph_t2` family, stored-value arithmetic, authored tangent frame and normal/specular uploads. Emissive defaults to (1,1,1). Existing bump/lighting/shadow policy is retained. |
| CEM-08 | `RSkinAdditive.fx` uses Color times light-scale RGB and alpha (vertex-saturated), UV plus scene time times UVScrollRate.xy, and sampled base RGB times vertex color with ONE/ONE composition. | Existing additive shader and typed bindings under its own t0 selector; native bone/weight streams and pose palette supply skinning. No depth writes, shadows or alpha attenuation. |
| CEM-09 | `RSkinAlpha.fx` uses spherical vertex diffuse plus Emissive, vertex specular with exponent 16, pixel RGB `2 * base.rgb * vertex_diffuse.rgb + vertex_specular`, and alpha `base.a * Diffuse.a * LIGHT_SCALE.a`. | Existing programmable alpha arithmetic under its own sph_t0 selector, stored values, native skinning, SRCALPHA/INVSRCALPHA and no depth writes. No forced BatchMesh fog-of-war multiply; existing opt-in scene fog remains available. |
| CEM-10 | `MeshShield.fxo` multiplies energy, wave and vertex RGB; energy UV is offset by one quarter of distortion RG. Base, wave and distortion UV scales and vertical time scrolls are authored independently, with distortion sampled at the wave coordinates and wave sampled at the distortion coordinates. Vertex color is saturated `COLOR * saturate(source_normal.z + EdgeBrightness) * Color`; texture and Color alpha do not attenuate additive RGB. | Explicit compiled t0 selector, ONE/ONE, no depth writes, no culling or shadows, stored values; existing scene effect clock. All three texture bindings are required, with missing bindings diagnosed rather than an invented unbound-sampler result. |
| CEM-11 | `MeshShieldFarseer.fxo` computes a rim at each vertex from the world normal and toward-eye vector: `2 * pow(abs(1 - dot(N,V)), AtmospherePower) * Atmosphere.a`. Pixel RGB multiplies that interpolated rim by `Atmosphere.rgb * Atmosphere.a`, so alpha is applied twice. Light scale and textures do not contribute. | Explicit compiled sph_t3 selector, ONE/ONE, no depth writes, back culling, no shadows or time input, stored values. Atmosphere defaults to (0,0,0,1), power to 4.5. |
| CEM-12 | `Planet_Deathstar.fx` combines a normal-mapped opaque surface with separately normal-mapped, horizontally scrolling clouds; spherical diffuse/reflection fill, independent ambient and sun light; Fresnel reflection; atmosphere and night-side city light; and filmic output. Highest-detail `sph_t3` reads the authored gamma, pi, light/ambient/fill factors, luminance weights, saturation, atmosphere/power and city color. Base alpha, Emissive, Diffuse and Specular do not contribute at this detail. | Stored-value opaque family with authored binormals and three required auxiliary textures. The environment owns its background placements and uses the same family upload; the existing effect clock advances CloudScrollRate, with no writes at zero rate. Ambient is independent RGB times alpha from the actual environment, or the existing P0 ambient for standalone previews. No authored-value bounds are inferred from installed content. |


CEM-12 qualification details (the Remake-owned highest-detail effect, ported
under the author's permission; these are behavior equations and authored defaults,
not retail shader source):

| Rule | Qualified behavior |
|---|---|
| CEM-12A | Diffuse fill is `pow(abs(SH(normal)), gamma) * fill_factor + ambient_factor * ambient`; reflection fill desaturates the gamma-transformed SH reflection toward its luminance by saturation_factor, then applies fill_factor and adds ambient. The reflection probe deliberately combines the model-space away-eye ray with the world normal. The renderer's precomputed normal matrix supplies the inverse-basis view direction, avoiding a per-vertex matrix inverse. Environment ambient is actual RGB times alpha, uploaded through an ambient-only setter: existing SH, sun, population policy and global shadow settings are untouched. Incomplete environment-light records are diagnosed and retain the established preview ambient. |
| CEM-12B | Surface roughness is `(1 - normal.a) * 0.5 + 0.25`. Fresnel starts at `roughness * 0.56 + 0.04` and approaches one with the fifth power of the clamped view edge. Sun diffuse uses the squared saturated normal/light dot; specular uses the saturated normal/half-vector dot raised to 12. The diffuse portion is scaled by `1 - roughness`. |
| CEM-12C | Cloud diffuse is scaled by 0.7; cloud Fresnel is `0.792 * edge^5 + 0.208`. Cloud alpha mixes cloud and surface radiance; the mixture is multiplied by light-scale RGB and unit RGB, then divided by authored pi. UV scroll is horizontal scene time times CloudScrollRate. |
| CEM-12D | Night city light is `(1 - cloud.a) * (1 - surface_fresnel) * night^2 * CityColor.rgb`, independent of BaseTexture alpha and light-scale. The inner atmosphere enters the surface term with its alpha, squared sun diffuse and atmosphere rim. The separate outer term is `0.5 * gamma_atmosphere * max(sun_diffuse, 0.001) * rim * gamma_sun * light_factor`. |
| CEM-12E | Gamma transforms use absolute RGB before the power. Filmic output first subtracts 0.004 and floors at zero, then maps each component x with `x * (6.2*x + 0.5) / (x * (6.2*x + 1.7) + 0.06)`. Output is stored-value RGB. The standard stored-ALBEDO writer marker remains so the renderer can apply its established Compatibility-backend transfer compensation. |
| CEM-12F | Unauthored defaults: gamma 2.2, pi 3.1415, light/ambient/fill factors 20/3/5, luminance weights (0.299,0.587,0.114), saturation 0.3, cloud rate 0.0025, atmosphere (0,0,0,1), power 4.5 and city RGB (1,1,1). Named normal/cloud/cloud-normal bindings are required; unread Emissive, Diffuse, Specular and base alpha do not add terms. |

The generic selector also admits this qualified family for explicit unit previews
and ordinary props. Background placements remain environment-owned, and the
existing catalog marker exclusion still applies. Clock admission stays in the
existing central family policy (as for the compiled shield): only a nonzero
CloudScrollRate subscribes, and the environment seeds the same engine-owned float
time binding used by populated materials. This adds no new timing service.

The ability-shell consumer also recognizes the explicit compiled shield names on the existing hidden `SHIELD` and `STEALTH` meshes. Texture shields retain the established `MeshShield.fx` adapter and shield clock, including the same authored texture diagnostics. Farseer uses its texture-free family and subscribes to no clock; its ordinary rim output is unchanged, while the engine-owned optional `eawr_cloak_tint_enabled` scalar enables the existing per-instance cloak tint only on cloak uploads. Mounted effect absence selects the shared opaque green missing-effect material and loads no authored textures. These routes use the existing DEFEND/cloak visibility gates and do not add a timer or simulation rule.

The Coruscant backdrop's atmosphere ring and cityscape are separate authored
surfaces using CEM-02 and CEM-03. Its planet body uses the existing `Planet.fx`
route. This does not qualify additional atmosphere or city-color arithmetic
inside that planet shader, nor any other compiled shader name.

Stock shader source and bindings remain unchanged. Synthetic captures compare
compiled aliases with their existing adapters exactly; the doubled route has
its own brightness and saturation control. Unsupported near-names and missing
required material fields remain diagnosed.
