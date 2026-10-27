# Land bridges, chokepoints and crowds

This is a retail-observation research note for the ground-combat architecture
programme. It supplies observed rules for L1 terrain geometry, L4 width admission
and L5 local movement under the existing land movement work (legacy EAWR-1451). It
changes no simulation code. Every rule below comes from an original-game rig
capture driven through the in-battle Lua debugger, with a static survey of the
stock map files where noted. Nothing here reads binary code.

Read the [land pathing walk](../../behaviour/walks/land-pathing.md) and the
[land combat walk](../../behaviour/walks/land-combat.md) first, plus
[R3](R3-land-path-budget.md), whose capture plan (U-WLPA-2, U-WLPA-3, U-WLPA-6,
U-LC-3 and the L1 geometry row) this note answers in part. R3's static contact
rules (LR3-07..13) name the mechanisms; these captures show the outcomes.

Evidence groups: ER9-A is the static map survey. ER9-B is the gate-width
searches (ascending gaps through a prop wall, one class at a time). ER9-C is
the contested-lane captures (head-on pairs, same-direction following, crowds
and wall removal). ER9-D is the geometry and boundary probes. Receipts (Lua
logs, driver logs, and the clips and frames behind them) stay private under
ignored `out/research/gc-land-r9/`, one directory per run: `run1-discover`,
`run2b-gtx`, `run3-sweep`, `run4-find`, `run5-contest`, `run6-bounds`.

## Method and measurement limits

- **Arena.** A flat, terrain-sealed lawn on a stock land map. A wall of spawned
  rock props at 35-unit spacing crosses the lane, with one gap in it. Spacing of 70
  units leaked units through, so 35 was used throughout. The gap is
  reported as the distance between the two innermost prop centres. The free
  width between prop footprints is unknown, so gap values are centre
  distances, not clearances.
- **Crossing test.** A class "takes the gate" when it crosses the wall line
  within half the gap plus 25 units of the gap centre and its lateral
  deviation before crossing stays at or below 80 units. Anything else is a
  detour or a failure to cross within the time cap (16 s infantry, 14 s bike
  and hover, 20 s tank, 24 s AT-ST, 30 s AT-AT).
- **Resolution.** Gaps were tried in 20-unit steps, ascending, one trial per
  gap. Thresholds are therefore "between the last failing and the first
  passing step", reported as the first passing step.
- **Company spawn.** A land-company type spawns three teams of five (members
  are not individually addressable through the Lua API); sampled position is
  the container position. Vehicles and walkers are single objects.
- **Sampling.** Positions at 10 Hz. Orders and spawns cost 6-10 s per adapter
  call, so the captures use an autonomous in-probe trial engine and read the
  log afterwards.
- **Engine label.** All captures ran on the original game's debug build.

## Observed rules

| ID | Retail rule | Evidence summary | Open item narrowed | Planned decision |
|---|---|---|---|---|
| LR9-01 | Bridges are not a dedicated mechanism in skirmish maps. Of 135 stock map files scanned for the five bridge-named prop types, one contains a placement, a campaign mission map. No lobby land map places one. Narrow crossings in skirmish are ordinary passable cells bounded by baked passability and placed props. | ER9-A: placement scan of every stock map file against the five bridge-named type names; `run1-discover` survey scripts. Extends WLPA-55 (no dedicated bridge symbol found). | U-WLPA-2: bridge destruction/repair producers have no skirmish-map consumer; bridge queues reduce to LR9-03..LR9-11 chokepoint behaviour. U-LC-3: "bridge" qualifier. | L1/L4: treat a bridge as passability data with no special object model for skirmish; keep a campaign/mod path open (mods may place such props). |
| LR9-02 | The decoded passability plane holds one 32-bit value per terrain vertex: `width*height` entries, not `(width-1)*(height-1)`. Every surveyed lobby land map has width equal to height (187, 218, 249, 280, 311 or 373). The plane uses five distinct values (1, 2, 32, 128, 256), with 1 and 2 dominant. | ER9-A: plane length equals width*height on every lobby land map surveyed (for example 311x311 = 96,721 entries). Complements LR3-16 (centre formula only). | LR3 L1 geometry row: file-level N-versus-N-1 closed (N entries per axis). Runtime allocation and vertex-to-cell mapping remain open. | L1 terrain field (legacy EAWR-4651): load the plane as N x N; do not drop a row or column at load. Runtime grid dimension stays qualified (see the qualification table). |
| LR9-03 | The smallest prop-centre gap each class takes directly (first passing 20-unit step): infantry company 100, scout bike 100, AT-ST 100, hover tank 120, ordinary tank 120, AT-AT 140. The next smaller step fails for each. Thresholds follow movement/collision footprint (small foot and bike classes, then medium vehicles, then the large walker), not speed: the bike is about five times faster than infantry and shares its threshold. | ER9-B: `run4-find` ascending searches (infantry 60/80 fail, 100 pass; tank 80/100 fail, 120 pass; AT-AT 120 fail, 140 pass; bike 60/80 fail, 100 pass; hover 60-100 fail, 120 pass; AT-ST 80 fail, 100 pass); `run3-sweep` coarse sweep agrees. | U-WLPA-2: narrow/wide choke thresholds measured per class. U-LC-3: width admission by class. | L4: width admission is a per-class footprint threshold. Source it from each type's authored collision/movement data, not these numbers. Use these numbers as validation targets in a synthetic prop-gate case, within the 20-unit resolution. |
| LR9-04 | The thresholds do not depend on where the lane meets the cell grid at this resolution: shifting the lane 10 units east (a half cell at pitch 20) gave the same first passing gap for infantry (100), tank (120) and AT-AT (140). | ER9-B/D: `run6-bounds` shifted-lane searches at lane x=3350 against `run4-find` at x=3340. | LR3 L1 geometry row: no half-cell cell-phase effect visible at 20-unit gap resolution. This does not prove the centre convention. | L1/L4: do not add a half-cell offset to width tests on the strength of gate behaviour; LR3-16's formula stands. |
| LR9-05 | Below its threshold a class does not squeeze through. Infantry, ordinary tanks and AT-ATs travel 270-310 units sideways along the wall and do not cross within the time cap. The bike crosses around the wall end 48-58 units beyond the last prop (x 3708-3718 against the end at 3660) after a 368-378-unit excursion. Hover tank and AT-ST cross at the gap after a 290-312-unit excursion at sub-threshold gaps (hover 60-100, AT-ST 80), taking 12-19 s instead of 6-9 s. | ER9-B: `run4-find` F/R lines (devpre and cross columns); `run3-sweep` gate records. One AT-AT crossed the 80-unit wall at 108 units off the lane after 39.7 s (`run3-sweep`), unexplained: see the qualification table. | U-WLPA-2: sub-minimum choke outcome (reroute, not stall, within the cap). | L4/L5: a failed width admission yields a long reroute or no route, never a squeeze. Keep a distinct "no crossing within budget" result. |
| LR9-06 | Movement speed in open ground: infantry company 46.8-46.9 units/s, ordinary tank 32.8, AT-AT 23.4, hover tank 107.6-107.8, scout bike 216-235 (two estimates). Tank, AT-AT, hover tank and bike match the authored maximum speed times 46.875 (0.7, 0.5, 2.3 and 5.0 give 32.8, 23.4, 107.8 and 234.4). The infantry company moves at 46.8, which is 1.0 on that scale, below the 1.1 authored on its team type (51.6). First motion follows the order after 0.27-0.47 s for every class except the AT-AT, whose first motion comes 7.0 s later, with 90% of steady speed 1.0 s after that. | ER9-C: steady-speed fits from the 10 Hz series of the five open-lane trials in `run3-sweep` and `run4-find` (`speeds`, `startup` scripts). Complements LR3-14/15 (member speed constants). | U-WLPA-3: parent-speed and start-delay quantities observed. U-LR13-02: AT-AT start edge. | L2/L5: keep a per-type start delay as authored data (a walker with a long start clip delays first displacement); do not assume instant acceleration for large walkers. |
| LR9-07 | Head-on friendly contact is resolved by one company yielding, not by passing through or splitting. In symmetric infantry/infantry, tank/tank, tank/infantry, bike/infantry and AT-AT/infantry pairs sent along the same lane, with or without a gate, one company reaches its target and the other ends 68-155 units short of it (270 for an AT-AT pair at a 220 gate). The company that yielded was the one travelling toward higher y, from the lane's south end, in every one of the twelve runs where only one arrived. AT-AT/AT-AT in open ground has both arrive after 73-95-unit sidesteps. The same AT-AT/infantry pair with a 150-unit lateral offset also has both arrive. | ER9-C: `run5-contest` symmetric head-on set (pairs meeting at the gate or mid-lane at the same moment); `run3-sweep`/`run4-find` first-generation head-on trials. | U-WLPA-6: head-on outcome observed. Consistent with LR3-10's equal-class tie rule (formation length, then first-member identity) but not proved (identities are not exposed through the Lua API). U-LC-3: allied crowd outcome. | L5: model the yielding company as a stopped or re-planned pair member; expect loss of the order on the yielding side to be an observable outcome rather than an error. Assign roles by LR3-10 inputs and verify against these captures. |
| LR9-08 | In ten of the fourteen pairs the yielding company was idle at the end (no member with an active order for 5 s), not still pathing; two other yielding companies were still ordered but not arriving, and two pairs (AT-AT/AT-AT in open ground, AT-AT/infantry with a lateral offset) had both arrive. The AT-AT's order went idle against infantry in both open ground and at a 200-unit gate. The cause (blocked path, deadlock recovery, or a replaced order) is not visible through the Lua API. | ER9-C: `run5-contest` idle verdicts (all members without orders for 5 s). | U-WLPA-6: order loss on head-on contact observed; its producer remains open. | L5: keep a distinct "order dropped by contact" state. Do not fall back to a re-issued order or stall timeout until the producer is traced. |
| LR9-09 | A faster unit behind a slower one in the same lane does not overtake in open ground. A scout bike started 180 units behind an AT-AT crossed the lane's reference line 3.6 s after it (23.6 s against 20.0 s); lateral deviation stayed at or below 39 units, where an unobstructed bike reaches the line in about 6-7 s. | ER9-C: `run4-find` same-direction trial, one run. | U-WLPA-6: same-direction following observed once. U-WLPA-3: member/container following under a slower leader. | L5: a trailing faster mover adopts the leader's speed when blocked from passing; do not give it a free overtake lane. One trial: treat as a hypothesis for synthetic cases. |
| LR9-10 | Crowd throughput at a gate at the infantry minimum (100) is staged, not first-in-first-out. Two infantry companies both cross at 10-12 s. Four cross in a pair at about 10-11 s and a second pair at about 20-24 s. Six log one crossing at 10.4 s and five between 18.9 and 21.5 s (one sample lacks a crossing record). Every company arrives; deviation grows up to 417 units for later companies. Widening the gate by 20 units (120) removes the staging: four companies all cross within 10-12 s. | ER9-C: `run5-contest` and `run4-find` crowd sets (2, 4 and 6 infantry companies at g100; 4 at g120). | U-WLPA-2: crowd priority/queue at a choke observed. U-LC-3: allied crowd. | L4/L5: no explicit queue is needed to reproduce this: capacity-limited waves emerge from width admission plus LR9-07 yielding. Keep a deterministic yield order. |
| LR9-11 | Heavy classes do not all cross a shared gate. At a gate at the class minimum: two AT-ATs at 140 give one crossing (21.6 s) and one stopped about 65 units before the wall, 240 units off the gate, after 42 s; three AT-ATs at 200 give one crossing and two stopped 50-65 units before the wall; four tanks at 120 give three crossings (12-23 s) and one stopped before the wall after 37 s. In a mixed crowd at a 180-unit gate, infantry, bike, hover and AT-AT cross (AT-AT at 43.8 s) but the tank is still south of the wall after 45.8 s, although a lone tank takes a 120 gate in 11 s. | ER9-C: `run5-contest` crowd set, `run4-find` mixed-crowd trial (single runs). | U-WLPA-2: persistent stall for heavy classes in a crowd observed. U-LC-3: persistent stalls. | L5: stalls under contention are an outcome, not a defect to remove; retain progress-based recovery (LR3-11/12) rather than an unconditional timeout. |
| LR9-12 | Removing blocking props re-routes a stalled unit about 3 s later. A tank trailing along an 80-unit wall (below its 120 minimum) turned toward the new opening 2.8-3.0 s after ten props were removed (two runs), then crossed 5.2-5.7 s after removal. An AT-AT in the same arrangement at 120 also crossed after removal. | ER9-C: `run4-find` and `run5-contest` open-up trials; 10 Hz series around the removal log line. | U-WLPA-2: reaction to a chokepoint opening (destruction analogue). | L4/L5: a stalled route is re-evaluated on a cadence of seconds, not on the same tick as an obstruction change. Qualify exact cadence with the land frame-map (R1) before any constant is chosen. |
| LR9-13 | A spawn on ground the unit cannot occupy is displaced to the nearest legal ground, silently, not rejected. A spawn requested at (4240, 660) arrived at (3979, 850); one at (2780, 4350) arrived at (3100, 3891). A teleport onto the original request landed exactly where asked. | ER9-D: `run3-sweep`, `run5-contest`, `run6-bounds` position records following spawn and teleport. | U-WLPA-2: destination/spawn adjustment in unreachable cells. LC walks: destination collision filtering. | L4: nearest-open adjustment is silent and class-aware; keep it as a separate step from route admission. |

## Remaining qualification and later captures

| Open question | Why it is still open | Capture that settles it | Consumer |
|---|---|---|---|
| LR3 L1 geometry: runtime allocation (N versus N-1) and the playable boundary. | Both boundary probes (north and south edges, nine trials in `run6-bounds`) landed the teleported unit exactly but every move order was a no-op (final position equal to start), so no edge cell was reached. The spawn probes were displaced (LR9-13). | Use an order that the unit demonstrably accepts at an interior cell near the edge, then step the target toward the surveyed playable edge in cell increments; or use a debug inspection of the world-to-cell conversion at first/interior/last indices. | L1 terrain field (legacy EAWR-4651), then L4. |
| Rectangular maps. | Every lobby-reachable land map is square. The one rectangular playable area found in the survey (a non-lobby map) cannot be selected with the lobby templates available to the rig. | Pin a new lobby template for that map, then repeat the edge probe on X and Y independently. | L1. |
| Gap thresholds (LR9-03) are first-passing steps at 20-unit resolution with one trial per gap and one half-cell shift. | A single trial per step cannot show variance; the free width between prop footprints is unknown. | Repeat the searches at 10-unit steps with three trials per step, and measure each prop's footprint with a unit placed against it. | L4 width admission. |
| AT-AT behaviour at sub-minimum gaps. | One AT-AT crossed an 80-unit wall 108 units off the lane after 39.7 s while another did not cross a 120-unit wall in 34 s. Possible crush of a prop is not excluded. | Query prop health/validity after the crossing and repeat with a durable obstacle (an indestructible structure), or with props the walker cannot crush. | L5. |
| Cause of order loss on head-on contact (LR9-08). | The Lua API shows only that no active order remains. | Debug inspection of the contact state and recorded blocker for the yielding company, with replacement-order and Stop controls. | U-WLPA-6, L5. |
| Member-level formation positions through a choke. | Members of a company are not individually addressable through the Lua API, so spacing and member order at a gate are unobserved. | Debug inspection of member slot and actual positions at a gate, with a one-member control. | U-WLPA-3, L2/L5. |
| AT-ST spawned and ordered beside a rock outcrop froze the debug build once (not reproduced; later AT-ST runs were stable). | A single hang; cause unknown. Avoid as a capture hazard. | Repeat the exact spawn once with the game under a debugger. | Rig tooling only. |
| Same-direction following (LR9-09), staged crowd throughput (LR9-10) and heavy-class stalls (LR9-11) are one-run observations. | No repeat trials. | Repeat each three times and record the yielding order. | L5. |
| Bridge destruction/repair and crusher flee timing (U-LC-3 remainder). | No bridge exists in skirmish maps (LR9-01) and no crush trial was run. | A campaign or mod map with a bridge prop, and a crusher against infantry in a choke. | U-WLPA-2, U-LC-3. |

## Programme handoff

- **L1 terrain field (legacy EAWR-4651):** the passability plane is N x N entries
  (LR9-02). The runtime allocation, vertex-to-cell mapping and the playable
  boundary remain open. Do not add a half-cell shift on the strength of gate
  behaviour (LR9-04).
- **L4 width admission:** admission is a per-class footprint threshold
  (LR9-03) and a failure is a reroute or no route, never a squeeze (LR9-05).
  Source thresholds from authored data. The measured prop-centre gaps are
  validation targets for a synthetic prop-gate case, not constants.
- **L5 local movement:** head-on friendly contact has one company yield, and the
  yielding company can end without an order (LR9-07/08). Crowds at a choke are
  capacity-limited waves and heavy classes can stall (LR9-10/11), so keep
  progress-based recovery and a deterministic yield order. A faster follower
  stays behind a slower leader (LR9-09, one trial). A stalled route
  re-evaluates seconds after an obstruction changes (LR9-12).
- **L2/L6:** member positions at a choke and walker start delay (LR9-06) are
  inputs to the member and presentation work. Member positions are not
  observed here.
- **Closure:** these captures narrow U-WLPA-2 (no bridge mechanism in
  skirmish; choke thresholds, wave behaviour, reaction to an opening),
  U-WLPA-6 (head-on outcomes) and U-LC-3 (allied crowds). They do not close
  them: order-loss producer, crusher timing, member positions and the boundary
  probes stay in the table above.
