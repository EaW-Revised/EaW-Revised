# Walk 27: land movement and pathfinding

## Scope and evidence

This walk covers terrain and object passability, footprint admission, land search,
company/squad slots, local collision correction, chokepoints, terrain speed and
garrison approach/exit. It prepares land skirmish and campaign land battles. The
visible contract is recorded separately from implementation mechanisms: the owner
permits different pathfinding internals when the visible behavior matches.

The comparison base is `cf94df0ba2928b80ce2b230e08a5dfdbd3fff97e`.
`src/assets/map_decode.cpp` retains validated land terrain and passability bytes;
`src/presentation/terrain/terrain.cpp` renders terrain. Neither supplies land
simulation. `src/sim/tactical/pathfind*.cpp`, `formation.cpp` and `motion.cpp`
implement space search/movement. Missing land behavior is classified **missing**,
not a measured regression in an existing land simulation. **Same** means a shared
interface/arithmetic contract that current code can reuse, with land inputs still
required. No game code, registry rows or retail assets change here.

Sources are **debug build**, **effective XML**, and existing sourced rules.
Opaque `ELPA-01` through `ELPA-07` name successful read-only inspection groups;
`DLPA-01` through `DLPA-05` name data snapshots and private hashes. The raw map,
decompilations, query receipts and registry audit remain ignored under
`out/research/walk-land-pathing/`. Failed lookups and symbol-name matches alone
are not evidence of behavior. No retail capture was made. Constants explicitly
marked **code** were read in an inspected branch; **XML** values are examples from
the selected effective data, not runtime defaults or bounds for mods.

Hand-offs: [land combat](land-combat.md) LC-01..22 owns companies, physical
placement, terrain/combat modifiers and crushing; LC-37..41 owns containment.
[Movement](movement.md) WMV-12 owns shared formation keeping, and
[space movement](../space-movement.md) owns space search and prediction layers.
Land AI owns decisions and threat arguments, not this search's obstacle mechanics.
Land environment owns weather/material/object presentation and destruction causes.

## Evaluation and visible contract

The groups below follow dependencies: derive cells, admit footprint/destination,
choose/map a route, follow slots, correct movement, complete an order. They are not
a serial per-object implementation or a new global schedule. Consult
[frame service schedule](../frame-service-schedule.md) FS-22/37..39,
FC-44..46/67..69 and FD-64 first. Those rows establish shared coordinator boundaries
and explicitly retain land-specific unknowns. Exact land collision, approximation,
quota and repath timers, dirty-cell propagation versus destruction and first motion
belong to U-WLPA-1 and the land frame-map programme; this walk does not trace them
ad hoc or turn XML timer values into proven service periods.

The following are source-based acceptance contracts, **not observed retail clips**:

| Player-visible contract | Verified mechanism / rule IDs | Remaining qualification |
|---|---|---|
| Infantry and vehicles can take different routes across water, walls, steep and infantry-only ground. | Authored `MovementClass` masks; all required cell bits must be admitted, WLPA-01 and the movement-mask inventory below. | Map flags are authoritative inputs; no invented height-to-cliff slope threshold. |
| A point legal for a small unit can be illegal for a wider company; a legal clicked destination can be adjusted around occupied ground. | Footprints, movement zones, width repair and nearest-open search; WLPA-06/12..16/22..25/29. | Exact chosen side/slot and camera-visible bridge sequence need U-WLPA-2. |
| Squads keep spacing, widen when spread out, turn/reassign slots and can slow the group for a straggler. | Authored arrangement, member geometry, remapping and catch-up slowdown; WLPA-26..32. | Error constants and full animation/turn-start behavior retain U-WLPA-3. |
| Land units can yield, steer, slow, pass or take a local path around another unit. | Runtime collision filters, vectors, speed overrides and temporary paths; WLPA-33..45/56. | No promise of a universal right-of-way or deadlock duration. |
| Crossing an authored slow surface changes speed and leaving it removes that source's modifier. | LC-14/16/19 and WLPA-46/47. | Mud is authored but its inspected consumer is not settled; U-WLPA-4. |
| Garrison entry approaches a host/bone and fades; exit uses a legal position and host state rather than arbitrary instant teleportation. | WLPA-48..50 and LC-38..41. | Emergency unload, host loss and blocked exits retain U-WLPA-5. |

An alternative deterministic planner may replace rectangle graphs or the local
search implementation. It must retain the sourced data admission, company identity,
destination/order semantics and qualified visible outcomes, and measure deviations
against a retail witness. The alternate planner must not treat unverified stalls,
bridge priorities or timers as settled behavior.

## Rules

### Cell derivation and occupancy

| ID | Retail rule and conditions | Source | Existing note | Ours / gap |
|---|---|---|---|---|
| WLPA-01 | A cell is traversable only when every bit of its derived passability is in the mover's authored mask: intersection with the mask must equal the complete cell value. Any-one-bit admission is insufficient for combined restrictions. | debug build ELPA-01/02; XML DLPA-01 | Same LC-08; adds exact subset predicate. | Missing P1: `pathfind.cpp` has space collision layers, not land cells. |
| WLPA-02 | Setting terrain passability sets the addressed cell's base value. The inspected setter also colors the terrain for debug display; those colors are not gameplay material or movement masks. It does not derive a cliff threshold from height. | debug build ELPA-01 | Missing there; LC-07 takes the existing grid as input. | Missing P1: `map_decode.cpp` keeps raw plane bytes; add a simulation cell profile. |
| WLPA-03 | Derived passability combines the base value with each primary occupant's instance occupation passability and each secondary occupant's type `SecondaryOccupationPassability`. Primary and secondary lists contribute independently. | debug build ELPA-01 | Missing source composition detail. | Missing P1: terrain/structure/prop occupancy registry. |
| WLPA-04 | If the combined value has any bit other than Clear, remove Clear. Only an actual derived-value change marks the partition state dirty. Removing one occupant must preserve other occupants and the base restriction. | debug build ELPA-01 | Same LC-07 dirty-state boundary; missing combination rule. | Missing P1: source-preserving derived state and invalidation. |
| WLPA-05 | Playable-boundary refresh marks cells unplayable when their centres are at or beyond any min/max XY boundary, including equality. It marks partitions dirty and cleans land pathfinder state after the pass. This is not inferred from rendered terrain edges. | debug build ELPA-01 | Missing there. | Missing P1: authored playable bounds and path invalidation. |
| WLPA-06 | Footprint occupancy refreshes dirty partitions and tests a square rectangle in cell coordinates using instance/type soft footprint. With no explicit mask, derive it from `MovementClass`; missing required object/type fails. Point occupancy instead includes `OccupationStyle` and applies the subset predicate to that occupied-cell value. Do not replace the footprint test with one centre sample. | debug build ELPA-01; LC-07 | Same LC-07; separates point and footprint queries. | Missing P1/P2: land queries beside `pathfind_collision.cpp`; shared soft-radius parsing is not this consumer. |
| WLPA-07 | Land obstacle occupancy uses `Obstacle_Proxy_Type` when present, else its own type. Ordinary objects use authored obstacle width/height/XY offsets/Z extent; combined objects use world-space model bounds. Raster samples step by code 10 in each local XY axis, then rotate by facing and translate by object position. | debug build ELPA-01 | Missing prop/structure source detail. | Missing P1: static-object footprint raster; no fixed prop-name list. |
| WLPA-08 | With `Fine_Tune_Occupied_Passability`, the obstacle raster requires a vertical model/proxy collision at each sampled position; the ordinary obstacle probe uses code Z endpoints -1000 and +1000. Without that option, the footprint samples are used directly. A proxy is disposed after its temporary occupancy probe. | debug build ELPA-01 | Missing model-refined occupancy. | Missing P1: authored refined raster geometry. |
| WLPA-09 | On base-water cells, ordinary obstacle occupancy is skipped when water elevation plus code 5 exceeds terrain height plus obstacle Z extent. Nonwater cells bypass that height gate. This determines obstacle contribution; it is not evidence that every bridge clears water or that submerged visuals are traversable. | debug build ELPA-01 | Missing water/obstacle boundary. | Missing P1: water/height/obstacle context and equality case. |
| WLPA-10 | Wall occupancy uses authored width/height/offsets, the same code 10 raster and facing transform, with optional model refinement. It registers the wall only once per sampled cell. Wall traversal still comes from the mover's authored passability mask. | debug build ELPA-01; XML DLPA-01 | Same LC-08 masks; missing wall contribution. | Missing P1: dynamic wall occupancy; destruction causes belong to environment. |
| WLPA-11 | Obstacle unoccupancy revisits its own authored/proxy or combined-object bounds and removes only matching occupied references. It clears the object's occupied-state flag. The inspected removal is not a blanket reset to Clear and does not prove every destruction/owner-transfer hook's schedule. | debug build ELPA-01 | Missing removal contract; same LC dirty-refresh interface. | Missing P1: removal/replacement hooks; U-WLPA-1 for exact phase. |

### Movement classes, search and destinations

| ID | Retail rule and conditions | Source | Existing note | Ours / gap |
|---|---|---|---|---|
| WLPA-12 | A mixed group intersects its members' movement masks and resolves an authored movement class with that common mask. Invalid common class is a distinct failure, not permission to use the least restrictive member. The two-class operation uses the same intersection. | debug build ELPA-04 | Same LC-08 authored masks; missing group combination. | Missing P2/P3: land group mask beside `formation.cpp`. |
| WLPA-13 | Formation route-class selection first resolves the common class and tests same-zone destination connectivity. A disconnected end may seek a same-zone cell with code search bound 30. When a matching no-shield class still connects start and candidate end, prefer that class; otherwise retain the original common class. This is not a universal command to enter shields. | debug build ELPA-04 | Missing route-class selection. | Missing P2: land connected zones and shield alternative. |
| WLPA-14 | Path-call initialization requires source object/type and reads `MovementClass`, occupation style and current soft coordinate radius. The destination overload also captures the object's current start and requested end. It does not substitute a fixed infantry radius for vehicles or mods. | debug build ELPA-02 | Same LC-07/08 inputs; missing call profile. | Missing P2: land search request snapshots. |
| WLPA-15 | Static search clears the output, resets search state and computes start/end. An explicitly forced trivial path appends start/end directly. Otherwise rectangle search wins when enabled; if disabled, enabled cell search is used; with neither enabled it fails. Trivial-path selection is a call configuration, not permission to bypass all command admission. | debug build ELPA-02 | Same LC-12. | Missing P2: land search dispatch beside `pathfind_search.cpp`. |
| WLPA-16 | Search first finds a route with minimum width zero. A positive requested width disables initial smoothing/display and repairs runs whose segment width is below requested width minus code 0.1. A width-constrained repair failure rejects the route. Restore requested width/smoothing/display and postprocess the combined route afterwards. | debug build ELPA-02; LC-11 | Same LC-11; no universal narrow-bridge permission. | Missing P2: width-constrained routes and failure result. |
| WLPA-17 | Cell expansion first asks whether the source object can enter the candidate cell. Its base step cost is code 1; when the call considers threat, multiply that cost by the threat read. This is distinct from terrain speed and from the dynamic graph's distance cost. | debug build ELPA-02 | Missing cost/input distinction. | Missing P2: land entry/threat query; AI owns the threat supplier. |
| WLPA-18 | Static postprocessing copies nodes directly when smoothing is off. With smoothing on, it runs binary smoothing, copies the surviving nodes and optimizes the path. Display work additionally needs both call and global display permission. Smoothing must preserve admission and required width. | debug build ELPA-02; LC-11 | Same LC width/postprocessing boundary; missing stages. | Missing P2: legal smoothing; renderer-only debug lines are not navigation. |
| WLPA-19 | Dynamic search builds an object/terrain visibility graph, initializes its search, and first excludes collision-marked connections. On failure it retries that graph allowing collision connections. Floating-destination search has the same fallback and refuses identical start/end. Graph-build failure returns failure; it is not an automatic direct path. | debug build ELPA-03 | Missing dynamic land fallback; WMV-04 excludes runtime collision only in space. | Missing P4: land dynamic local route search. |
| WLPA-20 | Dynamic expansion fails with zero source maximum speed. Otherwise it uses a minimum-cost open queue, retained per-call visited/best/closed arrays, and lower accumulated-cost replacement; reaching the exact graph end reconstructs the result. Base expansion cost is 3D segment length, multiplied by edge weight. Estimated time annotations use the source speed. Graph nodes add to the complexity counter. This does not establish quota-reset cadence. | debug build ELPA-03 | Missing dynamic search semantics. | Missing P4: bounded deterministic search; quota seam U-WLPA-1. |
| WLPA-21 | Dynamic edge restrictions retain annotations and end-object identity. Ordinary clear/special permitted edges have weight code 1. A direct start/end connection while start-overlapped, or a connection intersecting another nonendpoint object, uses `DynamicObstacleOverlapPenalty` (effective XML 15); the latter is collision-marked. The two-pass policy decides whether marked connections are usable. | debug build ELPA-03; XML DLPA-02 | Missing weighted collision fallback. | Missing P4: annotated temporary paths and contact ownership. |
| WLPA-22 | Dynamic destination validity first searches nearest terrain-legal position with object collisions temporarily ignored. It accepts adjustment no farther than `LandTemporaryDestinationProximity` (XML 20), including equality. It then tests the mover's oriented hard extents against the current instant land object-tracking tree, rejecting any reported object not ignored by this call's destination filter. Zero approach direction falls back to current facing. | debug build ELPA-03; XML DLPA-02 | Missing dynamic destination admission. | Missing P2/P4: legal temporary destination and separate object check. |
| WLPA-23 | Dynamic object filtering ignores null/self, crushable-by-source objects and noncollidable pairs. Other branches depend on request masks, deadlock/pass lists, deployed/deploying state, obstacle/stopped/friendly-stopped directives, relative motion and retained collision state. Deployed objects are not silently omitted by a generic stopped-object exemption. A default space-layer filter cannot stand in for this land policy. | debug build ELPA-03/05; LC-23..25 crushing boundary | Missing filter policy. | Missing P4: data/collision-state filters; full subtype mapping U-WLPA-6. |
| WLPA-24 | Nearest-open padding is the larger of code 0.25 times current hard radius and code 0.3 times (`FormationSpacing` times 20); null source gives zero padding. Radius and spacing remain authored/instance geometry. | debug build ELPA-04 | Missing nearest-open padding. | Missing P2: land candidate geometry beside `formation.cpp`. |
| WLPA-25 | Nearest-open placement carries ring/subdivision state for at most code 20 tries. It tests playable bounds and either same-zone connectivity or an explicit class's point occupancy; minimum-distance and facing-preference modes have separate candidate gates. Unless ignored, object collisions and predicted-segment overlap also reject candidates. Ring increment grows by code 0.8, capped by the smaller of code 10 and 1.5 times hard radius (code 5 cap fallback for zero radius). Failure preserves a distinct no-position result. | debug build ELPA-04 | Missing bounded candidate search; same LC-09 destination legality. | Missing P2: resumable nearest-open query, preserving admission and candidate order. |

### Companies, squad slots and following

| ID | Retail rule and conditions | Source | Existing note | Ours / gap |
|---|---|---|---|---|
| WLPA-26 | Team slot mapping normalizes requested facing and remaps only when never mapped or last/current facing dot is at most code 0.99. It dispatches the type's authored `ContainerArrangement` to rows or dartboard; successful mapping retains axis/mapped state. It is not a per-frame reshuffle of every company member. | debug build ELPA-04 | Missing team slot mapping; LC-03 owns container identity. | Missing P3: land team mapping beside `formation.cpp`/`fighters_formation.cpp`. |
| WLPA-27 | Rows collect members and sort them by spatial projection, using ceil(sqrt(member count)) as row capacity. Row geometry combines members' hard extents and each type's `FormationSpacing` times code 20; edge/intermember half-spacing and maximum row extents centre the rows. Active spread-out multiplies spacing by `Spread_Out_Spacing_Coefficient` (XML 1.8). Store each member's base offset, with optional placement/facing initialization. | debug build ELPA-04; XML DLPA-02/04/05 | Same LC-13 spacing interface; missing runtime row reassignment. | Missing P3: geometry-driven member slots, no fixed nine-member assumption. |
| WLPA-28 | Dartboard uses first-member geometry/spacing, ceil(sqrt(N)) and the required row count to size its candidate region. It samples a deterministic pair initialized from synchronization frame times container ID, rejects candidates too close to earlier slots, and tries at most code 1000 samples. Insufficient slots falls back to rows. Spread-out enlarges geometry by its authored coefficient. The exact extra separation allowance remains U-WLPA-3. | debug build ELPA-04; XML DLPA-02 | Missing dartboard/fallback policy. | Missing P3: deterministic scattered slots; alternative implementation requires visible comparison. |
| WLPA-29 | Base formation width starts at the largest member's formation hard-Y footprint. For multiple members it takes the larger of `ceil(sqrt(N))*largest + 0.25` and `sqrt(max(1, areaSum*1.8))` (code constants). Width-at-tracker tests a perpendicular cell line from the current tracker position toward the requested side and reports the admitted reach. | debug build ELPA-04 | Same LC-11 width requirement; missing width producer. | Missing P2/P3: live group geometry and chokepoint width. |
| WLPA-30 | Container straggler correction starts from the container type's actual maximum speed. For a member farther behind than the ideal oriented box's rear extent plus code 2, reduce group speed by `1-clamp(extraLag/10,0,1)`; take the smallest resulting speed over members. Failure to obtain a box or no members leaves the baseline. This slows the group; it is not an unconditional speed boost for every straggler. | debug build ELPA-04 | Missing container-specific catch-up; WMV-12 is shared formation keeping. | Missing P3: container lag feedback. |
| WLPA-31 | An infantry member follows its parent facing, base slot and jiggle offset. Longitudinal error blends parent speed toward member max for lagging slots and quadratically toward zero for overshoot; a turning member requests zero speed. Lateral error steers toward the slot, then runtime collision correction adjusts that direction. Exact error/steering constants and low-level first-step normalization remain U-WLPA-3. | debug build ELPA-04/05 | Missing member following; same WMV-12 conceptual boundary. | Missing P3: infantry-member locomotion rather than point teleporting. |
| WLPA-32 | Shared formation keeping changes side offset only beyond `FormationMinimumSideError` and normalizes it by `FormationMaximumSideError` (effective XML 0.1 and 30). Forward/side errors and deceleration constrain speed; escort speed and single-member space branches remain as WMV-12/40..42 describe. A shared scalar reader does not certify land containers. | WMV-12/41/42; XML DLPA-02 | Same shared mechanism. | Same shared profile/formation interface in `unit_motion.cpp` and tactical formation code; P3 adds land inputs/containers. |

### Runtime collision, avoidance and repath boundaries

| ID | Retail rule and conditions | Source | Existing note | Ours / gap |
|---|---|---|---|---|
| WLPA-33 | Collision eligibility rejects missing object/type. Multiple-locomotor objects consult the active locomotor's exclusion; a missing active locomotor does not itself make that object noncollidable. Static and dynamic collision paths are distinct from space layer prediction. | debug build ELPA-05 | Missing land eligibility; same WMV-04 space exclusion. | Missing P4: real land collidable registry and active locomotor adapter. |
| WLPA-34 | Pairwise collision needs at least one locomotor and rejects limbo. A build pad collides with a mobile nonally, while allied movement can pass its pad. A producer and its currently constructed/finished child do not collide with each other. These relationship and construction branches precede generic class handling. | debug build ELPA-05 | Missing pairwise gates. | Missing P4: relationship/producer inputs, not generic circle rejection. |
| WLPA-35 | Low obstacles collide with infantry; medium with infantry, bikes, vehicles and large vehicles; high also with giant vehicles. The test is symmetric in the pair. Other pairs pass the generic branch. `UnitCollisionClass` controls this independently from terrain traversal `MovementClass`. | debug build ELPA-05; XML DLPA-04/05 | Missing height-class matrix. | Missing P4: authored collision matrix; no hardcoded unit-name exceptions. |
| WLPA-36 | A team-container movement collision first tests cell-point occupancy. Failure calls static-terrain collision handling. That handler sets walk speed zero, creates terrain contact resolution and registers it; if no resolution can be created, it asks the locomotor to stop. An impassable next step does not commit movement through the cell. | debug build ELPA-04/05 | Missing land runtime terrain response; LC-07 admission. | Missing P4: movement commit/terrain-contact seam. |
| WLPA-37 | Movement-vector correction gathers the object's retained collisions (using container ownership for the admitted team-member branch), obtains each avoidance vector/priority and discards vectors that lead to terrain collision. Choose highest priority, longest vector on an exact priority tie; add other vectors only with dot product against the chosen vector at least code -0.5. The member branch adds its parent-relative avoidance correction. No contacts yields zero correction. | debug build ELPA-05 | Missing runtime vector policy; differs by scope from space's WMV-04. | Missing P4: local correction staged before land motion commit. |
| WLPA-38 | Velocity correction starts at requested speed and takes the minimum override over retained contacts, also returning the contact's instantaneous-deceleration flag. Moving/moving contacts return the assigned leader/follower speeds only with valid pair IDs and noninitial state; otherwise use the type's maximum speed. Steering and speed correction are separate interfaces. | debug build ELPA-05/06 | Missing local speed overrides. | Missing P4: contact speed/deceleration state, not permanent unit speed modification. |
| WLPA-39 | Basic avoidance is zero for missing objects, `Avoidance_Disabled`, invalid pair state or empty team contents. For a team obstacle it sums member-level resolving vectors except self; otherwise use the object-level vector. Scale the result by clamp(current walk speed / actual maximum speed,0,1). Terrain-valid left/right checks use cell lines in the mover's movement class. | debug build ELPA-05 | Missing local avoidance/terrain validity. | Missing P4: geometry-sensitive correction with authored disable and member roster. |
| WLPA-40 | Temporary path-around resolution uses dynamic land search, selected pass/avoidance directives, current hard radii and optional `MovementBoxExpansionFactor`. Terrain avoidance adds code 40 to its reach. A result needs more than one node and subtype acceptance; attach collision identity/type, obtain the compatible locomotor collision lock, add/mark the tracker started and retain the local destination. Failed admission returns failure; it does not erase the base order. | debug build ELPA-05 | Same shared tracker stacking interface; missing land contact producer. | Missing P4: temporary-path identity/lock state and return to base movement. |
| WLPA-41 | Overtaking refuses missing/deleting objects, a giant-vehicle passing source, missing movement coordinator/locomotor, zero speed or zero closing speed, or a source stopping at approximation end. It compares time to clear separation plus both soft radii against remaining travel time. Same-coordinator formation sort order further gates a pass; potential follower speed is capped to half passer speed in the inspected branch. This is not unconditional faster-unit right-of-way. | debug build ELPA-06 | Missing pass admission. | Missing P4: dynamic group and remaining-path context. |
| WLPA-42 | Same-formation offset exchange permits identical types. Different types require equal `FormationOrder` and mutual hard-extent compatibility after each type's half spacing (`FormationSpacing*20*0.5`). Different order or incompatible extents reject exchange. | debug build ELPA-06 | Missing slot exchange. | Missing P3/P4: safe same-formation pass; preserve member identity. |
| WLPA-43 | Stopped-overlap checks honor landing-transport collision permission and exempt an entrant from its targeted garrison. Individual infantry versus large/giant vehicles can be exempt when basic avoidance is zero. Otherwise compare oriented-box intersection coefficient strictly above `MinimumStoppedVsStoppedOverlapCoefficient` (XML 0.15); equality is not overlap by this predicate. | debug build ELPA-06; XML DLPA-02 | Missing stopped-contact admission. | Missing P4: typed stopped overlap, not every soft-circle touch. |
| WLPA-44 | Move truncation requires a locomotor and respects other retained collision/lock obligations. Giant/ordinary/large vehicle movement is not truncated merely for stopped infantry or bikes. Other eligible pairs may truncate through the subtype's path interface. This predicate supplies permission, not a universal immediate-stop rule. | debug build ELPA-06 | Missing moving/stopped truncation gate. | Missing P4: order-preserving contact response; full subtype consequences U-WLPA-6. |
| WLPA-45 | Path change gathers retained contacts involving that object and notifies each. New or interrupted movement therefore has a contact-resolution invalidation interface; a retained avoidance path cannot be assumed valid forever after replacement orders. Full reset/retry/deadlock and save/load producers remain U-WLPA-1/6. | debug build ELPA-05 | Same shared replacement-order boundary; missing land contacts. | Missing P4: contact notification and deterministic retention. |

### Speed, garrison and order completion

| ID | Retail rule and conditions | Source | Existing note | Ours / gap |
|---|---|---|---|---|
| WLPA-46 | Actual speed uses instance override, mode/unit base speed, crush-fleeing, disabled engines, retreat pursuit, movement modifiers, base shield and on-fire state as LC-14 records. Ordinary movement modifier output multiplies speed by `1+total`, rather than taking an additive world-distance delta. Effective fleeing and base-shield examples are XML 2 and 0.5. Terrain and contact overrides are separate inputs. | LC-14; shared ability/motion arithmetic; XML DLPA-02 | Same LC-14 shared pipeline. | Same shared speed/modifier arithmetic in `unit_motion.cpp` and tactical durability/abilities; P5 adds land context. |
| WLPA-47 | On logical-surface change, remove previous source-tagged modifiers before applying current settings selected by `SurfaceFX_Name`. The verified slow/water consumer applies `Speed_Mod` (infantry example XML -0.5); the same authored mud value alone does not prove a consumer. Rendering a muddy texture does not establish a movement modifier. | LC-16/19; XML DLPA-03/04/05 | Same LC terrain rules. | Missing P5: logical surface-to-sim bridge; `terrain.cpp` only presents it. |
| WLPA-48 | Garrison approach defaults to host position if entry data/bone selection is missing, no bone indices exist or transform lookup fails. With a valid bone use its world translation, extended radially from host centre by positive `Garrison_Enter_Dist`. Entrance movement, slot recheck and fade/commit remain LC-38/39; it is not immediate containment on issue. | debug build ELPA-07; LC-38/39 | Same physical entrance; adds destination producer. | Missing P6: garrison movement/bone target adapter. |
| WLPA-49 | For an exit-capable formation member, exit position begins at host or selected garrison bone, with outward facing. Positive `Garrison_Exit_Dist` supplies the offset; otherwise use the host world-bounds extent length. Nonformation objects take the host-specific fallback and this function reports no formation exit destination. | debug build ELPA-07 | Same LC-41 interface; adds exit geometry. | Missing P6: host/bone exit path and nonmobile fallback. |
| WLPA-50 | Un-garrison admission rejects gravity-field immobilization. For a hovering locomotor it searches near the current position with class, padding, objects and same-zone checks and refuses if the legal result differs from that position. Host-stationary/locomotor-state and free-exit-space gates from LC-41 still apply; emergency destruction exit is not settled by this predicate. | debug build ELPA-07; LC-41 | Same garrison exit boundary. | Missing P6: land exit permission/placement; blocked/hover cases. |
| WLPA-51 | Company landing offsets use authored member and squad grouping, facing, footprints and spacing, with all member positions admitted before the landing call. Creation's later partial-success behavior is a separate LC-01 stage. Tactical object count is not company count; population/parent identity must survive path/slot operations. | LC-01..04/13 | Same company identity interface. | Missing P3/P6: land member/squad creation beside tactical formation and landing, not a space squadron rename. |
| WLPA-52 | Shared queue/replacement semantics preserve current and pending destinations, repeat marks, completion history and member membership. An admitted replacement order removes addressed eligible members from their old coordinator; refusal/lock must not silently erase remaining members' orders. LC land commands supply admission; the shared order machinery remains authoritative. | FS-37..39; WMV-10/11/18; OR rules | Same shared order contract. | Same shared formation/command ownership in `session_step_movement.cpp`/`formation.cpp`; land adapters remain P3/P4. |
| WLPA-53 | Shared completed-destination rechecks and exhausted-search failure history are distinct from motion completion. FC-67/68 and FD-64 specify strict shared retry boundaries; a failed route is not a reached point and cannot consume a pending waypoint. This walk does not infer every land collision failure uses the space planner's exact retry clock. | FC-67/68; FD-64; WMV-15 | Same shared failure/result distinction. | Same shared retained formation wait in tactical planning; P2/P4 must qualify land-specific failures separately. |
| WLPA-54 | Effective XML authors land prediction interval 2, minimum prediction distance 60, repush distance 6, moving/moving look-ahead 1.5, position-approximation rebuild 8, collision check 4, dynamic complexity quota 100/reset interval 5 and movement reevaluation 10. Those values are data evidence, not proof of their unit normalization, first service, reset or every consumer. Preserve them as separate profile fields until the land frame-map closes U-WLPA-1. | XML DLPA-02; registry audit | Missing land consumer/timer qualification; FC-46 explicitly distinguishes coordinator reevaluation. | Missing P4: sourced clock/admission wiring, no invented periodic retry. |
| WLPA-55 | Bridge/chokepoint admission follows cells, connected zones, footprint, group width and destination constraints above. The giant-vehicle destination branch's detour limit is code 1.8 times direct distance (LC-10), not a universal bridge path bound. No inspected dedicated bridge symbol establishes a generic capacity, queue direction, priority, destructibility or reroute timer. Those visible details remain unverified. | LC-09..15; ELPA-01..04; unverified U-WLPA-2 | Same LC-15 unresolved crowd boundary, expanded mechanisms. | Missing P1..P4 verified adapters; capture gate before bridge/crowd policy. |
| WLPA-56 | Moving/moving resolution has distinct path-around, follower-speed, repulse, pass, infantry-push, same-direction-speed, no-action, offset-exchange and deadlock states. The inspected decision separates same-formation from other-formation pairs, motion direction, giant/crush gates and pass compatibility. These are retained alternatives, not one universal separation force. Virtual admission flags and every retry/deadlock producer are not fully qualified; U-WLPA-6. | debug build ELPA-06 | Missing collision state inventory; LC-15 did not settle it. | Missing P4: explicit contact state and known branch inputs; unresolved flags stay named interfaces. |

There are **56 rules: 0 differs, 52 missing, 4 same**. The shared same rows are
WLPA-32, WLPA-46, WLPA-52 and WLPA-53. Existing-note comparisons are per rule;
the land-combat note's LC-07..15 remains correct in its stated scope, while this
walk adds the cell derivation, dynamic/local collision and slot consumers it left
open. WMV-04's no-runtime-collision statement is explicitly space-only and must
not be copied to land.

## Effective movement-mask inventory

Source: **XML DLPA-01**, `MovementClassType` definitions. These are exact authored
set memberships, not a fixed engine whitelist. Custom masks and enum names must
continue through the validated data path. Some specialized classes are not ordinary
ground walkers; their presence does not prove every locomotor uses ordinary cell search.

| Class | Authored admitted cell values |
|---|---|
| Infantry | Clear, Shield, InfantryOnly |
| InfantryNoShield | Clear, InfantryOnly |
| Wheeled, Tracked, Walker | Clear, Shield |
| WheeledNoShield, TrackedNoShield, WalkerNoShield | Clear |
| Hover | Clear, Water, Shield |
| HoverNoShield | Clear, Water |
| LargeWalker | Clear, RebelWall, Shield |
| LargeWalkerNoShield | Clear, RebelWall |
| Flying, Space | Clear, Steep, Water, RebelWall, EmpireWall, Shield, Obstacle, InfantryOnly |
| Lancet | Clear, Water, RebelWall, EmpireWall, Shield, InfantryOnly, Steep |
| Spotter | Clear, Shield, InfantryOnly, Water |
| LargeHover | Clear, EmpireWall, Shield, Water |
| BombingRun | Flying set plus Impassable |

`UnitCollisionClass` is a different definition: ground data includes Infantry,
Bike, Vehicle, Large Vehicle and Giant Vehicle, feeding WLPA-34/35/41/43/44.
Do not use a collision size class as a terrain traversal mask or infer masks from
the unit's rendered model. The audited infantry `FormationSpacing` examples are
0.3, 0.4, 0.5 and 0.75; vehicle examples include 0.5 through 1.75. They are authored
examples, not minimum/maximum permitted values. The data's `Preferred_Pathfinder_Types`
list concerns invasion selection, not selection of the cell/rectangle search algorithm.

## Gap ownership and acceptance

These implementation packages reuse the existing land movement, terrain and
garrison work. They are not new gameplay rules or permission to duplicate their
tickets. Top five dependencies are P1 grid/obstacles, P2 width/destinations,
P3 company slots, P4 runtime collision and P5 reversible terrain speed. P6 supplies
the garrison movement seam after the common movement interfaces exist.

| Gap | Size | Rules / existing code boundary | Observable acceptance |
|---|---|---|---|
| P1 cell/mask/object topology | L | WLPA-01..11/55; `map_decode.cpp` data into a new simulation grid; occupancy lifecycle independent from `terrain.cpp`. Existing land passability package owns this. | Mixed-bit cells, boundary equality, overlapping primary/secondary occupants, proxy/refined/water raster, object removal and mod masks; class-specific tag application. |
| P2 width/search/destinations | L | WLPA-06/12..18/22/24/25/29/53/55; land search adapter beside `pathfind_search.cpp`, `pathfind_collision.cpp`. Existing width/destination package owns this. | Point versus footprint, disconnected zone, shield alternative, exact width tolerance, failed repair, nearest-open bounds and a legal alternative path. |
| P3 company mapping/following | L | WLPA-12/26..32/42/51/52; `formation.cpp`, tactical creation and distinct member locomotion. Existing company/movement packages own this. | Mod member count, rows/dartboard fallback, turn/remap threshold, spread out, mixed extents, straggler slowdown, slot exchange and identity preservation. |
| P4 runtime collision/local routes | L | WLPA-19..23/33..45/54..56; current space `motion.cpp` has no land contact system. Existing land movement umbrella owns verified collision mechanisms and their qualification. | Pair matrix, limbo, ally pads, construction parent, moving/stopped/crossing pairs, rejected terrain avoidance, speed override, replacement order and temporary path completion. |
| P5 terrain speed context | M | WLPA-46/47; `unit_motion.cpp`/modifier arithmetic with logical terrain state. Existing land terrain package owns this. | Enter/leave slow ground and water with stacked ability/base-shield modifiers; prove authored values and clear source-specific restoration; retain mud unknown. |
| P6 garrison movement | M | WLPA-48..51; tactical orders, model-bone interface and free-exit query. Existing garrison/landing packages own this. | Missing/valid bone, radius fallback, gravity/hover refusal, blocked exit, moving host, owner change and host destruction with correct containment identity. |

All implementation cases require equal results on 1/2/4/8 workers, immutable
partition inputs, disjoint staging and ordered commits. Count deterministic work
for performance regressions; do not reproduce retail's internal serial loops.
Presentation/retail captures then qualify motion, crowding and recovery. Both
correctness and visible deviation must be recorded for an alternate planner.

## XML tag-status audit

The private class-specific audit covers the comparison-base registry and its
shards; this documentation adds no applications or status changes.

| Status | Inputs / follow-up |
|---|---|
| `todo` | `MovementClassType/*`; `ApproximationForwardDistance`, `ApproximationSmoothCosAngle`, `FramesPerPositionApproximationRebuild`, `MinimumStoppedVsStoppedOverlapCoefficient`, `MovingVsMovingLookAheadTime`, `RepushDistance`, `VehicleFormationRecruitmentDistance`; some object classes of obstacle width/height/XY offsets, special-structure `Obstacle_Proxy_Type` and `SecondaryOccupationPassability`; some transport/unique `UnitCollisionClass`, and mixed container/space/unique `FormationOrder`. Implement the class-specific live consumer, not just a parser. |
| `deferred` | `BetweenFormationSpacing`, `Destination_Collision_Query_Extension`, `DynamicAvoidanceRectangleBound`, `DynamicObstacleOverlapPenalty`, `FramesPerCollisionCheck`. These were space milestone decisions; verified land consumers and U-WLPA-1 timing must now be owned explicitly. |
| `land-or-galactic` | Ground `MovementClass`, `UnitCollisionClass`, `FormationSpacing`, custom soft footprints, occupation/obstacle data and many prediction/form-up/surface/garrison inputs. This status does not establish land implementation. |
| `partial` / `applied` elsewhere | Shared soft footprint, speed, side-error or object bounds can have space/presentation/campaign consumers. Reuse the loader while adding land simulation evidence; do not mark the whole class/tag applied from its render mesh. |

The dynamic and mapping branches additionally read `Avoidance_Disabled`,
`MovementBoxExpansionFactor`, `FormationOrder`, `ContainerArrangement`,
`Spread_Out_Spacing_Coefficient`, `LandTemporaryDestinationProximity`,
`Garrison_Enter_Dist`, `Garrison_Exit_Dist` and garrison bones. Logical surface
profiles read `SurfaceFX_Name` and setting `Speed_Mod`; movement speed feeds the
ordinary modifier pipeline. Untraced form-up/recruitment/prediction scalars are
retained as evidence questions, not guessed effects.

## Unverified seams and retail captures

| Seam | Unknown | Capture / trace to close it |
|---|---|---|
| U-WLPA-1 | Complete land schedule: first/skip/reset/save-load for collision checks, approximation rebuilding, dynamic quotas, repath and obstacle invalidation; exact new-path acceptance/first physical motion. | Land frame-map programme. Log logical and synchronization frames, object/container/contact/formation IDs, dirty cells, old/new path and observed pose at t/t+1/t+2, equality and first due frame, through the in-battle Lua debugger with read-only inspection. |
| U-WLPA-2 | Visible bridge queues, narrow/wide chokepoint behavior, opposite traffic, crowd priority, persistent stalls and bridge destruction/repair producers. The static terrain and local contact mechanisms do not settle every outcome. | Fixed stock land map: one infantry company, bikes, ordinary/hover/large/giant vehicles through a surveyed choke, separately and together; then opposing directions, Stop/replacement and destructible-obstruction removal. Record route/mask/width/contact/order and a readable smooth-camera clip. No generic bridge capacity invented. |
| U-WLPA-3 | Every member error/steering constant, dartboard separation allowance, low-level displacement normalization, turn reactions and full arrangement consumer closure. | Targeted initializer/accessor trace, then a company with N=1/non-square/mixed member extents: remap near dot 0.99, spread out, one straggler and turn at rest/moving; record assigned offsets, container/member speed and first-step pose. |
| U-WLPA-4 | Additional mud/terrain-speed consumers, height-derived passability generation and weather/material-specific movement beyond LC-19's verified slow/water branch. | Fixed authored surface cells and unchanged unit/ability state across slow/water/mud/default, with modifier source and speed reads; terrain/environment walk owns conversion/destruction causes. |
| U-WLPA-5 | Blocked garrison exit, destruction/owner-transfer during entry/exit, emergency unload and specialized host overrides. | Enter/exit at valid/missing bones, stationary/moving/hover host, occupied exit ring; kill/transfer host in each phase. Record path, fade, roster, order and legal exit candidate. |
| U-WLPA-6 | Full contact subtype/virtual-flag producer closure, mover choice, retry/deadlock resolution and reset/save-load paths; exact right-of-way for every multi-contact configuration. Enumerated resolution states are not all qualified runtime outcomes. | Targeted subtype admission trace plus stopped/stopped, moving/stopped, head-on, same-direction, same/different formation and three-object corner cases; log retained contacts, leader/follower, speed, avoidance and temporary/base trackers. No universal deadline claimed. |

This inventory is complete for the stated verified rule boundary; the six seams
remain explicit qualifications. It does not claim retail route/crowd presentation
has been measured, or that a source-correct planner is already a live land game.
