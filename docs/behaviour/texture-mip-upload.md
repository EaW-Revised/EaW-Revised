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

## MT-02: chains for the single-level textures of opaque surfaces in the space compose (legacy EAWR-5989)

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-02 | The BaseTexture and the normal-map bindings of every opaque surface the space compose draws, ships, stations, props, asteroids and debris alike (every `LegacySelector` that is not a transparent-phase effect; the normal-map bindings are the family's `flat_normal` ones in `legacy/`) are handed to the renderer with their missing mip levels generated down to 1x1, in the texture's own format, when the texture stops above 1x1 (a single level or an authored partial chain). Each new level is a 2x2 box average of the stored 8-bit values, round to nearest, no colour-space conversion; authored levels stay byte for byte. A normal map gets the same plain box average (see below). The work runs on the texture read-ahead's reader threads while the initial compose loads, and never during the battle. | Debug build: the effect texture request asks for a full chain with mip filter 5 (box) in the file's own format (legacy EAWR-5989). Rig arm with retail-style chains on the 20 hull colour and normal maps of one Star Destroyer: blink events 102 to 37, hull brightness 0.99, lights peak 0.88. Repo tests `texture_chain_contracts`, `texture_prefetch_contracts`, `compose_texture_cache_contracts`. |

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
- Space battles only: land battles get no chains (the read-ahead and the chains live in the space compose).
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
