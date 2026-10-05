# FoC river-group selection

FoC TED stores ribbon groups at `1/257/9`, under a water header at `1/257/0`.
These groups include river water and other textured effects. The per-group
parameter mini `0x01` separates the two family-zero cases in the installed
FoC maps: mode **5** is Naboo river water, while mode **4** is
used by Bespin and Utapau station-light ribbons and the other family-zero
effect groups. The renderer keeps both as decoded geometry and submits both
mode 4 and mode 5 even when the header's water-family mini `0x16` is zero.
A family-zero header still produces no map-wide water plane. Mode 5 uses the
header tint and alpha and the group's flow mini `0x03` to scroll its texture.

Retail classification, from the FoC debug build
([audit](debug-build-audit.md#river-groups), debug-build behaviour audit): each group is a terrain track.
Mini `0x01` is the track's draw mode. Water, road and river are a separate track
type in mini `0x10` (0 water, 1 road, 2 river). Retail draws water-type tracks in
the water-decoration pass and every other track in the track pass. Over a river-type
track the water elevation is the terrain height plus the track's custom height
(mini `0x0c`), a gameplay value. Mini `0x03` is retail's U shift rate and `0x04`
its V shift rate. In the installed maps Naboo's four groups are river tracks with
draw mode 5 and the station-light ribbons are road tracks with draw mode 4, so mode
and type do not necessarily agree there. PI-8 now reads mini `0x10` and submits
non-water tracks before water-decoration tracks, independently of draw mode.
The FoC debug build was rechecked: the terrain track loader assigns minis `0x01`
and `0x10` to separate fields (PI8-01); water decorations accept only the water
type (PI8-02, PI8-05), and track geometry rejects that type (PI8-03, PI8-04).
The private address and decompiler map is in ignored `out/research/p2-294/evidence.md`.

Effective-overlay data matters. `_mp_land_utapau.ted` in `Patch2.meg` replaces
the older `Maps.meg` version. The effective map has family 0 and 18 mode-4
`EB_stationlights.tga` groups; the older version has family 1 and no groups.
The effective `_mp_land_bespin.ted` has family 0 and four mode-4
`EB_stationlights.tga` groups. Both have zero Riverbed terrain samples under
their rendered group centerlines (0/244 and 0/234 respectively), so a
Riverbed-layer gate would erase authored effects. Bespin's water-height field
is 124; Utapau's is -190. Neither height activates a plane with family 0.
The original FoC skirmish captures at
`out/original/mp_land-bespin/zoomed_out_x1.png`
and `out/original/mp_land-utapau/zoomed_out_x1.png`
show Bespin's dry platform and
Utapau's sinkhole with a blue, water-like surface far below the rim. Neither map shows a river along these
group centerlines. Their authored texture and mode identify them as effect
strips rather than river water; the visual record alone does not establish
the game's internal submission predicate. The large no-texture terrain slots
on these two maps make their pit backgrounds a separate rendering question;
the blue Utapau surface alone is not evidence that a mode-4 group is water.

FoC `_mp_land_naboo.ted` has family 0, water height 0, a white header tint,
header alpha 0.5, and four mode-5 `W_River_07.tga` groups. Each group has
flow -0.611111; its source-basis control points descend from about 200 to 35
units at the falls, and its widths range from 44 to 114 units. Their centerlines
touch the `W_Temperate_Riverbed01.tga` terrain layer at 408/430 sampled sections.
The owner's later FoC stream still and waterfall video show moving translucent
water over the cut, so the earlier acceptance of the dry cut was superseded.
The riverbed sample is corroborating map evidence; selection uses the group
field rather than a terrain-texture heuristic. The `0x01` numbers are treated
as observed mode values, without claiming their full retail enum semantics.

The complete effective FoC family-zero set with ribbon groups was checked
against the installed archives. Naboo alone has mode 5; the other seven maps
have only mode 4. Among those eight maps, only `um11` has water-type tracks
previously styled as mode-4 effect strips. The 42 tracks are indices 103–144
within its 231 authored groups, now submitted to water decorations. Across all
23 effective FoC land maps with tracks, 12 have type/mode pass mismatches. The
full per-track index list and source archive are in ignored
`out/research/p2-294/land-track-check.json`.

| Effective map | Former mode-based water → track | Former mode-based track → water |
|---|---|---|
| `_land_planet_abregadorae_02` | 16 | — |
| `_land_planet_bestine_02` | — | 0–2 |
| `_land_planet_bothawui_02` | 0 | — |
| `_land_planet_mandalore_01` | — | 7–8 |
| `_land_planet_mandalore_demo_01` | — | 0–1 |
| `_land_planet_myrkr_01` | — | 0–1 |
| `_land_planet_naboo_02` | 4–6, 11–13 | — |
| `_mp_land_bothawui` | 2 | — |
| `_mp_land_dantooine` | 0, 4–6 | — |
| `_mp_land_mandalore` | — | 0–1 |
| `_mp_land_naboo` | 0–3 | — |
| `um11_raiders_of_the_lost_holocron` | — | 103–144 |

| Map (basename) | Mode 4 | Mode 5 | Drawn before and after Naboo support | Retail track types |
|---|---:|---:|---:|---|
| `_land_planet_bespin_01` | 4 | 0 | 4 | 4 road |
| `_land_planet_utapau_01` | 18 | 0 | 18 | 18 road |
| `_mp_land_bespin` | 4 | 0 | 4 | 4 road |
| `_mp_land_naboo` | 0 | 4 | 0 → 4 | 4 river |
| `_mp_land_utapau` | 18 | 0 | 18 | 18 road |
| `um01_a_crimelord_unleashed` | 8 | 0 | 8 | 8 road |
| `um04_visions_of_the_past` | 65 | 0 | 65 | 65 road |
| `um11_raiders_of_the_lost_holocron` | 231 | 0 | 231 | 189 road, 42 water |

The original seven-map pixel comparison was true but insufficiently explained:
both its before and after reports already counted those mode-4 strips. A
regression check must assert the report counts as well as compare captures,
with a separately built baseline viewer to establish the before image.

## Naboo visual mapping

The original FoC stream is a translucent grey-olive strip over a visible
riverbed, flowing along the authored point order. The four mode-5 groups use
the base game's `W_River_07.dds` (declared as `.tga` in TED); mini `0x03`
is -0.611111 in all four. The renderer advances that value along ribbon V,
so negative scroll moves texture detail toward later control points. The
recorded waterfall shows a near-vertical grey sheet with moving white
highlights and small bright flecks. The 150-unit drops in the control points
shape that sheet; a slope-driven highlight approximates its sheen. Twenty-three
`P_Waterfall_Top` map particle proxies (three additive emitters each in the
viewer report) supply the flecks and mist around the drops. The authored
`W_Temperate_Riverbed01` terrain layer supplies the rough shoreline.

Family zero skips the map-wide `TerrainWater.fx` effect and its
reflection/refraction passes. The header's 0.5 alpha still tints mode-5
ribbons, letting the bed show through. The viewer uses a generic Godot water
material for these strips; exact retail blending, highlights and particle
brightness remain visual approximations.
