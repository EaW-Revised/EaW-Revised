# Space rocket path and endpoint contract

This weapons follow-up records the path contract required by area-damage gap G4
(legacy EAWR-1072). WAD-04 owns the terminal blast interface; WWP-62 owns rocket motion.
ROCKET is a distinct category from the target-tracking MISSILE category.
Broadside and Marauder remain behind their existing roster gates pending the
complete flight and presentation review.

| Rule | Sourced behavior |
|---|---|
| RFL-01 | Space construction retains the shot's fire-at position, including height. Ordinary target motion does not rebuild that route. Construction clears the missile target. |
| RFL-02 | The initial straight distance is the smaller of the authored `Projectile_Rocket_Straight_Distance` and half the planar origin-to-aim distance. It follows the projectile's launch facing. The next segment reaches the retained spatial aim. Space construction applies no terrain alignment. |
| RFL-03 | Each segment has ceil(spatial length / (1.1 times `Projectile_Rocket_Curve_Distance`)) subdivisions. Interior points receive offsets in two normalized spatial axes; endpoints receive none. The initial segment uses 0.1 times the authored offset; the main segment uses the full offset. Each offset draws an angle uniformly over a full turn and a radius uniformly from zero to the scaled authored offset. |
| RFL-04 | Coordinate curves are natural cubic splines with uniform parameter knots. Lookup chooses the first segment whose cumulative arc length is strictly greater than the requested distance, then evaluates its cubic at the fraction of that segment's arc length. Equality with the whole path length fails lookup. The presentation samples use ceil(segment arc length / 10), bounded to 1..100. |
| RFL-05 | With `Explode_When_Reached_Target_Radius` true, construction stops at the aim. Otherwise it constructs a post-aim extension from the late presentation tangent with 0.3 times the authored offset, and trims and reevaluates a path longer than authored maximum flight distance. The retained target-radius distance is the authored flight distance, independently of actual path length. |
| RFL-06 | Service increments distance along the path by current speed. A successful lookup supplies the next spatial position and updates facing. Failed lookup forces terminal advancement at the current position; it does not snap to the aim. A missing path after failed construction is a separate state from exhausted lookup. |
| RFL-07 | Hostile active/passive missile shields and sensor jamming can redirect on radius entry: the next position is inside or on the radius and the current position is outside or on it. Active ability radius overrides passive shield radius; a jamming ability supplies its radius. Space redirection builds a spatial spherical detour with a 30-unit margin, preserves remaining authored flight allowance, resets path distance and clears the missile target. Its complete construction and activation interfaces require a later weapons implementation. |
| RFL-08 | Ordinary collision runs before endpoint expiry. A hit suppresses the expiry blast. Positive travel allowance takes precedence over lifetime for ordinary space categories; otherwise lifetime expires only strictly after its authored duration. Rocket target-radius expiry compares distance along its path with the retained authored flight distance. DEFAULT target-radius expiry compares squared spatial displacement strictly beyond squared origin-to-aim distance. Generic removal adds no blast. |

The normal Diamond Boron profile authors speed 7.5, maximum flight distance 5000,
curve distance 500, curve offset zero, initial straight distance 500 and
target-radius expiry. Its BARRAGE variant inherits the path inputs and blast
damage 150, radius 200 and five dropoff tiers, overriding speed to 12 and maximum
flight distance to 3000. Ability proxy height and scatter are WAD-38 inputs.
`Fires_Forward` determines launch facing through its weapons owner; zero curve
offset alone does not prove every rocket route is collinear.

The read-only debug-build observations establish these decisions, but do not
establish retail presentation acceptance. Nonzero-offset custom rockets,
shield/jamming activation and detour parity, forward-firing launch alignment,
post-aim extension, and retail height/endpoint captures remain separate review
gates. Arc length recursively compares the chord with the two midpoint chords.
It returns their sum when either the relative or absolute difference is at most
0.000001, otherwise subdivides both halves; a smaller chord returns directly.
Q24 rounds that threshold to 17 raw units, an explicit numeric policy.
The area service must accept the terminal pose supplied by
flight and emit exactly one blast even while those broader gates stay open.
