# Focumentation cross-check: presentation, assets, tools, mod loading

Source: the Focumentation community wiki (fandom, CC BY-SA), pages listed in
`out/research/focumentation/MANIFEST.json`. Everything below is paraphrased; wiki pages are cited by title.
Wiki text was treated as untrusted data. Repo docs compared: `docs/rendering.md`,
`docs/asset-formats.md`, `docs/vfs.md`, `docs/shaders.md`, `docs/graphics-features.md`,
`docs/xml-model.md`, `docs/behaviour/particle-*.md`, `docs/behaviour/unit-animation.md`,
`docs/behaviour/battle-presentation.md`, `docs/behaviour/walks/{particles,audio,mod-loading}.md`,
`docs/behaviour/mesh-additive-vertex-colour.md`, `plan/inventories/particle-plugins.json`,
`plan/inventories/xml-tags.json`, `include/eawr/presentation/animation/unit_clips.hpp`.

Milestones used: M2 = space skirmish, M3 = galactic conquest, later = land / settings / tooling.

## Summary

- 6 CONFLICTS (4 are semantic gaps between the Particle Editor's UI vocabulary and the
  file-level rules the repo documents; 1 is a registry-key scope difference; 1 is a lifetime
  heuristic).
- 18 NEW items, mostly author-facing particle semantics, four undocumented proxy prefixes and
  eight SFX event tags that the repo has no row for.
- 32 AGREES. The wiki's animation type list matches the repo's 119-entry clip table name for
  name (114 of 119 listed, including the misspelled `CONTAIMINATE_LOOP`); 54 of its 62 SFX tags
  are already tracked.
- The wiki's mod-loading content is thin: it never mentions `MODPATH` or `STEAMMOD`. Its only
  path claims concern how the community tools find the install (registry), not how the game
  does. The "Translation and Localisation" page is an empty stub.
- The pages are most useful as a **glossary from Particle Editor labels to file fields**, and as
  pointers to community tools whose sources could settle the conflicts below without engine work.

Side note (repo-internal, not from the wiki): `docs/rendering.md` describes skip-time pre-roll in
1/30 s steps and freeze as triggering when local time "reaches" the value, while
`docs/behaviour/walks/particles.md` PS-07/PS-08 say 0.1 s presimulation steps and a strict
"exceeds" for freeze. One of the two passages is stale; the walk (PS-47 era) reads as the newer one.

## CONFLICTS

| # | Wiki claim (page) | Repo position | Owner doc | Milestone | Next step | Verdict |
|---|---|---|---|---|---|---|
| C1 | "Minimum scale" randomises each particle between the minimum percentage and 100 % of full size (Particle Editor). | Size spread is symmetric around the keyed width: width = key x (1 + u), u uniform in [-v, v], read raw from the size-spread field (asteroid pebbles 0.08 to 1.92 of key). | `docs/rendering.md` (V1 particle size), particles walk | M2 | Find which file field the editor's "minimum scale" writes and how it converts (the GlyphX particle-editor source is public, see tools). If the UI stores 1 - min%, document the mapping; if not, recheck the raw reading with a retail capture of a single-emitter test effect. | both partly; rendering V1, PS-28; (legacy EAWR-2171) |
| C2 | Lifetime is a maximum in seconds plus a "minimum lifetime" percentage; each particle dies at a random time between min% x max and max (Particle Editor). | Post-load age bounds are [max(0, A - A x variation), A] (PS-47, PL-01). | particles walk, `particle-lifetime.md` | M2 | Probably the same rule if the editor stores variation = 1 - min%. Confirm the field mapping from the editor source and add one line to PL-01 naming the editor label. | wiki right; PS-47, particle-lifetime; (legacy EAWR-2171) |
| C3 | "Emit from mesh" spawns from all vertices of the linked mesh (Particle Editor). | Mesh emission has four modes: off, random vertex, random surface point, every vertex (`particle-mesh-emission.md`); engine glows use the surface mode (BP-42). | `particle-mesh-emission.md` | M2 | Wiki oversimplifies. Record which editor control selects the mode so modders' expectations map to the four modes; no behaviour change. | not checked |
| C4 | The topmost emitter in the list (a burst one) fixes the total in-game lifetime of the effect, so it should have the longest lifetime (Particle Editor, advanced tips). | Object life comes from `Particle_Lifetime_Frames` (PS-06); a system instance lives until no emitter instance and no particle remain (detach D-07). No emitter-order rule. | particles walk, `particle-system-detach.md` | M2 | Likely a modder heuristic colliding with `Particle_Lifetime_Frames` or detach timing. Build a two-emitter test effect where the lower emitter outlives the top one and capture retail; add a rule or a "not a rule" note. | ours right; PS-50, PL-01; (legacy EAWR-2171) |
| C5 | Setting the Index track to 2 shows "the second image" in the atlas (Particle Editor). | Cell = integer part of the UV track value, row-major on an integer-sqrt grid (PS-30, rendering V1 notes), which reads as zero-based. | `docs/rendering.md` | M2 | Possible off-by-one in the wiki's wording. Check one corpus effect whose UV key is 0 and see whether it draws the first cell; record the base explicitly. | not checked |
| C6 | Shader/water detail stuck at 0 is fixed by editing `Petroglyph\StarWars\Profiles\Profile0\Player` (Shader and water detail graphics problem). | FoC stores its profile under the `StarWars FOC` key; EaW keeps a separate copy (graphics-features section 1). | `docs/graphics-features.md` | later | The wiki fix targets the EaW key only. Add a sentence that the FoC key differs, so the documented values (shader 0-3, water 0-2) are applied to the right profile. | not checked |

## NEW (grouped by owner doc)

### `docs/behaviour/walks/particles.md` / `docs/rendering.md`

| # | Item (source page) | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N1 | Particle Editor field glossary: Basic tab (spawn delay, skip time, freeze time, generation type burst/continuous/weather, lifetime), Appearance tab (colour/bump texture, texture elements, minimum scale, random colour addition with grayscale option, tail, rotation options, face-camera, heat, no-depth-test, blend mode), Physics tab (initial position type, initial speed type, inward speed, parent speed inherit, wind, acceleration, gravity, inward and object-space acceleration, ground interaction), tracks (R, G, B, A, scale, index, rotation speed) with linear/smooth/step interpolation. | M2 (doc only) | Add a table mapping each label to the file field the repo already names (0x32 freeze, 0x33 skip, 0x10 texture elements, 0x12 size spread, 0x34 mesh mode, interpolation mode 2 = step). Fill the gaps from the editor source. | done (glossary; face-camera unverified); particles.md; (legacy EAWR-2171) |
| N2 | Ground interaction has four author choices: none, disappear, bounce (with a bounciness slider), stick (Particle Editor). Repo: the terrain killer is supported, TerrainBounce (31 occurrences) is unsupported for lack of a reference, no "stick" family is documented. | later (land) | Identify which plugin/field encodes "stick" and the bounciness value; queue TerrainBounce semantics with the land particle work. | wiki right (remake supports mode 1 only); PS-48; (legacy EAWR-2171) |
| N3 | Emitter list order is draw order: the last emitter draws closest to the viewer, the first furthest back (Particle Editor). Repo documents per-emitter depth sorting (PS-32) but no rule between emitters of one system. | M2 | Retail capture of two overlapping additive/alpha emitters swapped in order; add a rule to PS-32/PS-33. | wiki right; PS-49; (legacy EAWR-2171) |
| N4 | Particles with a scale key above about 5000 units are not drawn in game (Particle Editor). | later | Unverified upper bound; test with a synthetic effect before treating it as a rule. Check against capacity/culling (PS-39). | not checked |
| N5 | Fixed-random rotation: the first value should be 1, not 0; full randomness is 1 +/- 180 degrees; with face-camera, 0 degrees points along +Z (Particle Editor). | M2 (rotation sampler) | Verify against the repo's rotation sampler (ConstantRotation/KeyedRotationRate are supported); if 0 really disables randomness, record it. | not checked |
| N6 | Weather generation keeps particles in front of the camera and overrides spawn options (Particle Editor). Repo only notes weather is a separate path outside space scope (PS-41). | later (land) | Carry into the land/weather owner. | not checked |
| N7 | Editor colour channels use a 0-100 scale (100 = 255); alpha keys have no effect in additive blending (Particle Editor). | later (tooling) | Note in the glossary; check that additive particles ignore track alpha in the repo's blend adapter. | not checked |
| N8 | Editor-only bugs: one child emitter per file is safe; cut/paste can reorder child emitters on save; long previews exhaust memory (Particle Editor). These are tool limits, not engine rules. | later | Mention in the glossary so nobody infers a one-child engine limit; repo already supports trail and death children. | not checked |

### `docs/behaviour/battle-presentation.md` (proxy emitter types)

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N9 | The prefix list adds `buildup`, `charged`, `pas` and `prs` to the six the repo documents (`pe`, `pte`, `pptw`, `pgw`, `pem`, `pi`) (Recognized particle prefixes). Meanings are not given; `buildup`/`charged` may relate to charge-style abilities such as the Peacebringer blast (speculation). | M2 scan, M3 use | Scan the ALO corpus for proxies with these four prefixes, then trace which ability/state toggles them. Add rows next to BP-42/BP-43. | both partly; BP-43, BP-72..74; (legacy EAWR-2176); (legacy EAWR-2171) |

### `docs/behaviour/walks/audio.md`

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N10 | Eight SFX tags on the wiki have no row in the repo docs, tag-coverage statuses or `xml-tags.json` (so stock FoC data does not author them): `SFXEvent_Defend`, `SFXEvent_Undeploy`, `SFXEvent_Retreat_Cancel`, `SFXEvent_Retreat_Engines_Jump`, `SFXEvent_Shields_Go_Down`, `SFXEvent_Shields_Go_Back_Up`, `SFXEvent_Select_With_Non_Hero_Ability`, `SFXEvent_Move_With_Non_Hero_Ability` (Unit SFX event tags). | M2 (shields, retreat), M3 (rest) | Confirm in the debug build that the GameObject tag mapper accepts them; if so add "recognised, unauthored in stock" rows so mod data using them is not flagged unknown. Shields and retreat are space-relevant. | wiki right; SND-73..77; (legacy EAWR-2171) |

### `docs/behaviour/unit-animation.md`

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N11 | No new names; the wiki omits `IDLE`, `DEPLOY`, `LAND`, `DEMOLITION`, `SELF_DESTRUCT` and gives no "when it plays" information (Possible animation types). | none | Nothing to change; the repo is the better source. | not checked |

### Space movement / Lua API

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N12 | A community Lua script spawns scripted space units at random heights chosen from preset values (LayerZ Script). The repo says nothing in space changes height after creation (LZ-01 to LZ-04, WMV-06), so the script must reposition through Lua. | M3 / later | When the Lua API inventory is extended, check which position call such scripts depend on and that it accepts a Z component in space. | not checked |

### `docs/graphics-features.md`

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N13 | On hybrid-GPU machines players force the discrete NVIDIA adapter for the game executable (NVIDIA graphics cards usage); combined with the Intel FF presets (shader/water 0) this explains the stuck-detail complaint. | later (settings) | Note the symptom and cause in graphics-features; EAWR's own adapter choice should not reproduce the vendor-based downgrade. | not checked |

### `docs/rendering.md` (shadows) / `docs/asset-formats.md`

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N14 | The ALO exporter refuses non-manifold shadow meshes (How to import and modify an existing .alo model). Retail uses stencil volumes from those meshes; EAWR casts from visible meshes. | later | Add one line that authored shadow meshes are not consumed, and what a mod relying on a shadow-only mesh would look like. | not checked |
| N15 | Blender export pitfalls: unapplied object transforms and bones rotating about their midpoint rather than head (same page). | later (tooling) | Useful for asset-format diagnostics: if the loader sees a non-identity root transform or odd bone rest poses, point modders at these fixes. | not checked |

### `docs/behaviour/walks/mod-loading.md` / `docs/host-paths.md`

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N16 | Community tools (ALO Viewer, Particle Editor) locate the game through an `ExePath` value under a LucasArts key in HKLM, which Steam installs do not write; they then enumerate the `Mods` folder (Modding tools path problem). Map Editor sees a mod's particles when launched from that mod's folder (Particle Editor). | later (tooling) | No engine impact. If the scanner ever offers auto-discovery, document that this key is unreliable for Steam; keep explicit roots. | not checked |
| N17 | DataMiner (from the Universe at War tools) builds per-type tag tables from a mod's XML and stops on missing includes (How to use the Universe at War mod tools Dataminer). | later | Optional cross-check for xml-model tag tables on real mods. | not checked |

### Land / environment (later)

| # | Item | Milestone | Next step | Verdict |
|---|---|---|---|---|
| N18 | Walls/structures aligned to the defender via a community script (Ground battle - Walls and structures player alignment); Terrain Editor lighting presets named by land/space and colour scheme (Map Editor Lightmap Presets, download link is empty); a free map pack (Maps). | later | Park with land-combat and p1-effective-environment as candidate test inputs once licences are known. | not checked |

## Community tools and references named on the wiki

Licences: none of the pages state one unless noted. Not fetched.

| Tool | Where the wiki points | Use for EAWR | Licence |
|---|---|---|---|
| Alamo Particle Editor (newest build) | GitHub `GlyphXTools/particle-editor`; older build on Google Drive (Modding Tools) | Settle C1, C2, C5, N1 by reading which UI field writes which file field | not stated on wiki (GlyphX Tools' alo-viewer is MIT per repo notices; check this repo separately) |
| ALO Viewer | GitHub `gryttr/alo-viewer` releases | Repo already ports `AlamoEngine-Tools/alo-viewer` (MIT); check whether the gryttr fork adds fixes | not stated |
| Blender ALAMO plugin (Gaukler) | GitHub `Gaukler/Blender-ALAMO-Plugin` | Export conventions (bones, proxies, shadow mesh checks) for asset-format docs | not stated |
| Alamo tools for 3DS Max 8/9 | wiki page link | export conventions | not stated |
| MEG editor, Map Editor, MTD editor | modtools.petrolution.net | MEG/MTD cross-checks | not stated |
| UaW DataMiner | from the petrolution UaW mod tools archive | XML tag table cross-check (N17) | not stated |
| String Editor (.dat editor) | ModDB, Yuuzhan Vong at War | text database test inputs | not stated |
| .dat String Validator, Hardpoint Generator | ModDB, Embers of Rebellion | text and hardpoint validation ideas | not stated |
| EAW Editor (VS Code extension) | GitHub `AlamoEngine-Tools/pg-starwarsgame-lsp` | already cross-checked in repo (MIT per repo notices) | not stated on wiki |
| Scavenger (map previews) | GitLab `shrademn_company/projects/scavenger` | minimap/preview generation reference | not stated |
| LayerZ Script + tutorial | `gitlab.yuuzhanvongatwar.com/Kad_Venku/LayerZScript`, documentation.yuuzhanvongatwar.com | Lua API usage sample (N12) | not stated |
| Wall alignment script (MaxiM) | Google Drive | land reference (N18) | not stated |
| Lightmap presets (Valerie) | link empty | environment test inputs | not stated |
| Xanofar map packs | Discord links | map-loading corpus | not stated |
| MegaBalta Particles v2016 | wiki page | particle corpus beyond stock | not stated |
| Tutorials: Pegasus Chronicles particle video, swgbex Particle Editor tutorial, ModDB rigging tutorials (Imperial Dimension Neptunia; Enpremi, YVE) | external | background reading | not stated |
| Blender 3D-Print Toolbox, 7-Zip, ReShade | named as helpers | none | third-party |

## AGREES (compact)

| # | Claim (page) | Repo anchor |
|---|---|---|
| A1 | Burst count 0 repeats forever (Particle Editor) | PS-09 |
| A2 | Spawn delay is per emitter, before its first event (Particle Editor) | PS-09, PS-10 |
| A3 | Freeze time stops the emitter but keeps drawing its state (Particle Editor) | PS-08, rendering V1 notes |
| A4 | Skip time pre-simulates the emitter; the wiki calls it useless, but the corpus uses it for engine trails and asteroid fields | PS-07, rendering |
| A5 | Texture elements must be a perfect square and round down | PS-30 (integer sqrt grid) |
| A6 | Index track animates atlas cells; step interpolation holds a value | rendering V1 notes |
| A7 | Heat particles are a separate kind | PS-33, rendering heat |
| A8 | No-depth-test is an independent flag | PS-32, PS-33 |
| A9 | Blend modes: none, additive, transparent, depth variants (need a depth map in the bump slot), bump | PS-32, PS-33, blend selector 11 |
| A10 | "Link particles to instance" keeps particles in the emitter frame | BP-40 (Emitter vs World translater) |
| A11 | Parent speed inherit applies continuously while moving | PS-27 |
| A12 | Particles can be affected by wind | PS-26, WindAcceleration plugin |
| A13 | Gravity is separate from ordinary acceleration | PS-24 |
| A14 | Object-space acceleration uses the proxy's axes | BP-40 |
| A15 | Spawn shapes exact/box/cube/sphere/cylinder, optional surface-only | PS-11, PS-12 |
| A16 | Random colour addition with grayscale option | ColorVariance plugin (inventory) |
| A17 | Tail stretches particles along motion | PS-31 (kite stretch) |
| A18 | "Disappear" on ground contact | Terrain killer plugin (inventory) |
| A19 | `Particle_Lifetime_Frames` counts frames at about 30 per second | PS-06, 30 Hz presentation |
| A20 | Particle .alo under Data/Art/Models, textures under Data/Art/Textures; textures beside the .alo are not used in game | asset-formats probe paths, ML-41 |
| A21 | A same-named file in a mod replaces the stock one | ML-05, ML-17 |
| A22 | Prefixes `pe`, `pte`, `pptw`, `pgw`, `pem`, `pi` | BP-42, BP-43, WHE-52, IS-09 |
| A23 | Animation type names become part of the clip file name | UA-01, UA-02 |
| A24 | All 114 listed animation names match `unit_clips.hpp` exactly | `unit_clips.hpp` |
| A25 | 54 of 62 SFX tags already tracked (audio walk, tag coverage) | SND rows, `statuses.json` |
| A26 | Shader detail tops out at 3, water detail at 2 | graphics-features |
| A27 | Intel adapters get presets with shader/water detail 0 (explains the stuck-at-0 and NVIDIA pages) | graphics-features FF sets |
| A28 | Background nebulae from vertex colours through MeshAdditiveVColour; start all vertices black | AVC-03, AVC-04 (texture x vertex RGB, added); this also settles the wiki's open question: the base texture must be white, a black one adds nothing |
| A29 | Wrong smoothing shows as rotated specular/normal map | rendering tangent frame: authored tangents preserved, so broken exports look broken as in retail |
| A30 | Stock space height is fixed per type, hence the LayerZ script | LZ-01 to LZ-04 |
| A31 | Localisation goes through the game's text database files | ML-43, ML-44 |
| A32 | A missing XML include breaks loading in tools as in game | ML-19 |
