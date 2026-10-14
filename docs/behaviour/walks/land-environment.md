# Walk 29: land map environment

## Scope and evidence

This walk prepares the land battle's environment: which lighting state and weather a
land map runs, what weather does to play, how the terrain under a unit decides its
modifiers, what props and map objects are (and are not) destructible, how indigenous and
neutral units behave on a land map, and who owns and captures map objects. It records
**gameplay effects** and **presentation effects** in separate sections, because the
same weather phase drives both and a presentation-only port must not claim either.

Sources are the **debug build**, the **effective XML/Lua data** and the stock TED maps.
Opaque evidence IDs `LEN-E01` to `LEN-E28` identify read-only debug-build inspections;
`LEN-D01` to `LEN-D12` identify data snapshots (XML tables, Lua scripts, and a census of
all 269 stock TED maps). The private map from IDs to inspections stays in ignored
`out/research/walk-land-environment/`. No retail capture was made. Code constants and
XML values are distinguished below; authored examples are not universal defaults.

The code comparison is against `cf94df0ba2928b80ce2b230e08a5dfdbd3fff97e`. The tactical
simulation (`src/sim/tactical/`, `src/units/`) is space only; the land scene is
presentation (`apps/viewer/src/map_mode_*.cpp`, `src/presentation/terrain/terrain.cpp`,
`src/presentation/lighting/`). **Missing** means no land component or data path exists.
**Same** is used only where an existing rule or code path already holds the contract.
**Differs** names a measured divergence.

Boundaries: [land combat](land-combat.md) owns combat modifiers and the surface/cover
rules LC-16..LC-22, crush LC-23..LC-25 and bombing/bombardment; [neutral objects](neutral-objects.md)
owns the generic neutral, capture and respawn rules (WNO-xx, space samples);
[GC tactical](gc-tactical.md) owns spawner admission and the strategic indigenous boundary
(WGT-103..WGT-111); the [effective environment note](../p1-effective-environment.md)
owns environment-record decoding, lighting, fog, sky and wind arithmetic;
[vegetation effects](../vegetation-effects.md) owns the wind feed into vegetation;
[abilities](abilities.md) owns the ability framework; [land AI](land-ai.md) and
[audio](audio.md) own their own services. Where an existing note asked a question this
walk could answer, the rule says so.

## Entry points and service order

At battle load a land map selects its current environment (WLEN-01..WLEN-05), then the
weather system resolves that environment's scenario and loads its first pattern
(WLEN-06..WLEN-08). Each logical frame the land mode service runs, in this order:
cinematics, battlefield modifiers, path-finder preparation, **weather service**, hero
clash, the land-battle music and base-attack timers, bombing/bombardment activity,
enemy-leader victory tests, then the common mode service that services objects
(WLEN-05). Object services that read weather (reveal range, firing inaccuracy) therefore
see this frame's weather state. Terrain-surface effects run inside each object's own
surface behaviour; indigenous wandering runs from the object's behaviour list; the
ownership remap runs once, at battle start, before the first frame.

## Rule list

### Environment records and lighting states

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-01 | Every stock map declares an ordered list of environment records (name, lights, fog, weather category, weather-scenario name, wind, lightning scalar). A land map's records are fixed lighting sets; there is **no time-of-day progression**: nothing in the weather or land service advances from one record to another. Day/night variants are separate records (`Sunrise_*`, `Noon_*`, `Nighttime_*` and map-specific names) of which exactly one is current at a time. | debug build `LEN-E14`, `LEN-E16`; data `LEN-D05`; effective-environment R-SEL-01 | Decode **same** as R-DEC (`src/assets/map_decode.cpp`, `EnvironmentDescriptor`); state machine missing. |
| WLEN-02 | The current environment of a battle is chosen once at load, uniformly at random among the map's records (skipping a record named `Cinematic`), or by requested weather category (WLEN-03). Stock land skirmish maps carry 1 to 5 records; e.g. Corellia and Endor mix two clear and two rain records, Hoth and Polus offer three snow records including `Nighttime_Snow`, Mandalore has `Nighttime_Clear`. The authored current index is kept only where a load-path flag skips the pick (R-SEL-02); with one record the stream is not consumed (R-SEL-03). | data `LEN-D05`; effective-environment R-SEL-02/03 | **Differs**: the viewer shows record 0 unless `--eawr-environment-record` is passed (`apps/viewer/src/map_mode_ready.cpp`); no session-level pick (legacy EAWR-795). |
| WLEN-03 | The weather-based pick takes a requested category 0 to 4 (clear, rain, snow, sandstorm, ash by corpus correlation). One matching record is taken directly; several are chosen between uniformly; none, or a category above 4, falls back to the ordinary random pick. | debug build `LEN-E15`; effective-environment R-SEL-05 | Same as R-SEL-05; missing in code. |
| WLEN-04 | Both picks draw from the **synchronized** random stream, the same one used by weather phase timers and indigenous wandering (WLEN-09, WLEN-60). Peers in lockstep that consume that stream identically therefore derive the same environment and the same weather timeline. The unsynchronized stream is used only by weather audio intermittents (WLEN-37). | debug build `LEN-E15`, `LEN-E03`, `LEN-E23`, `LEN-E13` | Answers effective-environment G-SEL-04's stream question; missing: a session RNG partition for environment and weather draws. |
| WLEN-05 | Weather is serviced **only from the land mode service**; the space mode service never calls it. Weather scenario names on space maps are inert. Within the land service the weather step runs after cinematics, battlefield-modifier service and path-finder preparation, and before hero clash, music/announcement timers and the common mode service. | debug build `LEN-E14` | Matches the order recorded by LC-69; missing as a sim service. |

### Weather scenario and pattern engine

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-06 | A scenario is named by the current environment's weather-scenario mini (upper-case in the TED, e.g. `RAIN_RANDOM`; the XML scenario names are mixed case and are upper-cased on load). Each service the system checks the current environment index and scenario name; if either changed, or no scenario is loaded, it reloads. An empty or unknown name means no weather: type clear, no emitter, and, if a scenario had been active, the weather audio is told "clear at 1.0". A scenario with no patterns is treated as no weather. | debug build `LEN-E04`, `LEN-E12`; data `LEN-D01`, `LEN-D05` | Missing. Name case folding is inferred (U-LEN-02). |
| WLEN-07 | A scenario is an ordered list of patterns. Each pattern authors a weather type (`CLEAR`, `ASH`, `RAIN`, `SANDSTORM`, `SNOW`; any other name is treated as `CLEAR` by the data's own comment), an emitter intensity `Emitter_Intensity`, a `Lightning_Intensity`, a `Duration`, an `Ease_Out_Duration` and an optional `Fog_Plane_Interpolation_Alpha`; the scenario's optional `emitter_name` attribute names the particle emitter. Stock: 13 scenarios; the `*_Random` ones have 5 to 8 patterns, the `*_Constant` ones one 500-second pattern. | data `LEN-D01`; debug build `LEN-E04` | Missing (no loader for the weather files). |
| WLEN-08 | Loading a pattern: on reload the pattern index becomes 0; afterwards it advances to the next index and wraps to 0 after the last; a one-pattern scenario reloads itself. The loaded pattern becomes current, the **next** pattern is the following one (itself for one pattern), the weather type is the pattern's type, the weather intensity is its emitter intensity, easing-out is cleared, and the weather audio is activated with that type and intensity for the map's environment type. If the weather **type** changed from the previous pattern, the object manager is notified (WLEN-12). | debug build `LEN-E02` | Missing. |
| WLEN-09 | On load a pattern's hold timer is set to a uniform random number of seconds in `[0.3 x Duration, Duration]` from the synchronized stream (`Duration` is a maximum, not a fixed length). The pattern's lightning scalar is written to the current environment, its fog-plane alpha is written if distance fog is enabled, and the emitter's energy setting becomes its intensity. | debug build `LEN-E03`, `LEN-E16`; data `LEN-D01` | Missing. |
| WLEN-10 | When the hold timer elapses the system starts **easing out**: the timer becomes a uniform random number of seconds in `[0.3 x Ease_Out_Duration, Ease_Out_Duration]`. Each frame of the ease, with `t` the fraction of that timer elapsed (clamped to 1), lightning scalar, emitter energy and fog-plane alpha are linear interpolations from the current pattern's value to the **next** pattern's value, and the weather audio intensity follows the emitter energy. When the ease timer elapses the next pattern is loaded (WLEN-08). Thus the authored `Ease_Out_Duration` is a cross-fade into the next pattern. | debug build `LEN-E01` | Missing. |
| WLEN-11 | Intensity gate: when the interpolated emitter intensity crosses 0.5 in either direction during an ease (from at most 0.5 to above it, or from above it to at most 0.5) the stored intensity is updated and the object manager is notified (WLEN-12); otherwise the intensity is stored silently. Gameplay effects use the same threshold (WLEN-16). | debug build `LEN-E01`, `LEN-E05` | Missing. |
| WLEN-12 | The weather-changed notification makes every object with a reveal behaviour re-evaluate its reveal range on its next service (not in the map editor). It is the only propagation path: sight modifiers (WLEN-18) reach fog of war through it. | debug build `LEN-E10` | Missing: land reveal service (LC-35, LC-36) has no weather input or re-evaluation trigger. |
| WLEN-13 | Lua `Force_Weather` advances the weather system immediately to its next pattern, as at the end of a pattern, without an ease. The stock tutorial land mission calls it once to "turn on sandstorms for a while". `Weather_Audio_Pause` is the paired audio hook. | debug build `LEN-E28`; data Lua `LEN-D12`; WBF-53 | Names listed in `src/script/authoritative/galactic_api_inventory.hpp`; no behaviour. |
| WLEN-14 | Weather timers count logical frames (the same frame clock as every other tactical timer), so tactical time controls scale weather, and pause holds it; the audio side is paused through the shared pause hook. | debug build `LEN-E01`, `LEN-E03`; WBF-53, SND-13 | Missing; follows the shared time-control contract once a tactical clock exists. |

### Weather: gameplay effects

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-15 | Object types carry an optional `Weather_Category` (`Infantry`, `Walker`, `LargeWalker`, `Speeder`, `Tracked`, `Hover`, `Wheeled`, `Flying`; `Default`). An absent or unrecognized value is `Default`. Stock non-variant types: infantry 55 (27 indigenous, 18 ground infantry, 8 slaves), walker 11, hover 10, flying 9, tracked 5, large walker 3, speeder 2; heroes, generic heroes, transports and most unique units author none and therefore use `Default`. | data `LEN-D04`; debug build `LEN-E07` | Registry rows are `land-or-galactic`; no loader or sim field. |
| WLEN-16 | Three modifier tables (sight range, speed, weapon accuracy) exist per weather type. A lookup returns 1.0 unless the current weather intensity is **strictly above 0.5**; then it looks up the object's category in the current type's table, falls back to the table's `Default` entry, then to 1.0. The accuracy table is keyed by projectile category (`Default`, `Laser`, `Missile`, `Rocket`, `MPTL_Rocket`, `Grenade`, `Bomb`); a category outside the engine's ten-entry enumeration is looked up as `Default`. Values are read as decimal text (a trailing `f` is accepted) after upper-casing the names; an unknown category name only logs a warning. | debug build `LEN-E05`, `LEN-E12`; data `LEN-D02` | Missing. |
| WLEN-17 | **Owner immunity.** If the object has an owner player whose weather flag is false, every lookup returns 1.0. The flag is the logical AND of the player's modifier list and is true when the list is empty. | debug build `LEN-E05`, `LEN-E06` | Missing. |
| WLEN-18 | **Sight.** The land reveal range of an object is its type's `Land_FOW_Reveal_Range` multiplied by the sight modifier for its category; a modifier at or below 0.0001 is replaced by 1.0. Stock sight tables: snow `Default` 0.5, infantry/walker/tracked/hover/wheeled 0.4, large walker and flying 0.7; rain infantry/walker/tracked/hover/wheeled 0.7, others 1.0; sandstorm all ground classes 0.4, flying 0.7, `Default` 1.0; ash infantry 0.7, others 1.0; clear 1.0. A `Speeder` category has no table entry, so it takes `Default` (0.5 in snow, otherwise 1.0). | debug build `LEN-E07`; data `LEN-D02`, `LEN-D04` | Missing: LC-35 needs a weather term; reveal re-evaluation is WLEN-12. |
| WLEN-19 | **Accuracy.** In land mode the firing inaccuracy radius of a weapon is `base + base x (1 - m)` where `m` is the accuracy modifier for the weapon's projectile category (multiplier `2 - m`). The rule is applied to the target-category inaccuracy list, to the fixed-radius inaccuracy (an active ability's positive fixed-radius override bypasses it) and to the hardpoint fire-inaccuracy distance. Stock multipliers while the weather gate is open: snow rocket 5, laser 2, missile and MPTL rocket 1.5; rain laser 2; sandstorm rocket 8, MPTL rocket 4, grenade and bomb 2, laser 1.5; every other entry 1; ash all 1. A category with no table entry (the data authors `STATIC_BOMB` and `LIGHT_SABER` projectiles) falls to `Default`, which is 1.0 in every stock table. | debug build `LEN-E08`, `LEN-E05`; data `LEN-D02` | Missing: the space inaccuracy rows (`src/sim/tactical/combat.hpp::InaccuracyRow`) have no weather term and no land mode. |
| WLEN-20 | **Speed has no effect in the inspected build.** The speed table is cached once per type-manager service in land mode, using a null object (so the owner immunity of WLEN-17 is skipped) with values at or below 0.0001 replaced by 1.0, and its only consumer is the max-speed computation. That consumer sits in a branch that the preceding control flow can never reach (it requires the object not to be a projectile after an earlier branch already handled every non-projectile). Every stock speed entry is 1.0 anyway. A mod authoring a non-1.0 speed modifier would observe no effect in this build. | debug build `LEN-E09`; data `LEN-D02` | Missing; do not implement a speed term without a retail capture (U-LEN-01). |
| WLEN-21 | **Weatherproof upgrades.** `Weatherproof_Ability` (activation `Ground_Automatic`, `Apply_To_All_Allies=Yes` in all stock uses) adds a permanent false entry to a player's weather flag list (immune: WLEN-17). It is carried by the Empire and Rebel upgrades (multiplayer cost 500, campaign cost 170, 15 s, one per allied group) and by the Felucia planetary advantage, which a campaign planet grants galaxy-wide to its owner. The all-allies loop iterates the playable allies but writes the **owner's** list on each pass as read; whether allies themselves become immune is unverified. Immunity is per player, not per object: it covers every unit of the owner at once. | debug build `LEN-E06`; data `LEN-D04` | Missing; ability framework is [abilities](abilities.md); registry has no rows for the ability. |
| WLEN-22 | Weather has no stock effect on construction, production, capture, abilities, economy or victory; no stock data authors a weather-dependent condition outside the three tables, the upgrade and the weather audio. The land tactical AI's `Is_In_*` weather queries are stubs in our AI host. | data `LEN-D02`, `LEN-D04`; debug build `LEN-E05` | Same as the recorded AI stubs (`src/script/foc/tactical_ai_bindings.cpp`); whether FoC AI reads weather is U-LEN-11. |

### Weather and lighting: presentation effects

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-23 | The scenario's emitter (`emitter_name`, e.g. a rain, sleet, blizzard, ash or dust-storm particle system) is created when a scenario loads and removed when it unloads or reloads; its energy setting is set to the pattern intensity (WLEN-09, WLEN-10). Weather emitters are excluded from the every-vertex draw-mask exemption (PS-35). | debug build `LEN-E03`, `LEN-E04`; data `LEN-D01`; PS-35 | Partial: the weather emitter flag exists in `src/presentation/particles/cpu_system.cpp`; no scenario-driven emitter. |
| WLEN-24 | **Lightning.** The pattern's `Lightning_Intensity` is written to the current environment's lightning scalar. Every write also zeroes both lightning timers, so the next flash is re-rolled; during an ease this happens every frame. This is the writer the effective-environment note could not find for the scalar of R-LIT-08. | debug build `LEN-E16`, `LEN-E01` | Missing; answers G-LTN-01 in part (the scalar is dynamic), flash arithmetic stays R-LIT-08. |
| WLEN-25 | **Fog plane.** When the current environment has distance fog enabled the pattern's `Fog_Plane_Interpolation_Alpha` (0 to 0.3 in stock data) is written as the factor `f` of R-FOG-02, so rain, blizzard, snow and dust-storm patterns pull the fog near plane toward the far plane by up to 30 percent. With fog disabled it is not written. Patterns that omit the tag are authored mostly in `CLEAR` phases; the default is not read (U-LEN-03). | debug build `LEN-E16`, `LEN-E01`; data `LEN-D01` | Missing; answers effective-environment G-FOG-02. |
| WLEN-26 | **Weather audio loops.** `Weather_SFXEvent_Loop` rows are `<TYPE>, <threshold>, <event>`. The loop for a type and intensity is the first row, in file order, whose threshold is at most the intensity and whose event has samples. Stock thresholds are 0.75, 0.5, 0.25, 0.01 and 0.0, so the loudest loop is found first. | debug build `LEN-E13`; data `LEN-D03` | Missing (audio walk covers only pause/order; WBF-13, SND-13). |
| WLEN-27 | **Weather audio intermittents.** `Weather_SFXEvent_Intermittent` rows are `<TYPE>, <threshold>, <event>, <min s>, <max s>`; selection is the same first-match-by-threshold rule (stock thresholds 0.66, 0.33, 0.01). A countdown starts at a uniform random delay in `[min, max]` seconds from the **unsynchronized** stream. When it ends the event plays at a random angle, 50 units from the listener, then the delay is re-rolled. For clear weather or zero intensity the map's environment-type ambient intermittent entry is used instead. | debug build `LEN-E13`; data `LEN-D03` | Missing. |
| WLEN-28 | Changing intensity re-selects the loop and intermittent. If the loop changed, the old loop fades out and the new one fades in. The local player's faction plays `SFXEvent_Weather_Begin` when intensity rises from zero and `SFXEvent_Weather_End` when it falls to zero, only for non-clear types and only if that player is affected by weather (WLEN-17). | debug build `LEN-E13` | Missing; registry rows for the two faction tags are `todo`. |
| WLEN-29 | **Warning icon.** A unit shows the weather icon when its category's sight modifier is below `Min_Sight_Range_For_Icon` (0.5) or the accuracy modifier of its current projectile category (laser, rocket, MPTL rocket, or an active rocket-attack ability) is below `Min_Accuracy_For_Icon` (0.4); a team shows it if any member does. With stock tables the icon appears for snow and sandstorm infantry sight (0.4), for snow laser (0.0) and rain laser (0.0), for snow and sandstorm rockets and sandstorm MPTL rockets; it does not appear for rain sight (0.7), snow `Default` sight (exactly 0.5), sandstorm laser (0.5) or ash. | debug build `LEN-E11`; data `LEN-D04`, `LEN-D02` | Missing; both GameConstants rows are `todo`. |
| WLEN-30 | Weather type display text, class text, description, objective text and icon come from the weather modifier file (`Display_Text`, `Class_Text`, `Description_Text`, `Objective_Text`, `Icon_Name`) and feed the in-battle weather information and the encyclopedia. The planet's own `Describe_Weather` and `Encyclopedia_Weather_*` are encyclopedia and loading-screen text only; they do not choose the live scenario. | debug build `LEN-E12`; data `LEN-D02`, `LEN-D04` | Loading-screen text/icon **same** (`src/presentation/ui/loading_screen.cpp`); live weather panel missing. |
| WLEN-31 | The environment's lights, ambient, shadow colour, bloom, fog colour and distance, wind vector, cloud layer and sky objects are the P1 note's R-LIT, R-FOG, R-WX and R-SKY rules, applied when the environment is applied; weather does not blend between environments and never changes the lights. | effective-environment R-LIT/R-FOG/R-WX/R-SKY; debug build `LEN-E16` | **Same** as the note (`src/presentation/lighting/lighting.cpp`, `wind.cpp`, `scene_bloom.cpp`); dynamic fog/lightning inputs per WLEN-24/25 missing. |

### Terrain surface types (gameplay)

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-32 | A terrain material record carries a surface-type integer (micro-chunk `0x14` of the `0x03` material record). Editor names map to numbers: `DEFAULT` 0, `GRASS` 1, `DIRT` 2, `SNOW` 3, `WATER` 4, `CONCRETE` 5, `MUD` 6, `ICE` 7, `LAVA` 8, `GOOD_GROUND` 12, `HIGH_GROUND` 13, `SLOW_GROUND` 14, `DAMAGE_GROUND` 15, `INFANTRY_GROUND` 16, `SPACE` 18; an unknown name is none (-1). Each stock material set authors 64 materials; the shipped sets use only DEFAULT, GRASS, DIRT, SNOW, CONCRETE, MUD, ICE and LAVA. | debug build `LEN-E17`; data `LEN-D06` | Missing: `TerrainMaterial` keeps the mini raw (`include/eawr/assets/map.hpp`), no surface-type decode. |
| WLEN-33 | **The logical surface under an object is the surface type of the material at the terrain vertex closest to the object's position.** The exception is a map with a water plane (render mode not 0): when the terrain height at the position is more than 0.5 below the water elevation, the surface is the water plane's type instead (mode 1 water, 2 lava, 3 ice, 0 none). There is no separate hint-zone or painted-region layer for gameplay surfaces. | debug build `LEN-E18`, `LEN-E19`; data `LEN-D06` | Missing; land presentation has the material slot per vertex (`src/presentation/terrain/terrain.cpp`) but no surface lookup. |
| WLEN-34 | Gameplay effects by logical surface (the same behaviour as LC-16..LC-20, re-read here for the surface-to-effect map): `GOOD_GROUND` adds the defense and damage modifiers unless the object is converted to the enemy; `HIGH_GROUND` adds the reveal-range and fire-range modifiers; `SLOW_GROUND` and `WATER` add the speed modifier; `DAMAGE_GROUND` and `LAVA` set the terrain-damage amount and delay; `MUD`, `ICE`, `SNOW`, `GRASS`, `DIRT`, `CONCRETE`, `DEFAULT` add nothing; `INFANTRY_GROUND` sets only a status icon and tint. Leaving a surface removes only that source's modifiers. | debug build `LEN-E20`; data `LEN-D11`; LC-16..LC-20 | **Same** rules as LC-16..LC-20, still missing in code (legacy EAWR-1452). |
| WLEN-35 | **Mud is a gate flag but not an effect.** The surface logic treats `MUD` like the other special surfaces for icon/modifier cleanup, yet has no add branch for it; `Speed_Mod` authored under `mud` is never applied. This confirms LC-19/U-LC-1 from a second reading of the same service; whether the retail executable differs is unverified (U-LEN-01). Stock maps make this visible: mud covers 71.8 percent of Felucia's vertices. | debug build `LEN-E20`; data `LEN-D06`, `LEN-D11` | Same as LC-19; adds the area census. |
| WLEN-36 | Values: both `Infantry_Terrain_Modifier` and `Vehicle_Terrain_Modifier` author high ground 0.25 reveal / 0.25 fire range, slow ground, water and mud speed -0.5, damage ground and lava 10 damage per 1.0 s; only the infantry set authors good ground defense 0.5 and damage 1.0. The tints are `Good` (0,255,0), `High` (0,0,255), `Slow` (255,255,0), `Lava` (255,0,0), `Infantry` (0,255,255). Which objects use them is per-type `SurfaceFX_Name` plus the `SURFACE_FX` behaviour. | data `LEN-D11`, `LEN-D04`; LC-17/18/19 | Same values as LC-D03; registry `SurfaceFX_Name` and the tints are `land-or-galactic`. |
| WLEN-37 | **What stock maps actually use** (share of terrain vertices by logical surface; materials counted through each map's own material table): no stock map authors `SLOW_GROUND` or `DAMAGE_GROUND`; `GOOD_GROUND` appears on 48 land maps but only as patches of at most 0.2 percent (base pads); `HIGH_GROUND` only on Naboo (0.1 percent); `LAVA` on Shola (9.8 percent) and, trivially, Kessel; `WATER` materials 0.3 to 3.5 percent on 9 skirmish maps plus the water planes of 12 maps; `ICE` 1.9 to 3.5 percent on Alderaan, Hoth and Polus; `SNOW` 86.0 and 79.2 percent on Hoth and Polus (no gameplay effect); `INFANTRY_GROUND` on 15 maps (0.1 to 2.2 percent). Water planes: stock family 1 on 12 maps, lava on Shola, ice on Polus. | data `LEN-D06`; census tables below | Missing; gives the land implementation its acceptance maps. |
| WLEN-38 | Steep-to-water conversion exists as a mode routine: for every interior cell whose base passability is steep, if the terrain height at all four points `(+-10, +-10)` around the cell centre is at or below the water elevation and the centre is inside the map rectangle, the cell's base passability becomes water, and the terrain vertices are marked dirty. No direct caller was found (virtual dispatch); it may be an editor/preprocess step. | debug build `LEN-E21` | Missing; belongs with the passability work (LC-07/LC-08). Caller unverified (U-LEN-05). |
| WLEN-39 | After a land map loads, the cell grid is initialized at **20 x 20** units over the terrain's width and height, and the default playable bounds are the map bounds inset by **400** units on each side in X and Y (the whole map if that inset box would be empty). Whether a map-authored volume later replaces the playable bounds is not established. | debug build `LEN-E22`; asset-formats | Terrain cell spacing 20 **same** (`Terrain::cell_spacing`); inset and playable-bound rule missing; map volumes unmodelled (`SourceVolume`). |

### Props, decorations and map objects

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-40 | Stock land maps place scenery as typed objects, not terrain: across the 24 skirmish maps the 27,053 prop placements are almost all tree, plant, rock, urban and prop classes (`Prop_*`, `Props_*`); 15,516 of them are flagged `Is_Decoration`, 2,341 carry `LAND_OBSTACLE`, 3,013 author hard footprint extents and a collision class. Campaign maps add walls, civilian spawn houses, turrets, bunkers and secondary structures. | data `LEN-D07`, `LEN-D08` | Placement decode **same** (`src/assets/map_decode.cpp`); land object roles missing. |
| WLEN-41 | **Stock skirmish scenery is indestructible.** Of the 27,053 prop placements on the 24 skirmish maps only 8 (all `Land_Speeder`) have health, are valid targets or are collidable by living projectiles; no placed prop is `Is_Squashable` or `Is_Squashable_By_Supercrusher`. Trees, rocks, crates, girders and buildings cannot be shot, crushed or destroyed in skirmish; only blocking (obstacle/hard extents) and cover materials (LC-21) apply. | data `LEN-D07`, `LEN-D08` | Missing; consistent with LC-24. |
| WLEN-42 | Destructible props exist for campaign and story use: `Exploding_Container` and `Exploding_Doorway` (health 50, `Death_Explosions`, `Remove_Upon_Death`), killable doors (500) and sealed doorways (10,000), mission interface/power nodes (5 to 10), quarters and palaces with `EARTHQUAKE_TARGET`. They are story objects (`Props_Story`) or three placements in campaign maps, not skirmish scenery. | data `LEN-D08`, `LEN-D07` | Missing. |
| WLEN-43 | Bridges are ordinary decoration props; walkability across them comes from the map's baked passability grid (TED `1/266`), not from a bridge behaviour. The stock bridge props (`Prop_Theed_Bridge`, `Prop_Dathomir_Bridge`, an arch) author no obstacle or health data. | data `LEN-D08` | Passability grid decode **same** (`include/eawr/assets/map.hpp`); land use missing (LC-07/LC-08). |
| WLEN-44 | `WIND_DISTURBANCE` objects (infantry bikes, indigenous creatures, speeders; 3 prop/indigenous types and 8 vehicle-class types in stock data) add a scene wind disturbance every service in which they have **moved** since two services earlier: position, direction = facing scaled by the displacement, `Wind_Disturbance_Strength`, `Wind_Disturbance_Radius` and `Wind_Disturbance_Sphere_Alpha`. Stock values: bikes 50 / 10 / 0.4, small indigenous 100 / 5 / 0.1, large indigenous 150 / 100 / 0.5. It moves vegetation only; it has no gameplay effect. | debug build `LEN-E25`; data `LEN-D09` | Missing; vegetation-effects names disturbances as unmodelled. Registry rows are `land-or-galactic`. |

### Indigenous and neutral units on land maps

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-45 | **Placement and owner.** Placed `Indigenous_Unit` objects (166 on the 24 skirmish maps) take their TED owner index through the ordinary remap (WLEN-56): index 3 is the Neutral player; hostile creatures (Rancor, Sarlacc, Wampa/Rancor caves, Felucia plants, Hutt creatures) are owned by the nonplayable Hostile, Sarlacc or Hutts players (indices 4, 5, 7). A spawned individual takes its spawner's owner (WGT-104). Neutral and Hostile are **not** the same relationship: Neutral objects are nonhostile to everyone (WNO-05), the Hostile faction is not neutral. | data `LEN-D07`, `LEN-D09`; WNO-03/05, WGT-104 | Remap **same** (WLEN-56); no land indigenous objects. |
| WLEN-46 | **Wandering.** An object with the indigenous behaviour services periodically (service interval from the behaviour). It does nothing while on an uninterruptible move; while on fire it takes fire damage `Burning_Damage_Per_Second / logical FPS` per service. When not on fire it does nothing while moving, attacking or in a non-idle animation. A unit whose type allows stopping (`Can_Indigenous_Unit_Stop`) takes a new walk only on a draw of at most 0.1 from the synchronized stream (or when the service-interval argument is not above the interval). | debug build `LEN-E23`; data `LEN-D09` | Missing. Timing argument semantics U-LEN-09. |
| WLEN-47 | **Destination.** If the object has a spawner record with a positive maximum allowed distance and a valid spawner location, the destination is a random point in a disc of that radius around the spawner (a uniform X offset, then a uniform Y offset within the circle's chord at that X). Otherwise it is a uniform random point inside the **map** bounds' X and Y (not the playable bounds). The candidate is replaced by a point along the same direction at a uniform random distance in **[50, 400]** units from the current position when its distance falls outside that range; while on fire the range is **[60, 150]**. The order is attack-on-path to a position, issued through the coordinated group move; a member of a team moves only through the team leader's order for the whole team. | debug build `LEN-E23`; constants `LEN-E23` | Missing. |
| WLEN-48 | **Explosion flight.** When an explosion object is initialized, every indigenous unit within **500** units of it is ordered to move to the point `550` units from the explosion along the line explosion-to-unit (radius 500 plus offset 50); team members move through their leader. This affects every indigenous unit on the map, including hostile creatures, and does not depend on ownership. | debug build `LEN-E24` | Missing. |
| WLEN-49 | **Civilians** (`Civilian_Independent_AI`, the urban, swamp and desert civilian variants): neutral affiliation, light weapon (damage 3, range 150), idle chase range 500, weather category infantry, `Is_Squashable`, victory-irrelevant; the Rebel/Empire/Pirate variants of the same bodies are owner-specific death-clone copies. Hostile creatures (Rancor 90 damage, 1,100 health, `Is_Squashable=No`; Sarlacc 25 damage, 550 health, stationary turret) target through their own priority sets and are the dangerous neutrals. | data `LEN-D09` | Missing. |
| WLEN-50 | Spawner structures on land maps (`Mandalorian_Spawn_House`, `Ewok_Spawn_House`, `Rancor_Spawn_Cave`, `Vornskr_Spawn_House`, `Wampa_Spawn_Cave`, `Noghri_Spawn_House`, `Twilek_Spawn_House`, `Jawa_Sandcrawler`, the civilian and Hutt spawn houses) carry the `SPAWN_INDIGENOUS_UNITS` data of WGT-103..WGT-111; stock skirmish maps place 8 Mandalorian, 4 Ewok, 2 Rancor, 2 Twilek, 1 Noghri, 1 Wampa, 2 Pyngani and 1 Jawa spawner. Admission, ownership and distribution are those rules; this walk adds the unit-side behaviour (WLEN-46..48). | data `LEN-D07`, `LEN-D09`; WGT-103..WGT-111 | Spawner rules shared with GC tactical; missing in code (legacy EAWR-2684) covers only the AI estimate. |
| WLEN-51 | `Indigenous_Spawn_Destruction_Reward` (GameConstants, 500) is the credit reward for destroying a generated indigenous spawner (TCFA-03); skirmish rewards are not stated by data. | data `LEN-D04`; TCFA-03 | Same as TCFA-03 (`land-or-galactic`). |
| WLEN-52 | **Survivor and corruption tags.** `Is_Indigenous_Unit_Corruptible` (false on stock civilians and containers) gates the post-load corruption conversion of WGT-109; `Can_Indigenous_Unit_Stop` is false only on birds (they never stop; a flying locomotor). Both are registry `land-or-galactic`. | data `LEN-D09`; WGT-109 | Missing. |

### Map-object ownership and capture

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| WLEN-53 | **Owner remap at battle start.** A temporary default player list gives each editor owner index its faction; the real list is built from the lobby. For every placed object not yet processed: an index beyond the default list is Neutral; an object whose faction is missing or unplayable-and-unmatched is removed; an object of a **playable** faction is removed; an object of a nonplayable faction takes the real player of that faction; if none exists, a decoration that is not discardable falls back to Neutral, anything else is removed. Objects whose player id changed are re-owned once, with special abilities terminated beforehand. | debug build `LEN-E26`; WNO-03/04 | **Same** (`src/skirmish/start.cpp::add_map_objects`): decoration/discardable fallback, removal records. |
| WLEN-54 | Afterwards: relationships are remapped for team-based skirmish, each required AI player gets its faction's basic AI player name and the lobby difficulty, every player gets a tactical build queue, special weapons are created per playable faction, every object's relationships and visibility are refreshed, the fogged-model manager is reset, unprocessed objects' special abilities are re-initialized, each player's tech level becomes 1 and its tactical population cap is recomputed. | debug build `LEN-E26` | Partly present across `src/skirmish/start.cpp` and session setup; land-specific ordering missing. |
| WLEN-55 | Land skirmish layouts use the "new marker system": team markers are objects named by a `Team_NN_` prefix (spawn point, base position, build-pad position, structure positions); the remap scans marker types for that name pattern. Stock skirmish maps place 87 spawn markers per team 00 and 01, 25 base positions, 244 build-pad positions per team and the structure markers (barracks, command center, factories, power generator, research facility, communications array, base shield). | debug build `LEN-E26`; data `LEN-D07` | Markers read generically; land structure/marker consumers missing (legacy EAWR-1458) and the land programme (legacy EAWR-17). |
| WLEN-56 | Neutral and nonplayable owners on stock land maps: 29,019 of the skirmish placements are owned by index 3 (Neutral); Rebel, Empire, Pirate, Hostile, Sarlacc and Hutt owners are a handful each (indices 0, 1, 2, 4, 5, 7). The campaign maps carry large owner-specific base layouts (walls, turrets, spawn houses owned by Rebel, Empire, Pirates, Underworld, Hutts). | data `LEN-D07` | Same remap; no land base layouts. |
| WLEN-57 | **Capturable secondary structures** (campaign): `Secondary_Abandoned_Sensor` (capture 10 s, reveal range 10,000, health 700, victory-irrelevant), `Secondary_Mercenary_Outfitter` (upgrade store; weatherproof, armor, mobility upgrades offered in campaign), `Secondary_Heavy_Factory_E/R` (capture 12 s; spawns its starting squadron on first playable owner), `Secondary_Mining_Facility` (capture 10 s; on first playable owner pays 800 credits and destroys itself after 3 s), `Garrison_Bunker_Capturable` (radius 220, transition 3 s, `Ownership_Sticks=No`), and the pirate outpost (spawn trigger within 250 of a human object). Ownership, capture progress and sticking use the shared capture rules (WNO-18..WNO-23). | data `LEN-D10`; WNO-18..23 | Shared capture rules **same** space (`src/sim/tactical/pads.cpp`); land structures and Lua scripts missing (legacy EAWR-1458). |
| WLEN-58 | `Destruction_Survivors` (types with probabilities) and `Land_Damage_Thresholds` / `Land_Damage_Alternates` (health fractions at which the model swaps to damage states) author what a destroyed or wounded land structure leaves and shows; survivors are a gameplay spawn, damage alternates are presentation. | data `LEN-D10` | Registry `todo` for survivors on secondary/special structures; missing. |

## Stock-map census

All tables come from the effective stock data (`LEN-D05`, `LEN-D06`). Environment names
are the authored record names; weather is the authored scenario.

| Map | Environment records (name: weather scenario) | Authored current index |
|---|---|---|
| alderaan | Sunrise_Clear: none; Noon_Clear: none | 0 |
| bespin | Sunrise_Clear: none | 0 |
| bothawui | Sunrise_Rain: rain random; Noon_Rain: rain random | 1 |
| corellia | Sunrise_Clear: none; Noon_Clear: none; Sunrise_Rain: rain random; Noon_Rain: rain random | 0 |
| dagobah | Sunrise_Rain: rain random; Noon_Rain: rain random | 0 |
| dantooine | Dantooine_Morning: none; Sunrise_Clear: none; Noon_Clear: none; Sunrise_Rain: rain random; Noon_Rain: rain random | 0 |
| dathomir | Environment 1: none | 0 |
| endor | Sunrise_Clear: none; Noon_Clear: none; Sunrise_Rain: rain random; Noon_Rain: rain random | 0 |
| felucia | Felucia_Light_SE: none | 0 |
| geonosis | Geo_CLear: none; Sunrise_Clear: none; Noon_Clear: none | 0 |
| honoghr | Environment 1: none | 0 |
| hoth | Sunrise_Snow: snow random; Noon_Snow: snow random; Nighttime_Snow: snow random | 1 |
| hypori | Sandstorm_Fondor: none (name only; no scenario) | 0 |
| kamino | Nighttime_Rain: rain constant | 0 |
| kessel | Kessel_Light1: none; Kessel_Light2: none | 0 |
| mandalore | Sunrise_Clear: none; Noon_Clear: none; Nighttime_Clear: none | 2 |
| naboo | Sunrise_Clear: none; Noon_Clear: none | 0 |
| polus | Sunrise_Snow: blizzard random; Noon_Snow: blizzard random; Nighttime_Snow: blizzard random | 1 |
| ryloth | Ryloth_00: sandstorm constant | 0 |
| saleucami | Sunrise_Clear: none; Noon_Clear: none | 0 |
| shola | Morning_Ash: ash random; Noon_Ash: ash random | 1 |
| tatooine | Sunrise_Sandstorm: sandstorm random; Noon_Sandstorm: sandstorm random | 1 |
| utapau | Sunrise_Clear: none | 0 |
| yavin | Sunrise_Rain: rain random; Noon_Rain: rain random | 0 |

Across all 269 stock maps (space and land) the scenario names are: none 277 records,
`RAIN_RANDOM` 73, `DEFAULT_ALWAYS_CLEAR` 14, `BLIZZARD_RANDOM` 13, `ASH_RANDOM` 12,
`SANDSTORM_RANDOM` 9, `RAIN_CONSTANT` 7, `SANDSTORM_CONSTANT` 5, `SNOW_RANDOM` 4. The
scenarios `Blizzard_Constant`, `Snow_Constant`, `Ash_Constant`, `Sandstorm_Tutorial`
and `Rain_Jabiim` are authored but referenced by no stock map; the tutorial mission uses
`Force_Weather` instead (WLEN-13). Weather categories co-occur with the scenario
families as the effective-environment note states.

| Scenario | Emitter | Patterns (types) | Total authored `Duration` / `Ease_Out_Duration` (s) |
|---|---|---|---|
| Rain_Random | rain, medium | 7 (clear, rain, rain, rain, clear, rain, clear) | 215 / 45 |
| Blizzard_Random | snow blizzard | 5 (snow, snow, clear, snow, clear) | 160 / 32 |
| Snow_Random | snow sleet | 5 (snow, clear, snow, clear, snow) | 120 / 40 |
| Ash_Random | ash rain | 6 (ash, clear, ash, clear, ash, clear) | 140 / 35 |
| Sandstorm_Random | dust storm | 8 (sandstorm x2, clear, sandstorm x2, clear, sandstorm, clear) | 230 / 55 |
| `*_Constant`, `Rain_Jabiim` | as family | 1 (500 s pattern, intensity 1.0; `Sandstorm_Tutorial` 1,000 s) | 500 / 5 |
| Default_Always_Clear | none | 1 (clear, intensity 0) | 20 / 10 |

Because pattern holds are drawn in `[0.3 x Duration, Duration]` (WLEN-09) the real cycle of
`Rain_Random` is between about 65 and 215 seconds plus eases, and the weather is
gameplay-active (WLEN-16) only while the interpolated emitter intensity is above 0.5:
the first rain pattern (0.5) is not active, the 90-second pattern (1.0) is.

| Map | Vertices | Logical-surface vertex share (surface types with a gameplay effect or a distinct class) |
|---|---|---|
| alderaan | 34969 | ice 3.3%, good ground 0.1%, infantry ground 0.1% |
| bespin | 78400 | good ground 0.2% |
| bothawui | 34969 | mud 0.5%, water 0.9% |
| corellia | 96721 | water 0.3% |
| dagobah | 62001 | mud 14.7%, water 0.6%, infantry ground 1.3% |
| dantooine | 96721 | mud 5.3%, water 2.0%, infantry ground 0.4% |
| dathomir | 62001 | none of these |
| endor | 96721 | infantry ground 0.5% |
| felucia | 62001 | mud 71.8%, infantry ground 1.7% |
| geonosis | 96721 | infantry ground 0.5% |
| honoghr | 62001 | none of these |
| hoth | 47524 | ice 1.9%, snow 86.0%, infantry ground 0.2% |
| hypori | 139129 | infantry ground 0.2% |
| kamino | 62001 | infantry ground 0.6% |
| kessel | 78400 | lava under 0.1% |
| mandalore | 96721 | water 1.7% |
| naboo | 62001 | water 3.5%, high ground 0.1%, infantry ground 0.3% |
| polus | 34969 | ice 3.5%, snow 79.2% |
| ryloth | 47524 | infantry ground 1.2% |
| saleucami | 47524 | water 0.6%, infantry ground 0.1% |
| shola | 34969 | lava 9.8% |
| tatooine | 34969 | good ground under 0.1% |
| utapau | 96721 | none of these |
| yavin | 47524 | good ground under 0.1%, infantry ground 2.2% |

## Gap ownership

The rule list has 58 rules. **Against the existing behaviour notes:** 12 are **same**
(restated or confirmed: WLEN-01, 03, 05, 14, 31, 34, 35, 36, 45, 50, 51, 53), none
**differs**, and 46 are **missing there** (new rules). Among the new rules, WLEN-04
answers G-SEL-04, WLEN-06..08 answer the scenario-consumer gate G-WX-01, WLEN-24 answers
G-LTN-01 in part and WLEN-25 answers G-FOG-02; the caller of the weather-based pick
(G-SEL-02) stays open (U-LEN-04). **Against our code:** 3 **same** (WLEN-31, 45, 53),
1 **differs** (WLEN-02), 5 **partial** (WLEN-01, 23, 30, 39, 54) and 49 **missing**.
Land simulation and weather are absent at the comparison base, so there is almost no
measured regression.

| Gap | Size | Rules / deliverable | Ownership |
|---|---|---|---|
| G1 | S | WLEN-01..05: land environment selection, synchronized pick, weather-category pick, per-battle environment state, mode-service placement. | Existing environment-selection bug (legacy EAWR-795) plus land mode adapter; lighting reader duplicate-field bug (legacy EAWR-794). |
| G2 | M | WLEN-06..14: deterministic weather state machine (scenario/pattern timers, ease, 0.5 gate, notification, `Force_Weather`). | Land weather service. |
| G3 | M | WLEN-15..22: weather categories, modifier tables, owner immunity, sight/accuracy terms, weatherproof upgrade, reveal re-evaluation, no speed term. | Land combat inputs (reveal LC-35, aim LC-31/33); abilities walk for the upgrade. |
| G4 | M | WLEN-23..31: emitter, lightning scalar, fog-plane factor, weather audio, icon, weather text. | Land presentation and audio. |
| G5 | S | WLEN-32..37: surface-type decode, closest-vertex and water-plane lookup, stock census acceptance. | Input to land surface modifiers (legacy EAWR-1452). |
| G6 | S | WLEN-38, WLEN-39: steep-to-water, 20-unit cell grid, playable bounds inset. | Land passability (land combat G2) (legacy EAWR-1451). |
| G7 | M | WLEN-40..44: indestructible scenery, obstacle/hard extents, story props, bridges, wind disturbance. | Land scene/object roles. |
| G8 | M | WLEN-45..52: indigenous wandering, explosion flight, civilians, hostile creatures, spawner unit side. | Land indigenous behaviour; spawner admission stays GC tactical (legacy EAWR-2684) for the AI estimate. |
| G9 | S | WLEN-53..56: land marker system and ownership remap order. | Skirmish start for land (land programme) (legacy EAWR-17). |
| G10 | M | WLEN-57, WLEN-58: capturable secondary structures, scripts, survivors, damage alternates. | Land control/build-pad bindings (legacy EAWR-1458). |

The highest-impact items for the first land battle are the weather state machine with its
sight and accuracy terms (G2, G3), the surface-type lookup (G5, so G3 of land combat has
its input), the environment pick (G1) and indigenous behaviour (G8) for maps such as
Felucia and Dagobah. This preparation changes nothing in the space battle.

## Tag-status audit

The registry is per `(object class, tag)` and, at the comparison base, has **no rows** for
the three weather configuration files (`weatherscenarios.xml`, `weathermodifiers.xml`,
`weatheraudio.xml`: `Weather_Scenario`, `Phase`, `Emitter_Intensity`, `Lightning_Intensity`,
`Duration`, `Ease_Out_Duration`, `Fog_Plane_Interpolation_Alpha`, `Sight_Range_Modifier`,
`Speed_Modifier`, `Weapon_Accuracy_Modifier`, `Weather_SFXEvent_Loop`,
`Weather_SFXEvent_Intermittent`) or for `Weatherproof_Ability` and `Apply_To_All_Allies`,
`Describe_Weather` and `Encyclopedia_Weather_*`. They need rows when G2 to G4 land.

| Registry status | Tags and scope |
|---|---|
| `todo` | `Min_Accuracy_For_Icon`, `Min_Sight_Range_For_Icon` (GameConstants); `SFXEvent_Weather_Begin`, `SFXEvent_Weather_End` (Faction); `Destruction_Survivors` on secondary and special structures; `Terrain_Texture_Modifier_Material` on markers and structures. |
| `land-or-galactic` | `Weather_Category` on ground classes and heroes; `Wind_Disturbance_Radius`, `Wind_Disturbance_Strength`, `Wind_Disturbance_Sphere_Alpha`; `SurfaceFX_Name` and the five `*_Ground_Color_Tint` constants; `Can_Indigenous_Unit_Stop`, `Is_Indigenous_Unit_Corruptible`, `Spawned_Indigenous_Units_Type`, `Spawn_Indigenous_Units_Chance`, `Indigenous_Spawn_Destruction_Reward`; `Land_Damage_Thresholds`, `Land_Damage_Alternates`; `Burning_Damage_Per_Second`, `Is_Combustible`, `On_Fire_Speed_Modifier`; `Land_FOW_Reveal_Range`; `Targeting_Fire_Inaccuracy` and its fixed-radius form on ground classes. Their exclusion from the space milestone is not evidence of land support. |
| `applied` or `partial` | `Is_Decoration` / `Is_Discardable` on markers, objects and secondary structures (skirmish start, WLEN-53); `Custom_Hard_XExtent` / `Custom_Hard_YExtent` on unique and special structure classes; capture tags on secondary, space-buildable and special structures. Ground-prop variants are `land-or-galactic`. |
| `foc-ignores` | `Is_Sprite` on infantry and vehicles. |

The terrain-material surface type, the weather files and the weatherproof ability are
read by no existing loader; none of this walk changes a registry row.

## Unverified and capture plan

| Question | What remains unknown | Evidence that would settle it / owner |
|---|---|---|
| U-LEN-01 | Retail-executable equivalence of the dead speed branch (WLEN-20) and of the mud gate (WLEN-35). All inspections are of the debug build. | Run a rain/snow/sandstorm map with a mod-authored speed modifier of 0.5, and a unit crossing Felucia mud and water, recording measured speed per frame; G3/G5. |
| U-LEN-02 | How scenario names are matched (case folding) and the key function. Upper-casing was observed in the modifier parser; the scenario lookup key was not read. | Author a lower-case scenario name in a test TED; observe whether weather loads; G2. |
| U-LEN-03 | Defaults of absent pattern fields (`Fog_Plane_Interpolation_Alpha`, `Lightning_Intensity`, `Ease_Out_Duration`) and the behaviour of an unknown phase name; the pattern XML parser was not read. | Read the pattern map parser; author phases omitting each tag; G2. |
| U-LEN-04 | Who requests the weather-based environment pick and with which category (the mode method is reached through a virtual slot with no direct caller). | Trace the virtual callers or capture which environment a campaign planet's weather text selects; G1. |
| U-LEN-05 | The caller of the steep-to-water conversion and whether the map's own playable volumes replace the default inset bounds. | Trace the virtual call sites at map load; compare a map with and without authored volumes; G6. |
| U-LEN-06 | Weatherproof `Apply_To_All_Allies`: whose list receives the entry for allies (WLEN-21). | Two allied players, one buys the upgrade, compare ally sight/accuracy in rain; G3. |
| U-LEN-07 | Weather state across save/load and the campaign-to-land handoff (save/restore routines exist; their contents were not read). | Save mid-ease, reload, record pattern index and timers; G2. |
| U-LEN-08 | Visible effect of lightning-timer resets every ease frame (WLEN-24) and the relation between lightning flashes and weather type. | Capture flashes in a `Rain_Random` map during an ease and during a hold; G4. |
| U-LEN-09 | Meaning of the service argument that gates indigenous wandering (WLEN-46), the service interval value, and the exact "can stop" default. | Observe a stock unit's walk starts per second; read the behaviour's interval; G8. |
| U-LEN-10 | Which explosion objects trigger the flight (WLEN-48): every explosion object, or only some classes. | Detonate a tank shell, a bomb and an ability explosion near a civilian; G8. |
| U-LEN-11 | Whether FoC's land AI reads weather or terrain modifiers (stock `Terrain_Effectiveness` tables are all 1.0). | Search AI Lua and perception equations for weather inputs; Land AI walk. |
| U-LEN-12 | How prop obstacle behaviour, hard extents and the baked passability grid combine at runtime (WLEN-40, WLEN-43). | Add/remove a prop in a test map and read cell passability before/after load; G7. |
| U-LEN-13 | Whether the weather emitter follows the camera and whether it reads the scene wind; the emitter's own lifetime settings are particle data. The energy write made when a pattern loads (WLEN-09) passes an argument that looks like the pattern record rather than its intensity, so the first-frame energy may differ until the first ease. | Particle walk; capture rain under camera pan with different wind headings; G4. |

No unverified row is an implementation requirement for an invented default. Each gap
ticket must retain its evidence tasks and record the resulting rules before filling the
uncertain branches.
