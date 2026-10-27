# Land pathfinding: the cell search

This note records the project's land path search (`include/eawr/sim/tactical/land_path.hpp`), the second
`PathService` backend beside the space state lattice. It consumes the land terrain field and occupancy grid
(`LandTerrain`, `LandOccupancy`) and implements the static-search half of the
[land movement walk](walks/land-pathing.md) (WLPA-12..18, WLPA-22..25, WLPA-29, WLPA-55) and LC-07..15 of the
[land combat walk](walks/land-combat.md), under the budget rules of the
[land path budget research](../research/land/R3-land-path-budget.md). Dynamic local routes, contacts and
deadlock recovery (WLPA-19..21, 33..45, 56) are land local movement and are not here.

The original walk allows an alternative deterministic planner when the visible outcome matches and the
deviation is recorded. The project searches the 20-unit cell grid directly; the original also has a rectangle
graph search that wins when enabled (WLPA-15). **Ours** below is that alternative. Evidence for each rule is
the walk row it cites; nothing here was measured against a retail recording.

## Interface

A `LandPathRequest` (entity, movement-class bits, start, end, soft footprint radius, requested width, optional
detour ratio, trivial and smooth flags) starts a `LandSearch`. A search runs in slices (`run(budget)`), can be
saved and restored (`checkpoint`, `LandPathBackend::restore`), and ends with a `LandPath` whose `status` is one
of `found`, `no_route`, `width_rejected`, `detour_rejected`, `destination_blocked`, `start_outside` or
`unstable`. The movement-class bits come from the content profile by data name
(`LandTerrain::movement_mask`); no class list, bit assignment or footprint is built in.

## Rules

| ID | Rule | Source | Ours / status |
|---|---|---|---|
| LP-01 | A cell's centre is `(i * pitch, j * pitch)` with no half-cell shift; a coordinate maps to a cell by adding half a pitch and truncating. A coordinate below minus half a pitch is outside the grid, never cell 0. | R3 LR3-16 | Same for in-grid values. The grid-allocation size (N versus N-1) stays unverified (LR3-16), so the grid is the terrain's width by height. |
| LP-02 | A cell is entered only when every bit of its derived passability is in the mover's mask (the subset predicate); base cost per cell entered is one. | WLPA-01, WLPA-17 | Same predicate, read from the committed occupancy grid, so primary and secondary occupants count (WLPA-03/04). Connectivity is not established by the walk: the search uses eight neighbours at 1000 per orthogonal and 1414 per diagonal step, with an octile heuristic. Remake choice; smoothing removes most of its shape. |
| LP-03 | A cell is admitted for a mover when its whole soft footprint rectangle is admitted; one centre sample is not enough. The rectangle runs from the cell of `centre - radius` to the cell of `centre + radius` (LP-01 conversion at both corners). | LC-07, WLPA-06, LR3-16 | The corner conversion is the project's reading of the two sources (unverified); a rectangle that leaves the grid is refused. Radius comes from the request, never a fixed infantry value (WLPA-14). |
| LP-04 | The start cell is exempt from admission (a unit may leave a cell the grid has since closed). The end cell must admit the footprint; otherwise the result is `destination_blocked`, not a path to a neighbour. | WLPA-14, WLPA-25 | Remake choice, unverified; destination adjustment is the placement query's job. |
| LP-05 | A diagonal step needs both orthogonal neighbours admitted (no corner cutting). | WLPA-17 | Remake choice. |
| LP-06 | A forced trivial path, or a destination inside the start's own cell, appends start and end directly. | WLPA-15 | Same. |
| LP-07 | A positive requested width first takes the unconstrained route; a route that has a cell narrower than the request minus 0.1 is searched again with every cell width-checked. A failed repair rejects the route (`width_rejected`). A line of `k` admitted cells is `k` pitches wide. | WLPA-16, LC-11 | Deviation: the original repairs the narrow runs of the route; the project re-searches the whole route constrained, which cannot return a narrower route. The width of a cell is the shorter of the admitted runs through it on the two grid axes, not the line perpendicular to the heading (WLPA-29), so a cell has one width whatever the approach direction. The start and end cells are exempt. The tolerance 0.1 is the original's code constant. |
| LP-08 | Smoothing is a postprocess after the width repair, and every sight line keeps the footprint and width admission. Nodes are the exact start, the surviving cell centres and the exact end. | WLPA-18 | Binary smoothing by doubling then bisecting a sight line from each anchor; the sight line crosses the cells between two centres, corner cells included. The original's second "optimize" pass is not done (gap). |
| LP-09 | The giant-vehicle branch takes a clear direct cell route without searching, otherwise accepts a detour of at most 1.8 times the direct distance (equality accepted) and rejects a longer one. The caller selects the branch from the unit's collision class and passes the ratio; it is no universal limit. | LC-10, WLPA-55 | The ratio is the original's code constant, exported as `land_giant_detour_ratio_raw`. Which length is measured (smoothed route, here) is unverified. |
| LP-10 | Work is counted, never timed: one per search step (an open-list pop, a route cell's width scan or a sight line) plus every footprint test and width read it made. | R3 LR3-04/05 | Remake unit; not the original's graph-node units (LR3-02). |
| LP-11 | The admission throttle lets a slice start only while the accumulated charge is strictly below the quota; equality refuses and a zero quota admits nothing. A slice is charged when admitted and may overshoot. A refused slice is a distinct outcome from `no_route`: the search stays in progress and is served first next frame. | R3 LR3-01/04/07 | Quota and slice size are remake profile inputs, not sourced land defaults (R3). |
| LP-12 | The quota resets every frame when the interval input is below 2, otherwise on frames divisible by it. Quota and interval are separate inputs. | R3 LR3-06 | Same. |
| LP-13 | A search checks its budget between two steps, so a slice can overshoot by one step; slicing never changes a result or the total work. | R3 LR3-04, BF-4 | Tested for slice sizes 1..50 against the whole search. |
| LP-14 | A checkpoint is a complete copy of the progress (request, open list, visited cells, route, smoothing cursor, counters, the scheduler's wait count and any grid change the search has not yet seen); restoring continues exactly where the search stood. A checkpoint that does not fit the grid is refused (`EAWR-SIM-0313`). | BF-4 | Round trip tested at every slice boundary, across a grid change committed after the last slice, and for the wait count. |
| LP-15 | Slices read the committed occupancy only; commits are serial. A search that sees the grid change marks itself, and before it ends re-walks its route against the current grid; a route the change closed is searched again (at most three times, then `unstable`). | WLPA-05, WLPA-11 | Remake rule for a search that spans commits; the original's search is synchronous. |
| LP-16 | Search storage is pooled (`LandPathArena`) and stamped, so starting a search costs no clearing and a warmed pool makes no heap allocation per slice (the finished route allocates its nodes). | project performance rule | Tested with an allocation counter. |
| LP-17 | `LandSliceScheduler` makes the admission decision serially in a total order (longest waiting, then ascending entity), then runs the admitted slices in the partitioned `land-path-slices` phase. Each search reads the committed grid and writes only itself; the caller publishes ended routes in ascending entity ID. Results equal on 1/2/4/8 workers and in reverse partition order. The phase has no tick placement yet: the land frame map (R1) places the dynamic collision service and its quota reset (LR1-09) but no static search service, so the session integration chooses and records it. | ADR-009, R3, R1 | Tested; placement open. |
| LP-18 | A mixed group moves with the intersection of its members' movement masks; if that intersection is not itself an authored class the group has no class (a distinct failure, never the least restrictive member's mask). | WLPA-12 | Same. The shield alternative (WLPA-13: prefer a matching no-shield class when it still connects start and end) needs the content profile to say which class that is; the caller passes that mask, nothing is derived from class names. |
| LP-19 | Connected zones are the 8-connected components of the cells a mask admits, with the search's corner rule, so two cells share a zone exactly when a footprint-free search can join them. Zones are rebuilt serially at the commit point when the occupancy revision moved (`LandZoneCache::prepare`); partitioned workers only read. | WLPA-13, WLPA-25 | Remake data structure; checked against a breadth-first oracle on every stock land map. Footprint-aware connectivity is the search's job. |
| LP-20 | Placement padding is the larger of a quarter of the hard radius and 0.3 times the formation spacing times 20; a zero radius with no spacing gives none. The padding widens the footprint tested at a candidate. | WLPA-24 | The 20 is the original's code constant, not the terrain pitch. Adding the padding to the tested footprint is the project's reading (unverified). |
| LP-21 | The nearest open position tries the desired position, then up to 19 rings around it: the ring grows by an increment that itself grows by 0.8 per try, capped at the smaller of 10 and 1.5 hard radii (5 for a zero radius). A candidate needs a position inside the grid and the playable bounds, the mover's zone when zones are given (a mover standing on ground its own mask does not admit has no zone, so the gate is off for it and callers must not rely on it there), an admitted footprint and the caller's filter. | WLPA-25 | Ring radii, the samples per ring (8 plus 8 per two pitches of ring radius, alternating either side of the direction toward the mover, nearest the mover first) and the order are the project's reading of "ring/subdivision state"; the original's candidate order is unverified. The filter carries the object collisions and predicted overlaps that land local movement owns. |
| LP-22 | No legal position within the tries is a distinct result (`none`) that keeps the requested destination; it is never an automatic path to the nearest cell. | WLPA-25 | Same. |
| LP-23 | The base width of a formation of N members is the largest member's extent for one member, otherwise the larger of `ceil(sqrt(N))` rows of the largest extent plus 0.25 and `sqrt(max(1, 1.8 * area sum))`. | WLPA-29 | Same (code constants); extents and area come from the members' authored geometry. |

## Not wired (named gaps)

- **Steep-to-water conversion** (WLEN-38): its caller is unverified (U-LEN-05), so it is not applied to the
  terrain; the cell masks are the authored bits.
- **Movement-class tags**: the request takes resolved bits; reading the XML movement-class definitions into the terrain's
  mask table is the content profile's job (tag rows stay `todo`).
- **Threat cost** (WLPA-17's optional multiplier) is the land AI's supplier; the search has no hook yet.
- **Rectangle search** (WLPA-15) is replaced by the cell search above; the walk's `BetweenFormationSpacing` and
  `Destination_Collision_Query_Extension` collision filters for a formation destination (LC-09) ride on the
  object tracking tree of land local movement and enter through `LandPlacementFilter`.
- **Bridge and crowd behaviour** (U-WLPA-2) stays gated on retail captures; nothing here queues or prioritizes.

## Cost

`path_bench --land` loads a stock land map and measures the search deterministically (work per search) and in
host time; the regression test counts work (`land_path_tests`), not time.
