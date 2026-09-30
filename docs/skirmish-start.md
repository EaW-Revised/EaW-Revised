# M2 skirmish start (tick zero)

P2-04 (EAWR-67). `eawr::skirmish`
(`include/eawr/skirmish/start.hpp`) builds tick zero of the pinned FoC space skirmish
([m2-skirmish.md](../plan/phase-2/m2-skirmish.md), SK-01 to SK-24 and SK-30 to SK-36) as a
[tactical setup](replay-format.md#tactical-replay-format-v2-p2-03). It reuses the EAWR-65 unit tables
(`eawr::units`) and the EAWR-66 `TacticalSession`. `read_start_inputs` reads the map and the XML facts
from a mounted FoC view; `build_start` is a pure function of those plain inputs.

## Fixture

`m2_fixture()` pins the map `data/art/maps/_mp_space_coruscant.ted` by SHA-256 (SK-01), slot 1
Rebel human on team 0 and slot 2 Empire AI on team 1 (SK-10), the lobby defaults pre-built base
and free starting units (SK-20, SK-21), the SK-22 fleets, and seed 67. The replay header's content
identity is the unit-table identity ([unit-data.md](unit-data.md#content-identity)).

## Start rules

- **Players.** A lobby slot is player `slot`, on team `team`, with the command flag. Its faction
  and type IDs are the CRC-32 of the ASCII-upper-cased name, the hash TED placements use for
  object types (`assets::object_type_crc`). Slot k takes the k-th `MP_Color_*` constant of
  `gameconstants.xml` (SK-12). The colour is census data, not tactical state.
- **Markers.** The k-th lobby player of team t, in slot order, takes the k-th
  `Team_tt_Space_Station` and the k-th `Team_tt_Spawn_Point_Marker` in retail marker search
  order (SK-11). Retail links each new object at the head of its object list and a marker search
  walks that list, so the search meets the map's markers in reverse TED record order (FoC debug
  build symbols; the Rebel start in the retail capture). M2 uses only k = 0: records 48 and 52
  for the Rebel, 54 and 57 for the Empire. Marker records must be yaw-only.
- **Station (SK-20).** The station marker's `Marker_For_Specific_Object_Type` entry whose
  `Affiliation` names the player's faction, placed on the marker with its yaw.
- **Companies (SK-21, SK-22).** The faction's `Space_Skirmish_AI_Default_Forces`, then the fixture
  fleet, each with the marker's yaw on the first free point near the spawn marker, in that order
  (EAWR-597, [space movement](behaviour/space-movement.md#placement-597) PL-01 to PL-07: rings around
  the marker up to 2500 units; the map objects, the stations and every company placed before
  block). A squadron company is its squadron's team container, at the centre of its craft; its
  craft follow the map objects (below) in the entity order, and each craft is placed by its own
  search (space-fighters FC-02).
- **Map objects (SK-04).** Every placement that is neither a marker (`Is_Marker`, the `Marker`
  element) nor a `SpaceProp` is a map object on its own position and yaw, owned as the next rule
  says. A map object with roll or pitch keeps its yaw alone (the Euler order is unresolved,
  [asset-formats.md](asset-formats.md)); the census lists the dropped angles. The capture points
  stay inert (SK-32): rules v1 has no capture system.
- **Retail map-object ownership (EAWR-272).** Evidence IDs OW-E1 to OW-E8 are FoC debug-build and
  FoC XML readings kept with the private research notes. A TED owner index is an editor player index: the
  editor makes one default player per faction in faction order, so the index names the faction
  `scene::faction_order` gives it, and an index past the factions names the Neutral one
  (OW-E1, OW-E2; AU-70). The skirmish players are the lobby slots, then one player without the
  command flag for every faction that is not `Is_Playable` and has
  `Create_Player_In_Multiplayer_Games`, in faction order (OW-E3). Each such player takes the
  next free player ID and its own team; retail puts them on no team (team −1), and the remake
  keeps separate teams (fidelity list). Then each map object is decided on its own:
  - an object of a playable faction (FoC: Rebel, Empire, Underworld) is deleted;
  - an object of a non-playable faction goes to that faction's player;
  - without such a player, a decoration that is not discardable (`Is_Decoration` yes and
    `Is_Discardable` no; retail's type defaults are no and yes, OW-E4) goes to the Neutral player,
    the player of the first `Is_Neutral` faction (OW-E8);
  - anything else is deleted.

  Retail visits the objects in object-list order and defers every deletion and owner change to
  the end of the pass, so no outcome depends on the order; tick zero keeps TED record order.
  The flags are read as retail reads booleans (true for `1` or a value starting with Y or T;
  an empty value or `TBD` keeps the default; AU-80 to AU-82). In FoC, Pirates, Neutral,
  Hostile, Sarlacc and Hutts get players (OW-E6). The census lists deleted map objects under
  `removed_map_objects` with the reason (`playable_faction` or `no_player`).

  On Coruscant the map owners are Neutral (index 3) and Hutts (index 7, the eight resource
  containers), both with players, so no object is deleted: the players are 1 Rebel, 2 Empire,
  3 Pirates, 4 Neutral, 5 Hostile, 6 Sarlacc and 7 Hutts, the pads, dock and gravity well
  belong to player 4 and the containers to player 7. That matches the owner's play
  observation: the containers are Hutt-owned, orange on the minimap, destructible neutral mines
  (RO-2, owner EAWR-312).
- **Entity IDs** count from 1: the lobby players in slot order (station, then companies in list
  order), then the map objects that stay, in TED record order, then every squadron company's
  craft in company then `Squadron_Units` order (role `craft`).
- **Transforms.** Positions are the TED binary32 values converted exactly to Q24
  (`scene::fixed_from_binary32`). The rotation for yaw y degrees is the Q24 quaternion
  `normalize(0, 0, sin h, cos h)` with h = `wrap_turn(y / 720)` turns.
- **Heights (SK-05, EAWR-666).** Every company, station, map object and squadron craft is raised by
  its type's `Layer_Z_Adjust` over the point it is placed on (a company or craft over the free
  point the placement found on its marker's plane); a squadron's team container is not ([space-movement](behaviour/space-movement.md#heights-666)
  LZ-01, LZ-02). The census lists the raised positions.
- **Economy (SK-30, SK-31).** No credits, income, production queue or population cap. The
  tactical state has no such fields; the census states them per player.
- **Launches (SK-23, SK-36).** Each spawner's `Starting_Spawned_Units_Tech_0` is census data for
  EAWR-75 (`"simulated": false`). Tick zero launches nothing, and reserves are not listed.

## Census and replay

```text
sim_headless --skirmish m2 --game-root <install> [--census-out <file.json>]
             [--replay-out <file>] [--ticks <n>]
sim_headless --replay <v2 file> --hash-out <file> [--census-out <file.json>] ...
```

`--skirmish m2` prints the start (players, colours, units, launches, tick-zero hash) and writes
the census and a replay v2 file that holds the setup alone, `final_tick_count` n (default 0) and
no commands. On a replay-v2 run, `--census-out` lists the same setup from the replay header
alone. Both censuses are `eawr-skirmish-census` version 1 JSON: `tick_zero` (state and snapshot
SHA-256, next entity ID, sensor profile count), the seed and content identity, and per player
and unit the setup fields (`player_id`, `team`, `faction_id`, `commandable`; `entity_id`,
`type_id`, `owner`, `position_raw`, `rotation_raw`). The fixture census adds names, markers,
colours, reveal ranges, `AI_Combat_Power` totals (SK-24) and launches.

`--skirmish m2` creates the session with `sensor_table(tables)`: one `SensorProfile` per unit-table
type with a sensor range, keyed by type ID (EAWR-68, EAWR-271: `units::sensor_range`). That is 12
profiles: the 7 stations and ships with `REVEAL`, the `Y-Wing` craft (the only craft with
`REVEAL`) and the 5 squadrons, which reveal with their team container's range (800; the Y-wing
squadron's container 1000). Squadron companies therefore reveal at tick zero at their own
position. Map objects have none (the tables do not load map object types). The sensor table is content, not
replay data. It changes the snapshot digest and never the state hash, so a replay-v2 census,
which binds no sensor table, has the same state hash and a different snapshot digest.

`tests/skirmish/fixtures/m2-start.eawr-replay` is that replay with 30 ticks. Its tick-zero state
hash is `506e48ffa375d8a6fd29c9fa69474f63d4f455a999126e5f232bef3aefd64a2a`. The CLI test
recomputes it from the header and setup bytes with the frozen `EAWRTST` encoding; with
`EAWR_EAW_GAME_ROOT` the start is rebuilt from FoC data and must write the same bytes.
Regenerate it only when a start rule or the unit-table identity changes:
`sim_headless --skirmish m2 --game-root <install> --replay-out tests/skirmish/fixtures/m2-start.eawr-replay --ticks 30`.

## Retail comparison

The retail default lobby (Coruscant, slot 1 Rebel, one AI) is captured on the rig with
`tools/validation/p1_capture/Invoke-FocMapCapture.ps1 -MapId coruscant-space -GraphicsPreset Highest`.
Positions are compared on the battle minimap, which maps the TED square −6100..6100 onto
160 × 164 pixels (`MINIMAP_WORLD` in `foc_capture.py`), about 76 TED units per pixel. A census
position matches when the centre of its minimap icon or blip lies within 2 minimap pixels
(about 150 TED units). Since EAWR-597 the companies spread around their marker as retail's do. Owners and colours are compared by the slot
colour on the unit, station and radar art. The Empire side is under fog in a slot-1 view, and
the SK-22 fleet is not a lobby start (fidelity list).
