# Space sensor visibility and query candidate order

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space, the M2 fixture
  ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md)). Data claims come from the FoC profile
  of the retail corpus: `spaceunitscorvettes.xml`, `spaceunitsfrigates.xml`,
  `spaceunitsfighters.xml`, `units_space_empire_tie_interceptor.xml`, `squadrons.xml`,
  `containers.xml`, `starbases.xml`, `secondarystructures.xml`, `spacebuildablesskirmish.xml`, `props_story.xml`
  and `gameconstants.xml`.
- Bounded question: which objects each player sees at a completed tick (binary sensor
  contact), and in which order tactical queries return candidates. Space queries and
  visibility (legacy EAWR-68).
- Source tags: **data** (a tag in the FoC files), **runtime** (the FoC debug build recorded
  with fog on: RO-1 in squadron-container fog reveal (legacy EAWR-271),
  stagings S-91 and S-92; RO-3 in retail fog-cell contact rule (legacy EAWR-274),
  staging S-93), **research** (an existing note:
  [space targeting](space-targeting.md) R-05, R-07, R-08 and G-01; the
  [FoC tactical AI](foc-tactical-ai.md) AI-14; and the FoC debug build as read for the
  [debug-build audit](debug-build-audit.md), evidence IDs AU-nn), **inference** (our
  conclusion, not established) and **project** (a remake decision).
- Retail fog in brief (research AU-40 to AU-49): each player has a grid of fog cells, and
  every cell counts the revealers that cover it. An object with the `REVEAL` behaviour marks, for
  every player allied to its owner, a rasterised circle of cells around its own cell: the radius
  in cells is its reveal range divided by the cell size, rounded to nearest. That range is the
  type's `Space_FOW_Reveal_Range` (at least 10) times any reveal modifiers. Dense (nebula) cells
  inside that circle use the range times `Dense_FOW_Reveal_Range_Multiplier` instead. A
  revealer redraws its circle only after it has moved at least one cell width in the plane. An
  object is fogged to a player when all its sample points lie in unmarked cells: its position,
  and with `Multisample_FOW_Check` also the corners and edge midpoints of its planar bounding
  box. Only objects with the hide-when-fogged or team behaviour can be fogged at all, and an
  allied revealer is never fogged. A session bound to the map's fog grid follows these cells
  (V-11 to V-17, retail fog-cell contact rule); without one it keeps the exact test of V-05 to V-07 (project). Since
  reveal ranges and fogged attackers the M2 start is bound to its map's grid (V-18), and a revealing unit that fires shows
  itself to the player it fires at (V-19).

## Interface

- Inputs: the session's players (ID, team) in ascending ID, at most 64; each live unit's
  stable ID, type, owner and Q24 position after the tick's systems; the setup's squadrons
  (container and craft IDs, replay v3); the sensor table (type ID → reveal range); and,
  optionally, the fog rules (`FogRules`: the map's grid origin, cell size and cell counts,
  the service period and the ramp-down step). The sensor table and the fog rules are
  content, named by the setup's content identity, and neither replay data nor state. For M2
  the sensor table comes from the space-unit data loading unit tables (`units::sensor_range`, V-01 and V-03).
- Outputs, per completed tick: for each unit a 64-bit mask (bit k is the k-th player by
  ascending ID) and its type's reveal range, both in the tactical snapshot (encoding v2,
  [replay-format.md](../replay-format.md#snapshot-and-events)); per team, on request, a
  FogGridV1 for the fog presentation hook (`sim::tactical::fog_grids`).
- Cadence: the tick-zero snapshot, then every step after the per-unit systems and the
  squadron phase, from that tick's final positions. Without fog rules nothing is retained
  between ticks: visibility is a pure function of the completed state and the sensor table,
  so it is published but not hashed. With fog rules each revealer's last circle and every
  player's cell values are state and are hashed ([replay-format.md](../replay-format.md#canonical-tactical-state-hash)).
- Queries: `sim::tactical::SpaceIndex` over (ID, owner, position) of one tick, with a closed
  box and a closed range (planar or spatial) query and an optional owner filter.

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| V-01 | A space object reveals only when its type has `REVEAL` in its `Behavior` or `SpaceBehavior` list. There is no default: the range is always the revealing object's own type's `Space_FOW_Reveal_Range`, in source units. In the M2 roster: both stations 2000 (`Skirmish_Rebel_Star_Base_1` inherits it from `Rebel_Star_Base_1`), `Nebulon_B_Frigate`, `Acclamator_Assault_Ship` and `Tartan_Patrol_Cruiser` 1200, `Corellian_Corvette` 1000. Of the craft only `Y-Wing` has `REVEAL` (600); `X-Wing`, `TIE_Fighter`, `TIE_Interceptor` (500) and `TIE_Bomber` (600) author a range they never use. Map objects author their own (`Skirmish_Merchant_Dock` 850, `N_Gravity_Well_Station` 800, `Mineral_Extractor_Pad` 275, `Defense_Satellite_Laser_Pad` 200, `Orbital_Resource_Container` 100); the unit tables do not load them yet. | data; runtime RO-1 |
| V-02 | `Corellian_Corvette` authors the tag twice in one layer, 1200 then 1000. The tables keep the last occurrence, as for every single-value tag ([unit-data.md](../unit-data.md)). | data; project |
| V-03 | A fighter squadron reveals through its team container, not its craft. Spawning a squadron creates its craft and one container whose type is the squadron's `Create_Team_Type`, or `Team` (CONTAINERS.XML) when it names none. The container reveals by V-01 with its own type's range: `Team` 800, so the four other M2 squadrons reveal 800; `Y-Wing_Squadron` names `Y_Wing_Squadron_Container`, a variant of `Darth_Vader_TIE_Fighter_Container`, which authors 800 then 1000, so 1000 (V-02). The container sits at the centre of its live craft's bounding box, per axis halfway between the extreme craft (the floor of the Q24 midpoint here), so there is one circle per squadron, not one per craft. It keeps its reveal when the leader dies, moves onto the last craft, and leaves the session with it (no event: it has no hull). A fighter without a squadron has no container and, without `REVEAL`, reveals nothing. At the M2 tick zero a squadron company is its squadron's container and its craft enter with it (fighter spawning and simulation, space-fighters FC-02). | runtime RO-1 (S-91, S-92); data; project (company at tick zero) |
| V-04 | A player always sees units owned by players of its own team, its own included. Retail shares coverage between allies (a revealer reveals for every allied player), but exempts an allied object from fog only when it reveals itself; an allied craft without `REVEAL` is fogged by cells like any other object. | research AU-40, AU-44; project (the exemption for all own-team units; M2 is one versus one) |
| V-05 | Without fog rules, a player sees a unit of another team when at least one unit of the player's team with a sensor profile lies within its own reveal range of that unit. Retail decides by fog cells, not by distance: V-11 to V-17. | project; research AU-43 to AU-47 (the retail rule differs by up to about one cell) |
| V-06 | Distance is Euclidean in the source XY plane; Z is ignored. The boundary is inclusive. Retail fog is a planar cell grid (`DesiredSpaceFOWCellSize` 100, `SpaceFOWHeight` −80 in `gameconstants.xml`, data), so height does not enter there either. | project; research AU-47 |
| V-07 | The test is exact: the squared XY difference of Q24 raw values against the squared raw range, in 192-bit integers. There is no square root, no rounding and no overflow for any int64 coordinate. Whether this matches retail binary32 comparisons stays gate G-02 of the targeting note. | project |
| V-08 | Contact is binary. Without fog rules it is also current; with them a released cell still reveals while it regrows (V-15). There is no last-known image and no radar-only state. | project; runtime RO-1 |
| V-09 | Rules v1 publishes visibility and consumes none of it. Targeting reads the mask for the R-08 "fogged to the firing owner" filter, for every owner: retail weapon targeting forces the fog test, so AI-owned weapons respect fog too. The AI's own planning ignores it while `AIUsesFogOfWarSpace` is `False`, the retail default (AI-14, SK-45); that flag relaxes only fog queries made without the force. | data; research (R-08, AI-14, AU-41) |
| V-10 | Neutral and hostile map owners are players like any other: their objects are seen only through contact, and their sensors reveal only to their own team. Retail reveals for allies of the owner (AU-44). An object without the hide-when-fogged or team behaviour is never fogged in retail; every M2 map object has the hide-when-fogged behaviour. | research AU-40, AU-44; data |

### Retail fog cells

These rules apply to a session bound to fog rules (retail fog-cell contact rule). The logical fog they give is what
targeting reads (V-09); presentation still draws F-01.

| Rule | Behaviour | Source |
|---|---|---|
| V-11 | Every player has a grid of `cells_wide` × `cells_tall` cells of `DesiredSpaceFOWCellSize` (100). Column c is floor((x − MapLeft) / cell); row r is floor((MapTop − y) / cell), so rows run toward −Y. Each cell counts the revealers that hold it and has a value from 0 (fogged) to 255. A revealer marks its circle in the grid of every player on its owner's team: each cell is held once more and its value becomes 255. | research (FoC debug build); runtime RO-1 (the recorded grid: 130 × 130 from −6500 on Coruscant) |
| V-12 | A revealer marks at its first tick and again only when it has moved at least one cell width in the plane since it last marked (squared distance ≥ cell², inclusive). Marking again releases the previous circle; a revealer that leaves the session releases its circle. The circle is centred on the cell of the revealer's position when it marks, clamped into the grid; where it stands inside that cell does not matter. So a moving revealer's circle lags by up to one cell. | runtime RO-1, RO-3; research (FoC debug build) |
| V-13 | The circle's radius in cells is int(range / cell + 0.5), with the range at least 10. The range is the type's V-01 range times (1 + the owner's battlefield reveal modifiers) times (1 + the unit's own reveal-range combat modifiers); nothing in the skirmish data feeds either (no `FOW_Reveal_Range_Multiplier` or reveal-range modifier outside galactic content), so in M2 it is the authored range. | research (FoC debug build); data (reveal ranges and fogged attackers); runtime RO-3 (Tartan 1200: 12 cells, Corellian 1000: 10) |
| V-14 | The circle is retail's midpoint (Bresenham) circle, filled on each row between the extreme points its eight octants plot there: radius 8 covers 221 cells, 10 covers 349 and 12 covers 489, as recorded. Near diagonals it reaches slightly past the exact disc: from a Tartan held at a cell centre, a target 9 cells east and 8 south (1204) is seen, one 9 west and 9 north (1273) is not, and at ±1 cell on the axes (1100, 1200, 1300) it agrees with the disc. Rows and columns outside the grid are clipped. | runtime RO-3 (S-93); research (FoC debug build) |
| V-15 | Linger. Player p's grid is serviced on the ticks t with t mod 16 = p mod 16 (`service_period` 16). A service leaves held cells at 255 and lowers each cell no revealer holds: to 238 at its first service, then by the ramp-down step, floor(238 × 16 / (`SpaceFOWRegrowTime` × 30)) = 21 for 6 s, down to 0. A released cell is therefore fogged at the 13th service after its release, 193 to 208 ticks later: in S-91 the squadron left e300's cell at t61 and e300 was fogged at t264, 203 ticks later, and S-92 gives 204. A cell marked again is held at once. | runtime RO-1; research (FoC debug build); data (`gameconstants.xml`) |
| V-16 | A player sees a unit of another team when any of its sample points lies in a cell whose value in that player's grid is above 0. A sample outside the grid is fogged. Every unit samples its position; types with `Multisample_FOW_Check` additionally use V-21. Own-team units are always seen (V-04). | runtime RO-3; debug build (sample construction and per-point query) |
| V-17 | Order within a step, from the tick's final positions (after the squadron phase): the grids due this tick are serviced, then in ascending revealer ID circles are released and marked, then the tick's flashes (V-19) are applied, then every unit is queried. Retail services its fog at the start of a frame, before its objects reveal. | research (FoC debug build); project (order within one tick) |
| V-18 | A skirmish map's fog grid spans the map's declared extents (TED root fields 0x10 and 0x11) centred on the origin, in cells of `DesiredSpaceFOWCellSize` rounded up, at most 512 a side, serviced as V-15 with `SpaceFOWRegrowTime`. On Coruscant that is 130 × 130 cells of 100 from (−6500, 6500), the recorded grid. The M2 start, its live session and its replays run on it (reveal ranges and fogged attackers); a revealed map (every sensor sees the map) does not. FoC then shrinks the cell so the cells fill the extents exactly; for extents that are not a multiple of the cell this is not modelled. | research (FoC debug build); runtime RO-1; data (`gameconstants.xml`); project (revealed maps) |
| V-19 | A unit whose type has `REVEAL` shows itself to the player it fires at: every shot (a hardpoint's shot counts as its ship's) sets a circle to 255 in the target owner's grid alone, without holding it, so it regrows like a released cell (V-15). The centre is the unit position plus its model collision-box centre, without unit yaw; the radius is the larger XY half extent of that box. Radius rounding is V-13 without the ordinary reveal's 10-unit floor. The flash ignores dense attenuation. A missing model uses radius 10; a model without collision meshes has bounds ±1. The remake uses the loaded bind-pose collision box; animated bounds changes remain unmodelled. A unit without `REVEAL` (the M2 fighters other than the Y-wing) is not revealed by firing. Applies only on a fog grid (V-18). | debug build (weapon notification, temporary circle and object-space collision bounds); data; owner (reveal ranges and fogged attackers) |

### Scripted whole-map reveal and extended fog geometry

| Rule | Behaviour | Source |
|---|---|---|
| V-20 | `FogOfWar.Reveal_All(player)` is valid only in a tactical game, requires a player as its first argument, accepts additional arguments, and returns nil. It holds every cell in that player's grid, independently of allied and local observers, and ordinary fog refresh does not release those holds. The space burn plan requests this for its AI player during a firesale. In a non-multiplayer skirmish it separately requests the local player's reveal; outside skirmish that second call requires `Allow_AI_Controlled_Fog_Reveal` to be 1. Thus the human sees the map open up when the AI firesales in an eligible single-player game. | debug build (API validation, per-player dispatch, whole-grid hold and cell refresh); authored `burnunits.lua` |

| Rule | Behaviour | Source |
|---|---|---|
| V-21 | `Multisample_FOW_Check` adds the four corners and four edge midpoints of the yaw-oriented hard coordination box to the position sample. Positive `Custom_Hard_XExtent` and `Custom_Hard_YExtent` override model collision half extents independently; both extents take `Scale_Factor`. The box centre is the position plus `Custom_Hard_XExtent_Offset` and `Custom_Hard_YExtent_Offset`, rotated by yaw but unscaled; it does not use the model box centre. Without a model or both positive custom extents, the type has zero extents. Deployed extents and ideal team boxes are outside the current space roster. | debug build (sample points, hard box construction and type extents); effective XML |
| V-22 | Dense fog is a shared boolean union of midpoint circles initialized from map placements with `SPACE_OBSTACLE` and any nebula, asteroid-field, impassable-asteroid or ion-storm flag. Circle centre is position plus raw unrotated `Space_Obstacle_Offset`; radius is raw `Space_Obstacle_Radius`. Neither scale nor custom soft radius applies. Ordinary reveal first floors the full range to 10, multiplies it by `Dense_FOW_Reveal_Range_Multiplier` (default 0.5), then rounds each range separately to cells. Full-radius coverage applies only to normal destination cells, reduced-radius coverage only to dense ones, wherever the revealer stands. Overlaps do not multiply reductions. Releasing a circle releases exactly those selected holds. | debug build (reveal ranges and destination-cell selection); WHZ-09; effective XML |
| V-23 | A reveal source changing owner immediately releases its previous circle for the old owner's allied players and marks its current circle for the new owner's allied players. This refresh does not require movement. Old-team values linger under V-15 after their holds are released; overlapping reveal sources retain their independent holds. | debug build (ownership notification releases old allied holds and reveals to new allies); WNO-23/35 |

The remake routes V-20 through the next tick's replay command queue. Each recipient's existing
fog rows become clear, and a canonical persistent hold prevents regrowth. Reveals share a clear
row buffer and assign rows through the partition executor; later snapshots and visibility read
the same grid. No reveal changes an allied observer's grid unless the script requests that
observer separately. The existing fog model represents held cells at 255 immediately (V-11);
the debug build's initial reveal ramp is part of that existing approximation.

### Candidate order

| Rule | Behaviour | Source |
|---|---|---|
| Q-01 | Every query returns candidates in strictly increasing stable entity ID. It does not depend on insertion or storage order, worker count or the index's cell size. Target scans do not use this order since the target collection order and fighter approach work: they take FoC's collection order from the per-player trees of the targeting note (CO-01 to CO-12). | project |
| Q-02 | Box query: the closed axis-aligned box `centre ± half extent` on X, Y and Z. The targeting broad phase is this box with half extent 1.1 × weapon range on all three axes. A negative half extent returns nothing. | research (R-07); project |
| Q-03 | Range query: the closed disc (planar, XY) or ball (spatial, XYZ) of radius r, compared as in V-07. A negative radius returns nothing; radius 0 returns bodies at the centre. | project |
| Q-04 | An optional owner filter keeps only one player's bodies, as R-07 queries each hostile player in turn. | research (R-07) |
| Q-05 | A consumer of this index that walks candidates in order and keeps the first of equal score therefore keeps the lowest ID. | project (consequence of Q-01) |

### Fog presentation source

| Rule | Behaviour | Source |
|---|---|---|
| F-01 | `fog_grids(snapshot, layout)` returns one FogGridV1 per team in the snapshot's player table, in ascending team order, with revision completed tick + 1. Cell (x, y) is 255 when its centre (lower corner plus floor(cell / 2) raw on each axis) is within V-05/V-06 range of a unit of that team with a sensor, and 0 otherwise. `DesiredSpaceFOWCellSize` 100 is the natural cell size. | project; data (cell size) |
| F-02 | The grid replaces the painted fog stub as the source the fog texture cache reads. It is presentation data: nothing reads it back into the simulation, and a UI override such as the viewer's paint mode changes presentation only. | project (ticket scope) |

The live battle draws the local player's fog in the world as FoC's fog plane: [space-fog-presentation.md](space-fog-presentation.md) (FW-01 to FW-15, world fog rendering).

## Cases

The C cases are the eight frames of the oracle fixture `tests/replay/fixtures/tactical-visibility.*`.
In each frame a Rebel `Nebulon_B_Frigate` (1200) is at the origin and an Empire `TIE_Fighter`
(500) is at the listed position. Rules v1 moves nothing, so each frame is a one-tick replay.

| Case | Fighter at (source units) | Rebel sees the fighter | Empire sees the frigate |
|---|---|---|---|
| C-01 | (2000, 0, 0) | no | no |
| C-02 | (1200 + 2⁻²⁴, 0, 0) | no | no |
| C-03 | (1200, 0, 0) | yes (inclusive) | no |
| C-04 | (500, 0, 0) | yes | yes (the fighter's own 500) |
| C-05 | (1100, 0, 700), 3D distance about 1304 | yes (planar) | no |
| C-06 | (720, 960, 0), exactly 1200 | yes | no |
| C-07 | (720, 960 + 2⁻²⁴, 0) | no | no |
| C-08 | (1500, −200, 0) | no | no |
| C-09 | Players 1 and 4 share a team; player 4's 500-range fighter is 500 from an enemy | both see the enemy (V-04, V-05) | |
| C-10 | The same 400 bodies indexed in three insertion orders | identical ID-ordered results for every query (Q-01) | |

Every frame also passes through the session with 1, 2 and 4 workers and with scrambled
storage; the state hash, the snapshot digest and the masks are the same each time.

The S cases stage the RO-1 and RO-3 recordings in `tests/replay/squadron_fog_tests.cpp`,
with and without the Coruscant fog grid (130 × 130 cells of 100 from (−6500, 6500)). S-91: a
Rebel squadron container at (2500, −2500) with five craft at their recorded offsets, the
fifth already teleported to (1000, −2500); Empire enemies e300 (2800, −2500), e700
(2500, −1800), e900 (2500, −3400) and eaway (1300, −2500). The leader dies at tick 2, three
more craft at tick 3.

| Case | Staging | Expected (both rules unless stated) | Retail |
|---|---|---|---|
| C-11 | S-91 tick 0 | e300 and e700 seen, e900 and eaway not; a lone fighter reveals nothing | S-91 t0 |
| C-12 | S-91 after one step | the container is at (1756.5, −2490), eaway is seen, and no circle surrounds the teleported fighter | S-91 t61 |
| C-13 | S-91 cells: e300 after the release | seen through tick 203, fogged from 204 (Rebel's phase 11 aligns the first service as in S-91) | fogged 203 ticks after release |
| C-14 | S-91 leader death, then one craft left | the container stays at (1756, −2490) with its reveal, then sits on the last craft; it leaves with it | S-91 t120, t242 |
| C-15 | S-93 cells: Tartan at the centre of cell (35, 40) | 1100 E, 1200 N and 9E/8S (1204) seen; 1300 W and 9W/9N (1273) not; 489 cells, and 349 for a 1000 corvette. The exact disc misses 1204 | S-93 |
| C-16 | Cells: a container moves 90, then 100 units | 90 keeps its circle; 100 marks again and the released cell regrows (V-12, V-15) | RO-1, RO-3 |

Each S case also runs with 1, 2, 4, 8 and the hardware worker count, with scrambled storage,
and through a written and parsed replay v3, with identical hashes and digests.

## Unknowns

| Gate | Unknown | Effect |
|---|---|---|
| G-V1 | Resolved by V-21: flagged objects use the hard box's corners and edge midpoints as well as their position. | The station boundary now matches the sourced multisample rule. |
| G-V8 | V-19 now uses the loaded model collision box for firing reveals. Animated changes to that box remain unmodelled. | An animated model can flash a slightly different circle in FoC. |
| G-V2 | Resolved by V-22 and WHZ-09: destination dense cells use the revealer's multiplier. The initialized mask remains static after hazard movement/destruction, the hazard walk's U-03. | Static skirmish hazards use the sourced reduced reveal range. |
| G-V9 | Remembered model retirement still samples its saved centre (FW-27), whereas current live visibility uses V-21. | A remembered station can remain until its centre clears after a box sample has already cleared. |
| G-V3 | `Initial_State_Visible_Under_FOW`, `Last_State_Visible_Under_FOW` and `Visible_On_Radar_When_Fogged` on stations and map structures show some fogged objects in retail. They are not modelled. | Presentation may hide a fogged station or pad that retail draws. |
| G-V4 | Team-shared coverage (V-04, V-05) is confirmed by the debug build (AU-44). | None in M2 (one versus one). |
| G-01 | Target scans follow the retail collector order since the target collection order and fighter approach work (targeting CO rules); what stays open is the targeting note's G-01. | An individual frame's equal-priority choice can still differ from retail's. |
| G-V5 | The `Y-Wing` craft authors `REVEAL` (600), so by V-01 each Y-wing reveals on its own besides its squadron's 1000 container; the debug build shows no rule that suppresses a craft's reveal inside a team. RO-1 recorded X-Wing and TIE Fighter squadrons only. | Needs a runtime check (a Y-Wing squadron staging). If retail suppresses it, the Y-wing craft reveal more here. |
| G-V6 | The retail container follows its craft one or two frames late (RO-1); here it moves in the same tick, from the tick's final positions. | A container, and its circle, can lead retail by one or two ticks. |
| G-V7 | The retail service phase is the frame counter modulo 16 against the player's index; here it is the tick against the player ID. Retail clips a circle that crosses the map's top edge by shifting its rows, and one that crosses a side edge wraps into the next row; here both are clipped to the grid. | The linger can end up to 15 ticks earlier or later than in a given retail run, but always lasts 13 services; circles at map edges differ. |
