# Hard numeric limits audit

Audit of (legacy EAWR-4721) (owner request: find hard numeric limits that deviate from retail in a bad way). Read-only: no code change, build or run. Baseline: `a632b9e94` (origin/main, 2026-11-23). It revalidates the first audit (2026-10-23, 7 harmful rows C01 to C07, 36 unknown rows U01 to U36), drops what has since been fixed, and adds data evidence. Complements the [data-assumption audit](data-assumption-audit.md) (fixed lists and path assumptions); this one is numbers.

Grades (from the ticket): **A** matches retail (sourced), **B** harmless, **C** deviates and hurts, **U** unknown (needs a debug-build walk or capture). Retail columns say "not verified" unless a behaviour doc is cited. An unknown retail maximum is not evidence that retail is unlimited.

## Data evidence used

Maxima of authored values, measured with a text scan of the effective data (no engine run). "Stock" = effective FoC data; "Remake 4.0" = local research copy of that mod's effective data (not committed, nothing copied here).

| Tag | Stock max | Remake 4.0 max |
|---|---|---|
| `Max_Squad_Size` | 9 (ground companies) | 200 (several space squadrons) |
| `Population_Value` | 15 | 300 |
| `Build_Cost_Credits` | 45,000 | 1,500,000 |
| `Tactical_Health` | 999,999 (structures) | 7,999,999 (props); 1,500,000 largest ship |
| `Fire_Range_Distance` | 20,000 | 2,000,000,000 (one superweapon hardpoint); next 50,000 |
| `Projectile_Max_Flight_Distance` | 150,000 | 999,999 |
| projectile `Max_Speed` | 400 | 2,850 |
| `Fire_Min_Recharge_Seconds` | 18 | 60 |
| `Max_Build_Queue` | not authored | not authored |

The follow-up scan reads authored numeric XML values after removing XML comments; stock and Remake refer to their effective data trees. Remake is an overlay reference, so unchanged stock values can be the maximum. The Remake reference excludes models, textures, maps and audio; the separate loose Workshop texture scan does not cover packed assets. These are data comparisons, not proof of retail equivalence.

Other mod evidence comes from ticket comments (Remake backdrops 479,578 to 1,758,454 units, (legacy EAWR-5704); Eclipse beam range 500,000, (legacy EAWR-4629); 186-craft roster, (legacy EAWR-4623); 60 ships per side, (legacy EAWR-4719)).

## Done since the first audit (drop from the open list)

| Old row | Limit | Status on this baseline | Evidence |
|---|---|---|---|
| C01 | Shader global-variable buffer | Set to 1,310,720 slots (20 MiB, 81,920 instance blocks of 16 slots) | (legacy EAWR-4719); `apps/viewer/project/project.godot:34` |
| C02 | Model-projectile slots per type (32) and effect pool | Pools grow; model-slot growth is stepped (`growth_step_cap` 128 above 512 slots) | (legacy EAWR-4869), (legacy EAWR-4883); `include/eawr/presentation/live/projectiles.hpp:98-99` |
| C03 | Energy beam range bound 65,536 | Fixed | (legacy EAWR-4882) |
| C04 | Purchase queue literal 5 | Read from the constant, default 5 (`src/skirmish/economy.cpp:803`) | (legacy EAWR-7274) |
| C06 | Team split by `Max_Squad_Size` | Squadron validation carries `max_squad_size` (`src/sim/tactical/fighters.cpp:77`) | (legacy EAWR-4845) |
| C07 | Rocket path 2,048 controls / 131,072 visits | Default policy is unbounded (`src/sim/tactical/rocket_internal.hpp:15`) | (legacy EAWR-4872) |
| U04 (grid part) | 4,096 collision-mesh grid | Per-mesh adaptive grid up to 2^18 | (legacy EAWR-4809) ((legacy EAWR-4806)) |
| U04 (oversized props) | Props beyond 2^18 refused | Load without a collision box | (legacy EAWR-5704), (legacy EAWR-5841) (`src/units/unit_combat.cpp:926-940`) |
| U01/U02 | 255 hardpoints and weapons per type | Now 65,535 | (legacy EAWR-4913) (`durability.hpp:159`, `combat.hpp:520`) |
| U03 (health) | Health bound 2^20 | Now 2^28; stock and Remake maxima (7,999,999) fit | `durability.hpp:167` (PR not identified) |
| "10 ships of one type" | Viewer purchase slots (10) | Warm-pool size only; growth supported | (legacy EAWR-4160), (legacy EAWR-4201); `apps/viewer/src/live_session_options.hpp:153` |
| (legacy EAWR-1707) | 64 craft / 64 entries / 255 bays | Removed | (legacy EAWR-4623) |
| (legacy EAWR-4718) | Two-ability cap | Removed | (legacy EAWR-4718) |

## Open table

Columns: limit | file:line | value | what can exceed it | grade | proposed action and owning lane. "Lane" names: SK (skirmish/sim), GC (galactic), PF (performance/viewer), MM (menus/modding), BK (backlog).

### Slice 1: sim and units (src/sim/tactical, src/units, include/eawr/sim/tactical)

| Limit | file:line | Value | What can exceed it (evidence) | Grade | Action / lane |
|---|---|---|---|---|---|
| Combat distance bound (ranges, flight distance, coordinates) | `include/eawr/sim/tactical/combat.hpp:522`; checks `src/sim/tactical/combat.cpp:98`, `:46`; `abilities.cpp:451-534`; `projectiles.cpp:202-206` | 2^18 = 262,144 units | Remake: one hardpoint `Fire_Range_Distance` 2,000,000,000; `Projectile_Max_Flight_Distance` 999,999 on several projectiles. Stock max 150,000 fits. Whether load rejects the object, the weapon or clamps was not traced here | C (likely; load result not run) | Trace what the lenient loader does with a range above the bound. If it drops the weapon or type, clamp or widen the range domain (a range above the map size behaves as "unlimited"). Retail clamp: not verified. SK |
| Combat power, sphere modifier, mesh extent | `combat.hpp:282` (4,096 mesh extent), `:528` (256 sphere modifier) | 4,096 / 256 | Scaled Remake capitals and props: Remake backdrops reach 1.76 M units (see (legacy EAWR-5704), handled by dropping the box). Extent 4,096 vs scaled ships: not measured | U | Measure mesh extents of the largest Remake ships; probe at 4,097. SK |
| Recharge | `combat.hpp:523` | 360,000 hundredths (3,600 s) | Max authored 18 s stock, 60 s Remake | B | none |
| Damage curve points, types, multiplier | `include/eawr/sim/tactical/damage.hpp:21,22,27` | 64 / 1,024 / 256 | Stock `Damage_Types` 99 entries, 53 `Armor_Types`, 2,426 `Damage_To_Armor_Mod` rows, maximum multiplier 10, `Diminishing_Firepower` maximum 5 points; all in `data/xml/gameconstants.xml`. Remake effective overlay of that file: 49 types, 41 armor types, 673 rows, maximum multiplier 10, 1 curve point. Per-projectile and ability-side use of the multiplier not scanned | B (data fits; retail U) | Retail bounds remain unverified. SK |
| Ability frames / multiplier | `abilities.hpp:455` (30*3600 frames), `:459` (64) | 1 hour / x64 | Stock `Mod_Multiplier` maxima by ability: cause damage 3 (power to weapons, `data/xml/groundcompaniesempire.xml`), speed 2.5, take damage 0.75, shield regeneration minimum -25 (the stock maximum is 3), energy regeneration 5; Remake speed 4 (`data/xml/units/ground/empire/vehicle_itt.xml`), the rest equal. Two authored 600 values, both `CAUSE_DAMAGE_MULTIPLIER` on lucky shot (`data/xml/containers.xml` line 126 stock and 129 Remake; `data/xml/units_hero_rebel_lukeskywalker.xml` line 438, both trees), are exempt from the 64 bound (`abilities.cpp:660-666`, checked at impact). Duration tags: stock `Expiration_Seconds` 180 and `Defense_Duration_In_Secs` 35; Remake `Recharge_Seconds` 999,999 in `data/xml/units/space/units_hero_empire_181st_fighter_wing_bu.xml`, which the loader saturates to one hour (`src/units/unit_abilities.cpp:36-40`). Remake `Defense_Duration_In_Secs` 999,999,999,999,999 in `data/xml/units/space/units_hero_minors_sith_nihilus.xml`: the value does not saturate: it fails the fixed-point decimal parse (`src/sim/math/fixed.cpp:173-178`, overflow above about 5.96e11 seconds), so the special-ability reader reports it as not a decimal and returns 0 (`src/units/unit_tables_abilities_special.cpp:305-319`), giving 0 frames and a load report instead of one hour (lines 25-29 of that file are the heal and light interval frame count, not this tag); static trace only, load not run. | C (one Remake value exceeds the parser; drops to 0, not clamped) | Decide whether an out-of-range duration should saturate; check the load report in a run. Retail bounds remain unverified. SK |
| Lure pulse clamp | `src/units/unit_abilities.cpp:338-339` | counts above int32 max become 0 | Stock and Remake effective XML maximum `Max_Number_Of_Pulses` is 40 in `data/xml/spaceunitsfighters.xml`. | B (data fits; retail U) | Check retail behaviour beyond the observed data. SK |
| Ion stun frames, reduction | `include/eawr/sim/tactical/ion.hpp:33,39` | 1 hour / 1,024 | Stock maxima: 30 s, 0.999 speed and 0.8 shot-rate reduction; Remake maxima: 900 s, 0.999 and 0.8 respectively; all in `data/xml/projectiles.xml`. | B (data fits; retail U) | Retail bounds remain unverified. SK |
| Fighter spin seconds | `include/eawr/sim/tactical/fighters.hpp:28` | 60 s | Stock `Spin_Away_On_Death_Time` max 2 s in `data/xml/groundvehicles.xml`; Remake max 5 s in `data/xml/units/space/units_space_neutral_cis_tri_fighter.xml`. | B (data fits; retail U) | Retail bound remains unverified. SK |
| Idle-cell search | `fighters.hpp:107` | 64 examined cells | Crowded fighter idle in large rosters (Remake squadrons up to 200 craft) | U | Retail fallback for a crowded idle cell: not verified; needs debug-build walk. SK |
| Motion domain | `include/eawr/sim/tactical/motion.hpp:133-140`; `src/sim/tactical/motion.cpp:141` | arc 180 deg, 360 rotations, 2^16 re-eval frames, 2^20 expansions, 16 tries, 1,024 windows/delay, 64 arc steps | Search budgets, not data limits; Remake projectile `Max_Speed` 2,850 vs motion-rate bound: not traced (the first audit's rate bound 1,024 lives in `motion.hpp`; line not located this run) | U | Confirm that projectile speeds do not pass through the unit-motion rate check; if they do, widen. SK |
| Land path restarts / expansions | `src/sim/tactical/land_path_internal.hpp:15` (3 restarts) | 3 | Crowded land maps | U | Walk retail retry behaviour. SK |
| Collection split depth | `src/sim/tactical/collection.cpp:15` | 17 (CO-09) | Documented rule; not a unit cap | A | none |
| Economy credits | `src/sim/tactical/economy.cpp:83` | 2^32 whole credits | Max authored build cost 1.5 M (Remake) | B | none |
| Population share denominator (C05) | `include/eawr/sim/tactical/economy.hpp:505`; `src/sim/tactical/economy.cpp:623-627` | 720,720 | Rosters that do not divide it, e.g. 17 craft: aggregate rounds above 720,720 and the squadron is charged one extra population. Remake squadrons up to 200 craft, `Population_Value` up to 300, make this common | C | (legacy EAWR-753) stays open: keep exact per-squadron shares, round once (retail PU-21 in [space-purchasing.md](../behaviour/space-purchasing.md)). SK |
| Tactical player / seat / unit / command / tick caps | `include/eawr/sim/tactical/types.hpp:27-33` | 64 players; 256 controller seats; 1,024 units per command; 1,000,000 units, commands, ticks | A command of 1,025 selected units is refused (Remake fleets pass 1,000 only in extreme late game, not measured); 1,000,000 ticks = 9 h 15 min at 30 Hz | U | Probe a 1,025-unit order; decide chunk or widen. Live-lifetime stop at 1 M ticks: decide whether to lift. SK |
| Match seats | `include/eawr/sim/seat.hpp:10` | 9 | Stock map maximum and retail seat limit not verified | U | Verify stock map positions and retail seat limit. SK |
| Fog | `include/eawr/sim/fog.hpp:24-26`; `src/sim/tactical/fog_cells.cpp:18-19` | 4,096 per side, 64 grids, 16 Mi cells, service period 65,536 | Allocation safety; retail layout is 512 per side (V-18) | B | none |

**Not found as limits on this baseline:** `max_queue` literal, `max_hardpoints` 255, rocket 2,048-control ceiling.

### Slice 2: skirmish, galactic, scripts, save, network

| Limit | file:line | Value | What can exceed it (evidence) | Grade | Action / lane |
|---|---|---|---|---|---|
| Galactic graph | `src/sim/galactic/graph.cpp:70-71,104` | 4,096 planets; 500,000 routes; 1,000,000 directed edges | Planet definitions per file: stock 65 (`data/xml/planets.xml`), Remake 208 (`data/xml/planets_legacy.xml`; no campaign loads more than the definitions it names, not traced). Campaign `Trade_Routes` entries: stock 294 (`data/xml/campaigns_underworld_gc.xml`), Remake 567 (`data/xml/campaigns_imperial_web_way_4grand.xml`); directed edges come from planet pairs, not route count (`src/sim/galactic/graph.cpp:94-108`, every pair within distance or on a lane): at 208 planets at most 208 x 207 = 43,056 directed edges, under 1,000,000 (`:104`) | B (planets, routes and edges fit) | None. GC |
| Galactic ids/ticks | `include/eawr/sim/galactic/types.hpp:1335-1338` | 100,000,000 ticks; 1,000,000 objects; 64 players; 256 seats | Long campaigns; object churn | U | Object-churn probe over a long campaign. GC |
| AI cost packing | `src/script/foc/galactic_ai_execution.cpp:18-25` | two 32-bit halves; 65,535 sentinel for missing distance | (legacy EAWR-4312) reported failure at 350 planets / 14 factions in the older 16-bit packing. Current code uses 32-bit halves; whether (legacy EAWR-4323) is closed was not checked | U (may be fixed) | Re-run the 350/14 probe or read (legacy EAWR-4323); if fixed, move row to done. GC |
| AI instance namespace | `src/script/foc/ai_engine.hpp:58-84` | 64 galactic id slots; first plan instance 100,000 | 64 slots matches the player cap; namespace exhaustion not exercised | U | none until churn probe above |
| Fleet rows / templates per slot | `src/sim/tactical/replay.cpp:405` (1,024 fleet rows) | 1,024 | Large reserves | U | 1,025-row probe. SK |
| Story trigger depth | `include/eawr/sim/galactic/story_engine.hpp:51-52` | branch reset 3 (WST-24, A); trigger depth 128 | Deep acyclic plots | U (128), A (3) | Walk retail recursion policy. GC |
| Invasion transports | `src/campaign/game_session_lifecycle.cpp:366` | 127 (WGP-136) | sourced | A | none |
| Reward drain rounds | `src/campaign/game_session.cpp:207` | 64 per step | Waits for next step | B | none |
| Offer pool references | `include/eawr/sim/galactic/offer_pool.hpp:19` | 4,194,304 | not measured | B | none |
| GUI clicks per tick | `include/eawr/sim/galactic/commands.hpp:310` | 8 | scripted GUI automation only | B | none |
| Checkpoint size | `include/eawr/checkpoint/checkpoint.hpp:25-26`; `core/checkpoint_limits.hpp:9` | 1 GiB total (now); 64 sections; tactical archive 64 MiB (`src/sim/tactical/checkpoint_archive_internal.hpp:23`) | Large late-match worlds with Lua state | U | Size a large saved world (Remake, 60 per side) against 64 MiB. GC / SK |
| Replay / fog sidecar | `include/eawr/sim/replay.hpp:20-23`; `include/eawr/sim/tactical/replay.hpp:55`; `include/eawr/sim/fog_sidecar.hpp:29-32` | Replay: 256 MiB and 1,000,000 each for entities, commands and ticks. Sidecar: 256 MiB, 1,000,000 events and final tick, 1 GiB evidence grid bytes | Same ticks bound as above | U | as ticks row |
| Lockstep session | `include/eawr/net/lockstep_session.hpp:21-24`; `include/eawr/net/setup.hpp:22` | 1 MiB batch, 1,024 commands per batch, 1,000,000 ticks, 1 MiB setup | Large rosters in setup; 1,025-unit order | U | Measure encoded Remake roster size. MM |
| Net misc | `src/net/lobby_session.cpp:25-28`; `src/net/relay_menu.hpp:28`; `src/net/desync_monitor.cpp:13` | names 64, chat 512, 1,024 roster | not data-reachable | B | none |
| Script host | `src/script/script_host_internal.hpp:25-27` | 4,096 coroutines; 1,000,000 queued; 10,000,000 instructions per operation | Mod AI scripts with many threads: not measured | U | Count peak coroutines in a 60-per-side Remake battle. SK |
| Lua parse/persist | `src/script/pglua.cpp:14-17`; `src/script/sflua/sflua_persist_internal.hpp:75-79` | 64 MiB input; 16 MiB string; depth 128; 1 M entries; stack 2^20; table pairs 2^24; prototype depth 200 | Largest effective Lua file is 144,420 bytes in both stock and Remake: `data/scripts/story/story_underworld_actiii_m11_land.lua`. File size fits the input cap; runtime table sizes and nesting are not measured. | U | Check runtime table sizes and nesting. SK |
| Authoritative bindings | `src/script/authoritative/bindings.cpp:26`; `include/eawr/script/authoritative/scheduler.hpp:107` | value depth 16; list depth 64 | Nested Lua tables in mod scripts | U | none until a mod script is found that exceeds it |
| Goal state rows | `include/eawr/script/foc/galactic_ai_state.hpp:15-16`; `src/script/foc/galactic_ai_outbox_state.cpp:22` | 262,144 rows; 1,048,576 nodes; 4,096 slices | Long campaigns | U | as checkpoint row |
| Decimal / number formatting | `include/eawr/script/numeric/decimal.hpp:48-50` | precision 1,100 digits | n/a | B | none |
| Mods / VFS / assets | `src/vfs/vfs.cpp:29`; `src/vfs/vfs_archive.cpp:11-12`; `src/assets/asset_internal.hpp:12-14`; `include/eawr/mods/chain.hpp:20-21` | 2 GiB per asset; 4,000,000 records per archive; 512 MiB per file; 16 Mi elements; 2,048 chain entries | Remake pack size 22 GB overall; per-asset sizes not measured | U | Check the largest asset and archive record count in Remake. MM |
| Texture pixels | `src/assets/texture.cpp:17` | 8,192 x 8,192 = 67,108,864 pixels | Stock (all unpacked archives and loose files, 4,698 DDS and 4 TGA) maximum 2,048 x 2,048 in `data/art/textures/mt_commandbarcompressed.dds`. Remake: loose Workshop files, 4,955 DDS-named files, maximum 4,096 x 4,096 in `Data/Art/Textures/w_hoth_mountains_b.dds`; 113 files named TGA (two of them, `W_Stars_Nebula_Cloud_*.tga`, are PNG data of 2,048 x 2,048, and the loader dispatches on content, `texture.cpp:193`); the largest real TGA is 4,096 x 4,096 (`MT_COMMANDBAR.TGA`); 466 PNG headers read, largest 8,192 x 4,096 = 33,554,432 pixels (`W_Galaxy_Starfield_1.png`); the one DDS packed in the Remake patch archive is 512 x 512. 20 Remake JPEG files, no BMP: the largest, `Data/Art/Textures/W_Land_Terrain_Snow_Valley.jpg`, is exactly 8,192 x 8,192 = 67,108,864 pixels, equal to the limit (zero margin; the JPEG path rejects only pixels above it, `texture.cpp:42`). | B (data fits; retail U) | Retail bound remains unverified. PF |
| Launcher instances | `apps/launcher/src/instance.hpp:14` | 8 (manager cap 4) | owner decision | B | none |

### Slice 3: presentation and viewer

| Limit | file:line | Value | What can exceed it (evidence) | Grade | Action / lane |
|---|---|---|---|---|---|
| Healing lightning records | `apps/viewer/src/battle_effects_impacts.cpp:45,619` | 256 live bolts; new records skipped when full (still open, ticket comment on (legacy EAWR-4883)) | Many healers in a late-match fleet | C (known open) | Make the bolt list grow with demand (same pool policy as (legacy EAWR-4883)). PF |
| Draining samples | `battle_effects_impacts.cpp:47`; `apps/viewer/src/debris_props.cpp:28`; `apps/viewer/src/unit_emitters_frame.cpp:31` | 300 samples | Effects authored with a tail longer than 300 samples | U | Scan longest authored effect lifetime. PF |
| Shield samples, spawn logs, audio log | `battle_effects_impacts.cpp:49` (64); `battle_effects_internal.hpp:24` (8,192); `apps/viewer/src/battle_audio_events.cpp:64` (256) | report retention only | n/a | B | none |
| Debris rows | `apps/viewer/src/debris_props.cpp:30` | 1,024 rows | Mass death in a large battle | U | Count per-battle debris spawns in a 60-per-side run. PF |
| First hits / fading log | `apps/viewer/src/live_session_frame.cpp:53,55` | 4,096 / 8,192 | log retention | B | none |
| Map-mode particles | `apps/viewer/src/map_mode.hpp:125` | 8,192 aggregate; attached share 256 | dense maps; explicit budget only | U | Probe 8,193 placements. PF |
| Particle spawn event budget | `src/presentation/particles/cpu_system.cpp:1062` | 100,000 per update | extreme rates | U | Follow the dropped counter on a large fleet. PF |
| Particle capacity per emitter | `include/eawr/presentation/particles/prewarmed_capacity.hpp:155`; `:80-81` | 5,003 per emitter | sourced (PL-02/PS-18) | A | none |
| Particle reuse / prewarm | `src/presentation/godot/particle_reuse.hpp:98-99`; `particle_prewarm.hpp:66-67,139` | 128 emitters / 32 MiB; 32 emitters / 8 MiB; 32 programs | cold allocation only | B | none |
| Hangar warm slots | `apps/viewer/src/hangar_warm_plan.hpp:25` | 32 | Remake squadrons of 200 craft | U | Confirm growth covers it (the purchase path does). PF |
| Purchase warm slots | `apps/viewer/src/live_session_options.hpp:153` | 10 (CLI 1..64), growth 8 pairs per frame | later growth works | B | Residual: non-purchase arrivals, backlog beyond 8 per frame; probe. PF |
| Audio voices | `include/eawr/presentation/audio/sfx.hpp:320-321` | 32 3D, 16 2D (SND-07, BA-04..09) | sourced | A | none |
| Speech FIFO | `include/eawr/presentation/audio/announcements.hpp:68` | 256 | scripted bursts | U | Retail queue admission: not verified. PF |
| Audio waiting bytes | `apps/viewer/src/battle_audio_prepare.cpp:1108` | 96 MiB | not measured | U | none |
| HUD selection cards / queue slots / hero slots / pool rows | `src/presentation/ui/hud_shell.cpp:105,115`; `include/eawr/presentation/ui/hud_shell.hpp:349`; `.../ui/hero_tray.hpp:26`; `.../ui/production.hpp:228` | 48 / 10 / 11 / 5 | Layout-defined (component names run to _47, tqueue09, hero 00-10); mods that change the layout or the queue constant exceed them | U | Retail overflow behaviour: not verified. With a queue constant above 10, entries beyond the 10 queue slots need an overflow rule (build tray, (legacy EAWR-7965)). MM |
| Renderer shader cache | `include/eawr/presentation/godot/renderer.hpp:44-45` | 256 entries, 2 MiB | cache miss only | B | none |
| Material samplers / uniform bytes | `src/presentation/godot/renderer_uniforms.cpp:24-28` | 5 samplers, 16,384 bytes | Mod shaders; retail domain: not verified | U | Scan Remake shaders for samplers per material. PF |
| Texture mip drop | `src/presentation/godot/texture_budget.hpp:27` | 4 | quality only | B | none |
| Fog harness | `apps/viewer/src/space_fog.hpp:22`; `src/presentation/space/fog_field.cpp:17` | 16 names; 4,096 cells | harness / allocation | B | none |
| Terrain chunk | `include/eawr/presentation/terrain/terrain.hpp:31` | 128 cells | not measured | U | none |
| Planet UI occluders / thumbnails | `apps/viewer/src/galactic/planet_ui_depth.hpp:33`; `planet_thumbnails.cpp:62,184` | 6 / 256 px / 4 loads | n/a | B | none |

### Slice 4: Godot project settings

| Setting | file:line | Value | Exceeded by | Grade | Action / lane |
|---|---|---|---|---|---|
| `rendering/limits/global_shader_variables/buffer_size` | `apps/viewer/project/project.godot:34` | 1,310,720 slots = 81,920 instance blocks | (legacy EAWR-4719) saw 2 M errors at 60 per side in the old setting; whether 81,920 instances covers the largest Remake late-match fleet (instances per ship are many: hull, turret and effect nodes) is not measured | U | Dense-fleet run counting instance uniforms at peak. PF |
| Other `rendering/limits/*`, `physics/*`, multimesh and canvas limits | not set in the project file (engine defaults) | engine defaults | Not audited: engine defaults need the exact shipped engine version | not audited | Inventory the patched engine's defaults (lights, decals, reflection probes, spatial indexer). PF |

## Not yet audited

- Engine defaults for `rendering/limits/*` and physics queues (only the project file was read, see slice 4).
- Integer widths in event structures and serializers (`uint8` enums in `sim/tactical/types.hpp` are enum bases, not counts; the old note that some hardpoint indices are encoded in `uint8` was not re-checked after (legacy EAWR-4913)).
- The remaining `apps/` tools (path_bench, sim_headless, launcher) beyond the constants listed.
- Retail equivalence for every U row: needs debug-build walks or captures; this audit cites none.
- Remaining data scans: debris counts, effect lifetimes and shader samplers (stock and Remake shaders ship as compiled effect files, so a text scan cannot count samplers). Damage tables, ability fields, stun, spin, lure counts and texture headers were scanned above; runtime effects and retail limits remain unverified.
- Line numbers for the motion rate and coordinate bounds (`motion.hpp`) and `rocket.cpp` depth constant were not relocated.

## Proposed follow-ups (for the coordinator to file; not filed here)

1. **Range domain above 2^18** (Remake superweapon range 2,000,000,000; flight distance 999,999): trace load result, then clamp or widen. SK.
2. **(legacy EAWR-753) population share**: exact per-squadron shares. SK.
3. **Healing lightning cap 256**: grow with demand. PF.
4. **Shader instance budget**: measure peak instance uniforms on a dense Remake fleet. PF.
5. **Late-game size of saves, replays and lockstep batches** against 64 MiB / 1 MiB / 1,024 commands. SK, GC, MM.
6. **Data-scan sweep** of the U rows with cheap tag scans (damage tables, durations, textures, shader samplers). BK.
7. **Re-check (legacy EAWR-4312)/(legacy EAWR-4323)** AI cost packing at 350+ planets. GC.

Refs (legacy EAWR-4721).
