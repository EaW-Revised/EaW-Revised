# Vegetation effects: Tree.fx and Grass.fx (P1 EAWR-32, wind EAWR-147)

## Applicability

The EaW `Tree.fx` and `Grass.fx` effects as published for mod tooling (knowledge only; no source
text is reproduced), and the FoC engine's wind feed for them, read from the symbol-bearing FoC
debug build (EAWR-147). Rules below are public-source or engine behaviour unless marked project
policy. The consumers are `src/presentation/godot/legacy/tree.hpp` and `grass.hpp`, the
engine-free rule `include/eawr/presentation/lighting/wind.hpp` and the land map mode.

The motion the owner saw on FoC Naboo (EAWR-147) is this vertex displacement. It is not a model idle
animation: the trees and ground cover are rigid `Tree.fx` and `Grass.fx` meshes that no idle clip
drives.

## Tree.fx

| Rule | Behaviour |
|---|---|
| T-01 | Render phase `Transparent`, vertex type `alD3dVertNU2`; the DX8/SM1.1 technique `sph_t1`, pass `sph_t1_p0` (a second pass only restores sampler filtering). `sph_t0` is a fixed-function fallback. |
| T-02 | Parameters: Emissive float3 (0,0,0), Diffuse float3 (1,1,1), Specular float3 (1,1,1), Shininess float 32 (unread), BendScale float 1, BaseTexture, NormalTexture (declared, never sampled). |
| T-03 | Vertex colour D = Diffuse × SH irradiance (SPH_LIGHT_ALL quadratic form of the world normal) × LIGHT_SCALE.rgb + Emissive, alpha LIGHT_SCALE.a; specular S = Specular × max(N·H, 0)^16 × light-0 specular (Blinn half vector from the eye). Both are saturated to [0, 1]. |
| T-04 | Pixel RGB = 2 × texel.rgb × D.rgb + 2 × S × texel.a; alpha = texel.a × LIGHT_SCALE.a. |
| T-05 | Alpha test GREATER, reference 128; Z write on, LESSEQUAL; SRCALPHA/INVSRCALPHA blending only while LIGHT_SCALE.a < 1. Cull mode is not set by the effect. |
| T-06 | Base sampler: point min/mag, linear mip. |
| T-07 | Wind: world position += BendScale × WIND_BEND_VECTOR.xyz × z² × WIND_BEND_VECTOR.w, z being the vertex's height in its own mesh space (the pivot at the base, z up). The bent position feeds the specular half vector; the normal is not bent. The effect has no time term: the engine animates WIND_BEND_VECTOR (W-02). |
| T-08 | Project policy: LIGHT_SCALE.a is 1, so the pass is drawn alpha-tested in the opaque pass. The wind inputs follow W-09. |

## Grass.fx

| Rule | Behaviour |
|---|---|
| G-01 | Render phase `SemiOpaque`, vertex processor `Grass`, Z-sort and billboard flags set; technique `sph_t0` (VS 1.1, PS 2.0), pass `sph_t0_p0`. No fixed-function technique. |
| G-02 | Parameters: Emissive float3 (0,0,0), Diffuse float4 (1,1,1,1) (alpha = base opacity), Diffuse1 float4 (1,1,1,1), BendScale float 1, BaseTexture. A parameter the effect does not declare is ignored. |
| G-03 | The engine's grass vertex processor rotates each clump about its vertical axis through the clump centre (a second UV set it fills) to face the camera. The model does not store the centre. |
| G-04 | Wave a = 0.5 + 0.5 × sin(6.28 × frac(ts × TIME + (x + y) / 20)) over the pre-billboard mesh-space position; w = BendScale × wind speed / 10, ts = 0.125 + 0.875 × w. |
| G-05 | The world position moves by (1 − V) × (10w + (10 + 10.5w) × a) along the unit wind direction (V is the first UV's v), after the clump billboard. |
| G-06 | Normal = object +z through the world matrix. Colour = saturate(irradiance × lerp(Diffuse.rgb, Diffuse1.rgb, a) × LIGHT_SCALE.rgb + Emissive); alpha = Diffuse.a × LIGHT_SCALE.a × distance fade. |
| G-07 | Pixel RGB = 2 × texel.rgb × fog-of-war.rgb × colour; alpha = texel.a × colour alpha. |
| G-08 | Alpha test GREATER, reference 8; SRCALPHA/INVSRCALPHA blending with Z write, LESSEQUAL; point min/mag, linear mip. |
| G-09 | Project policy: no distance fade or fog-of-war stage, and no clump billboard (G-03's centre rule is not established), so cards draw as authored with culling off. TIME and the wind follow W-09. |

## Engine wind feed (EAWR-147)

| Rule | Behaviour |
|---|---|
| W-01 | Scene clock: 0 s when the scene is created. Every render think advances it by the frame's elapsed time, capped at 1 s, and subtracts 28800 s once it exceeds 28800 s. The effect `TIME` parameter is a scene time the scene hands the graphics driver before it renders; it is taken to be this clock. |
| W-02 | WIND_BEND_VECTOR.xyz = wind × 0.5 × (1 + sin(2π t / P + 2π sin(2π x / D) sin(2π y / D))), with the scene wind (W-05), the scene clock t, the period P = 3 s and the phase dimension D = 1000 (both fixed at scene creation; no writer changes them). (x, y) is the centre of the placed model's world box (W-04). WIND_BEND_VECTOR.w = 1 / H², H being that box's height. 2π is 6.2831855. |
| W-03 | The bend inputs are taken once per placed top-level model and shared by every mesh of it: each render task copies the model's (centre x, centre y, box bottom z, box height). A model attached to another model (not to the scene) sets none of its own. |
| W-04 | The model's object box is the union of every sub-object's own box moved by its bone's reference pose; every mesh counts, hidden, shadow-volume and collision meshes included, and an optional user box is added. One sub-object type is left out (not identified; meshes are not it). A box moves by transforming its centre and summing its half extents through the absolute rotation-scale matrix; the world box is the object box moved so by the placement. |
| W-05 | Land mode sets the scene wind to speed × (cos h, sin h, 0) with the current environment's speed (`0x2c`) and heading (`0x2b`, degrees) whenever it updates its render settings (R-WX-01); nothing else writes it. A scene starts with no wind, so space scenes have none. The same heading turns the cloud-shadow velocity. |
| W-06 | WIND_GRASS_PARAMS = (wind / \|wind\|, \|wind\|) of the scene wind, the direction being 0 when the length is 0. So G-04's w uses \|wind\|, and G-05 moves along the unit direction. |
| W-07 | Consequences. With no wind neither effect moves geometry (the bend vector is 0; the grass direction is 0), but the grass colour lerp still follows the clock at ts = 0.125. A tree's top moves by at most BendScale × \|wind\| × (z_max / H)², back and forth along the wind every 3 s, the phase set per model by where it stands. A grass tip moves between 10w and 20.5w + 10 (w as in G-04) along the wind with period 1 / ts, the phase set per vertex. |
| W-08 | Consequence for Naboo (FoC `_mp_land_naboo.ted`): environment 0 (`Sunrise_Clear`) blows at 5.06 and environment 1 (`Noon_Clear`) at 2.0, both along +X. Its ground cover has BendScale 0.07 (0.25 on one model) and its trees 0.5 to 0.68. At 2.0 the ground cover waves with a 7.3 s period and moves its tips about 10 units; the tree tops swing by up to about 1 unit every 3 s. At 5.06 the ground cover period is 6.4 s. None of these models has an idle clip. |
| W-09 | Project policy: the wind is the one of the environment record the viewer's lighting uses (`--eawr-environment map`: record 0); without a map environment there is no wind. The scene clock is the populated scene's idle clock: in a capture its held sample plus `--eawr-map-idle-offset`, in 1/30 s, so a fixed-offset capture repeats; in the live view real frame time from that offset, advanced and wrapped by W-01. Both effects use it for TIME and for W-02's t. |

## Cases

| Case | Input | Expected |
|---|---|---|
| C-W1 | Wind (2, −1, 0), model box centre (0, 0), t = 0.75 s | Bend (2, −1, 0): a quarter period puts the sine at 1. At t = 0 it is (1, −0.5, 0). |
| C-W2 | Centre (125, 250), t = 0 | Phase 2π sin(π/4) × 1; bend = wind × 0.01805. |
| C-W3 | Tree vertex at mesh z 24, box height 48, BendScale 0.6, bend (1, 2, 0) | World offset (0.15, 0.3, 0). At z −24 the same; with box height 0 none. |
| C-W4 | Grass tip (v = 0) at mesh (0, 0), BendScale 1, wind (0, 4, 0), t = 0 | w = 0.4, ts = 0.475, a = 0.5; offset (0, 11.1, 0). At v = 1 no offset; with no wind no offset. |
| C-W5 | Two meshes: box (−1, −1, 0)–(1, 1, 10) on an identity bone, a hidden box (0, 0, 0)–(2, 2, 4) on a bone raised by 8 | Object box (−1, −1, 0)–(2, 2, 12). |
| C-W6 | Clock at 28799.5 s, frame of 1 s; clock at 2 s, frame of 3 s | 0.5 s; 3 s. |

## Unknowns

- The sub-object type W-04 leaves out of the model box, and the user box (not modelled).
- That the effect `TIME` is exactly W-01's clock (the scene hands over one of its fields; the
  field was matched by position, not by name).
- Unit wind disturbances: the scene keeps a list of wind disturbances that units with a wind
  disturbance behaviour add; which effect reads them was not followed, and they are not modelled.
- The distance-fade constants and the grass clump-centre rule (G-03, G-09).
