# Space rocket path and endpoint contract

This weapons follow-up records the path contract required by area-damage gap G4
(legacy EAWR-1072). WAD-04 owns the terminal blast interface; WWP-62 owns rocket motion.
ROCKET is a distinct category from the target-tracking MISSILE category.
Broadside and Marauder remain behind their existing roster gates pending the
complete flight and presentation review.

| Rule | Sourced behavior | Implementation / contract |
|---|---|---|
| RFL-01 | Space construction retains the shot's fire-at position, including height. Ordinary target motion does not rebuild that route. Construction clears the missile target. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (seeded_worker_and_target_independence). |
| RFL-02 | The initial straight distance is the smaller of the authored `Projectile_Rocket_Straight_Distance` and half the planar origin-to-aim distance. It follows the projectile's launch facing. The next segment reaches the retained spatial aim. Space construction applies no terrain alignment. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (cubic_height_and_boundary; seeded_spatial_offsets). |
| RFL-03 | Each segment has ceil(spatial length / (1.1 times `Projectile_Rocket_Curve_Distance`)) subdivisions. Interior points receive offsets in two normalized spatial axes; endpoints receive none. The initial segment uses 0.1 times the authored offset; the main segment uses the full offset. The first axis is the normalized cross product of segment midpoint plus world (1,1,1) with origin minus endpoint; the second is the normalized cross product of that unnormalized first cross product with origin minus endpoint. A degenerate axis remains zero. Each interior offset draws angle first, uniformly over a full turn, then radius uniformly from zero to the scaled authored offset. Zero amplitude consumes no draws under the deterministic project RNG policy. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (seeded_spatial_offsets; extension_and_construction_states). |
| RFL-04 | Coordinate curves are natural cubic splines with uniform parameter knots. Lookup chooses the first segment whose cumulative arc length is strictly greater than the requested distance, then evaluates its cubic at the fraction of that segment's arc length. Equality with the whole path length fails lookup. The presentation samples use ceil(segment arc length / 10), bounded to 1..100. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (cubic_height_and_boundary; extension_and_construction_states). |
| RFL-05 | With `Explode_When_Reached_Target_Radius` true, construction stops at the aim. Otherwise it constructs a post-aim extension with 0.3 times the authored offset. Its endpoint is retained aim plus the difference of the final two presentation samples (without normalization) times authored maximum flight distance minus initial spline length; this can extend backwards when that difference is negative. For a path longer than the authored maximum, lookup selects the cut point, controls starting at the selected segment index are removed, and that point is appended before reevaluation. The retained target-radius distance is the authored flight distance, independently of actual path length. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (extension_and_construction_states). |
| RFL-06 | Service increments distance along the path by current speed. A successful lookup supplies the next spatial position and updates facing. Failed lookup forces terminal advancement at the current position; it does not snap to the aim. A missing path after failed construction is a separate state from exhausted lookup. Construction commits its initialized path only after all stages succeed; construction refusal reports a diagnostic without committing path state, while a successfully retained route with fewer than three controls is initialized but unevaluable and exhausts lookup at the current pose. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (endpoint_and_retained_aim; extension_and_construction_states). |
| RFL-07 | Non-allied active/passive missile shields and sensor jamming can redirect on radius entry: the next position is inside or on the radius and the current position is outside or on it. Source position is unadjusted for rocket entry, unlike direct deflection. Declared shield ability radius overrides passive radius; a jamming ability supplies its radius when jammed. Space redirection builds a spatial spherical detour with a 30-unit margin, preserves remaining authored flight allowance, resets path distance and clears the missile target. Production consumes copied passive and active shield sources plus user-input jammer owners, with current-frame recipient stamps. Static sources retain creation order; jammer activation appends through the transactional PDEF ledger. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (rocket_defence_contracts). |
| RFL-08 | Ordinary collision runs before endpoint expiry. A hit suppresses the expiry blast. Positive travel allowance takes precedence over lifetime for ordinary space categories; otherwise lifetime expires only strictly after its authored duration. Rocket target-radius expiry compares distance along its path with the retained authored flight distance. DEFAULT target-radius expiry compares squared spatial displacement strictly beyond squared origin-to-aim distance. Generic removal adds no blast. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (default_endpoints_and_lifetime; impact_suppresses_expiry). |

The normal Diamond Boron profile authors speed 7.5, maximum flight distance 5000,
curve distance 500, curve offset zero, initial straight distance 500 and
target-radius expiry. Its BARRAGE variant inherits the path inputs and blast
damage 150, radius 200 and five dropoff tiers, overriding speed to 12 and maximum
flight distance to 3000. Ability proxy height and scatter are WAD-38 inputs.
`Fires_Forward` determines launch facing through its weapons owner; zero curve
offset alone does not prove every rocket route is collinear.

The read-only debug-build observations establish these decisions, but do not
establish retail presentation acceptance. Seeded nonzero offsets and post-aim extensions are implemented and contract-tested.
GC space entry admits these same profiles through its reachable-content
projection. It does not impose a separate zero-offset or target-radius-only
restriction. Its curve-distance guard names every invalid reachable rocket;
the common combat and flight validators retain their geometry domains, while
RFL-16 separates the authored travel allowance from world-coordinate limits.
Retail detour parity, forward-firing launch alignment, and retail height/endpoint
captures remain presentation review gates. Arc length recursively compares the chord with the two midpoint chords.
It returns their sum when either the relative or absolute difference is at most
0.000001, otherwise subdivides both halves; a smaller chord returns directly.
Q24 rounds that threshold to 17 raw units, an explicit numeric policy.
The area service must accept the terminal pose supplied by
flight and emit exactly one blast even while those broader gates stay open.

RFL-07 detour construction normalizes the last movement direction and requires
the source to lie ahead. It places lead points 30 and 60 units ahead, constructs
a sphere centered on the source through the latter point, and intersects a
forward ray starting another half-unit ahead (bounded to one million units).
No intersection leaves the old route and tracking in place but marks the attempt.
The current frame still uses its point selected before this construction.

On success, the route starts at the current pose, reaches the first lead point,
and follows normalized spherical midpoint controls. The retained seed controls
are the eighth and three-quarter directions; subdivision inserts midpoints in
order while angular separation times sphere radius exceeds authored curve
distance, then removes the first two entries when subdivision grew the list.
The final segment follows the normalized planar old direction for remaining
authored allowance minus 30. These detour straight segments use subdivisions
at the authored curve distance. The complete cubic is trimmed at the original
type's authored maximum, rebuilt, and its target-radius allowance reduced by
the consumed cursor before resetting that cursor and clearing tracking.
The original authored distance remains immutable for later detour trims.
Q24 uses the existing turn trigonometry and two-pi constant for angular length;
spherical subdivision uses ordered work and the numerical progress policy below.

The seeded route uses the existing keyed `CombatRandom` stream with the reserved
rocket path slot: session seed, first service frame, projectile ID. Interior draws
run in initial, main, then post-aim segment order. Retained paths draw nothing on
later service frames; stock zero-offset paths draw nothing. Construction grows checked dynamic storage from actual segment counts across
initial evaluation, extension and trim rebuilds. It has no authored-control,
route-wide visit or recursion-depth ceiling. The final two
presentation samples are evaluated directly, so extension does not allocate the
full presentation sample list. Diagnostics are noncanonical; the existing flight
state and spline blocks retain the resulting path without a format change.

RFL-05's unnormalized extension scale is confirmed by the debug build: the
final sample minus the preceding sample is multiplied directly by authored
maximum flight distance minus the initial spline length, then added to the
retained aim. There is no normalization or division by sample spacing.

The two-control non-target case has no linear extension fallback in the debug
build. RFL-04 evaluation returns failure before producing samples when fewer
than three controls exist; RFL-05 then requires at least two evaluated samples
and reports an assertion when that requirement is unmet. The project retains
the initialized, unevaluable route in this case, skips extension and exhausts
lookup at the launch pose under RFL-06. This is the chosen handling of an
unsupported debug-build case, rather than a claim that retail recovers the
same way. No stock rocket reaches it because stock rockets terminate at their
target radius; retail recovery after the assertion remains unverified
(legacy EAWR-2607).

`unit_tables_contracts` (`rocket_variant_inputs`) loads a synthetic profile with
the route values reached by `UM06_EclipseDefensePad`: speed 20, flight allowance
3000, curve distance 400, offset 180, straight distance 120 and target-radius
termination. Combat validation accepts that authored profile.

## RFL-09: construction representability and numerical progress (project policy)

The walked debug-build route uses dynamic controls and recursive arc/sphere
subdivision without an explicit control or visit quota. The project likewise
admits every representable control count, checking actual spatial segment
length and scaled authored spacing, existing prefixes, container element/byte
limits, and Q24 interpolation counts before growth. Extension and detour counts
come from their computed endpoints, rather than authored range alone. Numeric
profile domains, lifetime validation and strictly positive spacing remain unchanged.
The presentation sample clamp does not limit controls.

Ordered explicit work preserves left-before-right subdivision and addition,
natural cubic coefficients, control order and successful RNG draw order.
A Q24 arc interval whose midpoint equals an endpoint accepts its available
two-chord estimate and reports a noncanonical counter/diagnostic. Spherical
subdivision refuses a zero, endpoint-equal or repeated active midpoint instead
of recurring indefinitely. These are numerical project policies, not claims
about debug-build exhaustion recovery. There is no extra numeric work quota.
Exact visits, peak controls and midpoint estimates are noncanonical counters.

Deterministic storage/count or numerical refusal expires only that projectile
at its current pose through RFL-06/WAD-04, without another collision segment,
partial route/redirect metadata, or another attempt on a later tick. One ordered
diagnostic identifies type/projectile and required/observed controls and visits.
Other projectiles and later battle ticks continue. This exceptional terminal
handling is a project deviation: graceful retail resource recovery was not
established. A small explicit deterministic storage policy is injectable in
internal contracts; production uses representability alone. Available memory,
wall time and thread count never select a canonical trajectory or refusal.
Actual allocator failure remains an execution failure, since peer-local memory
success cannot safely choose a synchronized projectile expiry.

`tactical_rocket_contracts` covers former control boundaries, a 2049-control
reproduction, larger routes, extension and detour crossings, cumulative visits
above the former threshold against an unlimited recursive reference, atomic
refusal, Q24 nonprogress, malformed setup separation, and mixed successful/refused
flight and real-session hashes/diagnostic ordering on 1/2/4/8 workers.

## RFL-16: authored flight allowance is not a world coordinate

Read-only debug-build observations of the space path constructor, authored
distance accessor and rocket service establish that `Projectile_Max_Flight_Distance`
is retained directly, without a coordinate-derived cap or saturation. With
`Explode_When_Reached_Target_Radius` enabled the constructed route ends at the aim;
the authored allowance remains a separate endpoint input. Otherwise RFL-05 uses
the unchanged allowance in its extension and trim. It does not convert that
allowance into a lifetime.

Every representable nonnegative Q24 allowance is accepted, including `9999999`.
This removes only the allowance's coordinate-derived upper bound; it does not
establish retail rejection of negative values or alter the existing project
domains for geometry, subdivision spacing or lifetime. Invalid profiles report
the projectile name and failed XML input during binding; direct combat-table
validation distinguishes an object weapon from a numbered hardpoint.

Checked Q24 extension arithmetic and RFL-09 construction refusal remain in
effect. At the largest positive Q24 allowance, a ten-unit final sample delta
produces a finite endpoint in the debug build's floating-point domain that is
outside Q24. The project preserves the authored allowance and refuses that
individual unrepresentable construction atomically with one diagnostic, then
expires only that projectile at its current pose. This is RFL-09's documented
numerical deviation, not a retail clamp or a load-time rejection.

`tactical_rocket_contracts` (`oversized_flight_allowance`) covers seeded nonzero
offsets with `9999999`, a representable extension with that allowance, the
largest parsed Q24 allowance with target-radius expiry, actual extension
overflow, and canonical flight bytes and diagnostics on 1/2/4/8 workers.
`unit_tables_contracts` (`rocket_variant_inputs`) covers XML binding of the
large allowance and actionable malformed-subdivision diagnostics.

## RFL-10..RFL-15: unit-level salvo launch and ROCKET guidance ((legacy EAWR-4986) walk, 2026-10-25)

Read-only walk of the FoC debug build (`StarWarsI.exe`, GhidrAssist), cited by role.
Scope: the Victory I unit-level (non-hardpoint) `ROCKET` salvo and the community claims in (legacy EAWR-4986).
Not walked (STOPPED AT, see the end): the `FULL_SALVO` ability's own activation path and the
unit-level muzzle-bone selection (`Lookup_Firing_Bone_ID` caller). No XML for the Victory template
was available in the reference copy, so the per-bone axes are not established here.

| Rule | Sourced behavior | Ours |
|---|---|---|
| RFL-10 | **Launch heading for a non-`Fires_Forward` object weapon** (the debug build's projectile-facing calculation): the muzzle is the translation of the firing-bone matrix passed in. The heading is the facing from muzzle to aim, where aim is the target point after inaccuracy for LASER, GRENADE, MPTL_ROCKET, **ROCKET** and DEFAULT (the scattered point replaces the aim). It is refused (zero heading, shot not fired) if the aim has zero length or lies outside the cone of fire from the firing-bone matrix. A `Fires_Forward` unit instead copies the unit's own facing. For the rocket categories the caller discards this heading and launches along the firing bone's x axis (RFL-15, [research](../research/victory-salvo-launch.md)); here it only decides whether the shot fires. | **Same** for the heading and refusal (W-09/WWP-53: cone at the muzzle, aim = scattered point). The rocket launch facing is RFL-15. |
| RFL-11 | **ROCKET is unguided.** `Service_As_Rocket_Path_Projectile` advances `RocketDistanceAlongSpline` by speed, looks up the point on the retained spline and calls `Turn_To_Face(point, rate = -1.0)`: facing follows the path tangent at once with no turn-rate limit. It reads neither `Max_Rate_Of_Turn` nor `Projectile_Max_Scan_Range`. `Calculate_Rocket_Path_Space` clears `MissileTargetObject`. Only the MISSILE service path (`Service_As_Missile_Path_Projectile`) steers by turn rate. So a ROCKET variant of a guided missile inherits those tags but they have no effect. | **Same** (RFL-01, RFL-06). A symptom of a homing path in ours would be a defect elsewhere; check that the unit-level weapon path really builds a rocket spline and not the missile path. |
| RFL-12 | **Spline shape.** Straight length = min(`Projectile_Rocket_Straight_Distance`, half the planar origin-to-aim distance). The first segment goes from the launch position along `Get_Facing_Vector` (Z then Y Euler rotation of +X) with offset factor 0.1; the second to the fire-at point with factor 1.0 (curve factor 1.1); a post-aim segment with 0.3 when `Explode_When_Reached_Target_Radius` is false. So the straight leg follows the **launch facing** (RFL-10), and the arc then bends to the aim: a salvo "leaves sideways" when its firing bone's x axis points sideways (RFL-15). | **Same** (RFL-02..05); the launch facing is RFL-15. |
| RFL-13 | **Object-weapon scatter** (`Add_Random_Inaccuracy_To_Fire_At_Position`): with no fixed-radius override, r = distance(muzzle, aim) x `Targeting_Fire_Inaccuracy` / `Targeting_Max_Attack_Distance` (raw); three `SyncRandom` draws, each uniform in [-r, r], added to X, Y, Z (the larger of the unit's and the projectile type's inaccuracy is used when the type uses projectile-type data). With a fixed radius (including a BARRAGE override): X and Y draws, plus Z only in space, uniform in [-R, R]. This is a **box, not a circle**; there is no radius-in-circle sampling. Dead-eye mode and zero inaccuracy skip it. One scatter is drawn **per missile**. | **Same** (DG-24, WPJ-05, BARR-02). The community claim "random inside a circle" is wrong for both Victory `FULL_SALVO` scatter (20 on All, range-scaled) and BARRAGE (fixed box). |
| RFL-14 | **Shield redirect uses the same facing code.** `Turn_To_Face` is also what `Rocket_Repath_To_Avoid_Shields_*` relies on, so no rocket carries a persistent turn rate. | **Same** (RFL-07). |
| RFL-15 | **Rocket launch facing** (the debug build's projectile-firing step, category switch): for `ROCKET`, `MPTL_ROCKET` and `LIGHT_SABER` the projectile is created facing the normalised x axis of the firing matrix: the firing bone's world transform (the unit-level muzzle bone in WWP-47 turn, else `Fire_Bone_A`), or the unit's own matrix without a bone. The fire-at point stays the scattered aim. So the straight leg (RFL-12) runs along the bone axis; the Victory I muzzle bones point 70 degrees off the bow, outward, and its salvo leaves both flanks sideways, then curves to the target ([research](../research/victory-salvo-launch.md)). | **Same** ((legacy EAWR-4986)): the loader keeps each muzzle bone's x axis (`muzzle_axes`), the fire path turns it (with an object or hardpoint turret pose, then the unit's rotation) into the event's `launch`, and creation sets a rocket's step and facing from it. Our `MPTL_ROCKET` and `LIGHT_SABER` are not rocket flights, so they keep the muzzle-to-aim heading. |

### Differs from ours
1. Launch heading: found later and fixed as RFL-15 (the rocket categories launch along the bone x axis). None found for guidance or scatter distribution. The owner's "sideways then curves to the front" most likely comes from retail muzzle **positions** (bones on the hull sides) plus a cone-limited heading, which only a bone-position trace and a retail capture can settle.
2. Open: whether our unit-level muzzles use the same bones as retail for this model (G10 "model without `_00` bone launches from the unit origin" would produce the "invisible point" symptom).
3. Open: `Targeting_Fire_Inaccuracy All 20` applies as range-scaled box scatter; check our value is scaled by `Targeting_Max_Attack_Distance` raw, not by current distance only.

### STOPPED AT
Time budget (burn task). Not done: decode `FULL_SALVO` activation (pulse count, per-pulse aim reuse), the unit-level firing-bone selection and bone axes for the Victory model, and the cone-of-fire test constants. Next step: Ghidra `Is_In_Cone_Of_Fire` and the caller of `Lookup_Firing_Bone_ID`, then a 60-tick trace of ours.
