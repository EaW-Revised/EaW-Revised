# Rendering policies and fixtures

These are presentation contracts. Original-game findings live in
[behaviour](behaviour/README.md); project policies and synthetic fixtures do not
prove original visual fidelity.

## Colour policy

The viewer renders with Forward+ (Vulkan by default, D3D12 as Godot's automatic
fallback). Retail Direct3D 9 works in gamma space: it samples texels without
sRGB decode, computes on the stored 8-bit values and blends them in the
backbuffer. The viewer keeps those stored-value semantics:
- Shaders declare no `source_color` hint, so samplers return stored texels and
  colour uniforms are not converted. `ALBEDO` is the stored value. Adapters with
  a linear material equation (MeshGloss, the fixed-function mesh, RSKIN, tree and
  grass adapters) decode their texels, compute, and encode the result themselves.
- One compositor pass after the transparent pass
  (`src/presentation/godot/stored_output.hpp`) clamps the colour buffer to
  [0, 1], as the 8-bit backbuffer saturates, and decodes it once. With MSAA it
  clamps each sample before averaging them (see [render profiles](#render-profiles)).
  The linear tonemapper at exposure 1 / white 1 then encodes the value back, so
  the output pixel is the stored value.
- The environment enables no glow, adjustments, auto-exposure or debanding. Its
  clear colour is encoded once more, because the pass decodes it too. Retail
  bloom is a second pass after the decode ([bloom](#bloom)), not Godot's glow.
- Blends act on stored values in the half-float buffer, clamped once at the end
  instead of per draw. For additive (ONE/ONE) stacks such as Nebula.fx this is
  the retail result: the terms are non-negative, so one clamp of the sum equals
  saturating after each draw; only the 8-bit rounding per draw is not
  reproduced. Godot's linear-light features (SSAO, SSIL, GI, volumetric
  fog, glow) would see stored values, so they are not used.

Compatibility (`rendering/rendering_device/fallback_to_opengl3`) is an unpinned,
untested emergency fallback for one milestone after the Forward+ switch (Forward+ renderer decision,
Forward+ fallback cleanup). It is not a second supported renderer or a capture target. It writes
`ALBEDO` to its sRGB output through an approximate decode that darkens stored
values below about 60/255 by up to 4 levels. Only the stored-product adapters
(MeshAdditive, MeshAdditiveOffset, solid colour, space sky MeshAdditive)
compensate that: `compatibility_source` swaps their `eawr_stored_albedo` writer
for the fitted terrain and environment rendering compensation. `RenderingServer::shader_set_code` does not
run Godot's shader preprocessor, so `#if CURRENT_RENDERER` cannot make this
choice. Dark Forward+ scenes therefore sit above Compatibility: Coruscant by a
mean of 3.6 levels, and by 0.5 once its sky, nebula and planet are compensated
on Compatibility too.

Remove the retained Compatibility-only branches when the first new
RenderingDevice-only feature lands:
- `src/presentation/godot/shader_adapter.hpp`: `compatibility_albedo_writer`
  and `compatibility_source`, the fitted shader writer replacement.
- `src/presentation/godot/stored_output.hpp`: `backend_source` selects that
  replacement when no RenderingDevice exists; the no-device compositor path
  keeps the fallback runnable.
- `src/presentation/godot/scene_bloom.hpp`: the no-device path disables the
  RenderingDevice bloom compositor.
- `src/presentation/godot/renderer.cpp`: the no-device branch in the clear-colour
  conversion keeps Compatibility's output colour in stored space.
- `src/presentation/godot/renderer_upload.cpp`: the five-sampler limit and
  GL 3.3 checks in material admission keep shaders portable to Compatibility;
  retain the independent Vulkan uniform-block bound.
- `src/presentation/godot/legacy/mesh_additive.hpp`,
  `src/presentation/godot/legacy/mesh_additive_offset.hpp`,
  `src/presentation/godot/legacy/mesh_solid_color.hpp`, and
  `apps/viewer/src/space_environment_internal.hpp`: stored-product shader
  sources and comments identify the writer that receives compensation; the
  shader sources themselves also serve Forward+.

Fidelity backlog: recheck dark-scene colour and shadow output if the emergency
fallback is ever used; no Compatibility capture or parity result is pinned.

Shadow-receiving variants multiply the shadow term into the stored value; the
floor they use is set as described under [shadows](#shadows).

## Render profiles

Owner decision D2 = B on graphics defaults and parity decisions: players get enhanced defaults, and every
capture and test keeps the retail look unless explicitly overridden. The host applies one profile to the root viewport before any mode
starts (`apps/viewer/src/render_profile.hpp`):

| Profile | MSAA | Screen-space AA | Anisotropic filtering | Default for |
| --- | --- | --- | --- | --- |
| retail | off | off | off | every run with a capture, report probe, self-test or window-resize test, and the fixed runs |
| enhanced | 4x | SMAA | 16x | a player's view: `--eawr-camera-interactive`, or the launcher's start (`--eawr-launcher-start`), without a capture, self-test or resize test |

**Every scene uses the profile's own AA, space battles included (owner decision) (legacy EAWR-6972).** SMAA was switched
off in space battles for a while because it smears the single-pixel, low-luma starfield dots (stars at luma 12 or
more drop by about 40%, at 24 or more by about 75%); the owner restored it. One function decides,
`render_settings(profile, scene)` in `render_profile.hpp`; it takes the scene (front end and attract, space battle)
so a later per-scene rule has one place to go, and today returns the profile's settings for every scene. Every
apply, re-apply (options screen, options mode) and report goes through `render_profile_viewport.hpp`, which
recomputes profile, scene and the evidence override. Dots still blink while the camera turns (about a quarter of
them per frame, with or without SMAA, because a one-pixel dot changes pixel phase).

**A player's start.** The launcher always names a start report, and marks a start with `--eawr-launcher-start` when its
extra arguments name no evidence or probe switch (`player_start_requested` in `launch_plan.cpp`: any `--eawr-*` switch
beyond the few a player run takes keeps the start an evidence run). The report alone does not make that start
evidence: it reads the player's saved settings (window, vsync, volumes, texture detail, render profile) like any
player start. The profile is decided by `startup_render_profile` in `render_profile.hpp`: `--eawr-render-profile`
wins, then the saved AA level, then the default above (a saved `automatic` is the default).
Evidence runs (capture, probes, self-test, resize test, benchmark, perf trace) never read the saved settings.

**3D SubViewports follow the profile.** MSAA, screen-space AA and anisotropy are per viewport, and the profile is
applied to the root viewport. The SubViewports that draw 3D the profile smooths (the galactic world layer, and the
planet on the loading screen) register with `register_render_viewport()`; every later apply (options screen, scene
change) reaches them, and a freed one leaves the list (`unregister_render_viewport`, or pruned at the next apply).

The galactic map draws its 3D as a chain of full-size layers (background, midground, midforeground, world), each
layer's colour copied under the next as its canvas background, and the galaxy view composites the chain without
drawing 3D itself. Only the world layer takes the profile, so MSAA, SMAA and supersampling resolve once over the
composited picture; a middle layer takes it too when the galaxy's art authors models on it (`In_Midground` /
`In_Midforeground`; FoC authors no models there). FoC does draw particles on the middle layers (the corruption
particles on midground, the corruption lines on midforeground); particles alone do not make a layer take the profile,
because soft billboards have no geometry edges for MSAA to smooth, and those layers stay plain. The
background, middle layers without models and the galaxy view keep native, non-multisampled targets
(`galactic_layer_takes_render_profile` in `render_profile.hpp`). Giving every layer the profile redid MSAA, SMAA and
supersampling four times per frame.

**Options AA levels.** The AA slider has four integer steps, saved as `video.render_profile`:

| Level | Saved value | MSAA | SMAA | Anisotropy |
| --- | --- | --- | --- | --- |
| Off | off | off | off | off |
| Low | low | 2x | off | 16x |
| Medium | medium | 4x | on | 16x |
| High | high | 8x | on | 16x |

Supersampling is an independent Video option: Off (native resolution, default), 1.5x or 2x per axis,
saved as `video.supersampling` step 0, 1 or 2. It works with every AA level, including Off, and
uses 16x anisotropy when enabled. Higher settings use more GPU power and memory; the VRAM guard
below can reduce the applied scale without changing the saved choice. The control is a slider in Video >
Advanced. Cost figures are kept here only, never in the settings UI. On an RTX 2060 at 1440p, Medium
(4x + SMAA) took 5.7 GPU ms, Medium + SS2 took 16.7 ms (about 3x), and High + SS2 (8x + SMAA + SS2) took
25.6 ms. These figures describe that measured scene, not every scene or device.

Explicit levels keep this AA table in every scene, including space battles. Old saved `retail` migrates
to Medium with supersampling Off because launcher starts previously ignored that saved value.
Old `high` and legacy `enhanced` migrate to Medium plus 2x supersampling; old `max` migrates to High plus 2x.
The separate supersampling field identifies the new High (8x), so it round-trips without being mistaken
for old High (4x plus SS2). An explicitly saved supersampling step wins regardless of line order.
New explicit Off is stored as `off`, so it survives later starts without being mistaken for an old value.
An absent or automatic AA choice uses Medium (4x MSAA, SMAA in every scene), controlled by the single
`player_default_render_profile` constant. Options leaves an automatic choice unchanged until the AA
slider is touched. Supersampling defaults to Off independently of AA.
Shadow budgets retain the enhanced settings for each enabled level.

`--eawr-render-profile retail|enhanced|low|medium|high` forces an AA profile (`max` is accepted as an alias for High with 2x supersampling). `--eawr-aa msaa=8,ss=2,smaa=1`
is a run-local override for both players and evidence; any subset is accepted. MSAA accepts 0/2/4/8,
SS accepts 1/1.5/2 (also 1.0/2.0), and SMAA accepts 0/1. Unknown keys, duplicates, missing fields and invalid
values fail startup with a message. The older `--eawr-msaa 0|2|4|8` and `--eawr-smaa on|off` also work;
the combined switch wins for fields it names. Overrides survive later Options and scene changes and never
write themselves to the settings file. The launcher retains the player-start marker for `--eawr-aa` alone;
captures and other evidence switches still select an evidence run.

Supersampling scales each viewport's internal 3D resolution with bilinear filtering (the pinned engine's
only supported filter above 1.0). Its memory guard uses the rendering device's actual local heaps and the
smaller current Vulkan process budget, refreshed through the existing asynchronous render-thread probe
before live level, registration or resolution changes. The main thread retains the current targets while
polling the fresh sample without waiting. Root and registered 3D SubViewports share at most 35% of this
memory: each target gets an equal share, so multiple full-screen layers cannot each spend 35%.

The conservative planning estimate includes HDR colour (8 bytes/pixel), depth (4), two HDR resolve/post
targets (16), scratch (4), multisample colour/depth (12 per sample), native output (8) and
internal-resolution SMAA targets (16) when on. Live replacement counts the old target estimate plus the new estimate, since both may be
alive during the switch. The steady goal is retained separately: when possible, a temporary bridge
fits beside both the old and steady targets, then recovers the steady level on the following frame.
It is an allocation estimate, not an exact engine-memory measurement. To fit its allowance,
a target drops supersampling through 2.0, 1.5, 1.0, then MSAA through 8x, 4x, 2x, off; SMAA is disabled
last if needed. An independent 8192-pixel dimension guard remains. If native targets alone cannot fit,
the report says so rather than claiming safety. When VRAM cannot be read, supersampling is disabled
above 1920x1080 output pixels. Resizes, registration changes and completion of the device probe recompute
the decision. Without an old target, a 4 GiB device with one 1440p raw `msaa=8,ss=2,smaa=1` target selects SS1.5; an 8 GiB device
permits SS2 there; a 24 GiB device permits 4K SS2. Old targets, multiple viewports or a smaller current
process budget can lower these settings further.

Mode reports (including the front-end startup report) include the selected profile and actual MSAA,
supersampling, screen-space AA and anisotropy. Their `budget` object logs vendor/adapter, device-local
bytes, current process budget and source, old/new/transient target estimates, allowance, viewport count, requested MSAA/SS, clamp
reason and whether the result fits. Clamps also reach the console. `name` is `custom` when a run override
differs from the selected profile after safety budgeting. AA never changes frame pacing, vsync or the
AMD frame cap. The raw test switch can still request 8x MSAA with SS2 and SMAA; the guard applies to it
as well. This guard is a precaution, not a guarantee that a scene's textures, meshes and other allocations will fit.

- **Terrain sampler.** Only the terrain layer array requests anisotropic
  filtering. Retail TerrainRenderBump samples linear, so the retail profile
  turns anisotropy off (Godot's project default was 4x). That changed land
  captures once: the Naboo tactical view moved by a mean of 3.5 levels, with
  softer distant terrain. Unit, sky and foliage adapters keep their retail
  sampler hints in both profiles, so 16x reaches the terrain only.
- **MSAA and stored values.** Each MSAA sample stands for one sample of the
  retail 8-bit backbuffer, which saturates before the resolve. Godot resolves
  the unclamped half-float samples before the post-transparent callback, which
  would brighten the edges of overbright additive draws. The stored-value pass
  therefore re-resolves from the multisample buffer: it clamps each sample,
  averages them and decodes the result into the colour texture the tonemapper
  reads. Without MSAA it decodes that texture in place, as before.
- **SMAA** runs after the tonemapper, on stored values, which is the space it
  expects. Flat regions keep their value within one level; only edges change.
- **Bloom** ([bloom](#bloom)) runs in both profiles. It follows the decode, so
  with MSAA it blooms the per-sample-saturated resolve, and SMAA then runs on
  the bloomed frame.
- **Depth pre-pass switch.** Forward+ renders every opaque instance twice by default (the engine's
  `rendering/driver/depth_prepass/enable`, explicit in `apps/viewer/project/project.godot`). `--eawr-depth-prepass on|off`
  (after `--`, startup only, applied before the renderer exists, no per-frame cost; anything but `on` or `off` is
  reported as an error and ignored) overrides it for one run. The default is on; the default decision waits for the
  build host pair and the flag proof. Limits: the engine disables SSAO without the pre-pass (the viewer uses no SSAO, SSIL, SDFGI or
  VoxelGI), and only space scenes are pixel-compared so far (RaW60, Dev4: identical); a ground scene with
  alpha-tested foliage is not covered yet.
- **Alpha-tested foliage.** MSAA leaves Tree.fx cut-out edges stepped; SMAA
  smooths them. Alpha to coverage is not used. Godot 4.7.2 draws an
  alpha-to-coverage material in the transparent pass with alpha blending and
  without depth writes, and it stops casting shadows unless it also asks for a
  depth prepass. On Naboo two captures of the same scene, before and after the
  particles were released, then differed in 4,028 pixels at tree trunks behind
  foliage: without depth writes the edge samples follow the transparent list's
  draw order.
- **Mods.** The profile changes only viewport sampling and antialiasing, never
  a shader, texture or material, so mod assets render as in the retail profile
  apart from filtering and edges.

## Shadows

Retail FoC draws stencil shadow volumes. Object meshes cast through their
`MeshShadowVolume.fx` / `RSkinShadowVolume.fx` volumes; the terrain has no
volume and never casts. `StencilDarkenToAlpha.fx` marks shadowed pixels in the
frame-buffer alpha. `StencilDarkenFinalBlur.fx` averages that mask over four
taps 0.0015 UV apart and multiplies the gamma backbuffer by
lerp(colour, 1, lit) (`DESTBLEND=SRCCOLOR`), a multiply of stored values.

The viewer draws Godot's directional shadow map instead:
- **Casters.** Casters are the visible meshes. Terrain surfaces receive shadows
  but never cast them, as in retail; grass, water and sky domes do not cast
  either.
- **Stable cascades.** Godot 4.7.2 already fits each directional split
  to its frustum's bounding sphere and snaps the light-space bounds to texels
  (`servers/rendering/renderer_scene_cull.cpp`, `_light_instance_setup_directional_shadow`).
  There is no separate stabilization switch or public per-cascade projection
  setter. The viewer keeps four blended splits and a fixed tactical view-depth
  reach: 4096 source units on land, 8192 in space, clipped by the camera far
  plane. Pan and orbit no longer select a different initial reach from visible
  scene corners. Zoom/FOV changes still change the engine's sphere; this is
  not a custom engine cascade implementation.
- **Quality.** Retail uses a 4096 atlas, split offsets 0.125/0.25/0.5 and
  Soft Medium filtering; enhanced uses an 8192 atlas, offsets
  0.0625/0.1875/0.5 and Soft High. Four splits receive half the atlas edge
  each (2048 or 4096). Space halves these normalized offsets, preserving the
  near split depths despite its longer last cascade. Both use normal bias 5.
  Atlas storage
  remains 16-bit. Filtering complements the finer texels; screen-space AA
  alone cannot resolve shadow-map stair steps.
- **Filter and depth bias.** Godot multiplies the directional PCF
  kernel and the depth bias by the light's blur and by the filter radius (2
  texels for Soft Medium, 3 for Soft High). The depth bias is a percentage of
  each cascade's depth range (its sphere diameter plus the 20-unit pancake), so
  it grows with the cascade. A RenderingServer light starts with blur 0
  (a `DirectionalLight3D` node uses 1), which made every shadow a single hard
  tap and left the depth bias without effect: land captures with bias 0 and 50
  were identical. Land now sets blur 1 and depth bias 0.05: 1 to 3 source
  units in the two near enhanced cascades of a 1280x720 tactical view. Bias 2
  would have moved receivers 40 to 100 units toward the sun. Space also sets
  blur 1 and depth bias 0.05, with normal bias 5 (legacy EAWR-667): its earlier
  blur 0 disabled the soft filter and made the configured depth bias 2
  ineffective, leaving fine self-shadow hatching on sunlit hulls. Space reports
  record blur as well as bias so a capture identifies the effective policy.
- **Exceptions.** The top-down overview keeps a scene-fitted orthogonal map;
  the labelled debug-ship evidence view fits its shadow reach to the ship.
  Tactical shadows fade at the fixed reach, so distant geometry beyond it
  does not receive directional shadows. Capture reports record the atlas,
  split offsets, reach, filter and engine stabilization policy.
- **Floor.** The floor is the environment's `0x17` colour, per channel.
  The candidate environment reader decodes it with its per-record default
  (0.5, 0.5, 0.5); a present mini of the wrong size or with a non-finite channel
  rejects the record. All 415 EaW and FoC records carry it as three floats.
  Without `--eawr-environment map` the alo-viewer default environment gives
  0.5 grey. Under the stored-value mode the floor multiplies stored values like
  the retail blend; the Compatibility fallback multiplies linear values.
  `--eawr-environment-record` picks the record; the default is record 0.

The floor space was decided against the owner's retail screenshot of FoC
Naboo (`zoom_full.png`, kept under ignored `out/original/`). A building's crisp
shadow on the paved road, divided by the adjacent lit road, gives 0.43–0.52
red, 0.44–0.51 green and 0.66–0.76 blue (luma 0.45–0.52). For the tint 0.5 the
candidates give:

| Floor | Shadowed / lit, stored |
| --- | --- |
| **Stored space, tint (chosen; retail blend with `0x17` as its colour)** | **0.50** |
| Stored space, tint halved (FP-1) | 0.25 |
| Linear space, tint halved (P1 on Compatibility) | 0.54 |
| Linear space, tint | 0.735 |

The Highest-preset rig recapture confirms the stored-value multiply and
gives its colour: retail multiplies by the current environment's `0x17` colour,
per channel. A map with several environments draws one per battle at random
(R-SEL-03 in [effective environment](behaviour/p1-effective-environment.md)):

| Environment | `0x17` | Object shadows in the route's views |
| --- | --- | --- |
| Naboo 0 `Sunrise_Clear` (the viewer's default record) | 0.40, 0.42, 0.60 | Fall toward the camera |
| Naboo 1 `Noon_Clear` | 0.65, 0.64, 0.73 | Fall behind the objects, out of view |
| Coruscant 0 | 0.55, 0.55, 0.55 | |

Six Sunrise_Clear road and grass pairs (the owner's screenshot, a Highest rig
run and a stored-preset rig run) measure 0.42–0.51 red, 0.41–0.48 green and
0.64–0.84 blue. With `0x17` as the multiplier the pairs leave a mean offset of
2.5–4 levels per channel (−1 to 7 per pair); a free fit gives 0.38, 0.39 and
0.58 plus 4.5–6 levels. The offset lifts the dark blue channel most and explains
the raised blue; its source was not identified. A linear-space multiply by
`0x17` would give 0.66, 0.67 and 0.79. Against retail the fixed 0.5 used before
The earlier shadow floor was about 0.1 too light in red and green and 0.1 too dark in blue on
Sunrise_Clear, and 0.05 too dark on Coruscant. The Coruscant figure rests on `0x17` alone: at the
player-station and truss-station close-ups retail shows no stencil-shadow edge
to measure.

With `0x17` as the floor (RTX 4070 Laptop), each shadowed pixel over the same pixel
without shadows is `0x17` itself: 0.400, 0.415 and 0.600 on Sunrise_Clear, 0.65,
0.64 and 0.73 on Noon_Clear (`--eawr-environment-record 1`) and 0.55 on the
Coruscant Star Destroyer deck. The adjacent-patch method of the retail pairs
gives 0.41–0.44 red, 0.43–0.46 green and 0.60–0.78 blue over six Sunrise_Clear
pairs: the three shadow-retune sensor-view pairs and three zoom-full pairs whose patches
agree within 3% with shadows off. Before, those were 0.52–0.54, 0.51–0.54 and
0.54–0.66. The means sit 0.03 red, 0.01 green and 0.05 blue below the retail
pairs', about the 2.5–4 levels the retail pairs carry on top of `0x17`.

Acne was judged in matched before/after captures of Naboo buildings, rocks,
a cliff and the palace domes and of the Coruscant truss station. The share of
pixels a shadow darkens only partly (on/off ratio between the floor and 0.98;
acne and penumbrae) fell from 25–50% to 0.7–7% of each Naboo view.

## Bloom

Retail FoC draws `SceneBloom.fx` when the Bloom detail setting is on (Default_2
and Highest). Land and space mode turn it on for their scene every frame; the
map-preview render and reflection or refraction views turn it off. The pass
(bloom, traced in the FoC debug build):
- **Source.** After the transparent phase the backbuffer is copied, pixel for
  pixel, into a full-size 8-bit target (shared with heat distortion). Heat
  draws come after the copy and bloom is added after the heat pass, so bloom
  never sees heat distortion.
- **Targets.** Two A8R8G8B8 targets, each a quarter of the backbuffer per side
  (truncated), one mip level.
- **Bright pass.** Each target texel samples the copy at its centre through a
  bilinear, clamped sampler; at an exact quarter that averages the middle 2×2
  of its 4×4 block. A texel whose luminance (0.299, 0.587, 0.114; alpha has no
  weight) exceeds the cutoff keeps its colour, any other becomes colour⁵.
- **Blur.** Four iterations ping-pong between the targets. Iteration i averages
  four bilinear taps on the diagonals, size × (1 + 2i) / 2 target texels away
  on both axes; the half-pixel constants the shader scales are those of the
  bound quarter target.
- **Combine.** The last target, sampled bilinearly at each backbuffer pixel
  centre and multiplied by the strength, is blended with `ONE` /
  `INVSRCCOLOR`: src + dst × (1 − src) per channel, in the 8-bit backbuffer.
- **Parameters.** Strength, cutoff and size come from the current environment
  record (minis `0x23`, `0x24`, `0x28`; loader defaults 1.0, 0.9, 0.25), not
  from the effect's own defaults (0.1, 1.0, 0.25), which the engine overwrites
  every frame. Most FoC land records carry 1.0, 0.9 and 1.0, so the last blur
  taps 3.5 target texels (about 14 backbuffer pixels) away. Space records carry
  0.6 or 1.0, 0.9 and 0.25, or strength 0. No record exceeds 1.

The viewer draws the same pass as a second compositor effect after the
stored-value decode (`src/presentation/godot/scene_bloom.hpp`), in compute
shaders:
- It encodes the decoded colour buffer back to the stored value, clamps it and
  rounds it to 8 bits: that is the backbuffer copy. With MSAA it therefore sees
  the decode's per-sample-saturated resolve.
- The targets are RGBA8 storage textures of the render buffers, so the bright
  pass and every blur step round to 8 bits like the retail targets. Taps are
  explicit bilinear fetches with clamped addressing at the positions the D3D9
  half-pixel offsets produce.
- The combine encodes the frame, blends the scaled bloom on and decodes the
  result for the tonemapper.
- Map mode turns it on for land and space maps (`--eawr-bloom on|off`, default
  on) with the viewer's current environment's values (record 0 unless
  `--eawr-environment-record` picks another), or the
  loader defaults for a map without an environment record. Retail picks the
  current environment at random (R-SEL-03 in
  [the environment note](behaviour/p1-effective-environment.md)), so a rig
  capture may bloom with another record's values. Reports record the applied
  values under `scene_bloom`.
- Only a lit scene blooms. With the lighting policy off (no
  `--eawr-lighting`, a debug view whose hulls and terrain show their full-bright
  textures) bloom on top means nothing, so the default is off and the report
  says `scene_bloom.status: lighting_off`; an explicit `--eawr-bloom on` without a
  lighting policy is refused, like `--eawr-shadows on`. Such a capture has no
  `bloom_off` phase: its evidence frame (`scene_bloom.evidence_frame`) is the
  capture itself. Suites that compare a bloomed frame pass `--eawr-lighting sh`.
- Bloom spreads light past what drew it, so pixel attribution (units, effects,
  shadows, lighting policy, idle clips) cannot use a bloomed frame. With bloom
  on, the land comparison phases start with `bloom_off`, which redraws the
  configured scene without bloom, and every later comparison frame is drawn
  without it too. The evidence compares those frames (reported as
  `scene_bloom.evidence_frame`); the main capture keeps its bloom.
- Unit, effect and model modes and the space primary-sky evidence harness (a
  control or fog) keep it off. The effect then stays disabled and costs nothing.
  The Compatibility fallback has no compositor and no bloom.
- Known difference: the viewer draws particle heat in the transparent pass, so
  its bloom source includes heat distortion and retail's does not.

## Spherical harmonics

The encoder ports the MIT alo-viewer Calculate_Matrices contract at revision
9bb0053919cc5df8377610d4f91b11d956d6c2f4. Evaluate each light in D3DX order-3
basis with Z negated and coefficients weighted by RGB times alpha. Basis constants
are 0.282095, 0.488603, 1.092548, 0.315392 and 0.546274, with negative signs for
odd m. Pack per channel into the symmetric irradiance matrix with c1..c5 equal
to 0.429043, 0.511664, 0.743125, 0.886227 and 0.247708; add ambient RGB times
alpha to its constant term. Irradiance is n4-transpose times M times n4.

SPH_LIGHT_ALL includes sun and both fills; SPH_LIGHT_FILL includes fills only.
Source-basis matrices are conjugated into render basis (x,z,-y). The retained
meshgloss-hemisphere-v1 baseline uses ambient (0.08,0.08,0.10), directional
(2.0,1.88,1.72) and normalized render key direction (0.35,0.75,0.56).
A TED record's light directions take the retail heading of
[R-LIT-01](behaviour/p1-effective-environment.md#c-lighting-semantics-and-effect-parameters)
(`lighting::retail_light_direction`, light heading correction): heading 0 lies toward -Y, not
alo-viewer's +X. The SH matrices, the sun's directional light and shadows,
specular, the bump particles and the land sky sun all read this one direction.
Candidate TED field decoding does not confirm original environment selection;
see [effective environment](behaviour/p1-effective-environment.md).

## Hull bump and specular

`MeshBumpColorize` and `RSkinBumpColorize` draw their Highest technique,
`sph_t2` (hull bump and specular rendering; `src/presentation/godot/legacy/bump_colorize.hpp`), on stored
values like the retail ps_2_0:
- **Lights.** The two fill lights and ambient are per vertex
  (`SPH_LIGHT_FILL`, `eawr_sph_fill_*`), clamped to [0, 1] as a vs_1_1 colour
  output. The sun is per pixel against the `NormalTexture`: diffuse
  `saturate(n.L)` times light 0's colour, and specular `saturate(n.H)^16` times
  `DIR_LIGHT_SPECULAR_0` (environment specular RGB × 2 × sun intensity),
  gated by the normal map's alpha. This is the value retail uploads to
  `MeshBumpColorize.fx` / `RSkinBumpColorize.fx`, per
  [R-LIT-04](behaviour/p1-effective-environment.md#c-lighting-semantics-and-effect-parameters); both land and
  space bind it through `lighting::sun_specular`. Zero sun intensity therefore
  produces no highlight. `Shininess` is
  unread. The half vector uses the real camera position.
- **Tangent frame.** L and H are projected on the authored tangent, binormal
  and normal, then normalized and interpolated without renormalizing. Authored
  frames are often not orthogonal (in FoC's bump meshes a tenth of the vertices
  have a binormal more than 19 degrees off `cross(N, T)`), and Godot's
  `BINORMAL` is always that cross product. The upload therefore stores each
  binormal as `CUSTOM0` coefficients of the unit tangent, unit normal and their
  cross product, which skinning carries along. `MeshBumpColorize` works in
  object space, `RSkinBumpColorize` in world space with a normalized normal and
  unnormalized tangent and binormal, as the retail vertex shaders do.
- **Colour.** The team colour binds as its stored value (`scene::LegacySelector`
  `stored_values`), not decoded to linear light as for the linear adapters.
- **Textures.** `NormalTexture` reaches the upload as a per-binding texture; the
  renderer refuses the material rather than sample the base texture in its
  place. A reference that does not resolve or decode gets a flat normal with
  zero gloss in the viewer, recorded by the scene's `texture_unresolved` cause.
  Each family binding declares its fallback: flat RGBA (128, 128, 255, 0) for
  normals; black RGBA (0, 0, 0, 0) for gloss, whose red channel must be zero.

The fixed-function rows (`t0/t0_p0`, `sph_t0/sph_t0_p0`) stay renderer
selectors for the pinned hull fixtures below and the frozen `--eawr-model`
preview; the scene no longer selects them.

## DX8 mesh programs

`BatchMeshGloss` (land rocks, crates and props), `BatchMeshAlpha` (bushes and
ferns) and `MeshAlphaGloss` (the mineral extractor's asteroid shell, Gungan
and Naboo building glass) draw their Highest technique, `sph_t0` (legacy gloss and alpha shader paths;
`src/presentation/godot/legacy/dx8_mesh.hpp`), on stored values like the
retail vs_1_1/ps_1_1 pair:
- **Vertex.** D = `Diffuse` x `SPH_LIGHT_ALL` irradiance x `LIGHT_SCALE` +
  `Emissive` and S = `Specular` x max(N.H, 0)^16 x the sun's specular, both
  clamped to [0, 1] as vs_1_1 colour outputs. The half vector uses the real
  camera position; `Shininess` is unread.
- **Pixel.** 2 D x base plus S masked by the base alpha (`BatchMeshGloss`),
  unmasked (`BatchMeshAlpha`) or by the `GlossTexture` red channel
  (`MeshAlphaGloss`), saturated before blending. The alpha programs blend on
  base alpha x D alpha without depth writes.
- **Inputs.** `GlossTexture` reaches the upload as a per-binding texture, as
  the bump colorize `NormalTexture` does. The BatchMesh fog-of-war multiply is
  the derived fog-stub-v1 stage in every fog mode; their per-vertex distance
  fade and the engine distance fog are not reproduced.

The fixed-function `sph_t1/sph_t1_p0` rows stay renderer selectors for the
pinned fog and lighting fixtures; the scene no longer selects them.

## Hull shadow fixture

The fixture uses the mod-layer rebel_mon_calamari_mc_50.alo Hull and
Rebel_Mon_Calamari_Tide.dds, MeshBumpColorize t0/t0_p0, at rest. Exact asset,
surface and mask pins are in HULL_FIXTURE in
tests/presentation/lighting/test_hull_asset_shadow_runtime.py.
The hull alone is effectively convex at the tested shadow-map scale: the
predeclared edge/contact margins leave too few pixels for its self-shadow test.
A second copy is therefore the caster, translated 120 units toward the sun and
70 render-X units sideways, or (70,84.853,84.853).

The camera is (286.96,133.73,95.24) toward (0,4.60,-5.20), 45 degrees,
near/far 114.7/728.6. Use the orthogonal 4096 atlas, max distance 615 and the
fixture's frozen shadow bias (2, normal bias 5, blur left at its engine default;
the earlier space shadow policy): on Forward+ the
RenderingServer light defaults acne the whole self-casting Hull.
CPU masks ray-trace the source Hull triangles independently of GPU output; the
rigid rest bind moves this Hull by at most 0.000044 units. Fixture changes
cannot stand in for a passing result under the original label.
Controls toggle shadows, restore them, remove the caster, disable casting and
disable receiving. Rigid-skinned and unskinned uploads must retain identical
bind-space surface/mask pins. The plain route bakes the Hull's rigid rest
transform before clearing its bone link, matching the production upload since
Rigid-mesh placement and animation strips. Matched route captures permit up to 32 pixels, 4 of them in the lit
control, to differ by at most one decoded luma level; receiver-mask pixels must
agree. Forward+ skins in a compute pass, and the two routes then differ in
10-23 pixels (3 in the control), each by under one level, on the RX 7900 XTX. The run label binds
receiver casting and upload route; set_skin_pose must be accepted only on the
rigid route. This is an identity-palette rigid-skinning control, not a general
animation claim.

On Godot 4.7.2 Forward+ (Vulkan, RX 7900 XTX), as before on the GLES3 route,
both receiver casting settings pass the mean controls and CPU-mask geometric
agreement, without a self-shadow signature. The rigid-skinned and unskinned
routes agree. The older failing
verdicts came from drawing the Hull interior before the ALO winding fix;
temporarily restoring that upload reproduced both old verdicts on the same GPU.
The one-pixel CPU mask pin change follows the rigid rest-bounds correction.
These are bounded regression expectations, not an original-game comparison or
a general skinning result.

## Sun policies

eawr-sun-mode7-reference-av01-v1 is a pure CPU, source-basis evaluator of the
MIT reference rule, not recovered retail behaviour. It uses double, requires
proper rigid chains, rejects billboard ancestors and zero/near-vertical light
vectors whose reference result depends on signed zero. Tests derive expected
values independently from the [reference rules](behaviour/meshadditive-sun-billboard.md).

eawr-sun-mode7-retail-v1 is separate and follows the
[retail rules](behaviour/meshadditive-sun-mode7-retail.md). It uses caller-supplied
toward-light direction L; it does not infer L from TED angles. Open admission
gates keep sun_retail_admissible false. A pure evaluator does not admit a billboard
surface or establish world composition, winding, culling, depth or pixel parity.

## Prototype capture timing

The retained Godot prototype restores its baseline camera at frame 118, applies
replay snapshot index 1 at zero-based frame 119 and saves at displayed frame 120.
The report omits capture tick and does not check save_png's return value; consumers
must independently verify image creation and replay/input identity. Reproduction
details remain in the [prototype guide](prototypes/godot.md).

The capture tool's archived receipt pins are staging-receipt.json SHA-256
`39ca1c77e531b052c6e5c7af602b5306b90bc00499012243a9ace162e0f38712`
and windows-p0-repeat-readiness.json SHA-256
`abc6b0ec47c6f934031938667443c72fa61156b735222a1182116e894341278b`.
They identify inputs to the repeat tool, not qualification of a new build.

## Space sky

The `E-space-environment-v1` tactical view keeps both sky layers and the
sky-owned sun centred on the live camera. Each sky keeps its authored model
size and object scale; the view's far plane is at least 100,000 units (retail's
widened background range, BP-75), including when Space_Mode supplies a shorter clip.
It was 60,000 before (legacy EAWR-6447), which cut off the far part of large backdrop planets
(for example a scale-45 planet reaching about 72,600 units) when the camera pulled back,
and, once skies kept their authored scale (legacy EAWR-6358), the whole sky of a mod whose sky shell lies
beyond 60,000 units: the Remake Shola space map was reported to draw a flat background and no stars on that build (the reported symptom; not reproduced here).
The sun billboard is still pulled to 0.9 x 48,000 = 43,200 units, so on stock maps
(primary star mesh about 31,729 units out) it now sits outside the star shell, where it
used to sit inside a 48,000-unit shell; sky surfaces write no depth, so the shell does
not hide it. Planet and nebula placements keep their map positions, as retail's
background objects do: they are world-space, not camera-relative, so their apparent
size changes with camera distance.
This is the viewer's environment presentation rule; sky object lookup and
authored orientation still follow the TED environment fields below.

The nebula moves only through the effect `TIME` parameter. The
`W_NEBULA*` models have no idle clips. Nebula.fx offsets each vertex by
`DistortionScale * sin(2π * frac(SFreq * world + TFreq * TIME))` per axis and
scrolls its UVs by `3 * TIME * UVScrollRate`. Both Coruscant models author
DistortionScale 25, SFreq 0.002, TFreq 0.05 (a 20 s wave) and UVScrollRate
(0.001, 0.001). Retail sets `TIME` from the scene clock. Each render step adds
`LogicalFPS / 1000` = 0.03 s for every 30 Hz sim frame that step advanced, so
the clock stands still while the game is paused. A step adds at most 1 s, and
the clock wraps at 28,800 s. The viewer uses the idle clips' 30 Hz tick as the
sim frame (`space::environment_effect_time`): admitted tactical frames in a live
battle (FD-74), real time in other live scenes, and held after the particle
frames in a fixed capture, plus `--eawr-map-idle-offset`.
The environment view sets `eawr_effect_time` on each Planet and Nebula surface
whenever that tick changes.

The space slice selects both sky objects from environment 0 (TED 0x19 and
0x1a) from a semantically complete kind-2 map with no terrain. Unexamined
Planet/Nebula fields retain explicit dispositions. Model lookup
uses the viewer policy Space_Model_Name, Land_Model_Name, then Model_Name;
this is not asserted to be the original lookup rule.

| Rule | Evidence and outstanding question |
|---|---|
| SKY-6358-01 | The BFC Coruscant surface map's first environment names `STARS_HIGH` and `STAR_BACKDROP_RAW2_BLUE_ORANGE_HAZE_1`, with both TED scale fields 1 and all four orientation fields 0. Their XML models are `w_stars_high.alo` and `w_stars_nebula_blue_orange_haze01.alo`; their object scale factors are 1 and 20. Both set `Layer_Z_Adjust` 0, `In_Background` yes and `Exclude_From_Distance_Fade` true. Whether this particular map reproduces the reported difference needs a paired capture. |
| SKY-6358-02 | The stock `benchmark_space` first environment names `STARS_MEDIUM` and `STAR_BACKDROP_GREEN`, both TED scales 1, primary angles (0, 0), secondary angles approximately (-1, 323) degrees. Their XML scale factors are 1 and 25, and the same three background tags match SKY-6358-01. |
| SKY-6358-03 | Both example primary star meshes use MeshGloss with emissive RGB approximately 0.506, zero diffuse and specular; their sun billboards use MeshAdditive with white Color. Both example secondary meshes use MeshAdditiveVColor with zero UV scroll. The BFC secondary vertices are white; the stock secondary's vertex RGB ranges from 0 to 1. The preferred additive pass uses vertex RGB and light scale, not vertex or texture alpha, to attenuate RGB (AVC-01..04). |
| SKY-6358-04 | The debug build creates primary then secondary as ordinary objects and does not consult the TED scales in that path (R-SKY-04). Transparent task ordering and blending follow AVC-04 and AVC-07 for the documented effect. Before submission the object light scale starts at white and can be modified by object effects; `Exclude_From_Distance_Fade` bypasses the distance-fade branch. The resulting scale reaches the effect via the render task. Its value for a particular sky and the final comparison need further verification; the equal-radius viewer policy is not evidence of retail geometry or opacity. |
| SKY-6358-05 | The generic debug-build object transform passes the XML `Scale_Factor` to the attached model, unless a dynamic scale overrides it. The BFC example's primary star mesh has radius about 31,729 before its factor of 1, and its secondary has radius about 1,280 before its factor of 20. The resulting authored radii are therefore different. The model scales its renderable transform. The viewer applies each effective object scale (default 1) and retains its camera-centred sky policy. The visible consequence requires a paired capture. |
| SKY-6358-06 | The BFC Coruscant skirmish map's sole environment selects `STARS_HIGH` and `STAR_BACKDROP_BLUE`, with both TED scales 1, primary angles (0, 0) and secondary angles approximately (21, 275) degrees. The latter uses `w_stars_nebula_blue.alo`, `Scale_Factor` 25 and the same three background tags in SKY-6358-01. Its additive vertex-colour mesh has RGB spanning 0 to 1 and radius about 1,248 before scaling, or about 31,198 after. Its base texture is BC1 without alpha. The primary radius is about 31,729. |
| SKY-6358-07 | The BFC mod ships its sky textures as single-level images: the star texture `w_star_bkgnd_dense.dds` is 1024 px BC2 and the nebula `w_stars_nebula02.dds` 1024 px BC1, both with no mip chain, where the stock star texture is 512 px with a full chain. A paired capture of the same map on retail and on the viewer (GTX 970 rig, sky pixels only, same 1280x720) found retail with 30-80 star points per 100k sky pixels at luma 60 and above and the viewer with about 2900 (peak luma median 47 against 76, median sky luma 5-6 against 0). Forcing the sampled level (`textureLod` 0 to 3) and a level bias changed nothing in the viewer, i.e. the GPU texture had one level. Every environment texture now loads through one helper (`apps/viewer/src/environment_texture.hpp`: the default view, the evidence harness and the translated-shader texture loaders of the environment renderers) that completes a chain stopping above 1x1 with a plain box filter of the stored values (assets::extend_mip_chain, ChainKind::colour; the ship textures' loader requests a full chain the same way (legacy EAWR-5989) but gives additive ship materials the brightness-compensated chain: the sky's additive stars deliberately take the plain chain, which is what matches the retail statistics). The chain covers BC1 without 1-bit alpha, BC2, BC3 and the 24/32-bit formats (BFC's star, nebula, planet and city-light textures are BC2, BC1 and BC3, its atmosphere a TGA); other formats stay as decoded. Transparent and glow textures with an incomplete chain change too, which the ship policy leaves alone. A complete authored chain is never touched: all 184 stock `w_star*`, `w_nebula*`, `w_planet*` and `w_glow*` background textures have one, so stock skies are unchanged. The land map sky dome and its cloud layer (`apps/viewer/src/map_mode_ready.cpp`, a sky-only texture lookup passed to `compose_skydome`) load through the same helper (legacy EAWR-6722); the terrain and water lookup stays a plain load, and the one-time cost is the chain build on the decoded sky texture (a 8192x4096 DXT5 sky adds about a third of its base size in chain levels, built once at map load, before the battle starts). Unverified: that retail builds the chain for a sky texture rather than reaching the same look another way; the retail captures agree with a box chain by eye and by sky statistics. |
| SKY-6358-08 | A sky object whose `Scale_Factor` is zero or negative is reported as an invalid sky (the sky is missing) instead of collapsing or mirroring it. Unverified: retail's handling of such a value has not been observed. The installed FoC sky-dome data and the installed workshop mods' sky-dome files (about 34 files) author no zero or negative factor, so there was nothing to capture; a mod that authored a negative factor used to see a mirrored sky and now sees none. |
| SKY-6760-01 | An environment model names its textures as authored, and a mod may name a source-art suffix it does not ship: the Remake's ground-sunset sky dome names `W_Skydome_Sunset.jpg` and ships `W_Skydome_Sunset.dds` (4096 x 2846, BC3, one level). The environment loaders (the default view and the evidence harness) used to probe the authored name and the `.tga`/`.dds` suffixes only, so that sky resolved to no texture, drew no surface and left the flat clear colour with only the sun glow: a space battle at the Remake's Brentaal showed no sky (rig look-up captures before and after, ignored `out/eyecheck/`). They now use the asset layer's texture rule, `assets::resolve_texture_name`: the authored name first, then the name cut at its first dot plus `.dds` (RGC-122). The synthetic `test_primary_sky_texture_named_with_source_art_suffix_uses_the_shipped_dds` ships `.jpg`/`.png` names with `.dds` files. A dotted name that is not a `.tga`/`.dds` name follows the same cut: `tex.01` loads `tex.dds`, not `tex.01.dds`; no asset of stock FoC, Remake 4.0/5.0, Republic at War, BFC or Fall of the Republic relies on the old suffix probe resolving a dotted stem (scan of every texture-looking string in their models against their texture lists: no name resolved before and fails now; every difference is a name the retail rule resolves and the old probe did not), and the synthetic `test_primary_sky_texture_dotted_name_follows_the_first_dot_token_rule` pins the retail rule. Not changed here (fidelity list): the same suffix-only probe still serves the map terrain and particle loaders (`apps/viewer/src/map_mode.cpp`, `map_mode_particles.cpp`, `map_mode_ready_terrain.cpp`) and the space population loaders (`space_populate_arrivals.cpp`, `space_populate_materials.cpp`), where no known mod needs the retail retry. |

The opt-in eawr-space-sky-meshgloss-v1 route preserves authored MeshGloss
parameters and the [programmable semantics](behaviour/meshgloss-programmable.md).
Missing, duplicated, wrong-kind or nonfinite values fail; finite unusual values
remain unchanged. Unknown parameters and extra textures remain unsupported.
Proper rigid non-billboard hierarchies bake positions/normals; reject zero normals.
The space-sky-unlit-v1 policy uses zero SH matrices, zero light-zero specular and
unit light scale, yielding twice Emissive RGB times texture. Its eawr_sky_*
bindings are separate from scene-light uniforms. This is a project policy, not
an original sky light rig. Material arithmetic alone does not admit a model's
unsupported billboard surfaces.

The whole sky fails admission if any visible surface is rejected. Texture
conversion is per-surface and never substitutes a grey placeholder. The separate
synthetic MeshAdditive t0 route requires BaseTexture, Color and UVScrollRate;
billboards including mode 7 remain rejected. It does not change the authentic sky
ledger or admit a sun merely because its material can be evaluated.

The pinned Alderaan space fixture selects STARS_LOW, model
data/art/models/w_stars_low.alo, SHA-256
`3b1560115953fe3285fd892bf2495f6c00bf1d6b9477c08d22c73abae1f7b688`.
Its stars surface uses MeshGloss.fx and w_star_bkgnd_light.dds and is admitted;
sun_billboard uses MeshAdditive.fx and w_stars_sun00.dds and remains
hierarchy_unsupported, with unconsumed_parameter and shader_not_qualified causes.
Both textures are under data/art/textures. Tests retain these bounded asset
identities; they are not claims about every sky.

## Resource identity

Uploading identical RenderingServer-relevant content under an existing asset ID
adds a lease. Different content fails EAWR-RENDER-0002 without replacement;
replacement requires release then upload. A missing-asset wait emits one diagnostic
until the asset changes or the wait ends. Release removes pending skin poses;
temporary snapshot absence does not. Call clear_skin_pose when retiring an entity
ID. Shadow material/failure counts describe registered live resources.

## Fog binding

The cache key is stream/team. Source XY maps to render (X,-Z); R8 linear texels
represent half-open Q24 cells with nearest sampling, no sRGB or mipmaps, and dark
outside/unbound. Identical content avoids upload; revision/origin/cell-only changes
avoid pixel upload; same-size changed pixels update; resizing creates and rebinds
before deleting the old texture. Reject lower revisions, equal-revision different
content and backend failures atomically. Failure on the same selected grid may
retain its last binding; switching or missing selection unbinds to dark.

Fog is opt-in. Legacy selectors are BatchMeshGloss.fx and BatchMeshAlpha.fx
sph_t1/sph_t1_p0; modern spatial consumers must reflect all five correctly typed
uniforms, including an unambiguous sampler2D Texture2D. Attach saves overrides and
forces unbound state; unbind clears texture/mapping; final detach restores previous
overrides exactly. Legacy fog multiplies linear RGB once before output transfer,
preserving the original blend/depth/alpha route.

## Particle policy

V1 ALO root 0x900 has CPU parameter decoding; V2 root 0x1500 preserves IDs without
validated parameters. CPU advance uses caller-fixed seed, hard capacity and a
100,000 spawn-event guard; invalid/nonfinite delta does not advance. Unsupported
families remain explicit. Detach with leave_particles stops root scheduling while
live particles/children drain; finished requires detached and no live descendants.
Without leave_particles, release is immediate. Bone-hide detaches a proxy once;
reappearance creates a new instance while an old one may drain. stay_detached is
an explicit alternative. These reference-derived policies are not measured retail
behaviour; see [attachment visibility](behaviour/particle-attachment-visibility.md).

Authored effect admission validates the whole CPU definition, but render support
is per emitter. A renderer without an adapter retains its reported cause and
creates no backend resource; supported siblings still draw. Its CPU particles
continue to age and produce supported child emitters. An effect with no supported
render plan is refused explicitly. This keeps a mixed explosion visible without
substituting geometry for an unsupported primitive family.

Map particles advance in 1/30 s samples (`map_attachment_owner.hpp`). A fixed
capture takes one sample per rendered frame and holds after its particle frames.
The live view takes as many samples as the idle clips' real-time 30 Hz tick is
due, at most 30 per frame (`map_owner_samples_due`), so an effect keeps its
authored period at any display rate. Before the light-cycling fix the space live view took
one sample per rendered frame, so at 144 Hz effects ran 4.8 times too fast. Before
the persistent-particle fix the land live view stopped after a capture's 60 samples, so the Naboo
waterfall spray froze 2 s into the session. The
Coruscant sensor lights show this clearly: the `p_sensornode01` proxy on
`Skirmish_Merchant_Dock` and `Orbital_Resource_Container` loads `p_sensornode01.alo`,
one emitter that bursts one particle per vertex of the proxy's parent mesh every
1 s. Each particle lives 1 s. Its colour keys fade from yellow to red, and its
size keys hide it from 0.18 to 0.66 of its life, so each light shows yellow, then
off, then orange to red, once per second.

V1 emitter mini 0x33 (skip time) is applied as a pre-roll: on its first advance an
instance simulates each emitter for its own skip seconds in fixed 0.1 s steps (not
the 1/30 s presentation sample; PS-07), repeating while the accumulated emitter
time is at or below the skip time, so it can run one step past it, then rebases time
to zero. Mini 0x32 (freeze time) stops an emitter permanently when an update would
carry its local time strictly past it (equality still advances; PS-08): the whole
update is skipped, particles keep their state and nothing ages, moves or spawns. The
first-update freeze check comes before the pre-roll. The MIT
reference decodes both and applies neither; the rules come from the debug build
(PS-07, PS-08; checked again against the Focumentation page "Particle Editor",
which only says the emitter freezes after its freeze time) and the corpus
(engine trails skip 1 s, asteroid fields skip and freeze at 10 s). Heat renderers
with selector 1 or 2 draw through the PrimHeat distortion adapter, weighted by the
texture mask; selector 11 draws alpha-blended with its normal map lit by the
scene sun and fill (see [V1 particle size and bump lighting](#v1-particle-size-and-bump-lighting-238)).
A heat texel offsets its scene sample by at most texture alpha x vertex alpha x
0.01 of the screen, and the mix blend weights the offset sample by that alpha, so
`heat_distortion_pixel_change_bound` gives the largest summed RGB change one heat
draw can make to a pixel: each channel moves by at most floor(255 a w + 0.6)
8-bit steps for peak vertex alpha a, where w = 1 - (1 - fx)(1 - fy) is the
bilinear weight moved off the pixel by offsets fx, fy (each at most one pixel,
plus 1/256 of filter precision) and 0.6 of a step allows for the store's
rounding. The premise is that the covered pixel equals the screen copy (the
opaque scene). The map pixel check accounts a heat-only placement whose bound
stays under its 0.04 change threshold (the Alderaan speeder shimmer keys 5/255:
2 steps per channel at 1280x720, 3 in a 2558x1360 window) instead of requiring
it to change pixels; above 7/255 at 1280x720 heat is checked, although it
still moves less than a pixel.

<a id="space-ambient-particle-allocation-238"></a>

### Space ambient particle allocation

FoC `corruption/Data/config.meg`, `DATA/XML/SPACEPROPS.XML`, defines
`Asteroid Field Large` with `Space_Model_Name=w_asteroid_mass.alo`, scale 1,
`Layer_Z_Adjust=-100`, and both `Is_Decoration` and `Is_Discardable` false.
It declares no spawn/scatter list. The effective model comes from base
`GameData/Data/models.meg`, `DATA/ART/MODELS/W_ASTEROID_MASS.ALO`: twelve
rock meshes and two untagged proxies, `p_asteroid_field00` and
`p_asteroid_field01`. Neither proxy carries an ALT/LOD selector.

Their effective ALOs in that same base archive contain independent continuous
shape emitters, with mesh emission disabled. They draw the four-cell
`p_asteroids.tga` atlas with `p_asteroids_bc.tga` through bump blend selector 11.
The small shaded rocks are these normal-mapped billboards; no separate pebble
mesh or procedural scatter list is needed to reproduce their density.

| Effect / emitter | Births per second | Lifetime | Pre-roll | Freeze | Approximate live population |
| --- | ---: | ---: | ---: | ---: | ---: |
| `p_asteroid_field00` / inner | 33 | 10 s | 10 s | 10 s | 330 |
| `p_asteroid_field00` / outer | 50 | 10 s | 10 s | 10 s | 500 |
| `p_asteroid_field01` / burst (continuous despite its name) | 20 | 20 s | 20 s | disabled | 400 |

The former 256-particle per-proxy budget truncated both effects; the 8192 map
budget also excluded later proxies. Space population now sizes prewarmed
independent continuous shape systems from their maximum lifetime and birth
rate, bounded by freeze time, plus one 30 Hz scheduling step and a time-zero
birth per emitter. Cold effects and dependent/mesh/burst creators retain the
fallback policy. Automatic allocation is capped at 65536 per system and
131072 across the space attachment plan; explicit `--eawr-map-attached-capacity`
and `--eawr-map-particle-capacity` remain hard controls. Land budgets retain
their previous defaults.

Placement still uses the attachment planner's seed derived from map path,
TED record, scene and proxy ordinals, and the caller seed. Allocation changes
which authored births survive, not the emitter schedule, simulation or replay.
The space particle report includes each effect path, seed, allocation, live
count and particle-state hash so density and repeated captures are checkable.

<a id="v1-particle-size-and-bump-lighting-238"></a>

### V1 particle size and bump lighting

Facts from the FoC debug build and the FoC `PrimParticleBumpAlpha.fx` source;
they replace the MIT alo-viewer's V1 conversion where the two differ.

- Size keys (track 4) are the full quad width. The drawn half-extent is
  key x (1 + u) / 2 with u uniform in [-v, v], where v is mini 0x12 read raw
  (no halving and no division of the keys by 1 + v). A negative result draws
  nothing. The alo-viewer conversion (v / 2, keys / 2 / (1 + v / 2)) made every
  particle with a size spread smaller: the Coruscant asteroid pebbles
  (`p_asteroid_field00`, v = 0.92) drew at 0.37-1.0 of the key instead of
  0.08-1.92: 0.69 of the mean width and 0.39 of the mean area. Checked again
  against the Focumentation page "Particle Editor", whose "minimum scale" says
  each particle is sized between that percentage and 100 %: mini 0x12 is the
  only size-spread field the debug build's loader reads, and the birth draw is
  uniform in [-v, +v] with the key multiplied by 1 + u (stock v lies in 0 to
  1), so the spread is symmetric and its mean is the key. The wiki matches the
  lower end 1 - v only (the label may well show 1 - v); which editor control
  writes the field was not read from the community tool.
- The per-effect size scale (dynamic size setting) defaults to 1; the texture
  cell index is the integer part of the UV track's value at the particle's
  relative age, on a square grid of sqrt(mini 0x10) cells, row-major (death debris and station-death captures:
  the runtime rounded to the nearest cell until then; no V1 emitter in the
  corpus has an interpolated UV track, so no drawn cell changed). A track
  with interpolation mode 2 holds each key's value until the next key, so
  the explosion debris (`debre`, UV 7 then 2 at the end) draws cell 7 all its
  life. Retail evaluates every V1 track through a 32-entry table sampled at
  i/32 and read by linear interpolation at age x 31; the runtime samples the
  keys directly (fidelity list).
- Blend selector 11 (bump alpha): with bump support retail replaces each
  vertex colour's RGB by the particle's tangent, so the authored colour keys
  reach alpha only. At Highest the effect takes technique t2 (LOD DX8ATI):
  RGB = 2 base (saturate(n.L) light-0 diffuse + fill) + light-0 specular x
  saturate(n.H)^5 x normal alpha, alpha = vertex alpha x base alpha. The
  tangent frame faces the camera (the projection is right-handed, so the view
  Z axis points at the viewer), and the fill is SPH_LIGHT_FILL evaluated once
  at that camera-facing normal and clamped to [0, 1]. The viewer binds the
  renderer's lighting state (sun direction and colours, fill matrices) to
  every bump emitter; without scene lighting it uses the hemisphere fallback.
  Coruscant's sun (the R-LIT-01 heading, light heading correction) lies beyond the field from the
  default camera, so its pebbles are back-lit: dark bodies with light rims, as
  in retail.
