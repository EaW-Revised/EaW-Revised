# Graphics features: retail options, viewer status, Forward+ extras

Date: 2026-09-26. Planning survey for M1.5 and later. It lists every graphics setting retail FoC
has, what each one does in the renderer, where the viewer stands, and which Forward+ extras
are worth offering. The viewer baseline is the FP-1 branch (EAWR-149, PR EAWR-183, Forward+ with the
stored-value colour policy in [rendering.md](rendering.md)).

Sources: `guidialogs.rc` (`IDD_VIDEO_OPTIONS_DIALOG`, `IDD_ADVANCED_VIDEO_OPTIONS_DIALOG`),
`GraphicDetails.xml` (`config.meg`), the player profile values the game stores, the
`_GraphicsInit.txt` log, the published FoC shader source, and the FoC debug build for which
consumer reads which value. Clean-room: observed data and behaviour only, no retail code and no
addresses.

## 1. Retail options

### Dialog and stored values

The game stores each value under `HKCU\Software\Petroglyph\StarWars FOC\Profiles\Profile<n>\Player`.
EaW keeps its own copy under a separate key. A detail value of `-1` means "use the preset".

| Option (dialog) | Stored value | Range | What it drives in the renderer |
|---|---|---|---|
| Resolution (list) | `ScreenWidth`, `ScreenHeight`, `ScreenRefresh`; windowed: `WindowWidth`, `WindowHeight`, `Maximized` | device modes | Backbuffer size |
| Windowed mode | `Windowed` | on/off | Windowed or exclusive fullscreen |
| Widescreen | `WidescreenMonitor`, `WidescreenAspectRatio` (float, e.g. 1.78) | on/off | Gives the projection the monitor aspect on non-4:3 screens |
| Anti-aliasing (Off…Max) | `ScreenAA` | index into the device's multisample modes | D3D9 backbuffer MSAA |
| Detail level (Low…Max, 4 ticks) | `GraphicsDetail` | 0–3 | Picks one of the four presets of the matched hardware configuration (next table) |
| Gamma (Low…Max) | `Gamma` (float) | gamma = 0.25 + 1.5 × slider, 1.0 at mid | Device gamma ramp: each channel out = (i/256)^(1/gamma). D3D9 applies a ramp only in fullscreen, so windowed play ignores it. |
| VSync | `VsyncEnabled` | on/off | Present interval |
| Hardware mouse | `HardwareCursor` | on/off | OS cursor instead of the drawn one (UI) |
| Auto detect (button) | `MeasuredFillRate*`, `LastDeviceID` | — | Fill-rate benchmark, then the `GraphicDetails.xml` hardware match |
| Particle detail | `ParticleDetail` | 0–1 | Global particle LOD. For weather emitters and mesh-surface emitters the drawn share is ceil(count × group LOD × setting). A fixed index hash picks which particles stay. |
| Mesh detail | `MeshDetail` | 0–1 | Model `_LODn` meshes: index = floor(setting × (highest + 1)), clamped to the highest. 1.0 selects the highest-numbered (most detailed) LOD. A screen-coverage dynamic LOD exists but is switched only by a developer console command and a per-object Lua call. |
| Water detail | `WaterDetail` | 0–2 | Water technique t0/t1/t2 with reflection and refraction passes, coupled to shader detail. See [terrainwater-technique-selection](behaviour/terrainwater-technique-selection.md). |
| Shader detail | `ShaderDetail` | 0–3 | Every effect takes the first declared technique whose `LOD` ≤ the level (FIXEDFUNCTION 0, DX8 1, DX8ATI 2, DX9 3), same document |
| Texture detail | `TextureMipLevel` | 0–2 | Skips the top 0–2 mip levels on load. A low-RAM flag forces 2. |
| Shadow detail | `ShadowDetail` | 0–3 | 0 turns shadows off. Otherwise an object type gets stencil shadow volumes when the level ≥ its `Blob_Shadow_Below_Detail_Level` and the device runs the volume shaders, or else a terrain blob shadow (`BlobStencilMasked`, `Blob_Shadow_*` tags). |
| Environment detail | `EnvironmentDetail` | 0–1 | Share of discardable decoration objects kept visible. Each object is kept with that probability, and the draw is rerolled when the value changes. Also sets the combined-mesh LOD. |
| Soft shadows | `SoftShadows` | on/off | Blurs the stencil darkening mask with a 4-tap screen blur (`StencilDarkenFinalBlur`, DX9) instead of the hard darken pass |
| Bloom | `Bloom` | on/off | `SceneBloom` post pass, enabled every frame in land and space. From a backbuffer copy: bright pass into a quarter-size 8-bit target (a pixel stays if its luminance > cutoff, else it becomes pixel⁵), four 4-tap diagonal blurs with growing offsets, then added back at the bloom strength with an add-smooth blend (src + dst × (1 − src)). Strength, cutoff and size come from the current environment record (loader defaults 1.0, 0.9, 0.25); the FX defaults 0.1, 1.0 and 0.25 are always overwritten. Details in [rendering.md](rendering.md#bloom). |
| Heat distortions | `HeatDistortion` | on/off | Heat-phase draws (`MeshHeat`, `RSkinHeat`, particle `PrimHeat`) write a distortion map over a neutral clear. `SceneHeat` then offsets a scene copy by it (amount 0.25). When off, heat draws are skipped. AMD (vendor 0x1002) gets a variant with a different sub-pixel offset. |
| — (XML and profile only) | `ShadowVolumes` | Yes/No | Logged, but the traced apply path does not read it: the shadow type comes from shadow detail |
| — (XML only) | `DynamicLighting` | Yes/No | An engine switch, No in every shipped preset |

### Presets (`GraphicDetails.xml`, FoC)

| Value | Default_0 | Default_1 | Default_2 | Default_3 (Highest) |
|---|---|---|---|---|
| Resolution | 800×600 | 1024×768 | 1024×768 | 1280×1024 |
| AA level | 0 | 0 | 0 | 1 |
| Texture mip level | 2 | 1 | 0 | 0 |
| Shader detail | 0 | 1 | 2 | 3 |
| Water detail | 0 | 1 | 1 | 2 |
| Shadow detail | 0 | 1 | 2 | 3 |
| Shadow volumes / soft shadows | no / no | yes / no | yes / no | yes / yes |
| Heat / bloom | no / no | no / no | yes / yes | yes / yes |
| Particle / mesh / environment detail | 0.4 / 0.0 / 0.0 | 0.6 / 0.5 / 0.2 | 0.8 / 1.0 / 0.6 | 1.0 / 1.0 / 1.0 |

Intel devices get the `FF_0`–`FF_3` sets: the same presets with shader and water detail 0 and no
volumes, soft shadows, heat or bloom. Other devices are matched by pixel-shader version, texture
memory, CPU speed and measured fill rate to `Config_Highest`, `Config_High`, `Config_Medium` or
`Config_Low`, which default to tick 3, 2, 1 or 0. On the owner's RX 7900 XTX the log reads
`CONFIG_HIGHEST`. Its shadow-detail line is printed as a float from an integer value, so the
number it shows is not reliable.

### FoC-only

The dialog controls and stored values are the same in EaW and FoC (Steam build). The differences
are in data:

- FoC's `GraphicDetails.xml` drops EaW's `FF_LowTex_*` and `LowDx9_*` sets and raises Default_2
  mesh detail from 0.7 to 1.0.
- FoC matches hardware with per-shader-model fill-rate benchmarks (`FillRateTest.fx`, FoC only).
- `MeshAdditiveReflection.fx` is the only other FoC-only effect.

## 2. Viewer today (FP-1)

| Retail option | Viewer | Equivalent retail level | Gap |
|---|---|---|---|
| Resolution / windowed | 1280×720 window, `--resolution`, windowed only | — | No fullscreen or mode list |
| Widescreen | Godot uses the true aspect | on | none |
| Anti-aliasing | render profiles (EAWR-184): retail off, enhanced 4x MSAA + SMAA ([rendering.md](rendering.md#render-profiles)) | 0 in captures | Retail Highest sets AA level 1 |
| Detail level / auto detect | none | fixed | — |
| Gamma | none | 1.0 | Slider missing (identity at the default) |
| VSync | on (`vsync_mode=1`), no switch | on | Switch missing |
| Particle detail | every particle | 1.0 | none |
| Mesh detail | highest `_LOD` (`--eawr-unit-lod` picks another) | 1.0 | none |
| Environment detail | every decoration | 1.0 | none |
| Texture detail | full-resolution mips | 0 | none |
| Water detail | approximate plane and ribbons, no `TerrainWater` technique | below 0 | No reflection, refraction or bump (fidelity list) |
| Shader detail | per program, next table | mixed | See below |
| Shadow detail / soft shadows | Godot directional shadow maps (FP-2 EAWR-150 retunes), `--eawr-shadows on/off` | ≈3, different technique | No stencil volumes, screen blur or blob shadows |
| Bloom | `SceneBloom` port after the stored-value decode (EAWR-201), map mode only, lit scenes only (EAWR-307), environment 0's values, `--eawr-bloom on/off` | on | Bloom source includes particle heat ([rendering.md](rendering.md#bloom)) |
| Heat distortions | particles only (`PrimHeat` adapter, [rendering.md](rendering.md)) | partial | `MeshHeat`/`RSkinHeat` missing |

Shader level per program. Retail at Highest takes the first live technique (commented-out
techniques do not count):

| Program | Retail at Highest | Viewer | Gap |
|---|---|---|---|
| MeshGloss, MeshGlossColorize, RSkinGlossColorize | `sph_t0` DX8 | same | — |
| RSkinGloss, Tree | `sph_t1` DX8 | same | — |
| MeshAdditive, MeshAdditiveOffset / Grass | `t0` / `sph_t0` DX8 | same | — |
| MeshAlpha | `sph_t1` FF (its only live technique) | same | — |
| MeshBumpColorize, RSkinBumpColorize | `sph_t2` DX9: normal map and specular | same (EAWR-199, [rendering.md](rendering.md#hull-bump-and-specular)) | — |
| BatchMeshGloss, MeshAlphaGloss, BatchMeshAlpha | `sph_t0` DX8, SPH vertex lighting | same (EAWR-200, [rendering.md](rendering.md#dx8-mesh-programs)) | — |
| Planet | `sph_t3` DX9 | reduced | Atmosphere, city lights and normal map (fidelity list) |
| TerrainRenderBump | `bump` DX8ATI | no normal or specular | fidelity list |
| MeshBumpReflectColorize, RSkinBumpReflectColorize, RSkinAdditive(VColor), RSkinAlpha(Gloss), MeshAdditiveVColor, MeshAlphaScroll, MeshAdditiveReflection, TerrainMeshBump/Gloss, MeshShield, Mesh/RSkinOccludedUnit, TerrainClouds, TerrainLava/Ice, BlobStencilMasked, FrameEffect* | various | fail closed with a diagnostic | Not implemented |

## 3. Forward+ extras

All exist in Godot 4.7.2 (checked against its API dump). "Off" must give the retail frame.

| Extra | Godot 4.7.2 | Fidelity when on | Cost | Platforms |
|---|---|---|---|---|
| Anisotropic filtering | `Viewport.anisotropic_filtering_level` plus `*_anisotropic` sampler hints | Sharper terrain at grazing angles. Only the terrain requests it; the retail profile turns it off, so captures sample linear like retail (EAWR-184). | ~0 on desktop, noticeable on lavapipe | all RD |
| MSAA 2/4/8× (EAWR-184) | `msaa_3d` | Retail has it (AA slider). Alpha-tested edges stay aliased, as in retail. | memory, fill | all |
| FXAA / SMAA | `screen_space_aa` | Post blur on the output, mild softening | low | all |
| Supersampling (scale > 1) | `scaling_3d_scale`, bilinear | Safest AA; every shader is sampled | 1.5× scale = 2.25× pixels | all |
| Resolution scale < 1, FSR 1 | `scaling_3d_mode` FSR | Softer frame; for weak GPUs or ARM64 | saves fill | all RD |
| TAA, FSR 2, MetalFX temporal | `use_taa`, FSR2 | Ghosting unless every custom shader writes motion vectors (billboards, particles, UV scroll) | high to integrate | RD |
| VSync modes, frame cap | `window_set_vsync_mode` (off/on/adaptive/mailbox), `Engine.max_fps` | none | ~0 | all |
| Window modes, DPI | windowed, maximized, borderless and exclusive fullscreen; `content_scale_*` | none; UI scaling follows rule UI-L3 in [ui-layer](ui/ui-layer.md) | ~0 | all |
| HDR output | `window_request_hdr_output` | Content is SDR stored values: no gain, colour-shift risk | medium | Windows D3D12/Vulkan, macOS |
| Shadow tiers | atlas size, PSSM 2/4 splits, soft filter quality, PCSS | Tiers above the retail look are an enhancement | GPU | all RD |
| SSAO, SSIL, SDFGI, VoxelGI, volumetric fog, glow, SSR, DoF, auto-exposure | `Environment` | They compute in linear light, but the frame holds stored values ([rendering.md](rendering.md)), so the maths is wrong. They also add looks retail never had. Retail bloom has its own stored-space port (EAWR-201); fog needs one. | high | RD |

## 4. Recommendation

Estimate method from [estimate.md](estimate.md): planned tasks × class median (low) and p80 (high)
in worker-hours, ÷ 24 = agent-days, estimate = midpoint. Classes: fidelity (F) 1.35/1.93, new
feature (N) 1.98/4.54, infra (I) 0.76/1.43, remote infra (IR) 1.14/2.15, research (R) 0.74/1.38,
rig capture (C) 1.25/1.45.

### (a) Retail parity gaps to fix

| Feature | Retail? | Status | Recommend | Reason | Tasks | Worker-h | Agent-days | Size |
|---|---|---|---|---|---|---|---|---|
| Hull bump and specular (`MeshBumpColorize`/`RSkinBumpColorize` `sph_t2`) | yes, Highest | done (EAWR-199) | now | Most ship hulls lack normal map and specular | 2–3 F + 1 C | 3.95–7.24 | 0.23 | S |
| DX8 SPH for BatchMeshGloss, MeshAlphaGloss, BatchMeshAlpha | yes | done (EAWR-200) | now | Same adapter work as the row above | 1–2 F | 1.35–3.86 | 0.11 | S |
| Bloom (`SceneBloom` as a stored-space compositor pass, per-scene parameters) | yes, Default_2+ | done (EAWR-201) | now | Default at retail High and Highest. The rig references include it. | 1–2 N + 1 R + 1 C | 3.97–11.91 | 0.33 | M |
| Record the rig's applied detail settings with each original capture | — | missing | now | A comparison has to know whether bloom, heat and AA were on | 1 IR | 1.14–2.15 | 0.07 | XS |
| Mesh heat (`MeshHeat`/`RSkinHeat` via the `PrimHeat` adapter) | yes, Default_2+ | missing | later (P2-17) | Engine heat on moving ships | 1–2 F | 1.35–3.86 | 0.11 | S |
| Planet `sph_t3` | yes | reduced | later (before P2-22) | Space backdrop in M2 | 1–2 F | 1.35–3.86 | 0.11 | S |
| Unit programs that fail closed (reflect, RSkin additive/alpha, VColor, scroll, FoC `MeshAdditiveReflection`, terrain meshes) | yes | fail closed | later (P2-02/P2-17 unit sets) | Count usage per unit set first | 1 R + 3–5 F | 4.79–11.03 | 0.33 | M |
| Blob shadows, reinforcement and bombing-run blobs | yes | missing | later (M3) | Land gameplay decals | 1–2 F + 1 R | 2.09–5.24 | 0.15 | S |
| Water t0 reflection/refraction/bump; lava and ice | yes | approximate | later (M3) | Land maps; on the fidelity list | 2–4 F + 1 C | 3.95–9.17 | 0.27 | S |
| Terrain bump/spec, `TerrainClouds` shadows, distance fog | yes | missing | later (M3) | On the fidelity list | 2–3 F + 1 C | 3.95–7.24 | 0.23 | S |
| Occluded-unit silhouettes | yes | missing | later (M3) | Gameplay readability behind buildings | 1–2 N | 1.98–9.08 | 0.23 | S |

### (b) Retail options to expose as settings

| Feature | Retail? | Status | Recommend | Reason | Tasks | Worker-h | Agent-days | Size |
|---|---|---|---|---|---|---|---|---|
| Settings model: CLI flags, a user config file and report fields | — | none | later (with the first toggle) | Every row below needs it | 1–2 N | 1.98–9.08 | 0.23 | S |
| MSAA (EAWR-184) | yes | 4× in the enhanced profile, off in captures | done | Retail Highest uses AA level 1 | — | — | — | — |
| Resolution, window mode, VSync, frame cap | yes | partial | later (with settings) | Godot built-ins | 1–2 I | 0.76–2.86 | 0.08 | XS |
| Gamma (curve in the stored-output pass, retail formula) | yes | missing | later | Identity at the default | 1 F | 1.35–1.93 | 0.07 | XS |
| Bloom and heat toggles | yes | — | with the ports in (a) | On by default, as at Highest | 0 | — | — | — |
| Shadows off, quality tiers, soft filter | yes | on/off flag | later (after FP-2 EAWR-150) | Maps shadow detail 0 and the soft-shadow setting | 1 F | 1.35–1.93 | 0.07 | XS |
| Particle and environment detail (retail rules) | yes | fixed 1.0 | later, if a target misses frame time | Performance knobs for ARM64 or lavapipe | 2 F | 2.70–3.86 | 0.14 | S |
| Mesh detail (retail LOD rule) | yes | fixed 1.0 | later, same trigger | Cheap, deterministic | 1 F | 1.35–1.93 | 0.07 | XS |

### (c) Optional enhancements (off = retail)

| Feature | Retail? | Status | Recommend | Reason | Tasks | Worker-h | Agent-days | Size |
|---|---|---|---|---|---|---|---|---|
| Anisotropic filtering Off/2/4/8/16× | no | EAWR-184: off in captures, 16× for players (terrain only) | done | Off settled the terrain-sampler fidelity item | 1 F | 1.35–1.93 | 0.07 | XS |
| Resolution scale: supersampling and FSR 1 | no | none | later | AA without shader side effects; performance on weak GPUs | 1–2 I | 0.76–2.86 | 0.08 | XS |
| FXAA / SMAA | no | EAWR-184: SMAA for players | done | Covers alpha-tested foliage edges that MSAA misses | 1 I | 0.76–1.43 | 0.05 | XS |
| Shadow tiers above retail (8k atlas, 4 splits, PCSS) | no | none | later (the shadow-tier row in b) | Enhancement only | 0 | — | — | — |

### (d) Do not add

| Feature | Reason |
|---|---|
| SSAO, SSIL, SDFGI, VoxelGI, volumetric fog, Godot glow, SSR, DoF, auto-exposure | Linear-light maths on stored values, and a look retail never had |
| TAA, FSR 2, MetalFX temporal | Ghosting without motion vectors from every custom shader; changes the look |
| HDR output | SDR content, no benefit |
| Retail lower tiers (FF/DX8 techniques as options, water 0/1, texture reduction, auto-detect benchmark) | Retail picks lower tiers for Intel and older GPUs (`FF_0`–`FF_3`, `Config_High` and below). The remake renders the Highest preset on every GPU (owner decision D1 = C on EAWR-189), so these would mean porting fallbacks nobody uses. |
| `DynamicLighting` | Hidden and off in every preset |
| Widescreen toggle, hardware-cursor option in the 3D layer | The true aspect is always used; the cursor belongs to the UI phase |
| Stencil shadow volumes | Only if the owner picks D3-B below (1 R + 2–4 N + 1 C, 5.95–20.99 h, 0.56 agent-days, M) |

## 5. Owner decisions

| ID | Question | Options (recommended first) | Pros | Cons |
|---|---|---|---|---|
| D1 | How many retail detail settings do we support? | **A** Highest fixed, plus toggles for bloom, heat, shadows, AA, VSync, window and gamma, plus the (c) extras. **B** The full retail ladder (all shader/water/texture tiers and auto-detect). **C** Highest fixed, no settings until M6. | A: small, and it covers what players change. B: complete parity. C: zero cost now. | A: no low-end tiers. B: ports FF/DX8 fallbacks nobody needs, L-sized. C: MSAA EAWR-184 and VSync stay hard-coded. |
| D2 | Player defaults for the enhancements | **A** Retail Highest everywhere (bloom, heat, MSAA at retail level 1, aniso off); enhancements opt-in; captures pinned. **B** Enhanced player defaults (aniso 16×, MSAA 4×, SMAA); captures pinned to retail. **C** A single Retail/Enhanced preset switch. | A: what you see is retail. B: a sharper first impression. C: one clear choice for players. | A: looks dated on large screens. B: players never see the retail look by default. C: one more preset to test. |
| D3 | Shadow technique | **A** Godot shadow maps (FP-2 EAWR-150) with tiers mapped from retail shadow detail. **B** Port retail stencil volumes, screen blur and blob shadows (the models carry `MeshShadowVolume` geometry). **C** A now, B as a Phase 5 fidelity item if eye-checks show shape differences. | A: already in progress, cheap tiers. B: exact silhouettes and softness. C: pays for B only if it is needed. | A: shapes and softness differ from retail. B: M-sized, and Godot material stencil is not enough for two-sided z-fail, so it needs an RD pass. C: the difference stays until Phase 5. Recommend C. |
| D4 | When do the new parity gaps land? | **A** In M1.5, after FP-3 EAWR-151: hull bump/spec, DX8 SPH, the rig-settings record, then bloom (0.74 agent-days in all). **B** With P2-17 in M2. **C** Phase 5 fidelity list. | A: M2 starts with the retail look on every ship, and references are comparable. B: no M1.5 growth. C: no cost now. | A: adds about 0.7 agent-days to M1.5. B: M2 eye-checks run against a known-wrong hull look. C: the most visible gaps stay. |
