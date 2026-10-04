# Combat time to kill

The S-43 to S-50 scenarios hold the target still and silent and order the shooter to attack at tick 30. The Acclamator also launches its craft. This table separates each firing entity and weapon, so its fighters and bombers remain distinct. A hit is a projectile contact; the original recorder counted frames with damage, which can contain several hits. Those quantities cannot give a comparable hit rate.

The simulation measurements below use the installed FoC content and the repaired combat reporter. Native timing ranges are the two recordings documented in [the fidelity README](../../tests/fidelity/README.md#time-to-kill-536). The retained private archive supplies native shot counts per hardpoint for seeds 12345 and 4242, and contacts by projectile type and collided mesh for seed 4242. Its original S-40..S-47 IDs map to today's S-43..S-50; all sixteen runs match the current players, map, staging flags, units, positions, facings, orders and durations. Content pins match the recorded files. Native squadron shooters were not labelled: the reported spawn-binding error prevents their individual shot counts, while the target samples and projectile contacts remain available. Native contact records do not identify the firing hardpoint, so its individual hit count stays **unknown**.

Simulation measurement: 2026-10-03, head `ad6ecbd695a8ed33eacd6f5d73c29a1177253b76`, one worker. The squadron object-weapon reference trial in that head leaves capital shield and hull timings unchanged; S-50 has two fewer shots and three fewer contacts than its parent head. It is not evidence that dogfight outcomes agree. Native recordings: 2026-09-28, seeds 12345 and 4242.

Times are logical ticks, 30 per second, measured from scenario start. A dash means that the event did not occur within the scenario window. Torpedoes bypass shields.

| Case | Simulation shield / hull | Native shield / hull | Simulation shots / hits |
|---|---|---|---|
| S-43 nebulon-vs-tartan | 616 / 719 | 635–701 / 779–825 | 130 / 126 |
| S-44 tartan-vs-nebulon | - / - | — / — | 814 / 70 |
| S-45 acclamator-vs-nebulon | 1210 / 1562 | 1032–1122 / 1522–1564 | 452 / 402 |
| S-46 corvette-vs-tartan | 1470 / 2446 | 1353–1472 / 2360–2428 | 620 / 289 |
| S-47 xwing-vs-tartan | 2334 / - | 2426–2442 / — | 486 / 481 |
| S-48 ywing-vs-tartan | bypassed / 1272 | bypassed / 1374 | 24 / 23 |
| S-49 tiebomber-vs-nebulon | bypassed / 2524 | bypassed / 2326 | 53 / 48 |
| S-50 acclamator-vs-station | - / - | — / — | 833 / 552 |

## Per weapon

`entity.N` identifies a launched craft by its stable simulation entity ID. `object` is a fighter’s object weapon; `HP_BOMBER_02` and `HP_BOMBER_03` are bomber weapons. The shield columns describe the target’s state at contact, including shield-bypassing torpedoes. They do not claim which damage pool absorbed the hit. Native shot columns are seed 12345 / seed 4242, recovered from the matched archived runs. Native individual hardpoint hits remain unknown. A missing native count is not zero.

| Case | Firing entity / weapon | Shots | Hits while shield up | Hits while shield down | Native shots 12345 / 4242 |
|---|---|---:|---:|---:|---|
| S-43 | `shooter/HP_Nebulon_Weapon_BL` | 35 | 28 | 7 | 42 / 42 |
| S-43 | `shooter/HP_Nebulon_Weapon_BR` | 35 | 28 | 7 | 42 / 42 |
| S-43 | `shooter/HP_Nebulon_Weapon_FL` | 30 | 25 | 3 | 35 / 32 |
| S-43 | `shooter/HP_Nebulon_Weapon_FR` | 30 | 25 | 3 | 30 / 31 |
| S-44 | `shooter/HP_Tartan_Cruiser_00` | 270 | 21 | 0 | 275 / 276 |
| S-44 | `shooter/HP_Tartan_Cruiser_02` | 270 | 19 | 0 | 271 / 270 |
| S-44 | `shooter/HP_Tartan_Cruiser_03` | 274 | 30 | 0 | 280 / 270 |
| S-45 | `entity.11/HP_BOMBER_02` | 8 | 6 | 0 | unknown |
| S-45 | `entity.12/HP_BOMBER_02` | 8 | 6 | 2 | unknown |
| S-45 | `entity.13/HP_BOMBER_02` | 8 | 6 | 2 | unknown |
| S-45 | `entity.14/HP_BOMBER_02` | 8 | 6 | 1 | unknown |
| S-45 | `entity.3/object` | 24 | 16 | 8 | unknown |
| S-45 | `entity.4/object` | 28 | 18 | 10 | unknown |
| S-45 | `entity.5/object` | 26 | 18 | 8 | unknown |
| S-45 | `entity.6/object` | 28 | 21 | 5 | unknown |
| S-45 | `entity.7/object` | 25 | 19 | 4 | unknown |
| S-45 | `entity.8/object` | 25 | 19 | 4 | unknown |
| S-45 | `entity.9/object` | 22 | 17 | 5 | unknown |
| S-45 | `shooter/HP_Acclamator_Weapon_BC` | 6 | 4 | 2 | 6 / 6 |
| S-45 | `shooter/HP_Acclamator_Weapon_BL` | 41 | 25 | 7 | 39 / 42 |
| S-45 | `shooter/HP_Acclamator_Weapon_BR` | 39 | 27 | 7 | 39 / 42 |
| S-45 | `shooter/HP_Acclamator_Weapon_FC` | 12 | 9 | 0 | 9 / 9 |
| S-45 | `shooter/HP_Acclamator_Weapon_FL` | 72 | 50 | 14 | 72 / 72 |
| S-45 | `shooter/HP_Acclamator_Weapon_FR` | 72 | 48 | 8 | 69 / 72 |
| S-46 | `shooter/HP_Corellian_Corvette_01` | 120 | 32 | 23 | 120 / 120 |
| S-46 | `shooter/HP_Corellian_Corvette_02` | 120 | 37 | 26 | 120 / 120 |
| S-46 | `shooter/HP_Corellian_Corvette_04` | 125 | 35 | 21 | 120 / 116 |
| S-46 | `shooter/HP_Corellian_Corvette_05` | 125 | 37 | 22 | 120 / 123 |
| S-46 | `shooter/HP_Corellian_Corvette_06` | 0 | 0 | 0 | 120 / 125 |
| S-46 | `shooter/HP_Corellian_Corvette_07` | 130 | 32 | 24 | 125 / 125 |
| S-47 | `shooter.1/object` | 108 | 72 | 32 | unknown |
| S-47 | `shooter.2/object` | 105 | 70 | 34 | unknown |
| S-47 | `shooter.3/object` | 104 | 72 | 32 | unknown |
| S-47 | `shooter.4/object` | 88 | 63 | 25 | unknown |
| S-47 | `shooter.5/object` | 81 | 55 | 26 | unknown |
| S-48 | `shooter.1/HP_BOMBER_03` | 8 | 7 | 0 | unknown |
| S-48 | `shooter.2/HP_BOMBER_03` | 8 | 8 | 0 | unknown |
| S-48 | `shooter.3/HP_BOMBER_03` | 8 | 8 | 0 | unknown |
| S-49 | `shooter.1/HP_BOMBER_02` | 14 | 12 | 0 | unknown |
| S-49 | `shooter.2/HP_BOMBER_02` | 13 | 13 | 0 | unknown |
| S-49 | `shooter.3/HP_BOMBER_02` | 14 | 12 | 0 | unknown |
| S-49 | `shooter.4/HP_BOMBER_02` | 12 | 11 | 0 | unknown |
| S-50 | `entity.17/HP_BOMBER_02` | 4 | 4 | 0 | unknown |
| S-50 | `entity.18/HP_BOMBER_02` | 4 | 2 | 0 | unknown |
| S-50 | `entity.19/HP_BOMBER_02` | 2 | 2 | 0 | unknown |
| S-50 | `entity.20/HP_BOMBER_02` | 2 | 2 | 0 | unknown |
| S-50 | `shooter/HP_Acclamator_Weapon_BC` | 20 | 20 | 0 | 18 / 16 |
| S-50 | `shooter/HP_Acclamator_Weapon_BL` | 138 | 103 | 0 | 130 / 48 |
| S-50 | `shooter/HP_Acclamator_Weapon_BR` | 141 | 117 | 0 | 130 / 64 |
| S-50 | `shooter/HP_Acclamator_Weapon_FC` | 36 | 33 | 0 | 12 / 12 |
| S-50 | `shooter/HP_Acclamator_Weapon_FL` | 246 | 125 | 0 | 156 / 149 |
| S-50 | `shooter/HP_Acclamator_Weapon_FR` | 240 | 144 | 0 | 126 / 177 |

## Native projectile contacts

The seed-4242 damage hook records individual calls with their projectile type and collided mesh on the labelled target. These totals are separate from the README's frames with damage. Multiple calls can occur in one frame; a contact is not a distinct damage-pool loss. The seed-12345 batch did not yet have this hook. Projectile groups cannot resolve two guns firing the same projectile, and the simulation currently reports firing entity/weapon rather than projectile type.

| Case | Native projectile | Target contacts | Contacts with shield up / down |
|---|---|---:|---|
| S-43 | `PROJ_SHIP_LARGE_LASER_CANNON_RED` | 71 | 64 / 7 |
| S-43 | `PROJ_SHIP_TURBOLASER_RED` | 53 | 43 / 10 |
| S-44 | `PROJ_SHIP_MEDIUM_LASER_CANNON_GREEN` | 60 | 60 / 0 |
| S-45 | `PROJ_SHIP_CONCUSSION_MISSILE` | 9 | 6 / 3 |
| S-45 | `PROJ_SHIP_LARGE_LASER_CANNON_GREEN` | 122 | 93 / 29 |
| S-45 | `PROJ_SHIP_PROTON_TORPEDO` | 30 | 22 / 8 |
| S-45 | `PROJ_SHIP_PROTON_TORPEDO_LIGHT` | 5 | 4 / 1 |
| S-45 | `PROJ_SHIP_SMALL_LASER_CANNON_GREEN` | 124 | 90 / 34 |
| S-45 | `PROJ_SHIP_TURBOLASER_GREEN` | 67 | 52 / 15 |
| S-46 | `PROJ_SHIP_MEDIUM_LASER_CANNON_RED` | 287 | 172 / 115 |
| S-47 | `PROJ_SHIP_SMALL_LASER_CANNON_RED` | 474 | 333 / 141 |
| S-48 | `PROJ_ION_CANNON_MEDIUM_LASER_BLUE` | 6 | 6 / 0 |
| S-48 | `PROJ_SHIP_PROTON_TORPEDO` | 17 | 17 / 0 |
| S-49 | `PROJ_SHIP_PROTON_TORPEDO` | 48 | 48 / 0 |
| S-50 | `PROJ_SHIP_CONCUSSION_MISSILE` | 9 | 9 / 0 |
| S-50 | `PROJ_SHIP_LARGE_LASER_CANNON_GREEN` | 169 | 169 / 0 |
| S-50 | `PROJ_SHIP_PROTON_TORPEDO` | 6 | 6 / 0 |
| S-50 | `PROJ_SHIP_PROTON_TORPEDO_LIGHT` | 14 | 14 / 0 |
| S-50 | `PROJ_SHIP_SMALL_LASER_CANNON_GREEN` | 157 | 157 / 0 |
| S-50 | `PROJ_SHIP_TURBOLASER_GREEN` | 77 | 77 / 0 |

The native Y-wing run (S-48) includes six ion-bolt contacts in addition to seventeen torpedo contacts. The simulation also automatically activates the ion-shot ability for the fixture's non-human owner (AB-40, AB-68); its override projectile and ordinary torpedoes share the same bomber hardpoint label in the combat log. The grouped weapon row therefore does not establish a missing ion contribution. The log's shield-absorbed contact flag can distinguish ions from shield-bypassing torpedoes while the target's shield remains positive. S-46's native corvette fires weapon 06, which is silent in this simulation. Its other five guns have similar bounded counts. That fire-geometry difference is hidden by aggregate hull timing alone.

Archive provenance: the private `m2-ttk-536/out/rec-536/seed-12345` and `seed-4242` batches dated 2026-09-28. Original IDs S-40..S-47 were renumbered by +3 to avoid a scenario-number collision. Native shot columns and the projectile-contact table come from these matched batches; simulator columns come from the measurement head above. Raw recorder data, injected hooks, addresses, game files and captures remain private.

## Hardpoint deaths

The target's Nebulon-B hardpoints below use their suffix names. A dash means no death in the window. S-44 and S-50 lose no hardpoints. S-43 and S-46 to S-48 have no destroyable target hardpoints.

| Case | Target hardpoint | Simulation death | Native death |
|---|---|---:|---|
| S-45 | Weapon_BL | 1138 | 287–428 |
| S-45 | Weapon_BR | — | 930–1020 |
| S-45 | Weapon_FL | 1139 | 1155–1243 |
| S-45 | Weapon_FR | 1539 | 1361–1519 |
| S-45 | Engines | 1422 | — |
| S-49 | Weapon_BL | 1747 | 1517–1518 |
| S-49 | Weapon_BR | 1362 | 1919 |
| S-49 | Weapon_FL | 957 | 671 |
| S-49 | Weapon_FR | 1403 | 1495 |
| S-49 | Engines | — | — |

## Remaining differences

S-43 destroys the Tartan sooner than either native recording. S-45’s final hull time is close, but its first shield depletion is later and its hardpoint death order differs: the simulation’s BL/FL/FR/engine deaths are 1138/1139/1539/1422, while native BL/BR/FL/FR deaths are 287–428/930–1020/1155–1243/1361–1519. The intact BR and destroyed engine distinguish the damage distribution even when the final hull time agrees.

S-46’s shield timing lies within the native range and hull death is 18 ticks after its upper end. S-47’s shield empties earlier. S-48’s hull death is earlier and S-49’s is later; their run geometry and hardpoint distribution still differ. S-50 has no shield or hull death in either bounded recording. The existing S-45/S-46 timing contracts and the S-45/S-49/S-50 hull-to-hardpoint coupling contracts remain enforced.

Known geometry gaps remain in [weapon fire](space-weapon-fire.md) and [fighter flight](space-fighters.md). The native target-member reference is the firing craft’s parent squadron centre (WWP-49); that centre uses the union of world model bounds (WSQ-48). A lone object weapon’s nearest-member lookup still uses its object position rather than its muzzle (G-03). These are separate from the confirmed doubled hardpoint scatter and per-mesh capital collision rules (DG-24, DG-36 to DG-39), which must remain intact. Missing native per-weapon samples and unverified flight details do not justify adjusting damage, health or scatter to force a timing match.
