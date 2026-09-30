# Phase 1: Render the world

Rescoped 2026-09-24. These criteria replace the acceptance checklists in EAWR-22–EAWR-33 and the
earlier gate map. The earlier version is in git history. Target date is unchanged:
**2026-10-09**.

## What M1 is

The milestone from the original plan:

> A space map and a land map from Forces of Corruption render with the real shaders, units
> placed from XML, particles playing. Screenshot-fidelity milestone.

"Screenshot fidelity" means that someone who knows the game recognises the remake
screenshot as the same scene when it sits next to the original. It is not pixel equality,
a metric threshold or a reproduction of timing. Exact visual fidelity is a Phase 5 concern.
Until gameplay exists, it is tuned against a moving target.

Reference maps: the 2026-09-24 selection of two multiplayer skirmish maps remains.
The owner decided on 2026-09-25 that M1 targets FoC only; the original screenshots
come from FoC skirmish, and its expansion assets take precedence over base EaW:

- land: `data/art/maps/_mp_land_naboo.ted` (1,697 placements, 17 water records,
  112 texture references, 2 skies;
  adds `Tree.fx`)
- space: `data/art/maps/_mp_space_coruscant.ted` (58 placements, 1 texture reference,
  2 skies; planet, nebula, blue star backdrop)

`plan/inventories/map-reference-maps.json` pins these two maps with `role: m1_reference`.
The base EaW versions of both maps and the campaign maps `_land_planet_alderaan_02.ted`
and `_space_planet_alderaan_01.ted` stay as `role: regression_fixture`; tests measured on
them select their profile explicitly. Skirmish
spawns starting units, so the owner picks any route to a unitless original shot.

## How acceptance works

- **The owner accepts.** A ticket is done when its checklist below is met and the owner says
  so in an issue comment. No independent acceptance agent, audit or contract review.
- **Evidence is short.** One issue comment per ticket: what was built, the test command, and
  screenshots where the item is visual. Private images stay under ignored `out/`. Keep it
  under about 15 lines.
- **Visual items are judged by eye.** The owner takes original screenshots by any route and
  at a roughly matching camera. There is no fixed time, seed, pause qualification, capture
  channel or region mask. A visual item fails only when something is missing, broken
  (wrong orientation, black or magenta material, flicker, obvious wrong colour or scale)
  or not recognisable. Everything else goes on the fidelity list.
- **One code review per PR.** It looks for correctness bugs only. Nobody reviews
  documentation wording.
- **No reports by default.** Write a `docs/` note only when it records a decision or a
  format fact that later code relies on. Progress belongs in the issue comment.
- **Scope does not grow.** Anything not on a checklist below does not block the ticket.
  Record it on the fidelity list (at the end of this file) in one line and move on.
- **CI stays batched.** Run five-target CI at M1 exit, plus any batch the CI budget
  policy already calls for.
- **EAWR-5 does not gate Phase 1.** It was accepted on 2026-09-24 with the twelve missing EaW
  CIN includes as a known gap. Placement and catalog results use the current catalog.

## Checklists

The status column is a first read of existing reports. Confirm it before closing anything.

| Ticket | Likely status |
|---|---|
| EAWR-23 P1-02 shaders, EAWR-31 P1-10 atlas | Done, closed |
| EAWR-26 P1-05 TED loader, EAWR-28 P1-07 fog | Met apart from the confirmation noted below |
| EAWR-22, EAWR-25, EAWR-30 | One or two items left |
| EAWR-24, EAWR-27, EAWR-29, EAWR-32 | Real work left, mostly on the space map and animated/populated scenes |
| EAWR-33 | Last; needs original screenshots and one M1 CI batch |

### EAWR-22 P1-01 Production renderer

- [ ] Both reference maps render through `apps/viewer`, on Windows (hardware) and on Linux
  x64 (software).
- [ ] Unsupported materials fail with a diagnostic, not a crash or a silent wrong draw.
- [ ] Opaque, alpha-tested and transparent draws sort correctly on the reference maps.
- [ ] Switching scenes and reloading do not leak or leave stale instances (the existing churn
  probe).
- [ ] Headless replay hashes are unchanged.

Dropped: the Windows and Linux migration tolerance contracts, prototype-versus-production
pairs and prototype retirement. `prototypes/godot` can be deleted whenever convenient. The
ARM64 viewer package moves to the fidelity list.

### EAWR-24 P1-03 Skinned meshes and animation

- [ ] An infantry unit, a vehicle and a capital ship with turrets animate correctly by eye in
  the viewer. The reference is the original game or alo-viewer.
- [ ] ALA load failures are counted, with cause, in the existing inventory.
- [ ] Named attachment points follow the animated hierarchy, with synthetic tests.
- [ ] Headless replay hashes are unchanged.

Dropped: fixed-time original captures and approving unit-to-animation associations. The
356 association failures move to Phase 2, where units need their idle, move and attack
clips.

### EAWR-25 P1-04 Lighting and shadows

- [ ] SH lighting is ported with unit tests.
- [ ] Shadows are visible on the land terrain and on a real ship hull on the space map.
- [ ] Lighting on both reference maps looks plausible next to the original screenshot.
- [ ] The P0 hemisphere policy still reproduces its baseline.

Dropped: the SH-versus-hemisphere error metric, the lighting triplet contract and
confirmation of the TED sun mapping. A plausible reading is enough; the exact mapping is on
the fidelity list.

### EAWR-26 P1-05 TED map loader

- [ ] Every available vanilla, FoC and Remake map loads or is listed with a cause.
  (2,103/2,103 are recorded.)
- [ ] The placement resolution rate against the current catalog is reported per profile.
  Vanilla is at least 99%, or the shortfall is listed with causes.
- [ ] Synthetic minimal-map, truncated-chunk and bad-reference tests pass.
- [ ] A coordinate note exists for consumers.

Dropped: the dependency on accepting EAWR-5, and the reviewed environment contract.

### EAWR-27 P1-06 Terrain, water, sky and nebula

- [ ] Land reference map without units: terrain with layer blending, sky and water, next to
  an original screenshot. Approximate water is fine if noted.
- [ ] Space reference map without units: skydome, nebula, sun and planet, next to an
  original screenshot.
- [ ] Terrain and sky families that the reference maps do not use are listed, not
  implemented.
- [ ] One frame-time number is recorded on Windows at 1280x720, with the GPU named.

Dropped: exact mode-7 sun geometry. An approximate sun that sits in roughly the right place
is enough. Every family used anywhere in the corpus is also dropped.

### EAWR-28 P1-07 Fog of war

- [ ] A painted grid darkens terrain and units on the land reference map.
- [ ] Painting changes presentation only; replay hashes are unchanged on five targets
  (M1 CI batch).

Dropped: comparisons of original fog edges, and space fog.

### EAWR-29 P1-08 Particles

- [ ] The emitters on both reference maps play, and so do engine glow, a laser hit, an
  explosion and smoke on units. All are checked by eye against the original.
- [ ] Particle families in the corpus are counted, and unsupported ones are listed with
  their reason (this exists).
- [ ] Fixed-seed tests pass and there is no particle state in the sim.

Dropped: ten seed-matched original captures, identifying the owner of the hyperspace effect,
and the Remake-only effects (those move to Phase 5).

### EAWR-30 P1-09 Tactical camera

- [ ] Zoom limits, pitch and pan speed feel like the original on both maps (owner check).
- [ ] Camera constants are listed with their XML source.
- [ ] Bindings and focus handling work; replay hashes are unchanged.

Dropped: timed pan measurements and calibrated framing at three zooms.

### EAWR-32 P1-11 Populated scenes

- [ ] Both reference maps render populated from their XML placements, with team colour,
  animated units and attached effects. Objects that don't resolve on those two maps are
  listed.
- [ ] Every vanilla map is attempted, and the unresolved placement metadata is committed.
  (Done.)
- [ ] The asset-free import fixture hashes identically on five targets (M1 CI batch).

Dropped: the dependency on accepting EAWR-5, and paired captures against the original under a
contract.

### EAWR-33 P1-12 M1 sign-off

- [ ] The owner takes the original screenshots in one short session: land and space, two
  or three camera positions each.
- [ ] Matching remake screenshots are placed next to them, and the owner signs off on EAWR-15.
  Differences go on the fidelity list.
- [ ] M1 CI batch: five targets green and replay hashes unchanged. Linux x64 renders a
  reference scene in software and uploads the screenshot as an artifact.
- [ ] Phase 2 is ticketed and re-estimated from Phase 1's measured effort.

Dropped: Windows and Linux pixel thresholds, the independent audit, the original-identity
contract, the capture campaign, and the ARM64 runtime claim. The ARM64 core build stays
green in CI.

## Fidelity list

- Close-up validation (EAWR-231): the Naboo research-facility camera at source (4360,2606), zoom 0.15, produces its PNGs but fails the generic terrain SH-versus-hemisphere comparison in both baseline and revised builds; the evidence check needs to handle views without eligible terrain samples.

Known differences and deferred work. None of these block M1. They are input for Phase 5 or
for the phase that first needs them.

- SH versus hemisphere error metric, and confirmation of the TED environment sun mapping (EAWR-25)
- Exact mode-7 sun billboard geometry (EAWR-27)
- Space environment view (EAWR-27): Planet.fx atmosphere, city lights and normal maps (the t0 fallback drops them), the
  retail mode-6 glow rule, the sky orientation double conversion (G-SKY-02) and the original opening camera yaw
- Terrain, sky and water families not on the reference maps are listed in
  `plan/inventories/reference-map-unused-families.json` (EAWR-27)
- Terrain normal/specular maps, TerrainClouds shadows, distance fog, vertex intensity,
  exact retail river material/reflection and waterfall sheen/foam calibration (EAWR-27)
- The 356 ALA association failures (Phase 2, EAWR-24)
- Particle families not on the reference maps, the hyperspace effect and Remake-only effects (EAWR-29)
- Effect-mode host drawing, explicit EaW profile selection, multi-frame capture and rig captures (EAWR-29)
- Timed camera pan and calibrated zoom framing (EAWR-30)
- Further tactical zoom levels and the transition into them beyond the current Distance_Max (EAWR-30, owner, low priority)
- Land sky dome at the 5-degree Ctrl-orbit limit (seen on Naboo; fine at 25 degrees): the upper sky shows stretched vertical streaks that the retail pitch range never reveals (EAWR-27, EAWR-30)
- Windows and Linux pixel tolerance, the ARM64 viewer package and native ARM64/GPU runtime (EAWR-22, EAWR-33)
- The twelve missing EaW CIN includes, which the pinned build does not ship (a known gap accepted in EAWR-5), and the
  upstream schema merge-policy risk from the LSP comparison
- The original capture contract and campaign work on branches `wp-01` and `wp-09`, which
  can be reused if Phase 5 needs measured fidelity
- Space populate (EAWR-32): the hardpoint placement rule (a hardpoint ALO that repeats its owner's skeleton is drawn
  in the owner's model space) is inferred from the two Coruscant stations; Layer_Z_Adjust is not applied
- Shadows (EAWR-25, EAWR-150): the GLES3 station-hull acne is gone on Forward+ (rechecked on the Coruscant truss station in EAWR-150). Godot shadow maps stand in for the retail stencil volumes. The floor multiplies stored values by the environment's `0x17` colour per channel, as retail does (EAWR-225; Naboo Sunrise_Clear 0.40/0.42/0.60, Coruscant 0.55); retail shadows sit a further 2.5–4 levels higher, source unknown. The four-tap mask blur is not reproduced
- Close-up route (EAWR-150): a second `-CloseUpWorld` at the point the camera already looks at fails the look-at check (Naboo sensor at 20 notches after 14: offset 6.8 px), and a target near the map edge fails on a clipped footprint (Coruscant truss at 4830, 4805)
- Grass clump billboarding and skirmish lobby `MP_Color_*` player colours (EAWR-32)
- Vegetation wind (EAWR-147): the wind is TED environment record 0's (Naboo `Sunrise_Clear`, 5.06), where retail picks the environment at random at battle start (`--eawr-environment-record 1` shows `Noon_Clear`; the owner clip is `Noon_Clear`, 2.0: tree tops sway 2.5 times less and flowers wave every 7.3 s, not 6.4 s); tree shadows sway with the canopy, where retail's shadow volumes stay still; unit wind disturbances and the model box's user box and excluded sub-object type are not modelled; Naboo's flower heads draw red where the clip shows orange
- Space prop idle clips (EAWR-145): each placement's start frame is a hash of its TED record, not retail's random draw. Retail's later random idle variants (`Idle_Anim_01` and up) are not played. Fixed captures hold the clip clock at the particle-frame count (`--eawr-map-idle-offset` moves it); only the live view runs it in real time.
- Asteroid billboards (EAWR-238): density, V1 size and the bump shader follow retail; per-rock positions follow our RNG, not retail's; bump particles do not write depth (retail t2 writes depth with alpha test > 8/255).
- Space effect clock (EAWR-185): the viewer has no pause or game speed, so the nebula's `TIME` always runs at 0.03 s per 30 Hz tick. The Planet.fx cloud scroll (`TIME * CloudScrollRate`) lives only in the bump techniques, which the t0 fallback does not port.
- Land prop idle clips (EAWR-157) follow the same rule and gaps. Naboo's Team_00/01 `MultiplayerStructureMarker` comms arrays are drawn as the marker type, whose `rb_comcenter` clip plays once and holds; a skirmish replaces them with `Communications_Array_R/E`, which loop it.
- Naboo waterfall sparkle (EAWR-196 eye check against the owner's clip): retail shows a bright lavender-white sparkle column down the fall and spray at its lip; the live view of the west falls shows a sparser `p_waterfall_top` spray at the foot (the lip was out of frame). Both keep moving for the whole clip.
- Map close-ups (`--eawr-map-target`, `--eawr-map-zoom`) at a Naboo waterfall fail particle evidence (exit 2) when an in-view `p_waterfall_top` proxy changes no pixels in its projected bounds (cause not checked); the capture is still written.
- Bespin and Utapau no-texture terrain slots render as grey checker openings rather than the original orange and blue pit backgrounds (EAWR-27).
- Naboo's eight `MeshSolidColor.fx` Team_00/01 base and spawn position markers are editor/gameplay helpers, absent as visible coloured meshes in the FoC originals; keep their scene surfaces unadmitted (EAWR-32)
- Naboo's `MeshShadowVolume.fx` (461 placements, 604 surfaces on drawn placements) and `RSkinShadowVolume.fx` (7 placements/surfaces) are non-colour stencil shadow volumes; Godot shadows come from the visible meshes instead of those volumes (EAWR-25, EAWR-32)
- ALO bone billboards: mode 6 faces the view with a 1.18x space-glow margin; exact sunlight placement and the mode-4 light-axis and mode-5 wind-axis rules remain deferred (EAWR-27, EAWR-32).
- Bloom (EAWR-201): the viewer blooms with environment 0's values, where retail picks the environment at random (Naboo's two differ only in cutoff, 0.9 vs 0.9028). Its bloom source includes particle heat distortion, where retail copies the frame before heat. The Coruscant planet limb glow retail shows needs the planet drawn lit (EAWR-27 line above).
- Hull bump eye check (EAWR-199, Coruscant at Highest): with the retail light heading (EAWR-262) hulls, hull rocks and pebbles take retail's back-lit dark, teal-filled tone, and the station dome top reads a little darker than retail; the player station on its Team_00 marker looks turned about 90 degrees against retail, and retail close-ups sit at a lower camera pitch for the same size; the Empire start station is under fog at start; the Star Destroyer and corvette have no original reference until EAWR-43 part B; the P0 `--eawr-model` Hull preview and the hull shadow fixtures keep the fixed-function t0 technique, and the legacy-family GPU probe does not cover sph_t2.
- DX8 mesh eye check (EAWR-200, Naboo, Utapau and Coruscant at Highest): the Naboo battle (Sunrise_Clear) and the Utapau canyon close-ups read far darker and bluer than the viewer's environment-0 lighting, before and after the sph_t0 port, which again points at the candidate TED environment lights; the port itself only warms the BatchMesh props (blue cast gone) and whitens the MeshAlphaGloss gloss veins; the BatchMesh per-vertex distance fade (`DISTANCE_FADE_VALS`) is not reproduced; retail close-ups sit lower and closer than the viewer's `--eawr-map-zoom` poses; the default Utapau tactical capture frames no placement and fails its projection check on the base build too; the other MeshAlphaGloss users (sandspeeder window, Gargantuan, Gungan building, prison, guard tower) wait for EAWR-43 part B references. (re-check with the EAWR-262 retail light heading.)
- Unlit space hulls (EAWR-307): without `--eawr-lighting` the hulls keep the P0 legacy material constants, and the Star Destroyer of the Coruscant hardpoint-state captures still saturates to a white silhouette with bloom off (RTX 4070 Laptop, 2026-09-27), so bloom was not what over-brightened those captures; the suite checks states and counts, not pixels. A lit run (`--eawr-lighting sh`) or a fix of the unlit constants would make them readable.
- Shadow filter (EAWR-291): the space sun still runs with blur 0, so its configured Soft Medium/High filter and depth bias 2 do not apply (one hard tap per pixel); land sets blur 1 and bias 0.05. The owner's grey band on the research-station arm tops (RX 7900 XTX, live camera) did not reproduce in fixed captures on the RTX 4070 Laptop or the GTX 970.
