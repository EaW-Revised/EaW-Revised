# Portable CPU asset formats

P0-06 defines CPU-only readers for ALO models, ALA animations, DDS textures and TGA
textures. Runtime acquisition is exclusively through `eawr::vfs::Vfs`; the byte-span
overloads exist for tests, tools and callers that already obtained bytes from that VFS.
Every returned object carries the effective/raw record's logical path, layer, origin,
source ID and stored size. A mismatch between the supplied bytes and nonzero provenance
size is an error.

## Public contract and limits

`include/eawr/assets/assets.hpp` is the portable interface. The readers have no D3D,
renderer, filesystem or simulation dependency. Asset IEEE-754 values are preserved as
asset floats; converting presentation transforms to a simulation type is forbidden.

The common chunk header is two little-endian 32-bit words. The high bit of the size word
marks a group and the remaining 31 bits bound its payload. Mini-chunks have an 8-bit type
and 8-bit payload size. A reader checks every enclosing bound, limits files to 512 MiB,
limits a collection to 16,777,216 entries and limits nesting to 256. Counts are checked
before multiplication/allocation. Unknown essential chunks fail with
`EAWR-ASSET-0007`; no parser seeks past a bounded unknown or reports a partial object as
success. Diagnostics use stable `EAWR-ASSET-0001` through `0010` codes and put the
zero-based byte offset in the documented diagnostic column minus one.

## ALO model

The reader requires the observed top-level sequence: skeleton group `0x200`, zero or
more mesh (`0x400`) or light (`0x1300`) groups, and connections `0x600` last. It retains:

- bone names, signed parents, visibility, billboard mode and all twelve stored transform
  floats; parent indices and the complete graph are validated for range and cycles;
- mesh bounds/flags and every submesh's shader name, typed ordered material parameters,
  vertex-format name, vertex attributes, triangle indices and up-to-24-entry skin map;
- object-to-bone bindings in original interleaved mesh/light object order, lights,
  proxies and dazzles.

Master vertices are 144 bytes and legacy vertices are 128 bytes. Every index must name a
vertex. A nonzero bone weight must name an entry in the submesh skin palette, and every
palette entry must name a model bone. Collision-tree `0x1200` is the sole presently
bounded/skipped optional geometry block; all other unrecognized geometry is essential.

A submesh with a skin map (the `RSkin*` effects; no other effect has one in the EaW and
FoC corpus) stores bind-model-space vertices. A mesh without one is rigid: its vertices
and header bounds are in the space of the bone it is connected to, and the pinned MIT
viewer draws it with that bone's absolute transform.

The connection chunk supplies the rigid mesh bone; the extra mesh-info word
does not select another vertex space in the pinned viewer. A corpus check
found 6,141 rigid submeshes in EaW, 8,621 in FoC and 31,466 in the Remake;
4,770, 6,760 and 26,086 of them respectively have non-identity bind transforms.
Neither parentage nor billboard mode separates skydomes from ordinary rigid
meshes. As a geometric cross-check, the `EV_StarDestroyer` hull's bone-local
vertex bounds plus its bind translation line up with the root-bone shadow and
collision bounds. The `W_CantinaTat` placement near Theed likewise forms an
arched ruined building after its bone transforms; the untransformed version
resembles a straight log because its component meshes overlap.

Transforms are exposed exactly as twelve values in file order. This is the legacy
right-handed, Z-up, vector-left `float4x3` convention: three consecutive float4 columns
place translation in the fourth component of each column (positions 3, 7 and 11), matching
the accepted shader contract's dot-product rule. The pinned MIT viewer corroborates
right-handed view/projection construction and a `(0,0,1)` camera up vector. The CPU loader
does not transpose or change handedness. Presentation adapters use the common scene's
explicit proper rotation `(x,y,z) -> (x,z,-y)`.

### Static placed-building state

FoC static map placements use the effective object's `Land_Model_Name`; sampled Naboo
definitions include `Bunker_Pad -> NB_Bunker.alo` and
`Team_01_Base_Shield_Small -> NB_BASESHIELDGEN_small.ALO`. Those ALOs contain visible
`_ALT0` through `_ALT3` mesh variants, and the shield model also contains visible `girder*`
meshes. Its collision meshes are explicitly hidden by the ALO visibility flag. The sampled
effective XML definitions do not select a separate damaged model or `Death_Clone` for the
static placement.

The static scene has no build-progress or health state. For a model with an `_ALT0` mesh,
the scene therefore selects `_ALT0` as the intact, completed placement state, honors the
ALO visibility flag, keeps unmarked shared meshes, and hides `girder*` construction meshes.
Models without `_ALT0` keep their authored visible meshes because their alternate-state
meaning is not established. This is the policy used for static TED placements; the naming
evidence does not define runtime building damage transitions. A synthetic engine-free
contract covers all four ALT stages, girders, shared meshes, hidden ALO meshes, and the
no-ALT0 fallback.

### Uncaptured reinforcement points

FoC Naboo's three `Reinforcement_Point` TED records resolve through
`data/xml/markers.xml` (`CAPTURE_POINT` behavior) to `W_Pedistal_Faction.alo`.
The other FoC reinforcement-capacity variants use the same ALO layout. Their
Empire, Rebel and Underworld emblem meshes are marked visible at the mesh level,
but their connected bones switch first-frame visibility across the corresponding
`_idle_00` through `_idle_03.ala` clips. The pedestal and `light_neutral` bones
stay visible in every clip. An uncaptured placement therefore draws the invariant
meshes and hides the clip-varying bones, while still respecting each ALO bone's
bind visibility. TED's editor player index is not the runtime capture owner.

### Hardpoint state art

An object's `HardPoints` entries name `HardPoint` objects whose XML carries the
art for each hardpoint state (EAWR-136). `Model_To_Attach` is a separate ALO whose
root is placed at the bind frame of the owner's `Attachment_Bone`. All 618 FoC
attachments (121 objects with `HardPoints`, including every capital ship and
station) are authored about their own root. This also holds when the attached
skeleton repeats owner bone names, as the Star Destroyer's and the stations'
do. At the bone frame the visible meshes land within 97 units of the bone;
without it they sit at the hull origin, the sockets stay empty and the owner's
`Girders01` shows through.

`Damage_Decal` names an owner mesh, and `Damage_Particles` names an owner bone.
The particle proxies on that bone and on bones below it are damage emitters,
for example `p_hp_imperial_damage` under `HP_F-L_EmitDamage`. Decal meshes and
emitters are authored visible (1,477 emitters and 824 decal meshes in FoC).
Capital ships have no idle clips that hide them. A spawned object therefore
has every hardpoint intact:

| State | `Model_To_Attach` | `Damage_Decal` | `Damage_Particles` emitters |
|---|---|---|---|
| intact | drawn | hidden | hidden |
| damaged | drawn | hidden | hidden |
| destroyed | hidden | drawn | shown |

The retail FoC Coruscant capture shows a starting station without smoke, which
agrees with the intact row. The FoC debug-build study of the hardpoint
health-changed handler confirms that art stays unchanged while health is above
zero. At destruction it removes the attached model, shows the decal, and
unhides the emitters below `Damage_Particles`. EAWR-72 decides when a state changes;
`Death_Breakoff_Prop` and `Death_Explosion_Particles` are one-shot events and
are not state art. Several `HardPoints` entries may name
the same art, and the first entry owns it. `scene::hardpoint_art`,
`scene::hardpoint_owner_art` and `scene::hidden_hardpoint_proxies` implement
the table. The space population and its attached-effect plan apply it.

The same destruction handler hides sub-objects below `Engine_Particles` when
`Engine_Death_Hide_Engine_Particles` is true on that hardpoint. It defaults to
false; the FoC XML inventory contains no use of the flag, but mods may set it.
The viewer gates engine meshes and proxies with the destroyed state.

The FoC debug build confirms the intact row for the emitters (EAWR-284). When a
game object builds its hardpoints, it looks up each hardpoint's
`Damage_Particles` bone. Every model sub-object below that bone is then hidden
by code, whatever its authored visibility, so a freshly placed object shows
none of these emitters. An object without `HardPoints` never runs this step,
and its emitters keep their authored visibility. Hull-health damage is a
separate route: it switches the model's alternate index through
`Land_Damage_Thresholds` and is not what hides these emitters. All ten `p_RB_Station_Damage` proxies of `rb_station_01.alo`
(`Rebel_Star_Base_1`) sit below the `HP01_*_EmitDamage` bones of its
hardpoints. The intact station therefore runs only its blink light. The
un-hiding happens when the hardpoint is destroyed, as confirmed by the
debug-build health-changed handler.

A catalog marker (role `marker`, such as `Team_00_Space_Station` on
`eb_station_01.alo`) is editor-only in retail and draws nothing. It attaches no
proxies either, not even blink lights. Otherwise its model's damage and blink
proxies would float where the skirmish start station stands.

## ALA animation

An ALA is one `0x1000` group containing information `0x1001`, declared bone tracks
`0x1002`, and version-2 shared payloads. The object retains stored frame count, playable
frame count (`stored - 1`, because the terminal sample duplicates the loop start), FPS,
bone name/index and decoded translation/scale/quaternion/visibility samples. It does not
invent interpolation: presentation consumers interpolate translation/scale linearly and
rotation spherically when required by their animation policy.

Version 1 per-track packed vectors and quaternions are decoded using their recorded
offset/scale metadata. Version 2 translation and rotation shared-block indices must have
the documented 3-word/4-word alignment and remain in range. The permitted MIT reference
declares a version-2 scale block size but provides no corresponding payload-chunk
semantics; a nonzero/referenced version-2 scale block therefore fails as unsupported
essential data rather than returning zero scales. Visibility uses the individual bit
`(byte & (1 << bit))`, with exact bitset length validation.

ALA loading is intentionally model-independent. Bone-name/index agreement with a chosen
model is a binding-stage validation, not a reason to hide animation provenance.

## Presentation animation and attachments

`eawr::presentation::animation::Player` is the presentation-only binding stage. It
rejects an ALA track unless both its name and index identify the selected ALO bone, and
rejects malformed duration, sample count, non-finite sample and request values. A missing
track keeps that bone's ALO bind-local transform; tracks sample translation and scale with
their declared step/linear policy, rotation with declared step/linear/spherical policy,
and visibility as a step value. Looping uses `playable_frame_count / fps`, so the stored
duplicate terminal sample closes the interpolation interval without adding time. The
optional blend-to-bind factor is a presentation fade to the decomposed bind pose and does
not generate simulation state or events.

ALO bind transforms with a collapsed scale axis use the affine Moore-Penrose inverse for
orthogonal TRS: non-zero axes invert normally and collapsed axes remain collapsed. This
keeps real zero-scale marker bones finite without replacing their bind transform by identity.

The player converts the retained vector-left ALO `float4x3` transform into a column-vector,
column-major 4x4 only for presentation math. Hierarchy evaluation and the skin palette
remain in the native asset basis: `model = parent * local`, and `skin = animated_model *
inverse(bind_model)`. The Godot upload moves rigid vertices into bind model space with
their bone's bind transform (`Player::rigid_vertex_to_bind`), so one palette places both
routes. This applies to skydomes too: their rigid mesh vertices use the attached
bone's space. The land skydome's earlier map occlusion came from its depth, not
its vertex space; the map renderer projects it at the far clip depth so terrain
and placed objects remain in front. Space sky composition already bakes each
rigid bind chain into its surface model and clears its mesh bone. The sole
asset-to-render basis conversion is the proper rotation
`(x,y,z) -> (x,z,-y)`, applied to transforms as `C * asset * inverse(C)` when returning a
model attachment or uploading a Godot bone palette. A world attachment prepends a caller-supplied transform already
in render basis; it never applies this conversion again. Named attachment lookup returns a
value-owned matrix and a missing name is `EAWR-ANIMATION-0003`; references to a `Pose` bone
remain valid only until that `Pose` is moved, destroyed or reassigned.
An intermediate `blend_to_bind` (strictly between 0 and 1) is supported only when every tracked
bone has a proper TRS bind (no mirror, shear or collapsed axis) and a positive animated scale;
anything else fails with `EAWR-ANIMATION-0005`. The 0 and 1 endpoints are unrestricted.

### Idle clip playback

The retail game starts a placed object's first idle clip (`Idle_Anim_00`, the
corpus `<model>_idle_00.ala`) when the object's model is created, on land and in
space alike. Only a space `DUMMY_STARSHIP` is exempt from the random start; land
has no exemption. The clip plays at the object type's `Idle_Anim_00_Rate_Mod` (default 1.0)
from a frame drawn uniformly from the clip's frames, and it loops when
`Loop_Idle_Anim_00` is set (default no). The IDLE behaviour restarts a
finished idle clip from frame 0 at rate 1, so for an IDLE object a
non-looping clip in effect loops too; an object without that behaviour holds
the last frame. An object has IDLE when its `Behavior` list names it, or the
list for the map's mode does: `LandBehavior` on land, `SpaceBehavior` in space
(`scene/idle_tags.hpp`). The
FoC space props (`Asteroid Field *`, `Space_Junk_*` in `spaceprops.xml`) carry
no translation tracks. Their tumble comes entirely from these clips: one turn
of each asteroid bone per 60 s loop (300 frames at 5 fps) and roughly one
turn of each junk bone per 19.7 s loop (590 frames at 30 fps). Placements start
at different frames, so neighbouring fields do not turn in step. On FoC Naboo
the animated land placements are the reinforcement points
(`w_pedistal_faction_idle_00`, 6 s, no loop or IDLE), the mineral pads
(`w_pedistal_idle_00`, 1 s, IDLE), and the Team_00/01 comms arrays
(`rb_comcenter_idle_00`, a 120 s dish turn) and power generators. Only the dish
visibly moves.

`presentation/animation/idle_playback.hpp` implements this rule. The random
start frame is replaced by a hash of the placement's map path and TED record,
so every capture repeats. `Idle_Anim_00_Rate_Mod` is parsed as the exact
decimal fraction it spells. The clip position is kept in whole
`1/(ticks per second * denominator)` frames, so a looping clip wraps on its
exact frame, like `Player::sample_tick`. Retail's later random idle variants
(`Idle_Anim_01` and up, chosen by the IDLE behaviour) are not modelled.

## DDS, TGA and BMP

DDS accepts the 124-byte legacy header and these portable layouts: BC1/DXT1, BC2/DXT3,
BC3/DXT5, 24-bit BGR, 32-bit RGBA/BGRA, L8 and A8. A DX10 extension is accepted for a
single 2D BC1/2/3/4/5/7 resource. Compressed blocks remain compressed CPU payloads;
uncompressed bytes retain their declared channel order. Every mip has dimensions, row
pitch and exact bytes. Some corpus writers under-report the final 1x1 level; it is
accepted only when the exact remaining byte count is one final format-sized mip.
Cubemaps, volume textures and unrecognized masks/resource shapes are explicit failures
because the current `Texture` API cannot represent them.

TGA accepts grayscale or true-colour types 2/3 and their RLE forms 10/11 at 8, 24 or 32
bits as applicable. RLE packet expansion is bounded by the declared pixel count. BGR(A)
is converted to RGBA8, missing alpha becomes 255, grayscale remains L8, and the original
top-left/bottom-left origin is retained. The standardized 26-byte TGA 2.0 footer is the
only accepted trailing metadata. Rows are not silently flipped.

BMP accepts the 40-byte BITMAPINFOHEADER with one plane, 32-bit uncompressed BI_RGB
pixels and no palette. Mod atlas pages preserve all four BGRA bytes, including alpha;
positive heights retain bottom-left origin, negative heights top-left. Unsupported DIB
headers/compression, invalid dimensions, offsets, truncated payloads and inconsistent
file/image sizes fail with named asset diagnostics. BMP and TGA have a texture-only
67,108,864-pixel budget (8192 squared); other asset element limits are unchanged.

The reader identifies BMP/DDS/TGA by bytes rather than trusting the suffix. This admits real
BMP bytes with a `.tga` name (RaW/FotR command-bar pages) and
TGA bytes with a `.dds` name but rejects PNG bytes and metadata sidecars carrying texture
suffixes. Standard TGA 2.0 extension/developer areas are bounded through the footer
offsets. Two encountered exporter files omit the footer's terminal NUL; the otherwise
well-bounded 25-byte variant is accepted with a notice rather than silently normalized.

Texture colour space is deliberately not guessed from filenames. The common scene marks
the material use; a renderer decides whether a binding is colour or linear data.

## TED maps

TED starts with a special little-endian root (`u32 id`, `u32 size`) whose payload is a
bounded stream of 8-bit mini-records. The remainder is ordinary Alamo chunk framing;
the high bit of a chunk size marks a recursively bounded group. The loader requires root
ID zero, format version `0x0201`, and kind 1 (land) or 2 (space), retains every root
field and chunk tree, and rejects all truncated, over-bound, or unsupported-version
framing before semantic decoding. A duplicated required root mini (`0x00` or `0x01`) and
a malformed UTF-16 root string (`0x08`, `0x09`, `0x0A`: odd length, missing terminator,
interior NUL or unpaired surrogate) are also framing failures. A VFS record larger than
512 MiB is refused from its `stat` size, before `open` reads it.

**Failure model.** Only framing is fatal. Once framing succeeds the load succeeds: a
semantic view that cannot be decoded is left empty, the raw chunks stay in `Map::chunks`,
and a typed `MapNotice` (`Map::issues`, mirrored in `Map::notices`) names the
`MapIssue`, chunk path, byte offset and size, and where relevant the record ordinal and
mini id. `semantic_complete` is true only when every required view for the declared kind
decoded and the body agrees with the kind. Unknown root minis, unknown chunks, an absent
`1/256/8` or `1/259`, and the `19` preview block are retained and named once per id or
path.

**Kind and structure.** Kind 1 predicts `1/257`, `1/266` and `1/267`; kind 2 predicts
none of them. A disagreement either way is a `kind_structure_mismatch` notice per group.
A space map never gains a terrain view, even when it carries land groups.

**Root views.** Root `0x09` is exposed as `context_name` (validated UTF-16LE decoded to
UTF-8, empty allowed; not an asset path). The newer-header binary32 pair `0x10`/`0x11`
is exposed as `DeclaredExtents` with its field ids kept, because which of the two is the
width is not pinned. Old EaW and FoC headers without these minis stay valid and nothing
is invented for them.

Land terrain child `1/257/5` is retained and decoded as row-major
`i16 height, u8 material_slot, u8 vertex_intensity`; source positions use 20-unit XY
spacing and `height * 25/512` on source Z. The view is rejected (notice, no partial
terrain) when the dimensions are zero or overflow, the header cell count disagrees, the
plane is not exactly `width * height * 4` bytes, or any slot is outside the declared
material array, including an empty array. A legacy `1/257/1` plane has no vertex
intensity; it produces a `terrain_legacy_plane` notice and no terrain view, and no
intensity byte is invented. `Terrain::header_fields` keeps every `1/257/0` mini.
Passability (`1/266`) is exposed only after its size is validated against a decoded grid.

Water is kept apart from the grid header: `WaterRecord`s hold the non-grid `1/257/0` minis
and every mini-stream leaf under `1/257/9` (wave records nest one group deep), each with
its chunk path. Water minis `0x1D`/`0x1E` are `Map::water_textures` references with their
field ids, and they join the texture denominator whether or not the terrain view decoded.
For rendering, terrain descriptor `1/257/2/3` minis `0x07`/`0x08`/`0x09`/`0x0A`
carry float tile size, rotation, tilt and RGB tint; each grid sample's material
slot selects a descriptor and adjacent samples blend bilinearly. Retail turns a
descriptor into the diffuse pass's TexU/TexV as rows 0 and 1 of
`Scale(1/tile) * Rotate_X(tilt) * Rotate_Z(rotation)` (row-vector order):
TexU = (cos r, -sin r, 0)/tile, TexV = (sin r cos t, cos r cos t, -sin t)/tile,
each dotted with the source position, for any sign of tilt. Header minis
`0x15`/`0x16`/`0x1A`/`0x20` give water height, family (0 none, 1 water,
2 lava, 3 ice), RGB tint and alpha. River groups `1/257/9/256` contain
257 parameters (`0x03` flow), 258 control points (`0x07` vectors), 259 a
bare texture-name string, and 260 widths (`0x08` floats, one per point).
`1/259` vector pairs are `SourceVolume`s in mini order, with both field ids and values
exactly as stored; volume id meanings are not pinned. Environment fields, patch data and
unknown fields remain lossless raw records unless their listed contract is proven.

`asset_validate --inspect-map` adds `source_bounds_ledger` schema version 1 for one map.
Its `source_coordinates` value is `unchanged_ted`; `interpretation.kind` is
`structural_source_bounds`, with no selected camera or playable boundary and no inferred
volume meaning. `declared_extents` is either null or the finite `0x10`/`0x11` pair with
both values and original mini header offsets. Ordered `volumes` carry zero-based
`pair_ordinal`, both field ids and mini offsets, and unconverted minimum/maximum XYZ arrays.
Only adjacent vector minis with consecutive ids, finite components and componentwise
minimum <= maximum form a pair. `volume_fields` lists every retained `1/259` mini in
source order with id, header offset, payload length and pair participation, without
payload bytes. Relevant existing bounds notices retain their issue codes, paths, offsets
and sizes. The ledger does not assign width/height, infer volume roles, or alter bulk
inventory reports.

`asset_validate --inspect-map` also adds `environment_candidate_ledger` schema version 1,
after `source_bounds_ledger`. It has `mapping_status: "unconfirmed"` and
`selected_environment: null`. `environments` has one entry for each decoded `1/256/4/6`
record, in record order. Each entry has the original `environment_ordinal`, so a malformed
sibling leaves a gap. Each entry also has exactly fourteen `fields` rows, for candidate mini
IDs `0x00`–`0x0d` in ID order. The rows come from the lighting module's
`diagnose_candidate_environment` applied to the retained `EnvironmentDescriptor::fields`.
Each row has:

- `field_id`
- `status`: `decoded`, `missing`, `wrong_size` or `nonfinite`
- `expected_size`, which is 12 or 4
- `observed_size`
- `occurrence_count`
- `selected_field_ordinal`: the index in the record's retained mini list
- `selected_byte_offset`: the original TED mini header offset

The last two are `null` when the field is missing. When a mini ID repeats, the first
occurrence is the one judged, even when it is malformed and a later one is valid. Later
occurrences are only counted. This rule differs from the environment reference ledger's
last-valid rule. `decoded` means only that the field has the expected size and finite
floats. It does not confirm the mapping, select an environment or say which lighting takes
effect. The ledger does not print float values. It depends only on the TED bytes, not on
the XML catalog, and it is not added to the bulk report.

Persisted-object candidates are read by direct children along
`1/258/1/1100/1113/1200`, using a standard CRC-32 of an uppercase XML logical type name,
resolved as a multimap against active effective catalog winners. Collisions and misses
are diagnostics, never first-wins. Every `1100` is one placement row with
`record_ordinal` equal to its index among the `1100` siblings; a `1100` without exactly
one `1113/1200` payload stays an unresolved row with a `placement_record_shape` notice,
and a `1200` under an optional group is not a second placement. Source position (mini 4)
and Euler `(roll X, pitch Y, yaw Z)` (mini 5) are the authored transform; cached minis 18
and 21 stay raw. Only yaw-only orientation is presentation ready; nonzero roll/pitch
remains `unsupported_three_axis_order` until a clean composition-order contract is
available, a missing or malformed mini 5 is `absent`, and a NaN or infinite component is
`nonfinite`.

Placement mini 2 is the owning player index (signed 32-bit). On the reference and Alderaan maps it
follows the catalog's faction load order (0 Rebel, 1 Empire, 2 Pirates, 3 Neutral, 7 Hostile); the
static scene takes that faction's `<Color>` as the placement's team colour (`scene::team_colour_statuses`).
A skirmish lobby recolours player-owned objects; that is not modelled.

### TED coordinate behaviour

The asset boundary performs no coordinate conversion. Every position, dimension and
angle below is reported in the source basis exactly as authored, so a presentation owner
converts once, visibly, at its own boundary rather than inheriting a silent rotation.

- **Basis.** TED source space is right-handed with **X right, Y forward, Z up**. `SourceVec3`
  is that basis; nothing in `eawr::assets` changes its handedness, negates an axis or
  swaps Y and Z. A Y-up consumer (the Godot runtime among them) applies its own documented axis
  map; it is not applied here.
- **Cell spacing.** Terrain samples are row-major over `width * height` cells with a
  **20-unit** spacing on both X and Y (`Terrain::cell_spacing`). Sample *(column, row)*
  therefore sits at source `(column * 20, row * 20)`. The spacing is a constant of the
  format, not a value read from the file.
- **Height scale.** The stored `i16 height_sample` is a raw sample, not a world height.
  Source Z is `height_sample * 25/512` (`Terrain::height_scale`). The scale is applied by
  the consumer; `samples` keeps the signed source value so nothing is lost to rounding.
- **Rotation.** Placement mini 5 is three Euler angles in **degrees**, ordered
  `(roll X, pitch Y, yaw Z)`. A record whose roll and pitch are both exactly zero is
  marked `OrientationStatus::yaw_only` and **is** usable: a single rotation about source Z
  by `orientation_degrees.z` is unambiguous regardless of composition order. The asset
  boundary reports the angles only; the static scene's `scene::placement_transform` adds
  FoC's fixed +90 degree model turn (R-ROT-01, `behaviour/p1-effective-environment.md`),
  and the simulation takes the yaw unchanged as the heading (R-ROT-04).
- **Absent or nonfinite orientation.** A record with no valid mini 5 is
  `OrientationStatus::absent` and one with a NaN or infinite component is
  `OrientationStatus::nonfinite`, each with a notice. Neither is ever read as an identity
  or yaw-only rotation: the static-scene builder reports `orientation_absent` and
  `transform_nonfinite` respectively and does not draw the placement.
- **Three-axis quarantine.** Any record with a nonzero roll or pitch is marked
  `OrientationStatus::unsupported_three_axis_order` and carries a notice. The composition
  order of the three angles is not established, and a guessed order silently mis-orients
  the placement rather than failing, so no quaternion is derived for these records. The
  angles are still retained verbatim in `orientation_degrees`; the quarantine is about
  interpretation, not retention. Lifting it requires a clean, approved composition-order
  fact, at which point the status becomes a decode and not a guess.

### TED reference resolution

Resolution is reported, not enforced: an unresolved reference stays a counted row in its
denominator and is never dropped to improve a rate.

- A placement's `type_crc` resolves against the profile's **active XML catalog winners**
  as a multimap. The four outcomes — no valid CRC mini, no matching winner, several
  matching winners, exactly one winner — are counted separately and always sum to the
  placement count.
- A uniquely typed placement's **model chain** is read from the resolved effective object:
  `Land_Model_Name` on a land map, `Space_Model_Name` on a space map, otherwise
  `Model_Name`. An object that declares no model tag anywhere in its derive chain is
  counted as undeclared, never as unrenderable.
- File references are probed under `data/art/models/` and `data/art/textures/`. Authored
  names carry the source-art suffix (`.tga`) where the shipped asset often uses another
  (`.dds`), so the literal name and the name's stem with each known suffix are both
  tried. An unrecognised suffix is never truncated.
- Water surface textures (`1/257/0` minis `0x1D`/`0x1E`) are texture references in the
  same denominator as terrain-material and cloud textures.
- Environment **skydome** fields name an XML skydome object by id, not an art file. They
  resolve against the catalog first; only then is the resolved object's own model chain
  probed, which is reported as a separate count.

## Validation report

`asset_validate --report <json>` writes schema version 2. Its sorted `records` array
contains one row for every selected ALO, ALA, DDS, TGA and TED occurrence: logical path,
format, loaded/failed outcome, SHA-256 of the exact VFS bytes, layer/origin/source ID and
stored size, notices and a fixed count object. Model rows count meshes, skeleton bones,
submesh material bindings, vertices and indices; animation rows count the decoded
animation, bone tracks and track samples; texture rows count mip levels; TED rows count
maps, terrain samples, persisted-object records, and semantic maps, plus the separate
resolution fields `placements_crc_*` (four buckets summing to `placements`),
`placements_model_*` (four buckets summing to `placements`), `texture_references` /
`textures_resolved` / `texture_slots_untextured`, and `sky_references` /
`skies_resolved` / `skies_model_renderable`. Inapplicable or unavailable counts are
explicit zeroes rather than absent fields.

A `map_resolution` block reports the derived rates — placement type resolution, placement
model renderability, terrain and cloud textures, environment skies, environment sky
models — each as its exact numerator, denominator, quotient and a `meets_99_percent`
verdict. A zero denominator reports a `null` rate and a false verdict rather than an
invented 100%. A `map_unresolved` block then names every distinct unresolved reference
with the number of maps it appears in: unresolved placement type CRCs (a missed CRC has
no recoverable name, so the hash is the identity and nothing is guessed back into one),
model names, texture names and skydome object ids. It is a ledger of names and never
feeds an aggregate. `asset_validate --inspect-map <logical-path>` prints the same
accounting, and the same four unresolved lists, for a single map.

The top-level per-format and total aggregates are sums of those rows, never independent
counters. Every parser failure retains the content hash and adds stable diagnostic code,
cause, affected feature, byte offset and a concrete follow-up. A VFS acquisition failure
cannot truthfully have a content digest, so the field is present as JSON `null`; the
accepted corpus has no such failures. The older `--summary/--failures` pair remains for
automation compatibility and is derived from the identical in-memory rows.

## Evidence and clean-room boundary

Chunk IDs and CPU record layouts were checked against the pinned MIT `alo-viewer`
revision `9bb0053919cc5df8377610d4f91b11d956d6c2f4`. The implementation was newly written
against that public contract and original fixtures; no GlyphX source, decompilation,
proprietary effect body, D3DX decoder or private research tool was used. Corpus reports
record every failure instead of treating reference-reader rejection as permission to
skip a file.

## Mega-texture directory (MTD)

MTD is little-endian: u32 entry count, then 81-byte entries containing a
NUL-terminated, zero-padded ASCII name[64], u32 x/y/width/height and u8 has_alpha
(0 or 1). Rectangles use top-left page coordinates. There is no authored flip
field; derived flip_x and flip_y remain false. Atlas binding requires every
rectangle to fit. Automatic sibling lookup accepts exactly one DDS or TGA page,
rejecting neither/both; an explicit page path resolves ambiguity.

## Animation association and scene inventories

Structural compatibility alone cannot approve a clip/model association. Preserve
declared provenance and separate binding failure, model-match failure and selected
model parse failure. A compatible alternative remains a candidate until selected
with explicit provenance; inventory denominators include failures.

All-map scene inventories use effective VFS views. A resolved XML placement is
not necessarily drawable; report model/asset/material causes separately and retain
source identity and placement order. Missing catalog inputs do not become present
because a scene renders. Exact per-map rows remain in
plan/inventories/unresolved-placements.json and unresolved-placements-foc.json.

The 356 failures P1 EAWR-24 froze (`tests/presentation/animation/corpus_association_frozen.tsv`,
taken on the Remake view) are re-read on the FoC view under FoC's own clip naming rule
([unit animation](behaviour/unit-animation.md) UA-01) by `animation_corpus_association_foc`.
`corpus_association_foc.tsv` gives each row its disposition and cause: 256 are Remake mod files
absent from FoC, 20 bind to the model FoC loads them for, 53 are never reached by FoC's rule (no
`_NN` index, no type name, or no model or override of that base) and 27 still fail the strict
track binding. A CTest checks the file against the frozen manifest; regenerating it needs the
installed game.
