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
| RFL-06 | Service increments distance along the path by current speed. A successful lookup supplies the next spatial position and updates facing. Failed lookup forces terminal advancement at the current position; it does not snap to the aim. A missing path after failed construction is a separate state from exhausted lookup. Construction commits its initialized path only after all stages succeed; bounded construction failure reports a diagnostic without committing path state, while a successfully retained route with fewer than three controls is initialized but unevaluable and exhausts lookup at the current pose. | Implemented in `rocket.cpp` / `projectiles.cpp`; `tactical_rocket_contracts` (endpoint_and_retained_aim; extension_and_construction_states). |
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
the existing 2,048-point and depth-32 bounds also cap spherical subdivision.

The seeded route uses the existing keyed `CombatRandom` stream with the reserved
rocket path slot: session seed, first service frame, projectile ID. Interior draws
run in initial, main, then post-aim segment order. Retained paths draw nothing on
later service frames; stock zero-offset paths draw nothing. Construction caps
controls at 2,048 and arc visits at 131,072 across initial evaluation, extension
and trim rebuilds, with the existing depth-32 subdivision bound. The final two
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
