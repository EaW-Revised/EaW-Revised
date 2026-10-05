# M2 skirmish lock

**Scope:** M2 skirmish lock. **Date:** 2026-09-25;
owner starting-force, replenishment and AI-role answers applied 2026-09-26;
SK-04 corrected and the FoC tactical AI host status added on 2026-09-28; SK-30 and SK-31 replaced by station
purchasing on 2026-09-28, following the owner’s hyperspace-arrival, full-purchasing and
station-upgrade/build-pad decisions.
**Owner decisions:** FoC only; map `_mp_space_coruscant.ted`; Empire vs Rebel; the fixed start
forces plus FoC's skirmish economy (credits, income, the station build queue and hyperspace
arrival; the owner’s full-purchasing choice A replaced the fixed-force economy lock);
victory and defeat follow the space victory rules; the
opponent is the original FoC tactical AI in its galactic-conquest (GC) configuration (owner choice B), run by the FoC tactical AI host, with a small
project-authored fallback. The owner chose the retail start plus a GC fleet
chosen by the owner, no squadron replenishment, an AI attacker and Normal difficulty.

This page pins the fixture that every M2 ticket shares. The owner has answered every open
question (see Owner answers), so no rule is pending. Cite rules by ID (for example SK-21).

Source tags:

- **data**: the FoC profile of the retail corpus (FoC archives over base EaW), as resolved
  by `plan/inventories/xml-manifest.json`. Files and hashes are listed under Sources.
- **capture**: the rig capture of the retail default lobby on this map (run
  `20260924T234003Z-8cd8ea`, one Easy AI, default factions). The images stay under the
  ignored `out/original/mp_space-coruscant/`.
- **research**: symbol names in the FoC debug build, read under the clean-room rule. No
  addresses or decompiler output are committed; the private evidence is under ignored `out/`.
- **owner** / **ticket**: an owner decision or a ticket's scope.
- **inference**: our conclusion. It is not established until a capture or the owner confirms it.

## Map

| Rule | Behaviour | Source |
|---|---|---|
| SK-01 | The map is `data/art/maps/_mp_space_coruscant.ted` from FoC `Data/Maps.meg`: 94,765 bytes, SHA-256 `91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286`. It is a space map with 58 placement records (`1/258/1/1100`). | data |
| SK-02 | The map is north-up in the retail radar: TED +X is east and +Y is north. | capture (the nebulae and merchant dock at +X+Y draw top right) |
| SK-03 | Two teams have start markers, Team_00 in the north-west and Team_01 in the south-east (table below). Each team has one `Team_NN_Space_Station`, one `Team_NN_Base_Position_Marker` and three `Team_NN_Spawn_Point_Marker` records. | data |
| SK-04 | All other placements are unowned by lobby players. TED owner index 3 (Neutral) holds the props, 6 `Mineral_Extractor_Pad`, 7 `Defense_Satellite_Laser_Pad`, `Skirmish_Merchant_Dock` and `N_Gravity_Well_Station`. Owner index 7 holds 8 `Orbital_Resource_Container` (20 hull, `Victory_Relevant = no`). Index 7 is the Hutts in FoC's faction order; this page first read it as Hostile, the base-EaW order. The owner confirmed that the containers are Hutt-owned in play: orange on the minimap, destructible neutral mines. | data; owner |
| SK-05 | Every object the start creates stands at its placement point (a marker, its TED position, the free point the start's placement finds) raised by its type's `Layer_Z_Adjust`; a squadron's team container is not raised. The M2 markers lie at z = 0, so the Nebulon-B starts at -90 and the MC80 at -290. | research (space-movement LZ-01, LZ-02); recordings S-32, S-51 |

Start markers (TED record ordinal, source position in TED units, yaw in degrees):

| Team | Station marker | Base position | Spawn points |
|---|---|---|---|
| Team_00 (NW) | record 48 (−3811, 4460, 0), yaw 227 | record 49 (−4661, 4761, 0), yaw 336 | record 50 (−4661, 5112, 0), record 51 (−4921, 4947, 0), record 52 (−5057, 4700, 0); yaw 336 |
| Team_01 (SE) | record 54 (3685, −4171, 0), yaw 45 | record 53 (4620, −4603, 0), yaw 45 | record 55 (4118, −4976, 0), record 56 (4431, −4670, 0), record 57 (4706, −4393, 0); yaw 149 |

## Players and teams

| Rule | Behaviour | Source |
|---|---|---|
| SK-10 | M2 is one versus one. Lobby slot 1 is the local human, faction **Rebel**, team 0, starting on the Team_00 markers. Lobby slot 2 is the AI, faction **Empire**, team 1, on the Team_01 markers. This is the retail default lobby. | owner (matchup); capture (slot 1 Rebel at north-west) |
| SK-11 | A player's starting companies all appear at the k-th `Team_NN_Spawn_Point_Marker` of its team in marker search order, where k is the player's index within the team. The search visits the newest object first, which is reverse TED record order. In a one-versus-one game that is the last marker in record order: record 52 for the Rebel, record 57 for the Empire (the skirmish-start work (legacy EAWR-67); the M2 skirmish lock (legacy EAWR-64) had read map order as record order and named records 50 and 55). The other two markers per team are for team games and stay unused. The local player's camera starts on its marker, and the marker's facing is the player's reinforcement facing. | research; capture (the skirmish-start work (legacy EAWR-67): the Rebel units' radar blip and the start camera sit on record 52); inference (placements load in record order) |
| SK-12 | Lobby players use the multiplayer colours, not the faction `<Color>` values (Rebel red, Empire blue). Slot 1 is `MP_Color_Blue` (78, 150, 237) and slot 2 is `MP_Color_Red` (237, 78, 78). The capture shows only the local slot-1 view, so the skirmish-start work (legacy EAWR-67) confirms that the colours follow the slot and not an own/enemy radar rule. | data (`gameconstants.xml`); capture (radar and selection pixels); inference |
| SK-13 | The factions and the AI player never swap in M2. A lobby editor stays out of scope. | ticket |

## Starting forces

Retail default, for reference: with the lobby defaults (`MP_Default_Pre_Built_Base = yes`,
`MP_Default_Free_Starting_Units = yes`), each player gets its level-1 skirmish station on its
station marker and the faction's `Space_Skirmish_AI_Default_Forces` at its spawn marker. That
start holds fighters only. The M2 tickets need a corvette, a frigate with engine and weapon
hardpoints, and a carrier (legacy EAWR-70, EAWR-72, EAWR-75, EAWR-76). The M2 fixture therefore adds a small fleet
of FoC GC starting-force types, chosen by the owner in the starting-force, replenishment and AI-role decisions.

| Rule | Behaviour | Source |
|---|---|---|
| SK-20 | **Station: yes.** Each player starts with the station its marker names: `Skirmish_Rebel_Star_Base_1` on record 48 and `Skirmish_Empire_Star_Base_1` on record 54. Both are variants of `Rebel_Star_Base_1` / `Empire_Star_Base_1` (base level 1, 1600 hull, 300 shield, `Victory_Relevant = yes`). | data (`multiplayer_structure_markers.xml`, `starbases.xml`, `gameconstants.xml`); capture; owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q1: keep the retail start) |
| SK-21 | **Free starting units.** When the lobby option is on, the engine copies the faction's `Space_Skirmish_AI_Default_Forces` into the player's starting-forces list: Rebel 2 × `Rebel_X-Wing_Squadron`, Empire 2 × `TIE_Interceptor_Squadron`. The list is created at the SK-11 marker. | data (`factions.xml`); research; capture (two X-wing squadrons of 5 craft at the Rebel marker) |
| SK-22 | **GC fleet.** Each player's starting-forces list is extended, after the free units, at the same marker and by the same rule, in this order. Empire: `Tartan_Patrol_Cruiser`, `Acclamator_Assault_Ship` (the Empire's Coruscant space garrison in `Sandbox_Gateways_Rebel` and `Sandbox_Gateways_Underworld`). Rebel: `Y-Wing_Squadron`, `Corellian_Corvette`, `Nebulon_B_Frigate`, `Calamari_Cruiser`. The first three Rebel types are FoC GC Rebel starting forces (for example Muunilinst in `Sandbox_Origin_Rebel`), and the order follows the GC lists, which put squadrons before corvettes, corvettes before frigates and frigates before cruisers (Yavin in `campaigns_multiplayer.xml` lists its squadrons, corvettes and `Alliance_Assault_Frigate` before its two `Calamari_Cruiser`). The Nebulon-B is the Rebel type `Nebulon_B_Frigate`, not the Underworld type `Nebulon_B_Underworld`. The MC80 is `Calamari_Cruiser` (`spaceunitscapital.xml`, `Affiliation` Rebel), added by the owner on 2026-09-28 (Q1 amended). | owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q1; Q1 amended 2026-09-28); data (`campaigns_underworld_gc.xml`, `campaigns_multiplayer.xml`, `spaceunitsfrigates.xml`, `spaceunitscapital.xml`, `squadrons.xml`); inference (order) |
| SK-23 | **Launched squadrons.** Each spawner (`SPAWN_SQUADRON`) launches its `Starting_Spawned_Units_Tech_0` list once, after tick zero, with its `Spawned_Squadron_Delay_Seconds`. The Rebel station launches 2 × `Rebel_X-Wing_Squadron` and 2 × `Y-Wing_Squadron` with a 10 s delay. The Empire station launches 2 × `TIE_Fighter_Squadron` and 2 × `TIE_Bomber_Squadron` with a 10 s delay. The Acclamator's hangar (`HP_Acclamator_Fighter_Bay`) launches 1 × `TIE_Fighter_Squadron` and 1 × `TIE_Bomber_Squadron` with a 5 s delay. No other roster ship spawns squadrons (the `Calamari_Cruiser` has no `SPAWN_SQUADRON` behaviour and no spawn list), and no FoC spawner lists a tech level other than `Tech_0`. The launch is a timed sequence, not an instant garrison, and its timing belongs to the squadron simulation work (legacy EAWR-75). | data (`starbases.xml`, `spaceunitsfrigates.xml`); research; owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q1: deploy the Acclamator's hangar; Q2: launch once) |
| SK-24 | Tick zero therefore holds the Rebel station, 2 × `Rebel_X-Wing_Squadron`, `Y-Wing_Squadron`, `Corellian_Corvette`, `Nebulon_B_Frigate` and `Calamari_Cruiser`, and the Empire station, 2 × `TIE_Interceptor_Squadron`, `Tartan_Patrol_Cruiser` and `Acclamator_Assault_Ship`. By `AI_Combat_Power` (unit table below; a squadron counts as the sum of its craft, PL-13 in [foc-tactical-ai.md](../../docs/behaviour/foc-tactical-ai.md)), tick zero is Rebel 13,525 and Empire 9,590. With every SK-23 launch out it is Rebel 14,575 and Empire 11,045. Without replenishment (SK-36) neither side ever fields more. | data; debug build (squadron sum) |

Balance by `AI_Combat_Power`:

| Side | Tick zero | SK-23 launches | All launched |
|---|---|---|---|
| Rebel | station 5,000 + 2 × X-wing squadron 600 + Y-wing squadron 225 + Corellian 1,250 + Nebulon-B 2,200 + MC80 4,250 = **13,525** | station 2 × X-wing 600 + 2 × Y-wing 450 = 1,050 | **14,575** |
| Empire | station 5,000 + 2 × TIE Interceptor squadron 840 + Tartan 1,250 + Acclamator 2,500 = **9,590** | station 2 × TIE Fighter 490 + 2 × TIE Bomber 480; Acclamator TIE Fighter 245 + TIE Bomber 240 = 1,455 | **11,045** |

Pinned unit types (the space-unit data scan list; the scan decides fields and abilities):

| Type | Role | Craft | Hull / shield | `AI_Combat_Power` | Unit ability (data) |
|---|---|---|---|---:|---|
| `Skirmish_Rebel_Star_Base_1` | Rebel station | | 1600 / 300 | 5,000 | income stream and Supply Dock bonus (SK-30); radar ability off (SK-31) |
| `Skirmish_Empire_Star_Base_1` | Empire station | | 1600 / 300 | 5,000 | income stream and Supply Dock bonus (SK-30); radar ability off (SK-31) |
| `Corellian_Corvette` | Rebel corvette (8 laser hardpoints, not targetable) | | 750 / 600 | 1,250 | `TURBO` |
| `Nebulon_B_Frigate` | Rebel frigate (engine and 4 laser hardpoints, all targetable; no hangar) | | 3600 / 700 | 2,200 | `DEFEND` (autofire script, SK-47) |
| `Calamari_Cruiser` | Rebel cruiser, the MC80 (engine, 4 laser and 2 ion-cannon hardpoints, all targetable; no hangar) | | 8500 / 2000 | 4,250 | `DEFEND` (autofire script, SK-47) |
| `Tartan_Patrol_Cruiser` | Empire corvette (5 laser hardpoints, not targetable) | | 500 / 800 | 1,250 | `POWER_TO_WEAPONS` |
| `Acclamator_Assault_Ship` | Empire frigate and carrier (engine, fighter-bay and 6 weapon hardpoints, all targetable) | | 2000 / 600 | 2,500 | `power_to_weapons` (lower case in the data) |
| `Rebel_X-Wing_Squadron` | Rebel fighters | 5 × `X-Wing` | 60 / 20 each | 60 each | `SPOILER_LOCK` |
| `Y-Wing_Squadron` | Rebel bombers | 3 × `Y-Wing` | 60 / 30 each | 75 each | `ION_CANNON_SHOT` |
| `TIE_Interceptor_Squadron` | Empire fighters | 7 × `TIE_Interceptor` | 65 / 0 each | 60 each | `HUNT` |
| `TIE_Fighter_Squadron` | Empire fighters | 7 × `TIE_Fighter` | 50 / 0 each | 35 each | `HUNT` |
| `TIE_Bomber_Squadron` | Empire bombers | 4 × `TIE_Bomber` | 60 / 0 each | 60 each | none |

Only `Nebulon_B_Frigate` and `Calamari_Cruiser` have a `Lua_Script` (SK-47); no other type loads a unit object script
(AI-32 in the [AI note](../../docs/behaviour/foc-tactical-ai.md)) in M2.

## Battle rules

| Rule | Behaviour | Source |
|---|---|---|
| SK-30 | **Credits and income (legacy EAWR-530).** Every lobby player starts with FoC's skirmish default, `MP_Default_Credits` = 6000, and earns from its station's income stream: 30 per 10 s, paid every frame, plus 20 while the station's Supply Dock hardpoint stands (5 credits a second in all). The population cap is the faction's `Space_Tactical_Unit_Cap`: Rebel 25, Empire 20. Only units brought in from the reinforcement pool count toward it. The rules are PU-01 to PU-07 and PU-21 of [space purchasing](../../docs/behaviour/space-purchasing.md). Until the station purchasing work (legacy EAWR-530) this rule was 0 credits and no income or cap. | owner (the hyperspace-arrival scope decision (legacy EAWR-461), the full station-purchasing decision (legacy EAWR-522) = A); data; debug build |
| SK-31 | **The station build queue (legacy EAWR-530).** Each level-1 station builds its `Tactical_Buildable_Objects_Multiplayer` list for its owner's faction (Rebel: X-wing and Y-wing squadrons; Empire: TIE interceptor and TIE bomber squadrons) at the multiplayer prices and build times; a finished unit waits in its owner's reinforcement pool until the owner brings it in through hyperspace at a point (PU-10 to PU-39). The list's upgrades and the level-2 station upgrade (`Next_Level_Base`) are shown but not built in the station purchasing work (legacy EAWR-530); the owner put them and the build pads into M2 as the station upgrades (legacy EAWR-540) and the space build pads and mining facilities (legacy EAWR-541) (the station-upgrade and build-pad scope decision (legacy EAWR-538) = A). No comm-array radar ability. | owner (the full station-purchasing decision (legacy EAWR-522) = A, the station-upgrade and build-pad scope decision (legacy EAWR-538) = A); data; debug build |
| SK-32 | Map capture points stay inert: build pads, the merchant dock and the gravity-well station are never captured. They keep their neutral owner and their hull. The build-pad and mining implementation is tracked separately (legacy EAWR-541) (the station-upgrade and build-pad scope decision (legacy EAWR-538) = A). | owner (no economy or production; the station-upgrade and build-pad scope decision (legacy EAWR-538) for the pads); inference |
| SK-33 | Victory: the retail default space condition, `MP_Default_Space_Tactical_Win_Condition = SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED`. A player wins when the enemy starbase is destroyed. Objects with `Victory_Relevant = no` (containers, pads, gravity-well station) never count. The full victory and defeat rules are in [space victory](../../docs/behaviour/space-victory.md). | data (`gameconstants.xml`); the victory and defeat rules (legacy EAWR-77) |
| SK-34 | Retreat is off. `CanRetreat` is 0 for both players. | victory/defeat scope (retreat excluded) |
| SK-35 | No heroes, superweapons, random events or game timer. The lobby defaults allow heroes and superweapons, but the fixed roster holds none. | data (lobby defaults); owner (fixed forces) |
| SK-36 | **No replenishment.** A launched squadron that dies is not replaced. The fixture ignores every `Reserve_Spawned_Units_Tech_0`: the stations' −1 (unlimited) for each type they launch and the Acclamator's 2 × `TIE_Fighter_Squadron`. (The Acclamator's own data comments say it spawns once with no reserves, but its reserve tag lists 2.) Retail replenishes from these reserves; starbase-hangar replenishment is tested in a later phase (fidelity list). | owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q2); data (`starbases.xml`, `spaceunitsfrigates.xml`) |

## AI setup

The AI is the Empire (SK-10). The detail is in the [AI note](../../docs/behaviour/foc-tactical-ai.md);
this table pins the inputs that note left open (its AI-G03).

| Rule | Behaviour | Source |
|---|---|---|
| SK-40 | AI player `BasicEmpire` (`data/xml/ai/players/basicempireplayer.xml`), the Empire's `Basic_AI` in `factions.xml`. Space template `Test_Space`, space freestore `BusyTacticalFreeStore`, goal set `BasicOffensiveSpaceSet`. The mounted selection is the 17 scripts of AI-30 plus six skirmish economy plans (SAE-03), their library chain and the `GetDistanceToNearestSpaceField` evaluator. | data; AI note AI-11 to AI-15, AI-30; SAE-03 |
| SK-41 | Skirmish context: `Game.IsCampaignGame = 0`; live economy inputs and production plans follow [skirmish AI economy](../../docs/behaviour/skirmish-ai-economy.md) SAE-01 to SAE-10. The campaign combat regression explicitly retains its earlier context. | owner; space AI data |
| SK-42 | AI difficulty Normal (`Normal_Default`: every space and tactical multiplier 1.0, space goal-cycle sleep 0; only the galactic `Galactic_AI_Contrast_Multiplier` 1.1 and `Galactic_Build_Time_Multiplier` 0.8 and the Underworld `Bribe_Cost_Multiplier` 0.25 differ, none of which apply to this fixture). Rig captures for M2 use a Normal AI. The P1 capture route used Easy, which scales the AI's health by 0.4, shields by 0.3 and damage by 0.6. | data (`difficultyadjustments.xml`); owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q4) |
| SK-43 | `Variable_Self.IsDefender = 0` for the AI (the GC attacker role). | owner (the starting-force, replenishment and AI-role decisions (legacy EAWR-179) Q3) |
| SK-44 | `Variable_Self.BaseLevel` follows the owned live station's `Base_Level`, initially 1. `CanRetreat = 0` (SK-34). Credits, population room, reinforcement power and pad/structure counts read completed economy state (SAE-02). Skirmish goals can buy, reinforce and upgrade through ordinary commands (SAE-03; PU-50 to PU-52). | data; project fixture; skirmish AI economy SAE-01..08 |
| SK-45 | AI fog of war stays off (retail default `AIUsesFogOfWarSpace = False`), so the AI sees the whole map. | data; AI note AI-14 |
| SK-46 | With SK-20 to SK-45, the AI note's plan table gives these live plans: `destroyunit`, `flankplan` (Tartan and Acclamator give the 2 corvettes or frigates it needs), `destroyunitminimal`, `areasweep`, `spacescout`, `escortplan`, `bombingrun` (roster-dependent: 3 bomber squadrons, the Empire station's 2 and the Acclamator's 1 from SK-23), `hidesurpriseunits`, `turboattack`, `turboattacklocation`, `movetolocationrush` (pad goal, SK-32) and `burnunits`. These stay dormant: `spaceartillery`, `hidetransports`, `retreatplan`, `movetolocation` and the station-defence plan. | inference from the AI note plan table |
| SK-47 | **Unit object script.** `Nebulon_B_Frigate` and `Calamari_Cruiser` name `Lua_Script` `ObjectScript_PowerToShields` (`objectscript_powertoshields.lua`, AI-32), which loads for either owner and exits outside space mode. For an AI owner it activates `DEFEND` whenever `Get_Rate_Of_Damage_Taken()` exceeds 20 and the ability is ready. For a human owner it does the same only while the player has set `DEFEND` to autofire. Both are the human's in M2 (SK-10), so they idle until autofire is on. The FoC tactical AI host (legacy EAWR-79) therefore needs the AI-32 script with `PGStateMachine` and the AI-43 functions, even though the AI owns neither. | data (`spaceunitsfrigates.xml`, `spaceunitscapital.xml`, the script); AI note AI-32, AI-43 |

## Owner answers

The owner answered the four open questions in the starting-force, replenishment and AI-role decisions (legacy EAWR-179)
on 2026-09-25.

| ID | Question | Answer | Rules |
|---|---|---|---|
| Q1 | Starting forces: the retail start only, the retail start plus a GC fleet, or a GC-style battle where only the defender has a station | Retail start plus a GC fleet, modified. The Empire keeps `Tartan_Patrol_Cruiser` and `Acclamator_Assault_Ship`, and the Acclamator deploys its own hangar. The Rebel gets one `Corellian_Corvette`, one `Nebulon_B_Frigate` (it has hardpoints) and a `Y-Wing_Squadron`. **Amended by the owner 2026-09-28** (after play-testing 8702ba82): the Rebel also gets the MC80 `Calamari_Cruiser`, for more force against the Empire composition. | SK-20 to SK-24 |
| Q2 | Station and carrier squadron replenishment: retail reserves, or no replenishment | No replenishment; each spawner launches its starting squadrons once. Starbase hangars are tested later. | SK-23, SK-36 |
| Q3 | AI role in the GC context: attacker (`IsDefender = 0`) or defender (`IsDefender = 1`) | Attacker. | SK-43 |
| Q4 | AI difficulty for the fixture and for rig captures | Normal. | SK-42 |

Later owner decisions on the economy:

| Issue | Question | Answer | Rules |
|---|---|---|---|
| D2: hyperspace arrivals (legacy EAWR-461) | Hyperspace arrivals in M2 | Needed in M2 for the units bought at the station; full reinforcements stay with galactic conquest (Phase 3). | SK-31, PU-30 to PU-39 |
| the full station-purchasing decision (legacy EAWR-522) | Station purchasing in M2 | A: full purchasing (credits, income, the build menu and queue, the bought units' hyperspace arrival). | SK-30, SK-31 |
| the station-upgrade and build-pad scope decision (legacy EAWR-538) | Station upgrades and build pads in M2 | A: M2 becomes a complete skirmish; the level-up and upgrades (legacy EAWR-540) and the pads and mining (legacy EAWR-541) build on the data-driven purchasing foundation. | SK-31, SK-32 |

The data backs the Q1 roster. The Acclamator has a hangar in FoC data (`SPAWN_SQUADRON`,
`HP_Acclamator_Fighter_Bay` and a starting-squadron list; SK-23). It has no other space
garrison: no garrison tag (`Spawn_Garrison_On_Load`, `Num_Garrison_Slots`, `Garrison_*`)
appears in `spaceunitsfrigates.xml`, `spaceunitscorvettes.xml` or `starbases.xml`, and its
`Transport_Capacity` of 8, which the Nebulon-B shares, is not a spawn list.

## Consequences for other tickets

| Ticket | Uses |
|---|---|
| the space-unit data scan (legacy EAWR-65) | The pinned unit types above, including the `Nebulon_B_Frigate` and `Calamari_Cruiser` `Lua_Script` (SK-47). |
| the skirmish-start work (legacy EAWR-67) | Tick zero from SK-01 to SK-24 and SK-30 to SK-36. The retail default lobby reproduces the stations, free units, markers and colours. The SK-22 fleet (Rebel `Y-Wing_Squadron`, `Corellian_Corvette`, `Nebulon_B_Frigate`, `Calamari_Cruiser`; Empire `Tartan_Patrol_Cruiser`, `Acclamator_Assault_Ship`) is not a lobby start, so its capture needs a staged start through the original-game recording lane that places these six at the SK-11 markers and lets the Acclamator launch its SK-23 squadrons. |
| the fixed-force battle recordings (legacy EAWR-69), the original-game recording lane (legacy EAWR-43) | Scenarios use the pinned types, including the SK-22 fleet, and a Normal AI (SK-42). Retail replaces lost squadrons from the reserves and the fixture does not (SK-36), so squadron scenarios compare only the first launch wave. Record the Nebulon-B and MC80 `DEFEND` autofire state (SK-47). |
| the ship movement work (legacy EAWR-70), the projectile, damage and shield work (legacy EAWR-74) | Corvettes: `Corellian_Corvette`, `Tartan_Patrol_Cruiser`. Frigates: `Nebulon_B_Frigate`, `Acclamator_Assault_Ship`. Cruiser: `Calamari_Cruiser`. |
| the hardpoint damage work (legacy EAWR-72) | Targetable engine and weapon hardpoints on both sides: `Nebulon_B_Frigate` (`HP_Nebulon_Engines` and 4 laser `HP_Nebulon_Weapon_*`) `Acclamator_Assault_Ship` (`HP_Acclamator_Engines`, 4 laser, 1 missile and 1 torpedo `HP_Acclamator_Weapon_*`, and `HP_Acclamator_Fighter_Bay`) and `Calamari_Cruiser` (`HP_Calamari_Cruiser_Engines`, 4 laser `HP_Calamari_Cruiser_Weapon_BL/BR/ML/MR` and 2 ion-cannon `HP_Calamari_Cruiser_Weapon_FL/FR`). In the roster, only the stations have shield-generator hardpoints (`HP_Rebel_Station_One_ShieldGen`, `HP_Empire_Station_One_03`). The corvettes' weapon hardpoints are neither targetable nor destroyable. |
| the squadron simulation work (legacy EAWR-75) | Launch once from the stations and the Acclamator hangar (SK-23); no replenishment (SK-36). |
| the space abilities work (legacy EAWR-76) | The abilities in the unit table, subject to the space-unit data scan: `TURBO`, `POWER_TO_WEAPONS` (Tartan and Acclamator), `DEFEND` (Nebulon-B and MC80, with the SK-47 autofire script), `SPOILER_LOCK`, `ION_CANNON_SHOT` and `HUNT`. |
| the victory and defeat rules (legacy EAWR-77) | SK-33 and SK-34. |
| the FoC tactical AI host (legacy EAWR-79) | SK-40 to SK-47. Status on 2026-09-28 (the initial FoC tactical AI host (legacy EAWR-446), in review): the AI runs the SK-40 freestore, which commands every Empire unit and attacks. The SK-46 plans load but none starts yet, because the AI goal system they need is not built (owner decision D1 in the [Phase 2 plan](README.md#decisions-needed)). |

## Sources

FoC effective winners, checked against `plan/inventories/xml-manifest.json` (FoC profile):

| Logical path | Archive | Bytes | SHA-256 |
|---|---|---:|---|
| `data/art/maps/_mp_space_coruscant.ted` | FoC `Data/Maps.meg` | 94765 | `91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286` |
| `data/xml/factions.xml` | FoC `Data/Config.meg` | 124969 | `27c880a17e0b642dbfbe40ff0b0bad620d63cb72a6ac06f1ebf4ef7e03998a15` |
| `data/xml/gameconstants.xml` | FoC `Data/Patch2.meg` | 298649 | `fb4986e9a9866e2b8ec4f8c44a32050378c8c51bcde7390a5a7446e61f2ca3b1` |
| `data/xml/multiplayer_structure_markers.xml` | FoC `Data/Config.meg` | 30928 | `18de78b0ff7fe1233bd00e7e17c8b88f98dadb86c21af2ad17a38379153947f7` |
| `data/xml/markers.xml` | FoC `Data/Config.meg` | 64279 | `74fe3b607d75b65e40ea9c52ade5abbe9fa86ac3b4f05c147b92d0dd72eaa3e1` |
| `data/xml/starbases.xml` | FoC `Data/Config.meg` | 132207 | `07b602d8810c4c4f6afba3d3ac852482b2182a3501838d180c621f3ee8921578` |
| `data/xml/campaigns_underworld_gc.xml` | FoC `Data/Config.meg` | 414641 | `5d9c49c968061ac8672c13d7cbed10ac24d039a66cd659805bfb72b0039b81ee` |
| `data/xml/spaceunitscorvettes.xml` | FoC `Data/Config.meg` | 73976 | `0d14f23d9ce3070537c05970a22fdfe8690866157c6717570dd97f8f67209949` |
| `data/xml/spaceunitsfrigates.xml` | FoC `Data/Patch2.meg` | 82492 | `57da2b587e513980ea8dfd29928e76f714ce0b67439aa7f4cbfa4946457aacba` |
| `data/xml/spaceunitsfighters.xml` | FoC `Data/Patch2.meg` | 73101 | `28b569b62a88641791447d0f3ad75a0a36edca3e94b0ab53ecfb5f200f0d4958` |
| `data/xml/squadrons.xml` | FoC `Data/Patch2.meg` | 55872 | `e2a1a512efc1abf46e64ce73030b55c78e923886239ef900f868b3c3de48aa78` |
| `data/xml/hardpoints.xml` | FoC `Data/Patch2.meg` | 328376 | `c059f2701d1a9b304919c1a17b4bf6157f1471b23c4e7b8c1f7526da2fd2f434` |
| `data/xml/units_space_empire_tie_interceptor.xml` | FoC `Data/Config.meg` | 11471 | `35261c0267c9f7490582f7c2a7bcc3ead123c5988b4d1b032556caf2b90b86ca` |
| `data/xml/difficultyadjustments.xml` | FoC `Data/Config.meg` | 2996 | `576176423b06476153bc744ac5d527a9335a24824540c20cae482567e2c6fb91` |
| `data/xml/ai/players/basicempireplayer.xml` | FoC `Data/Config.meg` | 1108 | `f4003cfbdb5ca70d54beb1c664f848e8fde2f1ee755e3199980da8eb7892cb82` |
| `data/xml/ai/perceptualequations/offensivespaceequations.xml` | FoC `Data/Config.meg` | 32215 | `663083391c6d09b606f3df65a883c436002d3a8d1ffd807af4d9cbb58d9ce03f` |

The SK-47 script, `data/scripts/gameobject/objectscript_powertoshields.lua` (3596 bytes, SHA-256
`504d13ba5bb5e75faf9e9e30fbd1a690dd3135f6ebfd1ebfcd754ce68f9e00ea`), is listed with the
[AI note](../../docs/behaviour/foc-tactical-ai.md) sources.
