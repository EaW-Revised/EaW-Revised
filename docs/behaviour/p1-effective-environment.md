<a id="effective-ted-environment-lighting-fog-and-sky-inputs-p1-252627-wp-13"></a>

# Effective TED environment, lighting, fog and sky inputs (P1 lighting, loading and rendering, WP-13)

## Interface contract

- Inputs:
  - the map's ordered environment records. Each is a stream of 8-bit mini records
    (`field id`, `length`, payload) with little-endian binary32 floats and narrow strings;
  - the map's `1/256/8` record: an authored current-environment index and an authored saved index;
  - at battle start, a general-purpose random number stream shared with other game systems;
  - at runtime, script requests to change the environment, to toggle the cinematic environment and
    to enable or disable distance fog.
- Outputs, for the one **current** environment:
  - three directional lights (unit vector toward the light, colour, intensity), one specular colour,
    one ambient colour, and the derived effect parameters;
  - distance-fog enable, colour, start and end;
  - wind vector, cloud-layer texture, scale and velocity;
  - primary and secondary sky object names with an orientation pair each;
  - a weather category, a weather-scenario name, and a lightning scalar.
- Coordinates: the TED source basis of the TED map loader contract (right-handed, X right, Y forward, Z up).
  No axis conversion happens anywhere in this note. The Godot adapter's single conversion stays
  outside it.
- Cadence: the environment is decoded once, at map load. Its lighting and fog are **applied** when
  the map is activated and again after every selection change, cinematic toggle or fog toggle
  (R-SEL-08). Wind and the cloud layer were traced only on the random and weather-based selection
  paths (R-WX-01, R-WX-02). Whether script, cinematic or fog toggles update them is gate G-WX-02.
  Whether the environment is also applied every frame was not established (gate G-LTN-01).
- Retained state: the environment list, the current index, the saved index, the fog master flag and
  the lightning timers.

## Ordered behaviour rules

### A. Decoding one environment record

| Rule | Behaviour |
|---|---|
| R-DEC-01 | `1/256/4` holds zero or more leaf chunks `6` (one environment each, in authored order) and is **terminated** by an empty chunk `5`. Reading stops at the `5`. In every one of the 445 EaW and FoC effective maps the `5` is the last child. |
| R-DEC-02 | The minis of one record may come in any order. Each known id is read into that record's value. When an id repeats, **every occurrence is read and the last successful read wins**. No record in the EaW or FoC corpus repeats an id. |
| R-DEC-03 | A numeric read consumes exactly the expected number of bytes (4 per float or integer, 1 for the fog byte, 12 as three floats for a colour). The read succeeds only if all of those bytes lie inside the mini. A longer mini is read as its prefix. A shorter mini leaves the value unchanged. |
| R-DEC-04 | A failed read in any three-float field (ids `0x00`–`0x04`, `0x0f`, `0x15`, `0x17`) makes the **whole map load report failure**. A failed read in a scalar field is ignored and the prior value is kept. |
| R-DEC-05 | A string mini supplies all its bytes except the last. The last byte is the terminator. |
| R-DEC-06 | Ids not listed in the field table below are skipped. The corpus has id `0x16` (one byte) and id `0x18` (one float) in one record each. Both are ignored. |
| R-DEC-07 | Every field except the light fields `0x00`–`0x0d` starts each record at the default in the field table. The light fields have **no per-record default**. A record that omits one of them gets a value that is not defined by that record. A clean implementation must fail closed instead (see Failure behaviour). Every EaW and FoC record carries all fourteen light fields. |
| R-DEC-08 | Fog distances: start = the smaller of mini `0x11` and 10000; end = the larger of that start and mini `0x12`. So start never exceeds 10000 and end is never below start. |
| R-DEC-09 | The weather category (`0x13`) is clamped to be at least 0. |
| R-DEC-10 | Sky orientation minis `0x1d`–`0x20` are authored in **degrees**. They are converted to radians when loaded. |
| R-DEC-11 | When the primary sky name (`0x19`) is empty, the primary sky's scale and angles become (1, 0, 0). The same holds for the secondary sky (`0x1a`). Both resets happen after the numeric sky minis are read, so the numeric values are replaced. |

#### Field table

Defaults apply per record (R-DEC-07).

| Id | Shape | Default | Meaning and consumer |
| --- | --- | --- | --- |
| `0x00`, `0x01`, `0x02` | 3×float | none | Light colour of light 0 (sun), light 1 and light 2 (R-LIT-03) |
| `0x03` | 3×float | none | Specular colour of light 0 (R-LIT-04) |
| `0x04` | 3×float | none | Ambient colour (R-LIT-05) |
| `0x05`, `0x06`, `0x07` | float | none | Intensity of light 0, 1, 2 |
| `0x08`, `0x09`, `0x0a` | float | none | Heading of light 0, 1, 2, **radians** (R-LIT-01) |
| `0x0b`, `0x0c`, `0x0d` | float | none | Elevation of light 0, 1, 2, **radians** (R-LIT-01) |
| `0x0f` | 3×float | (1, 1, 1) | Distance-fog colour (R-FOG-01) |
| `0x10` | byte | 0 | Distance fog enabled when nonzero (R-FOG-01) |
| `0x11` | float | 1000 | Distance-fog start (R-DEC-08) |
| `0x12` | float | 10000 | Distance-fog end (R-DEC-08) |
| `0x13` | int32 | 1 | Weather category used by the weather-based selection (R-SEL-05) |
| `0x14` | string | template name | Environment name. The exact name `Cinematic` is special (R-SEL-03, R-SEL-07) |
| `0x15` | 3×float | (0.3, 0.3, 1.0) | The sky background colour: the environment loader passes it to the scene's sky-background setter (IS-15). Its draw was not followed. |
| `0x17` | 3×float | (0.5, 0.5, 0.5) | Copied into the scene together with the lighting: the environment loader passes it to the scene's shadow-colour setter (IS-15). Retail shadowed/lit pixel ratios on FoC Naboo follow it per channel, in stored values, so it is the shadow colour (Forward+ shadow tuning, [shadows](../rendering.md#shadows)). The viewer's shadow floor (per-channel shadow floor) |
| `0x19` | string | empty | Primary sky object name (R-SKY-01) |
| `0x1a` | string | empty | Secondary sky object name (R-SKY-01) |
| `0x1b`, `0x1c` | float | 1.0 | Stored with the primary and secondary sky. Consumer not followed |
| `0x1d`, `0x1e` | float, degrees | 0 | First orientation component of the primary and secondary sky (R-SKY-02) |
| `0x1f`, `0x20` | float, degrees | 0 | Third orientation component of the primary and secondary sky (R-SKY-02) |
| `0x21` | float | 0 | Lightning scalar; 0 disables lightning (R-LIT-08) |
| `0x23`, `0x24`, `0x28` | float | 1.0, 0.9, 0.25 | Bloom strength, cutoff and size, copied into the scene's bloom parameters when the environment is applied (stored-space bloom compositor, [bloom](../rendering.md#bloom)). FoC land records mostly carry 1.0, 0.9 and 1.0; space records 0.6 or 1.0, 0.9 and 0.25, or strength 0, which adds nothing |
| `0x29` | string | empty | Weather-scenario name. Consumer not followed |
| `0x2b` | float, degrees | 0 | Wind and cloud heading (R-WX-01) |
| `0x2c` | float | 2.0 | Wind speed (R-WX-01) |
| `0x2d` | float | 500 | Cloud-layer scale parameter (R-WX-02) |
| `0x2e` | float | 30 | Cloud-layer speed (R-WX-02) |
| `0x2f` | string | `W_Clouds00` | Cloud-layer texture name (R-WX-02) |

The weather category values co-occur with the weather-scenario names as follows (EV-C-01, EaW):
0 with empty or `DEFAULT_ALWAYS_CLEAR` names; 1 with `RAIN_*`; 2 with `BLIZZARD_*` or `SNOW_*`; 3 with
`SANDSTORM_*`; 4 with `ASH_*`. Only a few records break the pattern (for example, one `RAIN_CONSTANT` at
category 0). The category names *clear, rain, snow, sandstorm, ash* are therefore corpus
correlations. The engine itself only compares the integers.

### B. Selection: which environment is current

| Rule | Behaviour |
|---|---|
| R-SEL-01 | The map's `1/256/8` record carries an **authored current index** (mini `0x25`) and an **authored saved index** (mini `0x2a`). Loading the map sets the current and saved indices from them. In the corpus the saved index is −1 or absent in every map. The current index is 0 in most maps, but it is nonzero in 21 EaW maps and 26 FoC maps. It is 3 in the Alderaan land regression fixture `_land_planet_alderaan_02.ted` (environment `Noon_Rain`). |
| R-SEL-02 | Two tactical-mode set-up steps were followed; both **replace** the authored index with a **uniformly random** pick (R-SEL-03). One always does. The other does unless a game-mode flag is set or its load argument has a particular value; in that case it keeps the current (authored or restored) index. The flag and the argument were not identified. Restoring a saved battle is a hypothesis, not an observation (G-SEL-01). Neither step was tied to a specific game mode (G-SEL-03). |
| R-SEL-03 | **Random pick.** With one environment, index 0 is chosen and the random stream is not used. With n > 1, one integer is drawn uniformly from 0…n−1, inclusive. If the drawn environment's name is exactly `Cinematic` (case-sensitive), the next index is used instead, wrapping to 0 after the last. The result is clamped to 0…n−1. |
| R-SEL-04 | The random draw comes from a general-purpose game random stream shared with many other systems. The chosen environment therefore **cannot be predicted from the map data**. It depends on everything that consumed the stream earlier. Whether this stream is synchronised across multiplayer peers was not determined. A single-player skirmish reseeds it from the processor's performance counter at every battle start, so the same map draws independently from battle to battle (no-fog capture setup). |
| R-SEL-05 | **Weather-based pick** (a second, separately callable entry). Given a requested category c: if c > 4, the random pick of R-SEL-03 is made. Otherwise the candidates are the environments whose weather category (`0x13`) equals c. One candidate is taken directly. Several are chosen among uniformly from the same stream. With none, the random pick of R-SEL-03 is made. No caller of this entry was resolved, so the source of c (planet weather, story reward or skirmish option) is unknown. |
| R-SEL-06 | **Script change.** A script request with a number sets the current index to that number truncated to an integer and clamped to 0…n−1. The number is zero-based. The environment is then applied immediately. The shipped EaW, FoC and Remake Lua in the committed manifest contains **no** call site of this request (`Set_New_Environment`). |
| R-SEL-07 | **Cinematic environment.** Turning it on searches the list in order for an environment named exactly `Cinematic`. If one exists and is not already current, the current index is saved and that environment becomes current and is applied. If the name appears more than once, each later match is also switched to in turn. The last match ends up current, and the saved index then holds the previous match rather than the original environment. Turning it off restores the saved index (clamped), applies it, and clears the saved index to −1. Turning it off when nothing is saved does nothing. The shipped Lua calls this request (`Set_Cinematic_Environment`) at 6 EaW call sites. Two EaW maps (`_mp_space_kuat.ted`, `rm01_space_planet_kuat.ted`) contain a `Cinematic` environment. |
| R-SEL-08 | Every change of the current environment, and every fog toggle (R-FOG-03), immediately re-applies from the new current record: the lighting of R-LIT-01…08, the fog of R-FOG-01…05, and the `0x17` and `0x23`/`0x24`/`0x28` scene copies. There is **no blend** between the old and new environment. The one interpolation factor found (R-FOG-02) is never written during load. This rule does **not** cover wind and the cloud layer. They were traced only after the random and weather-based picks (R-WX-01, R-WX-02). Whether the script change, the cinematic toggle or the fog toggle updates them is gate G-WX-02, and a fog toggle must not be assumed to update them. |
| R-SEL-09 | A map object created without any environment record holds one built-in fallback environment. Its values differ from alo-viewer's default environment: ambient (0, 0, 0); light 1 and light 2 swapped in heading (120° and 210°); intensities 0.5 / 1 / 1. Every corpus map has at least one record, so the loader always replaces this fallback. |

### C. Lighting semantics and effect parameters

| Rule | Behaviour |
|---|---|
| R-LIT-01 | For light i (0 = sun, 1, 2), heading a = mini `0x08`+i and elevation e = mini `0x0b`+i are **radians**. The engine applies sine and cosine to them directly. The light's vector, pointing **toward the light**, in the TED source basis, is **(sin a·cos e, −cos a·cos e, sin e)**. It is unit length. At heading 0 the light lies toward −Y; heading increases toward +X; positive elevation is above the horizon. |
| R-LIT-02 | This convention equals alo-viewer's *settings-dialog* heading convention (the dialog subtracts 90°). The repository's candidate reader applies it (`lighting::retail_light_direction`, environment light-heading correction). Before the environment light-heading correction work that reader fed the TED heading straight into alo-viewer's angle-to-vector formula, so its toward-light vector was the retail vector rotated **+90° about +Z** (EV-N-01; exact to 6e-17). Elevation, colour, intensity, specular and ambient assignments of the candidate mapping agree with R-LIT-01…05. |
| R-LIT-03 | For each light i the engine derives a diffuse value (colour_i × intensity_i, 1) and a vector value (vector_i from R-LIT-01, 0). For i = 0 these are the effect inputs `DIR_LIGHT_DIFFUSE_0` and `DIR_LIGHT_VEC_0`. The index-1 and index-2 values are not uploaded (R-LIT-06). |
| R-LIT-04 | `DIR_LIGHT_SPECULAR_0` = (specular colour × **2** × sun intensity, 0). Lights 1 and 2 have zero specular. This differs from alo-viewer, which binds the plain specular colour. |
| R-LIT-05 | `GLOBAL_AMBIENT` = (ambient colour, 1). The ambient colour is not scaled by any intensity. |
| R-LIT-06 | The per-draw upload sets `GLOBAL_AMBIENT` and the index-0 sun parameters only: diffuse, specular, vector, view-space vector and object-space vector. The engine collects the handles for the index-1 and index-2 parameters, but a whole-program scan found no upload of them. The committed shader-corpus inventory (`plan/inventories/shader-corpus.json`) likewise records only the index-0 per-light semantics for every effect it lists. Lights 1 and 2 therefore reach effects only through the SH matrices. |
| R-LIT-07 | `SPH_LIGHT_FILL` projects ambient + light 1 + light 2. `SPH_LIGHT_ALL` projects the same terms plus light 0. Each light's weight is colour × intensity; its direction is the R-LIT-01 vector. Ambient adds its colour uniformly to the irradiance. The arithmetic is the standard nine-coefficient directional-light irradiance with cosine-lobe convolution. It agrees with the MIT alo-viewer projection-and-packing port in `lighting.cpp` to below 1e-6, the level of float-constant rounding (EV-N-01). With seed 13, the largest difference over 2,000 random light sets and normals is 7.9e-7. Over seeds 0–9 it stays at or below 9.4e-7. Since environment light-heading correction the two take the same direction input (R-LIT-02) and differ only in the lightning term (R-LIT-08). |
| R-LIT-08 | **Lightning.** When the lightning scalar (`0x21`) is nonzero, the engine keeps a random wait timer and a random flash-length timer. Both are drawn from a presentation random stream, separate from the R-SEL-04 stream, and scaled by the scalar. During a flash a white light is added. Its direction is random, and its intensity is random up to about half the scalar. It is added either to the SH terms or to the ambient colour; which one depends on a global renderer condition that was not identified. The timers advance only when the environment is applied (see the cadence gate). |
| R-LIT-09 | The environment's light-0 vector is the vector read by the mode-7 sun rule. Gate G-03 of the mode-7 note is therefore answered for light angles: L = R-LIT-01 applied to minis `0x08` and `0x0b` of the **current** environment (R-SEL). The remaining uncertainty is which environment is current (R-SEL-02…05), not the angle mapping. |

### D. Distance fog

| Rule | Behaviour |
|---|---|
| R-FOG-01 | When an environment is applied and the map's fog master flag is on, the engine uses the environment's fog byte (`0x10`) as fog-enable, its colour (`0x0f`) as fog colour and its start/end (R-DEC-08). A map-level fog override exists but is not in use after map construction (R-FOG-04). A further global check can then force fog off; what it tests was not identified. |
| R-FOG-02 | Fog start (near) used by the renderer is (1 − f)·start + f·(end − 0.0001), where f is a per-environment factor. f is 0 at load, so near = start. No writer of f was found. |
| R-FOG-03 | The fog master flag is on when the map is constructed. The script request `Enable_Fog(bool)` sets it and re-applies the environment. It is valid only in a land tactical game. The shipped EaW Lua calls it at 3 sites. With the flag off, fog is disabled whatever the environment says. |
| R-FOG-04 | A map-level fog override (enable, colour, start, end) is initialised from the engine's base fog settings at construction. It is used only if the "use environment fog" flag is cleared. That flag is set at construction. No script or loader path clearing it was found. |
| R-FOG-05 | The renderer's fog parameter `FOG_VALS` is (1 / (near − far), −far / (near − far)). The effect fog factor, saturate(slope × distance + offset), is therefore 1 at near and 0 at far. The same near and far are also given to the fixed-function fog start and end states. The hop from the map's fog fields to this setter was not followed instruction by instruction. |

### E. Wind, cloud layer and weather

| Rule | Behaviour |
|---|---|
| R-WX-01 | After a random or weather-based pick (R-SEL-03, R-SEL-05), the scene wind vector is set to speed × (cos h, sin h, 0), with speed = `0x2c` and h = `0x2b` converted from degrees. **This heading convention differs from the light convention of R-LIT-01**: wind heading 0 points along +X and increases toward +Y. `WIND_GRASS_PARAMS` = (unit wind direction, wind speed) of a scene wind vector. The only traced writers of wind are these selection paths. The environment-apply step shared by the script change (R-SEL-06), the cinematic toggle (R-SEL-07) and the fog toggle (R-FOG-03) was not seen to write wind or the cloud layer. Whether those requests update them by another route is unknown. |
| R-WX-02 | In the same step, the cloud layer receives its texture (`0x2f`), scale parameter (`0x2d`) and velocity speed × (cos h, sin h, 0), with speed = `0x2e` and the same h. How the scale and velocity become `CLOUD_TEX_U`/`CLOUD_TEX_V` was not followed. |
| R-WX-03 | The weather-scenario name (`0x29`) is stored per environment. Its consumer was not followed. |

### F. Sky objects

| Rule | Behaviour |
|---|---|
| R-SKY-01 | On map activation, each non-empty sky name (`0x19`, then `0x1a`) is looked up as an XML game-object type. An unknown name clears that sky's name and parameters (R-DEC-11), and nothing is spawned. A known name spawns one object of that type at a point derived from the map bounds (the exact point is gate G-SKY-01). |
| R-SKY-02 | The spawned sky object's orientation triple is (first component, 0, third component) = (`0x1d`, 0, `0x1f`) for the primary sky and (`0x1e`, 0, `0x20`) for the secondary sky, already converted to radians (R-DEC-10). |
| R-SKY-03 | The generic object transform (R-ROT-01) converts its orientation triple from degrees to radians **again**. If nothing intervenes, a sky authored with third component 128 turns by about 2.2°, not 128°. This double conversion is a static reading only. It needs a runtime check before anyone reproduces it (gate G-SKY-02). |

### G. Object orientation composition

| Rule | Behaviour |
|---|---|
| R-ROT-01 | A game object's orientation triple (x, y, z) is in **degrees**. The object's world transform maps a model-space point v to T + Rz(z)·Ry(y)·Rx(x)·Rz(+90°)·v. T is the object position. Rz, Ry and Rx are right-handed rotations about +Z, +Y and +X, with column vectors. In words: the model first gets a fixed quarter turn about its own +Z. It then rolls by x about world X, pitches by y about world Y and yaws by z about world Z (extrinsic X, Y, Z). That is the same as intrinsic yaw, then pitch, then roll (Z, Y′, X″). The same matrix is handed to the object's attached model object. |
| R-ROT-02 | TED placement mini 5 (`roll X, pitch Y, yaw Z` degrees, TED map loader) **is** this triple (G-ROT-01, placed-object model orientation). A map object is loaded from the same micro-record stream as a saved object: micro 1 is the type CRC, micro 4 the position and micro 5 the orientation triple, read verbatim into the object's facing and copied to its display facing. The transform is rebuilt from the display facing (ROT-E2..ROT-E4). |
| R-ROT-03 | Two consequences follow. (a) The three-axis quarantine can be lifted with this composition order; placed-object model orientation does not do so. (b) Every placement, **including yaw-only ones**, carries the extra fixed +90° model-space turn about Z. Since placed-object model orientation the static scene applies it (`scene::placement_transform`) to every land and space placement and so to everything composed on it (attached models, hardpoints, shadows, evidence bounds). G-ROT-02 confirmed it on Naboo and Coruscant captures. |
| R-ROT-04 | The simulation heading is the triple's yaw **unchanged**, 0 along +X, forward (cos z, sin z): the facing is stored as read and the turn exists only in the model transform. FoC ship models have their nose on model -Y, so a ship at yaw z is drawn heading z. The remake's skirmish start gives each unit its marker's yaw, and so does FoC: a skirmish company is created with its marker's facing, in free space near the marker (IS-12, IS-13). The shadow blob (not drawn by the remake) is placed from the yaw alone, without the turn (ROT-E3, static reading). |

## Failure and diagnostic behaviour

The retail engine reports nothing for most of these cases. A clean implementation must fail closed
with named statuses. It must not reproduce undefined values:

- **A missing light field (`0x00`–`0x0d`).** Retail leaves the value undefined (R-DEC-07). Report the
  environment as non-effective: *light field missing*. Never fill in a default.
- **A short three-float mini.** Retail fails the map load (R-DEC-04). Report *map environment
  malformed* and do not treat the map as having valid lighting.
- **A short scalar mini.** Retail keeps the default. Report it as a diagnostic, keep the default, and
  label the result `retail-default`.
- **A non-finite float.** Retail propagates it unchecked. The remake should reject it with the
  existing `nonfinite` status. This is remake policy, not retail behaviour.
- **An unknown sky name.** Retail spawns no sky and clears the name (R-SKY-01). Report *sky
  unresolved*. This matches the existing reference ledger's missing-catalog status.
- **An empty environment list.** Retail would use its fallback (R-SEL-09). No corpus map has one.
  Report *no environment* rather than use the fallback.

## Original expected-outcome cases

### Case C-01: heading basis

Given an environment whose light 0 has heading π/2 and elevation 0.
Expected: the toward-light vector is (1, 0, 0). At heading 0 it is (0, −1, 0). At heading π/2 with elevation π/6 it is (cos π/6, 0, 0.5). A reader that returns (0, 1, 0) for heading π/2 is using the rejected candidate basis.

### Case C-02: last occurrence wins

Given a record with two `0x05` minis, holding 0.2 and then 0.6, and all other fields valid.
Expected: light 0 intensity is 0.6. With a third `0x05` mini that is only 2 bytes long, the intensity stays 0.6 and the record still loads.

### Case C-03: fog clamps

Given `0x11` = 12000 and `0x12` = 5000.
Expected: start = 10000 and end = 10000. With `0x11` = 300 and `0x12` = 200, start = 300 and end = 300. With neither mini present, start = 1000 and end = 10000.

### Case C-04: random pick with a Cinematic record

Given environments `[A, Cinematic, B]` and an injected uniform draw.
Expected: the results are A, B and B. With `[A, B, Cinematic]` and draw 2, the result is A (wrap). With `[cinematic, A]` (lower case) and draw 0, the result is the `cinematic` record. With a single environment the result is index 0 and no draw is consumed.

### Case C-05: weather pick

Given environments with categories `[0, 1, 1, 3]` and requested category 1.
Expected: draw 0 gives index 1 and draw 1 gives index 2. Requested category 2 (no candidate) falls back to the uniform pick over all four records. Requested category 7 is also the uniform pick.

### Case C-06: script change and cinematic toggle

Given three environments, current index 1, and no `Cinematic` record.
Expected: a script request for index 2 selects 2; a subsequent request for index −1 clamps to 0. Turning the cinematic environment on changes nothing and saves nothing. Given a `Cinematic` record at index 2 and current index 0, turning it on makes 2 current with saved index 0. Turning it off makes 0 current and the saved index −1. Turning it off again changes nothing.

### Case C-07: effect parameters

Given sun colour (1, 0.5, 0.25), intensity 0.5, specular (1, 1, 1), ambient (0.1, 0.1, 0.1), and fill intensities 0.
Expected: `DIR_LIGHT_DIFFUSE_0` = (0.5, 0.25, 0.125, 1), `DIR_LIGHT_SPECULAR_0` = (1, 1, 1, 0), `GLOBAL_AMBIENT` = (0.1, 0.1, 0.1, 1). `SPH_LIGHT_FILL` evaluates to 0.1 per channel for every normal. `SPH_LIGHT_ALL` evaluated toward the sun equals 0.1 + 1.0625 × the sun's weighted colour, per channel.

### Case C-08: fog values

Given fog enabled, start 400, end 2400, f = 0.
Expected: `FOG_VALS` = (−0.0005, 1.2). The fog factor is 1 at distance 400, 0.5 at 1400 and 0 at 2400. With the fog master flag off, fog is disabled.

### Case C-09: wind

Given wind heading 90 (degrees) and wind speed 4.
Expected: the vector is (0, 4, 0). `WIND_GRASS_PARAMS` = (0, 1, 0, 4). Contrast C-01: the light heading convention would send heading 90° toward +X.

### Case C-10: orientation composition (gated for placements)

Given an object at the origin with orientation (0, 0, 0).
Expected: model +X maps to world +Y and model +Y maps to world −X. With orientation (0, 0, 90), model +X maps to world −X. With (90, 0, 0), model +X maps to world +Z. With (0, 90, 0), model +X maps to world +Y and model +Y maps to world +Z.

## Uncertainties and gates

| Gate | Unknown |
| --- | --- |
| G-SEL-01 | What the load-path flag and argument that skip the random pick mean (save restore, replay, editor or other) |
| G-SEL-02 | Who calls the weather-based pick, and where the requested category comes from |
| G-SEL-03 | Order of the two tactical set-up steps and their game-mode coverage (skirmish, campaign, multiplayer, story) |
| G-SEL-04 | Whether the pick is synchronised across multiplayer peers |
| G-LIT-01 | Negative scan only for uploads of the fill-light per-light parameters |
| G-LTN-01 | How often the lightning timers advance (whether apply runs per frame), and the condition that picks SH or ambient |
| G-SHD-01 | Answered (inferred-behaviour claim audit): `0x17` is the scene shadow colour (IS-15) |
| G-ENV-01 | Answered (inferred-behaviour claim audit): `0x15` is the sky background colour (IS-15) |
| G-ENV-02 | Answered (stored-space bloom compositor): `0x23`, `0x24` and `0x28` are the bloom strength, cutoff and size of `SceneBloom.fx` ([bloom](../rendering.md#bloom)) |
| G-FOG-01 | The final hop from the map fog fields to `FOG_VALS` and the fixed-function fog states |
| G-FOG-02 | Writers of the fog interpolation factor f |
| G-FOG-03 | The global check that can force fog off (possibly a graphics-detail setting) |
| G-FOG-04 | Any path that clears "use environment fog" |
| G-WX-01 | Consumer of the weather-scenario name `0x29` (rain and snow particles, audio, gameplay modifiers) |
| G-WX-02 | Whether wind and the cloud layer change after the script change, cinematic toggle or fog toggle. They are traced only after the random and weather-based picks |
| G-WX-03 | Cloud texture-coordinate transform from scale and velocity |
| G-WX-04 | Answered for land (vegetation wind animation, `vegetation-effects.md` W-05/W-06): land mode writes the scene wind from the current environment whenever it updates its render settings, and `WIND_GRASS_PARAMS` and `WIND_BEND_VECTOR` read that scene wind. Space scenes set none |
| G-SKY-01 | Exact spawn point of sky objects |
| G-SKY-02 | Whether the sky orientation is really converted twice (R-SKY-03) |
| G-SKY-03 | Meaning of `0x1b`/`0x1c` (always 1.0 in the corpus) |
| G-ROT-01 | Answered (placed-object model orientation): TED placement mini 5 is the R-ROT-01 triple (R-ROT-02). The private evidence map (ROT-E1 to ROT-E4) is under the ignored `out/research/` |
| G-ROT-02 | Answered (placed-object model orientation): original captures of the Naboo bridges and props and of Coruscant show the fixed +90° model turn (R-ROT-03) |
| G-FOC-01 | FoC executable equivalence of every rule |
