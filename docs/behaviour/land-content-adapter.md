# Authored land content bindings

The skirmish boundary's `build_land_bindings` converts loaded land unit tables into
`LandSessionContent` and L1 movement masks. `build_land_start` has an authored
overload using that boundary, alongside its explicit-binding test interface.
No tactical simulation code calls the unit loader. Space never runs the adapter
or appends its output to content identity.

| Input | Application | Source |
|---|---|---|
| `Max_Speed`, `Object_Max_Speed_Multiplier_Land` | Member and team maximum speed, multiplied by the domain-selected scalar and 30 logical frames per second. For land containers, the instance cap is the minimum of type cap and successfully created member base speeds before this common multiplier. Raw team data stays intact. | BF-2 domain selection; LC-06/14; shared MV-01 units |
| `OverrideAcceleration`, `OverrideDeceleration` | Separate physical rates, multiplied by the same scalar and the square of the logical frame rate. An absent or negative override uses raw maximum speed. | MV-01 shared accessor fallback; LR3-14; LR13-23/25; LSL-09 |
| `Max_Rate_Of_Turn` | Degrees per frame become turns per second after the speed scalar and logical frame rate, then division by 360. Absent is the type default 2.3; zero never turns. | Shared MV-01 units; LR13-23; LSL-02/03 |
| `Custom_Soft_Footprint_Radius`, `Land_Model_Name`, `Scale_Factor` | A positive authored custom radius, or otherwise the maximum collision-bound XY half extent, multiplied by the scale's magnitude (absent scale 1.0). Missing model geometry with no positive radius refuses the binding. This supplies WLPA-06/14's soft footprint, not a guessed unit-class radius. | WLPA-06/14; shared AV-05 geometry; LSL-02/04/10 |
| `FormationSpacing` | Absent spacing contributes zero additional spacing. Member hard extents remain in landing slots; absent hard geometry retains a named refusal instead of borrowing the soft radius; the first member's spacing is kept separately from team-type and per-member spacing. Session placement applies the code factor 20 once. | LC-79/80; WLPA-24/27 |
| `MovementClass`, `MovementClassType/*` | Memberships are loaded from the engine's movement-class table through the VFS winner. Each named admitted passability resolves through the explicit raw-bit input; their union is the class mask. Member aliases differing only in case retain exact lookup names. | WLPA-01..11 |
| `UnitCollisionClass` | The authored name decodes against the engine-fixed set of eleven class names (absent = None; an unknown name refuses the member). The decoded class is copied onto the member locomotion profile, and its ordering rank still resolves through an explicit independent input and is copied into the live member. It never supplies terrain bits. | WLPA-35, WLPA-73..75; LR3-10 |
| `Walk_Transition` | Selects the live member's start/stop transition mode; animation completion remains an authoritative external gate. | LR13-17/18 |

These rules reuse the cited behaviour and research rows; they do not establish a
new retail timing or raw enum assignment. The engine table path is a schema
entry point, like the category and property enum files. Class names, membership
lists, companies and members come from data, including mod data.

Every value the retail parser accepts for a locomotion, company, team or static
geometry tag binds (loader-strictness-land.md, LSL-P1..13): absent values take
the type defaults, zero speeds are immobile members, non-positive custom
footprints use the model bounds and huge values saturate. The combat and
targeting rows of a land body are not covered yet (that note's Open section). Missing model geometry
without an authored replacement still returns the motion diagnostic with the
object and tag named. There are no unit-name exceptions. The land-only table record includes authored locomotion
and memberships. The adapter identity also includes resolved physical profiles,
raw-bit gates, collision ordering actually used, contact settings and occupations.

Session creation measures WLPA-30's ideal rear extent from member slots in the
formation's local forward axis (`max(0, -slot.x)`). Slots remain fixed after
creation, so turning rotates the same ideal box; checkpoint state carries the
measured extent. Formation-wide clearance at obstacle corners remains unverified:
the static-occupancy route proof uses a single-member squad, while the wider
formation's terrain-contact stall remains a named unresolved case.

## Explicit inputs and remaining behaviour

- **Unverified passability bits (R2/R3):** `unverified_passability_bits` must supply
  unique named single bits, including Clear. Enum order and the walk's example
  class table are never used to invent raw map bits. Installed adapter tests use
  explicit test assignments; they prove wiring and deterministic starts, not the
  unresolved retail bit assignment.
- **Collision ordering:** the caller supplies named ordering values separately
  from terrain masks. The adapter preserves the authored collision name and
  refuses an unbound name; numeric enum qualification remains external.
- **Animation completion (LR13-17..21):** start/stop/turn/deployment completion,
  stop points and attack movement admission remain `LandMotionGates` inputs.
  `Stopped_Rate_Of_Turn`, `Has_Pre_Turn_Anim` and authored hard XY extents are
  retained in the company data for the later state-dependent geometry/animation
  adapter. They do not replace the ordinary turn rate or current soft footprint.
- **Contact timing and capacity:** contact settings, caller-supplied static occupations, map
  bounds and last-known player capacities stay explicit validated inputs. This
  adapter does not invent service timing or a lobby.
- **R9 gate witnesses:** 100/120/140 are measured prop-centre distances at a
  20-unit test resolution (LR9-03/04), with unknown clear distance between prop
  footprints. They are validation witnesses, not per-class radius minima. No
  stock class threshold or unit-name table is embedded here.

Every loaded stock company is attempted individually by the installed test and
produces either a binding or a printed named refusal. Authored 1v1 and 4v4 starts
run 300 move ticks with equality on 1/2/4/8 workers and reversed partitions.
Walk-transition units still require the live animation gate producer; this
content-loading test does not fabricate clip completion.

## Static scene roots

`read_land_start_inputs` decodes a land-only `static_tables` sidecar from every
nonmarker scene type and every pre-built marker candidate. It follows variant
inheritance and `Obstacle_Proxy_Type` references through the catalog. The authored
start overload merges that sidecar with the company tables. Explicit-binding
callers may use `load_land_static_tables` and `build_land_static_binding` directly.
Space neither loads this sidecar nor encodes its records.

An attached `LAND_OBSTACLE` in effective `Behavior` or `LandBehavior` selects the
ordinary static producer across XML classes (WLPA-66). The geometry-selected
object is `Obstacle_Proxy_Type` when present; primary behavior contributions
remain those of the original occupying object.

The sidecar uses the existing ALO decoder and bind frames to retain the selected
model's XYZ collision bounds and collision triangles. Each absent or zero
`Obstacle_Width`, `Obstacle_Height`, `Obstacle_X_Offset` and `Obstacle_Y_Offset`
independently falls back to WLPA-61: width `2*abs(Y)*S`, height `2*abs(X)*S`,
offsets `-abs(Y)*S` and `-abs(X)*S`. Z extent is `abs(Z)*S`. These use half
extents; an off-centre box does not shift the accessor result. Nonzero authored
fields pass through without another scale multiplication. Missing model geometry
produces zero dimensions; zero and negative dimensions have an empty raster (LSL-08).

`Fine_Tune_Occupied_Passability` defaults true. Each raster sample requires a
vertical collision-triangle hit between world Z -1000 and +1000; false selects
rectangle samples. Empty geometry is never filled as a bounding rectangle.
An absent static `Scale_Factor` remains the named `unverified-static-scale` gate;
its constructor default is unsourced and may be qualified explicitly by the caller.
An authored zero or negative scale sizes the geometry by its magnitude (LSL-10).
Proxy collision uses its selected model and scale with the original instance
pose; the refined route requires the original model. Dropped map roll/pitch
remains a named transform gate. Combined-object world geometry and live
ownership/destruction invalidation remain separate producers.

Raster axes start at zero, advance by 10, and remain strictly below each extent
(WLPA-65). Samples rotate and translate by the instance pose. Cells are deduplicated
per instance; the water test excludes only when water plus 5 exceeds ground plus
Z extent, preserving equality (WLPA-09).

Primary passability is the union of attached behavior defaults: ordinary None,
obstacle Obstacle, wall EmpireWall. A non-None `OverridePassability` replaces
each default before union. `SecondaryOccupationPassability` remains independent.
The caller still qualifies raw enum bits through `unverified_passability_bits`;
`LandStaticGate::primary_passability` is an explicit raw Obstacle-bit input for
callers whose named enum qualification is incomplete. It supplies no geometry,
refinement or endpoint witness.

The installed audit prints each stock scene type as bound, census-only or a named
refusal. The earlier authored-only audit bound two Alderaan types and one Dantooine
type; absent width was the dominant refusal. Current receipts record the
model-backed counts and retain raw-bit qualification as an explicit test input.

Companies resolve only their own `Create_Team_Type`, with schema type `Team` as
the fallback (LC-81). Member references never substitute for that choice. The
generic `TEAM_LOCOMOTOR` path remains a named refusal pending member path/start
dispatch (legacy EAWR-4871); its raw type default 0.25 is never used as a universal
team motion rate. Transport-only members, unique-ground creation and absent
locomotors also retain distinct named refusals (LC-84).

After ownership/removal and pre-built placement, admitted static roots retain
their entity, type, owner and exact pose. All their occupations commit before
company placement, so L4 search and L5 motion see them at initialization. They
are absent from movable squads and reject movement commands. Company expansion
keeps its existing path. Bound profiles, gates, static records and rasterized
occupations participate in land content identity.

Section 18 remains version 1: its existing occupancy snapshot stores static
occupations. Static roster and poses rederive from setup and immutable content;
restore checks their identity, pose and exact occupation rather than adding
bytes. Replay reconstructs the same initial roots and occupations. Synthetic
contracts cover a custom prop, model fallbacks, strict endpoints and triangle refinement, an ordered squad
detouring around a structure, and checkpoint/replay equality. Installed starts
audit named bound/gated/census-only/refused types on the 1v1 Alderaan and 4v4
Dantooine maps, then compare 300 move ticks on 1/2/4/8 workers and reversed
partitions. Raw-bit test assignments remain labelled as such in the audit.

The land-only unit identity tail uses format 2 for model geometry and team roles.
Space encoding and its installed identity pin are unchanged.
