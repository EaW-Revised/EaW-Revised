# Start to main menu (opt-in timeline)

`--eawr-menu-timeline` (a viewer user argument, after `--`) prints how the start to the interactive main menu spends its
time. Without it every call returns before it reads a clock, the process or the engine.

| line | content |
|---|---|
| `EAWR menu_mark <label> t_ms=… private_mib=… peak_commit_mib=… ws_mib=… peak_ws_mib=… unix_us=…` | wall clock since the process was created (the viewer reads the creation time, so engine start-up is inside `t_ms`), private commit and its peak, working set and its peak |
| `EAWR menu_frame n=… dt_ms=…` / `menu_hitch` (> 100 ms) / `menu_responsive` / `menu_frames_summary` | the front end's presented frames: the first five, every hitch, the first frame after 30 consecutive frames under 50 ms, and a summary of the first 600 |
| `EAWR menu_costs {…}` | the load-phase table (`core/load_profile.hpp`) and the per-layer cost, cumulative, at `setup_ready`, `front_end_ready`, `attract_start_end`/`attract_ready` and frame 600 |

Marks, in order: `timeline_begin`, `host_ready_options`, `host_ready_profile`, `fonts_provisioned`, `setup_construct`,
`setup_load_begin`, `manifest_chain_resolved`, `vfs_mounted`, `catalog_loaded`, `lobby_defaults_read`, `maps_listed`,
`dialog_catalog_loaded`, `text_database_loaded`, `ui_atlas_loaded`, `setup_loaded`, `setup_built`, `setup_ready`,
`fe_text_database`, `fe_built`, `fe_campaign_screen`, `fe_audio`, `front_end_ready`, `attract_start_begin`,
`battle_stage0..4` (the attract battle's own load stages), `attract_start_end`.

Per layer (`menu_costs.layers`): `loose_ms`/`loose_files` (the loose-tree walk of the mount), `meg_ms`/`meg_entries`
(MEG header parse), `xml_registry_ms`/`xml_registry_files` (registry-listed XML: parse and definitions),
`xml_inventory_ms`/`xml_inventory_files` (the second parse of every other effective XML, which the viewer's catalog loads no longer do; the tools still do) and `xml_bytes`.
`catalog_registry`, `catalog_inventory`, `catalog_index`, `vfs_loose_walk` and `vfs_meg_parse` are the matching phases.
Phase times are exclusive of their nested phases; the reads that the layer timers do not cover (opening a registry
file) are in the phase and not in the layer.

Pair it with `--eawr-memory-attribution` for the `load_mark` lines and `--eawr-attract off` to measure the menu without the
demo battle behind it.

## Baseline and lever A (PF, 2026-10-27)

Host: the GTX 970 rig (i7-4790K, 16 GB, game and mods on a 5400 rpm HDD), official Godot 4.7.2 with the **Separate render
thread** (explicit `--render-thread separate`; the engine log prints the separate-thread warning), viewer build of the lever A
head (`0139056db`, the code of this change). Remake = 3229239424;2794270450;1770851727, RaW = 1129810972. "Cold" purges the
standby list first; "warm" is the run that follows it. Seconds since process creation; one run per cell unless a second pair
is listed. The `whole` rows read every map whole to decide eligibility (`--eawr-map-list-whole-read`, the behaviour before
this change); the `prefix` rows are the default.

| run | engine | fonts | mount | catalog | maps | ui | ready | attract load done | responsive | GiB at ready | peak commit GiB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| stock cold, whole | 2.5 | 3.2 | 4.0 | 6.6 | 30.8 | 32.4 | 33.6 | 87.0 | 88.9 | 0.94 | 1.52 |
| stock cold, prefix | 2.8 | 3.5 | 4.2 | 6.8 | 13.6 | 15.1 | 16.0 | 70.0 | 71.8 | 0.94 | 1.53 |
| stock warm, whole | 1.3 | 1.7 | 2.0 | 2.5 | 3.3 | 4.1 | 5.0 | 26.4 | 28.0 | 0.94 | 1.53 |
| stock warm, prefix | 1.4 | 1.8 | 2.1 | 2.6 | 3.3 | 4.1 | 5.2 | 27.1 | 28.9 | 0.94 | 1.53 |
| RaW cold, whole | 2.9 | 3.6 | 14.5 | 19.2 | 112.4 | 115.3 | 116.8 | 262.8 | 367.3 | 1.28 | 3.64 |
| RaW cold, prefix | 2.4 | 3.1 | 10.8 | 15.9 | 37.3 | 40.3 | 42.0 | 176.4 | 265.4 | 1.28 | 3.65 |
| RaW warm, whole | 1.3 | 1.6 | 2.3 | 3.3 | 4.8 | 5.6 | 6.6 | 65.6 | 70.4 | 1.28 | 3.65 |
| RaW warm, prefix | 1.4 | 1.6 | 2.5 | 3.6 | 4.7 | 5.3 | 6.1 | 66.3 | 70.0 | 1.28 | 3.65 |
| Remake (attract off) cold, whole, pair 1 | 2.6 | 3.2 | 10.4 | 21.5 | 77.1 | 85.4 | 89.4 | - | 89.9 | 2.79 | 2.86 |
| Remake (attract off) cold, prefix, pair 1 | 2.7 | 3.4 | 25.0 | 45.3 | 72.1 | 80.5 | 84.7 | - | 85.2 | 2.80 | 2.86 |
| Remake (attract off) cold, whole, pair 2 | 2.3 | 3.0 | 22.2 | 33.5 | 87.7 | 95.9 | 99.7 | - | 100.2 | 2.79 | 2.85 |
| Remake (attract off) cold, prefix, pair 2 | 2.2 | 2.9 | 12.0 | 23.3 | 41.8 | 50.0 | 53.7 | - | 54.0 | 2.79 | 2.85 |
| Remake (attract off) warm, whole, pair 2 | 1.4 | 1.7 | 3.4 | 6.2 | 9.4 | 11.2 | 12.2 | - | 12.7 | 2.80 | 2.86 |
| Remake (attract off) warm, prefix, pair 2 | 1.3 | 1.6 | 3.3 | 6.2 | 9.2 | 11.0 | 12.0 | - | 12.4 | 2.80 | 2.86 |

What the table shows:

- The data load alone is small when warm (stock 5 s, Remake 12 s) and large when cold on a hard disk. Cold, the map list is the
  biggest stage with the whole-file read (stock 24 s, RaW 93 s, Remake 54-56 s between `catalog` and `maps`); the header-prefix
  read shortens it to 7, 21 and 19-27 s. Warm, the two reads are the same within noise.
- The mount and catalog stages of the Remake cold runs vary by 10-25 s between runs on this drive (pair 1's prefix run had the
  slower mount and catalog), so the Remake totals move by 5 to 46 s while the map-list stage is 2-3 times shorter in both pairs.
- The main-menu attract battle is still the largest cost after the menu is up: a full `MapMode` load of the mod's demo map
  run on the main thread inside the menu's first frame (stock 27 s warm, RaW 66 s warm, from `ready` to `attract load done`),
  during which the menu does not respond. This change does not touch it.
- Remake's bare menu holds 2.8 GiB.

## Levers measured or in flight (legacy EAWR-5197)

See the PR description; the milestones above are the baseline every lever is compared with on the same host.
