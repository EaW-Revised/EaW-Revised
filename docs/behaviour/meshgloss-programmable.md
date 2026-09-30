# MeshGloss programmable behavior and prototype lighting

The `sph_t0/sph_t0_p0` arithmetic applies to the pinned `MeshGloss.fx` (SHA-256 `d44c399eb75bfa27f274a85d80c24727f4ef73c4b08d485c965c14ad88b5abd0`). See [shader translation](shader-translation.md) for pass selection.

## Observable source-supported rules

- Position uses the supplied world/view/projection transform. Lighting uses
  world position and the normalized normal transformed by the world's upper
  3-by-3 portion. Preserve the documented vector-left convention through any
  representation conversion; do not replace this with an inverse-transpose
  normal transform as an unsolicited behavioral correction.
- Diffuse irradiance has three independent channel matrices. For a normalized
  world normal extended with homogeneous component one, each channel is its
  quadratic form with that channel's matrix (`m_sphAll`). There is no additional
  clamping or normalization of the resulting irradiance in this path.
- Specular is evaluated per vertex. The eye direction points from world position
  to the supplied eye and is normalized. The half direction is the normalized
  sum of that direction and the supplied light-zero direction. The nonnegative
  normal/half-direction cosine is raised to **16**, then multiplied by the
  light-zero specular RGB. The shader does not normalize the supplied light
  direction itself. `Shininess` does not change this exponent.
- The vertex diffuse RGB is material Diffuse times irradiance times light-scale
  RGB, plus material Emissive. Its alpha is light-scale alpha. The vertex
  specular RGB is material Specular times the preceding specular-light RGB;
  light-scale RGB does not multiply that term. Lighting is interpolated from
  vertices, not recomputed per fragment. UVs pass through unchanged.
- After sampling the base texture, the fragment RGB is twice interpolated
  diffuse RGB times texture RGB, plus interpolated specular RGB times texture
  alpha. Fragment alpha is interpolated diffuse alpha. Texture alpha therefore
  controls gloss contribution, not output coverage in this path.
- Sampling is linear minification/magnification/mip filtering, wrap U/V and
  clamp W. Preserve the common scene's explicit sRGB texture convention.
  Depth writes and less-or-equal depth comparison remain enabled. Blending is
  enabled exactly when light-scale alpha is below one, using source-alpha and
  inverse-source-alpha. The accepted renderer baseline disables fog; a produced
  fog varying does not authorize adding an extra fragment fog operation.
- `team_colour` and `Shininess` remain recorded common/material inputs but are
  not consumed by this selected path. Do not invent tinting or roughness mapping.
  Degenerate normals/eye-half vectors are not a newly defined compatibility
  oracle; synthetic qualification and the common scene use nondegenerate inputs.

## Original common-scene uniform policy

The following is a **prototype presentation choice**, not a recovered original
engine light-to-SH encoder. Both renderers must use it identically. It fills the
previously unspecified external uniforms without changing shader arithmetic.

Use common world coordinates for positions, normals, eye and light. Normalize
the scene's `direction_toward_light` once to obtain L. Let A be the scene ambient
RGB and C be directional linear RGB multiplied by its scalar intensity. The
chosen smooth hemispherical irradiance is A + C times (1 + normal dot L)/2.

Represent that irradiance with three symmetric 4-by-4 channel matrices: the
upper 3-by-3 entries are zero, the bottom-right entry is A[channel] + C[channel]/2,
and the three last-row/last-column off-diagonal pairs are C[channel] times the
corresponding component of L divided by four. These are logical row/column
entries; transpose/pack only as required to preserve their mathematical meaning.
Do not copy the effect's illustrative default coefficient matrices.

Set `m_lightScale` to (1,1,1,1). Supply normalized L as light-zero direction and C
as light-zero specular RGB. Supply the current common camera eye as `m_eyePos`.
Material parameters come unchanged from the selected ALO submesh. No second
ambient, directional, PBR, tone-map or exposure contribution may be silently
added by the host engine. Record output transfer and any unavoidable composition
conversion separately; both final comparison paths must agree.

## Original discriminating cases

These are arithmetic checks before framebuffer blending/output transfer, with
linear sampled RGB values. They are not claims of GPU bitwise equality.

1. A coefficient matrix with only bottom-right 0.3 yields irradiance 0.3 for
   every unit normal. With off-diagonal x/homogeneous entries both 0.2 and all
   others zero, irradiance is 0.4 for +X and -0.4 for -X; this detects a hidden
   clamp and a lost symmetric contribution.
2. Under the common policy, normals L, perpendicular to L, and -L yield A+C,
   A+C/2, and A. For the frozen scene these are respectively
   (2.08,1.96,1.82), (1.08,1.02,0.96), and (0.08,0.08,0.10), within stated
   floating tolerance. This tests matrix packing independently of a screenshot.
3. Aligned nondegenerate normal, eye direction and light direction give unit
   specular cosine. With material Specular (0.6,0.4,0.2) and light specular
   (1,0.5,0.25), the vertex specular RGB is (0.6,0.2,0.05). Changing Shininess
   from 4 to 64 must not change this path's output; a half-cosine gives the
   factor 1/65536 for either value.
4. Use irradiance (0.4,0.8,0.2), material Diffuse (0.5,0.25,1), Emissive
   (0.1,0,0.05), and light-scale (0.5,1,2,0.3). Vertex diffuse becomes
   (0.2,0.2,0.45,0.3). With case 3's vertex specular and sampled texture
   (0.25,0.5,1,0.4), fragment output is (0.34,0.28,0.92,0.3). Changing only
   texture alpha to zero removes the specular term but leaves output alpha 0.3.
5. Use distinct lighting values at triangle vertices to distinguish interpolation
   of vertex lighting from a per-fragment reconstruction. Compare an independently
   computed interpolation at an interior sample under a controlled projection.
