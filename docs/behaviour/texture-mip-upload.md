# Texture mip upload

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-01 | A valid multi-level DDS chain may stop before 1x1. Preserve the decoded asset and every authored level; complete only the upload copy through 1x1 because Godot's image API accepts a full-chain boolean rather than an explicit level count. Single-level images remain unmipped, except under MT-02. | Partial-chain upload failure (legacy EAWR-4638): debug-build texture creation requests default mip levels; the referenced texture API defines a complete chain. Synthetic renderer contracts and graphical DXT1/DXT3/DXT5/RGBA uploads cover seven authored levels of a 1024-square image. |

Missing uncompressed levels use a channel-wise box average, including odd edges
and one-pixel axes. Missing compressed levels replicate the nearest source block
on a halved block grid, retaining its encoding and selectors. This is an explicit
approximation for the absent levels, rather than a claim of identical retail
filtering. Existing compressed levels remain byte-identical and compressed,
avoiding a full-image expansion or recompression during loading. The same rule
applies to mesh and normalized particle uploads. GPU accounting includes the
generated tail, including when texture pressure drops authored top levels.

The DDS decoder's accepted formats, validation and partial mip counts do not
change. The generated tail never becomes part of the decoded asset or content
identity. No mod names, file names or retail mip counts drive this adaptation.

## MT-02: chains for the single-level textures of opaque surfaces in the space and land composes (legacy EAWR-5989, EAWR-6723)

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-02 | The BaseTexture and the normal-map bindings of every opaque surface the space or land compose draws, ships, stations, props, asteroids and debris alike (every `LegacySelector` that is not a transparent-phase effect; the normal-map bindings are the family's `flat_normal` ones in `legacy/`) are handed to the renderer with their missing mip levels generated down to 1x1, in the texture's own format, when the texture stops above 1x1 (a single level or an authored partial chain). Each new level is a 2x2 box average of the stored 8-bit values, round to nearest, no colour-space conversion; authored levels stay byte for byte. A normal map gets the same plain box average (see below). In the space compose the work runs on the texture read-ahead's reader threads while the initial compose loads; in the land compose it runs on the loading thread while the map composes; never during the battle. | Debug build: the effect texture request asks for a full chain with mip filter 5 (box) in the file's own format (legacy EAWR-5989). Rig arm with retail-style chains on the 20 hull colour and normal maps of one Star Destroyer: blink events 102 to 37, hull brightness 0.99, lights peak 0.88. Repo tests `texture_chain_contracts`, `texture_prefetch_contracts`, `compose_texture_cache_contracts`, `python_presentation_renderer_land_texture_chain`. |

Why: a surface far away (a ship's hull, above all) samples a full-size texture at 16-64 texels per screen pixel, so a pixel lands on a
bright or dark texel and the surface speckles and shimmers. A chain averages them. Fine detail is softer at
distance, as in retail.

Formats: BC2 (DXT3), BC3 (DXT5), BC1 (DXT1) without 1-bit alpha, and uncompressed 24- and 32-bit (rgba8, bgra8,
bgr8). Other formats (BC4, BC5, BC7, 8-bit) and BC1 with alpha are left as they are. Block-compressed levels are
re-encoded by this project's own encoder (`src/assets/texture_chain.cpp`, written from the public S3TC block
layout; no third-party code and none of retail's).

Colour space: the images are uploaded as plain 8-bit data and the adapters compute on stored values (docs/rendering.md
colour policy), so the average of stored values is what the sampler's own linear filtering already blends; no sRGB
conversion happens in the chain.

Normal maps: retail's request is a plain box filter with no renormalisation, and the bump adapters do not renormalise
the sampled normal either, so the averaged normals get shorter at distance and the highlights soften. The chain does
the same: a normal map gets the plain box average, as the rig arm measured. Scaling each averaged normal back to unit
length is written (`renormalize_normals`) but off: `renormalize_normal_chains` in `texture_chain.hpp` is the one place
that decides, because unit normals keep their full specular peaks at distance, which is unmeasured and may bring hull
specks back.

The ship window lights are not in the class: the additive effects are transparent-phase selectors, and a chain dims
their far lights (retail's look) while the owner prefers the original brightness. Adding them back is one change in
`texture_processing_for` (`apps/viewer/src/compose_texture_cache.hpp`).

Scope and limits, so nothing here is a hidden rule:
- Selected by effect phase, material slot (from the family table) and format, never by file or mod name. It covers every
  opaque surface the space compose draws, not only ships: stations, map props, asteroids and debris too, and a planet
  family's own normal binding (`Planet_Deathstar`). Alpha-tested foliage (`Tree.fx`) has its alpha averaged, so cut-outs thin
  at distance, as in retail.
- Only the initial compose, and only beside the texture read-ahead (Separate render thread; a texture the read-ahead fails or has not started is chained by the loader instead, same result, longer load); the Safe render thread
  has no read-ahead and keeps the single-level textures. The slots composed with the start (the starting fleet and the
  purchase slots, up to `purchase_slots` per type and player) get chains. A ship composed during the battle (a slot
  grown beyond those, an ability object) keeps the single-level texture, so no step during play grows by the cost of a
  chain; it uploads its own single-level copy beside a chained copy of the same file, if the initial load made one.
- Land battles get the same chains (legacy EAWR-6723): the land compose loads the BaseTexture and the family normal-map bindings of every
  placed unit, building and prop through one loader that applies the same policy (`texture_processing_for`, by effect phase and
  family slot, never by file or mod name). There is no read-ahead on land: the chain is built on the loading thread while the
  map composes, once per path and policy, and never during the battle. A land texture re-admitted after a GPU eviction gets the
  authored levels back, as in space.
- Land water gets the plain chain (legacy EAWR-6764): the water plane texture and the river, track and water-decoration textures are
  sampled with linear mip filtering, so the land compose completes a chain stopping above 1x1 on the loading thread with the plain
  2x2 box average (`ChainKind::colour`, the kind the environment textures take), once per water texture, never during the battle.
  The water shader samples no bump texture, so there is no normal chain. Debug build: the terrain water textures are read with
  the create-mips flag on (mip filter 5, box), as in MT-02. A stock water texture ships a full chain (273 water-class files
  scanned, all complete bar a loading screen), so stock output is unchanged; the gap was mod-only.
- The terrain blend layers need no rule: the renderer keeps only the top level of each layer, resizes the layers to one edge and
  regenerates the whole chain, so a layer is mipped whatever its file holds (`test_land_water_chain.py` pins this).
- A file wanted both by a chained slot and by another effect is held and uploaded twice (chained and plain).
- Odd sizes floor-halve like the level sizes (a last odd row or column drops out of the average).
- The renderer holds only the finished chain: dropping top levels under texture pressure (GM-04) reads the resident
  levels back and never regenerates.
- CPU memory: the texture's CPU snapshot is freed after upload like any other. The recipe verifies the file against the
  authored levels alone (`assets::Source::authored_levels`, `texture_recipe.hpp`), so a chained texture costs no
  system RAM beyond the read-ahead's transient buffers. The exception: an upload that is not eagerly resident (a
  small card under VRAM pressure) keeps its snapshot until its first admission, as it always did; that snapshot is now a
  third larger for a chained texture. A model re-admitted during play (after a GPU eviction) gets
  the authored levels back, not the generated ones: a regeneration costs 16-218 ms a texture on the thread that asks,
  and play does not pay it. That ship looks as it did before this rule until the next load.

## MT-03: chains for the single-level textures of particle systems (legacy EAWR-6724)

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-03 | The colour texture and the bump normal texture of every particle system (battle effects, unit emitters, debris, land map particles, galactic corruption, the effect probe) are decoded through one loader that completes a chain stopping above 1x1 (a single level or an authored partial chain) with a plain 2x2 box average of the stored 8-bit values (`assets::extend_mip_chain`, `ChainKind::colour`), in the texture's own format. Authored levels stay byte for byte; a complete chain, or a format the generator does not handle, comes back as decoded. The generator covers BC1 without 1-bit alpha, BC2, BC3 and the 24- and 32-bit uncompressed formats; BC4, BC5, BC7, L8, A8 and BC1 with 1-bit alpha stay as authored (a gap, not covered here). The chain is built when the texture is first resolved by an owner: the battle effects, unit emitters and debris resolve every texture their battle can draw at load (the effect prewarm), so no chain is built on a battle frame; the load-time counter `particle_chains` in the startup report (`builds_after_first_frame`) shows it. The land map and galactic corruption owners resolve on the first spawn of an attachment, outside battle play. Additive particles take the plain chain, not the brightness-compensated one: the sprites are smooth, so a box average keeps their mean brightness (a lit battle capture on a large mod, same map, pose and tick, differs from the unchained build in 27 pixels by more than 8 levels and not at all in whole-frame mean luma), and the plain box is what the retail request builds. The normal texture takes the colour kind too, which is the same output while `renormalize_normal_chains` is off (the loader asserts that). | Debug build: a particle system loads its colour and normal textures through the file loader with the create-mips flag on, which asks for a full box-filtered chain whatever the file holds (legacy EAWR-6724). Repo tests `particle_texture_load_contracts`, `python_presentation_renderer_particle_texture_origin`. Cost numbers are recorded in the pull request that added this rule. |

## MT-04: chains for the remaining space compose textures (legacy EAWR-6725)

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-04 | The space compose (MT-02) also completes the chain of (a) the BaseTexture of the alpha-blended (non-additive transparent) materials, with the plain box chain (the additive materials keep their brightness-compensated chain); (b) every texture a material's family samples besides BaseTexture and its normal maps (gloss, specular, wave, distortion, cloud), with the plain box chain; the family's normal maps keep the normal chain; and (c) the BaseTexture and family textures of the fog-of-war unit surfaces, with the same processing the live compose gives the same materials; and (d) the BaseTexture, wave and distortion textures of the shield and cloak shells, with the plain box chain (built by the read-ahead's loader, since nothing offers them ahead). Each uses the same generator, formats and load-time read-ahead as MT-02: authored levels stay byte for byte, a complete chain or an unsupported format comes back as decoded, and a ship or shell composed during the battle (no read-ahead) still keeps its textures single-level. Mid-battle compositions stay out: a chain costs 16-218 ms a texture, which the battle frame cannot pay. | Debug build: every effect texture request asks for a full chain with the box filter, whichever slot or blend phase it belongs to (legacy EAWR-6725). Repo tests `compose_texture_cache_contracts`, `python_presentation_renderer_space_meshgloss_sky`. |
