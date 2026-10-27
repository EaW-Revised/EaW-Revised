#include "rocket_internal.hpp"

#include "motion_internal.hpp"
#include "tactical_internal.hpp"
#include "eawr/sim/tactical/combat.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <list>
#include <limits>
#include <vector>

namespace eawr::sim::tactical {
std::string_view flight_profile_error(const FlightProfile& profile) noexcept {
    // RFL-16: retain the authored scalar; RFL-09 checks actual evaluated geometry.
    if (profile.authored_distance.raw() < 0) return "Projectile_Max_Flight_Distance must be nonnegative";
    const auto bounded = [](const math::Fixed value) {
        return value.raw() >= 0 && value.raw() <= max_motion_coordinate * math::Fixed::scale;
    };
    if (!bounded(profile.curve_distance)) return "Projectile_Rocket_Curve_Distance outside supported geometry domain";
    if (!bounded(profile.straight_distance)) return "Projectile_Rocket_Straight_Distance outside supported geometry domain";
    if (!bounded(profile.curve_offset)) return "Projectile_Rocket_Curve_Offset outside supported geometry domain";
    if (profile.lifetime && (profile.lifetime->raw() < 0 || profile.lifetime->raw() > 3600 * math::Fixed::scale))
        return "Projectile_Max_Lifetime outside supported [0, 3600] seconds";
    if (profile.kind == FlightKind::rocket && profile.curve_distance.raw() == 0)
        return "Projectile_Rocket_Curve_Distance must be positive for ROCKET subdivision (RFL-09)";
    return {};
}
} // namespace eawr::sim::tactical

namespace eawr::sim::tactical::detail {
namespace {
using math::Fixed;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::whole;
constexpr Fixed arc_tolerance = Fixed::from_raw(17); // RFL-04: 0.000001 rounded to Q24

Vec3 add(Calc& q, const Vec3& a, const Vec3& b) {
    return {q.add(a.x, b.x), q.add(a.y, b.y), q.add(a.z, b.z)};
}
Vec3 sub(Calc& q, const Vec3& a, const Vec3& b) {
    return {q.sub(a.x, b.x), q.sub(a.y, b.y), q.sub(a.z, b.z)};
}
Vec3 scale(Calc& q, const Vec3& a, const Fixed factor) {
    return {q.mul(a.x, factor), q.mul(a.y, factor), q.mul(a.z, factor)};
}
Vec3 evaluate(Calc& q, const RocketSegment& segment, const Fixed t) {
    return add(q, segment.a, scale(q, add(q, segment.b,
        scale(q, add(q, segment.c, scale(q, segment.d, t)), t)), t));
}

// RFL-04: midpoint chord convergence, not fixed tessellation or arc inversion.
Fixed arc_length(Calc& q, const RocketSegment& segment, const Fixed low, const Fixed high, RocketPathWork& work) {
    struct Frame { Fixed low, high, left; unsigned stage{}; };
    std::vector<Frame> pending{{low, high, {}, 0}};
    Fixed value;
    while (!pending.empty()) {
        auto& frame = pending.back();
        const auto mid = Fixed::from_raw(frame.low.raw() + (frame.high.raw() - frame.low.raw()) / 2);
        if (frame.stage == 1) {
            frame.left = value;
            frame.stage = 2;
            pending.push_back({mid, frame.high, {}, 0});
            continue;
        }
        if (frame.stage == 2) {
            value = q.add(frame.left, value); // preserve recursive left/right addition order
            pending.pop_back();
            continue;
        }
        ++work.arc_visits;
        const auto first = evaluate(q, segment, frame.low);
        const auto last = evaluate(q, segment, frame.high);
        const auto chord = q.length(sub(q, last, first));
        value = chord;
        if (chord < arc_tolerance || !q.ok()) { pending.pop_back(); continue; }
        const auto middle = evaluate(q, segment, mid);
        const auto halves = q.add(q.length(sub(q, middle, first)), q.length(sub(q, last, middle)));
        value = halves;
        if (!q.ok() || halves.raw() == 0) { pending.pop_back(); continue; }
        if (mid == frame.low || mid == frame.high) {
            // RFL-09: finite Q24 interval cannot subdivide; retain the two-chord estimate.
            ++work.midpoint_estimates;
            pending.pop_back();
            continue;
        }
        if (q.abs(q.sub(q.div(chord, halves), whole(1))) <= arc_tolerance
            || q.abs(q.sub(chord, halves)) <= arc_tolerance) { pending.pop_back(); continue; }
        frame.stage = 1;
        pending.push_back({frame.low, mid, {}, 0});
    }
    return value;
}

// Check all element counts before growth. A genuine allocation failure is not a
// deterministic projectile outcome and deliberately propagates to the executor.
template<class Container>
bool room(const Container& values, const std::size_t extra, RocketPathWork& work,
    const RocketPathPolicy& policy) {
    const auto limit = std::min(values.max_size(), policy.max_controls);
    if (extra > std::numeric_limits<std::size_t>::max() - values.size()) {
        work.required_points = std::numeric_limits<std::size_t>::max();
        return false;
    }
    work.required_points = std::max(work.required_points, values.size() + extra);
    return values.size() <= limit && extra <= limit - values.size()
        && values.size() + extra <= std::numeric_limits<std::size_t>::max() / sizeof(typename Container::value_type);
}

core::Diagnostic refusal(const Projectile& projectile, const RocketPathWork& work, const std::string& reason) {
    return diagnostic(diagnostic_codes::resource_limit, "rocket type "
        + (projectile.profile_key ? std::to_string(projectile.profile_key->type) : std::string("unbound"))
        + " projectile " + std::to_string(projectile.id) + ": " + reason
        + "; required controls " + std::to_string(work.required_points)
        + ", observed controls " + std::to_string(work.control_points)
        + ", arc visits " + std::to_string(work.arc_visits));
}

bool append_segment(Calc& q, std::vector<Vec3>& points, const Vec3& from,
    const Vec3& to, const Fixed curve_distance, RocketPathWork& work, const RocketPathPolicy& policy, const bool detour = false) {
    const auto delta = sub(q, to, from);
    const auto length = q.length(delta);
    const auto spacing = detour ? curve_distance
        : q.mul(curve_distance, Fixed::from_raw(Fixed::scale * 11 / 10));
    if (!q.ok() || spacing.raw() <= 0 || length.raw() < 0) return false;
    // Retain a useful required count even when its Q24 quotient cannot be represented.
    const auto geometric_count = static_cast<std::uint64_t>(length.raw() / spacing.raw())
        + (length.raw() % spacing.raw() != 0 ? 1U : 0U);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    work.required_points = geometric_count > maximum - points.size() ? maximum
        : std::max(work.required_points, points.size() + static_cast<std::size_t>(geometric_count));
    const auto count = q.div(length, spacing).ceil_to_integer();
    if (!q.ok() || count < 0) return false;
    if (static_cast<std::uint64_t>(count) > std::numeric_limits<std::size_t>::max()) return false;
    if (!room(points, static_cast<std::size_t>(count), work, policy)) return false;
    // whole(count) is used in the unchanged Q24 interpolation below.
    if (count > std::numeric_limits<std::int64_t>::max() / Fixed::scale) return false;
    points.reserve(points.size() + static_cast<std::size_t>(count));
    for (std::int64_t i = 1; i <= count; ++i) {
        // Preserve the exact authored endpoint despite Q24 fraction rounding.
        points.push_back(i == count ? to : add(q, from, scale(q, delta, q.div(whole(i), whole(count)))));
    }
    work.control_points = std::max(work.control_points, points.size());
    return q.ok();
}

Vec3 normalized(Calc& q, const Vec3& value) {
    const auto length = q.length(value);
    return length.raw() == 0 ? Vec3{} : scale(q, value, q.div(whole(1), length));
}
Fixed dot(Calc& q, const Vec3& a, const Vec3& b) {
    return q.add(q.add(q.mul(a.x, b.x), q.mul(a.y, b.y)), q.mul(a.z, b.z));
}
// RFL-07: insert normalized midpoints in the retained ordered seed list.
bool sphere_points(Calc& q, const Vec3& from, const Vec3& to, std::list<Vec3>& points,
    const std::list<Vec3>::iterator before, const Fixed radius, const Fixed spacing,
    RocketPathWork& work, const RocketPathPolicy& policy) {
    struct Frame { Vec3 from, to; std::list<Vec3>::iterator before; unsigned stage{}; Vec3 middle; std::list<Vec3>::iterator inserted{}; };
    std::vector<Frame> pending{{from, to, before, 0, {}, {}}};
    while (!pending.empty()) {
        auto& frame = pending.back();
        if (frame.stage == 2) { pending.pop_back(); continue; }
        if (frame.stage == 1) {
            frame.stage = 2;
            pending.push_back({frame.middle, frame.to, frame.inserted, 0, {}, {}});
            continue;
        }
        const auto cosine = std::clamp(dot(q, frame.from, frame.to), whole(-1), whole(1));
        const auto sine = q.sqrt(std::max(Fixed{}, q.sub(whole(1), q.mul(cosine, cosine))));
        const auto angle = q.mul(q.div(q.atan2_deg(sine, cosine), whole(360)), motion_detail::two_pi);
        if (!q.ok()) return false;
        if (q.mul(angle, radius) <= spacing) { pending.pop_back(); continue; }
        const auto middle = normalized(q, scale(q, add(q, frame.from, frame.to), Fixed::from_raw(Fixed::scale / 2)));
        // RFL-09: normalized Q24 midpoint must make progress, including degenerate/antipodal seeds.
        if (!q.ok() || middle == Vec3{} || middle == frame.from || middle == frame.to) return false;
        for (const auto& ancestor : pending) {
            if (ancestor.stage != 0 && ancestor.middle == middle) return false;
        }
        if (!room(points, 1, work, policy)) return false;
        frame.inserted = points.insert(std::next(frame.before), middle);
        work.control_points = std::max(work.control_points, points.size());
        frame.middle = middle;
        frame.stage = 1;
        pending.push_back({frame.from, middle, frame.before, 0, {}, {}});
    }
    return q.ok();
}

std::vector<Fixed> second_derivatives(Calc& q, const std::vector<Fixed>& values) {
    const auto count = values.size();
    std::vector<Fixed> second(count), upper(count), rhs(count);
    // Natural cubic through uniform knots: M[i-1] + 4 M[i] + M[i+1] = 6 delta2[i].
    for (std::size_t i = 1; i + 1 < count; ++i) {
        const auto diagonal = q.sub(whole(4), upper[i - 1]);
        upper[i] = q.div(whole(1), diagonal);
        const auto difference = q.add(q.sub(values[i + 1], q.mul(whole(2), values[i])), values[i - 1]);
        rhs[i] = q.div(q.sub(q.mul(whole(6), difference), rhs[i - 1]), diagonal);
    }
    for (std::size_t i = count - 1; i-- > 1;) second[i] = q.sub(rhs[i], q.mul(upper[i], second[i + 1]));
    return second;
}

core::Result<void> build_path(Calc& q, FlightState& flight, const std::vector<Vec3>& points,
    RocketPathWork* diagnostics = nullptr) {
    RocketPathWork local_work;
    auto& work = diagnostics ? *diagnostics : local_work;
    std::vector<RocketSegment> path;
    // RFL-06: a retained unevaluable route fails lookup at the current pose.
    if (points.size() < 3) {
        flight.path_initialized = true; flight.path.clear(); flight.path_distance = {};
        return core::Result<void>::success();
    }
    // All spline arrays are linear in the actual controls; check each element type.
    if (points.size() > std::vector<Fixed>{}.max_size()
        || points.size() - 1 > path.max_size()) return core::Result<void>::failure(
            diagnostic(diagnostic_codes::resource_limit, "rocket spline storage is unrepresentable"));
    path.reserve(points.size() - 1);
    const std::array<std::vector<Fixed>, 3> coordinates = [&] {
        std::array<std::vector<Fixed>, 3> result;
        for (auto& axis : result) axis.reserve(points.size());
        for (const auto& point : points) {
            result[0].push_back(point.x); result[1].push_back(point.y); result[2].push_back(point.z);
        }
        return result;
    }();
    std::array<std::vector<Fixed>, 3> seconds;
    for (std::size_t axis = 0; axis < 3; ++axis) seconds[axis] = second_derivatives(q, coordinates[axis]);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        std::array<std::array<Fixed, 4>, 3> polynomial{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto& value = coordinates[axis]; const auto& second = seconds[axis];
            polynomial[axis] = {value[i], q.sub(q.sub(value[i + 1], value[i]),
                q.div(q.add(q.mul(whole(2), second[i]), second[i + 1]), whole(6))),
                q.div(second[i], whole(2)), q.div(q.sub(second[i + 1], second[i]), whole(6))};
        }
        const auto coefficient = [&](const std::size_t k) {
            return Vec3{polynomial[0][k], polynomial[1][k], polynomial[2][k]};
        };
        RocketSegment segment{coefficient(0), coefficient(1), coefficient(2), coefficient(3), {}};
        segment.length = arc_length(q, segment, Fixed{}, whole(1), work);
        path.push_back(segment);
    }
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket spline evaluation"));
    flight.path_initialized = true;
    flight.path = std::move(path);
    flight.path_distance = {};
    return core::Result<void>::success();
}

Vec3 cross(Calc& q, const Vec3& a, const Vec3& b) {
    return {q.sub(q.mul(a.y, b.z), q.mul(a.z, b.y)),
        q.sub(q.mul(a.z, b.x), q.mul(a.x, b.z)),
        q.sub(q.mul(a.x, b.y), q.mul(a.y, b.x))};
}

Vec3 offset_axis(Calc& q, const Vec3& value) {
    const auto length = q.length(value);
    return length.raw() == 0 ? Vec3{} : Vec3{q.div(value.x, length), q.div(value.y, length), q.div(value.z, length)};
}

bool append_offset_segment(Calc& q, std::vector<Vec3>& points, const Vec3& from,
    const Vec3& to, const FlightProfile& profile, const Fixed offset_scale,
    CombatRandom* random, RocketPathWork& work, const RocketPathPolicy& policy) {
    const auto begin = points.size();
    if (!append_segment(q, points, from, to, profile.curve_distance, work, policy)) return false;
    work.control_points = std::max(work.control_points, points.size());
    const auto offset = q.mul(profile.curve_offset, offset_scale);
    if (offset.raw() == 0 || points.size() <= begin + 1) return q.ok();
    // RFL-03: the spatial axes depend on the segment midpoint as well as its direction.
    const auto backward = sub(q, from, to);
    const auto anchor = add(q, scale(q, add(q, from, to), Fixed::from_raw(Fixed::scale / 2)),
        Vec3{whole(1), whole(1), whole(1)});
    const auto perpendicular = cross(q, anchor, backward);
    const auto first = offset_axis(q, perpendicular);
    // RFL-03: a positive scale on the first cross product cancels when the
    // second axis is normalized. Use its unit vector to bound Q24 intermediates.
    const auto second = offset_axis(q, cross(q, first, backward));
    if (!random || !q.ok()) return false;
    for (auto index = begin; index + 1 < points.size(); ++index) {
        const auto turn = Fixed::from_raw(random->uniform(0, static_cast<std::uint32_t>(Fixed::scale - 1)));
        const auto radius = q.mul(offset, Fixed::from_raw(random->uniform(0, static_cast<std::uint32_t>(Fixed::scale))));
        const auto direction = add(q, scale(q, first, math::cos_turn(turn)), scale(q, second, math::sin_turn(turn)));
        points[index] = add(q, points[index], scale(q, direction, radius));
    }
    return q.ok();
}

// RFL-05: trim removes controls starting at the selected segment, adds its
// evaluated point and rebuilds; it does not merely shorten the cached last arc.
core::Result<void> trim_path(Calc& q, FlightState& flight, std::vector<Vec3>& points, RocketPathWork& work) {
    Fixed passed{};
    for (std::size_t i = 0; i < flight.path.size(); ++i) {
        const auto end = q.add(passed, flight.path[i].length);
        if (flight.profile.authored_distance < end && flight.path[i].length.raw() > 0) {
            const auto endpoint = evaluate(q, flight.path[i],
                q.div(q.sub(flight.profile.authored_distance, passed), flight.path[i].length));
            points.resize(i); points.push_back(endpoint);
            return build_path(q, flight, points, &work);
        }
        passed = end;
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<Fixed> rocket_arc_length(const RocketSegment& segment, const Fixed low, const Fixed high, RocketPathWork& work) {
    Calc q;
    if (low < Fixed{} || high > whole(1) || low > high) return core::Result<Fixed>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "invalid rocket arc interval"));
    const auto length = arc_length(q, segment, low, high, work);
    if (!q.ok()) return core::Result<Fixed>::failure(q.error("rocket arc integration"));
    return core::Result<Fixed>::success(length);
}

bool valid_flight(const FlightProfile& profile) noexcept {
    return flight_profile_error(profile).empty();
}

core::Result<void> prepare_rocket_path(Projectile& projectile, CombatRandom* random, RocketPathWork* diagnostics, const RocketPathPolicy& policy) {
    auto& flight = *projectile.flight;
    if (flight.path_initialized) return core::Result<void>::success();
    if (!valid_flight(flight.profile)) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "invalid rocket path profile (RFL-03/05)"));
    if (flight.profile.curve_offset.raw() > 0 && !random) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "nonzero rocket offset requires synchronized draws (RFL-03)"));
    RocketPathWork local_work;
    auto& work = diagnostics ? *diagnostics : local_work;
    work = {};
    auto route = flight;
    Calc q;
    const auto delta = sub(q, flight.aim, projectile.position);
    const auto initial = std::min(flight.profile.straight_distance,
        q.div(q.length(delta.x, delta.y), whole(2)));
    std::vector<Vec3> points{projectile.position};
    work.control_points = 1;
    Vec3 start = projectile.position;
    bool bounded = true;
    if (initial.raw() != 0) {
        const auto direction = scale(q, projectile.step, q.div(whole(1), projectile.speed));
        start = add(q, projectile.position, scale(q, direction, initial));
        bounded = append_offset_segment(q, points, projectile.position, start, flight.profile,
            Fixed::from_raw(Fixed::scale / 10), random, work, policy);
    }
    bounded = bounded && append_offset_segment(q, points, start, flight.aim, flight.profile, whole(1), random, work, policy);
    if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket path construction").message));
    if (!bounded) return core::Result<void>::failure(
        refusal(projectile, work, "control storage refused"));
    auto built = build_path(q, route, points, &work);
    if (!built) return core::Result<void>::failure(refusal(projectile, work, built.error().message));
    if (!flight.profile.target_radius && !route.path.empty()) {
        Fixed length{};
        for (const auto& segment : route.path) length = q.add(length, segment.length);
        const auto& last = route.path.back();
        const auto samples = std::clamp(q.div(last.length, whole(10)).ceil_to_integer(), std::int64_t{1}, std::int64_t{100});
        // RFL-04/05: only the final two presentation samples are needed. Keep
        // their difference unnormalized, including when the allowance is negative.
        const auto late = evaluate(q, last, q.div(whole(samples - 1), whole(samples)));
        const auto tangent = sub(q, evaluate(q, last, whole(1)), late);
        work.tangent_samples += 2;
        const auto endpoint = add(q, flight.aim, scale(q, tangent, q.sub(flight.profile.authored_distance, length)));
        bounded = append_offset_segment(q, points, flight.aim, endpoint, flight.profile,
            Fixed::from_raw(Fixed::scale * 3 / 10), random, work, policy);
        if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket extension construction").message));
        if (!bounded) return core::Result<void>::failure(
            refusal(projectile, work, "extension storage refused"));
        built = build_path(q, route, points, &work);
        if (!built) return core::Result<void>::failure(refusal(projectile, work, built.error().message));
        built = trim_path(q, route, points, work);
        if (!built) return core::Result<void>::failure(refusal(projectile, work, built.error().message));
    }
    if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket path construction").message));
    flight = std::move(route);
    return core::Result<void>::success();
}

core::Result<void> repath_rocket(Projectile& projectile, const Vec3& source, const Vec3& previous_step,
    RocketPathWork* diagnostics, const RocketPathPolicy& policy) {
    RocketPathWork local_work;
    auto& work = diagnostics ? *diagnostics : local_work;
    work = {};
    auto flight = *projectile.flight;
    if (!flight.shield_redirected) flight.shield_allowance = flight.profile.authored_distance;
    flight.shield_redirected = true;
    Calc q;
    const auto heading = normalized(q, previous_step);
    if (dot(q, heading, sub(q, source, projectile.position)).raw() < 0 && q.ok()) {
        *projectile.flight = std::move(flight);
        return core::Result<void>::success();
    }
    const auto first = add(q, projectile.position, scale(q, heading, whole(30)));
    const auto lead = add(q, first, scale(q, heading, whole(30)));
    const auto offset = sub(q, lead, source);
    const auto radius = q.length(offset);
    // The forward ray starts half a unit beyond a point already on this sphere.
    const auto intersection = q.neg(q.mul(whole(2), dot(q, offset, heading)));
    if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket detour ray").message));
    if (intersection < Fixed::from_raw(Fixed::scale / 2) || intersection > whole(1000000)) {
        *projectile.flight = std::move(flight);
        return core::Result<void>::success();
    }
    const auto exit = add(q, lead, scale(q, heading, intersection));
    const auto near = normalized(q, offset);
    const auto far = normalized(q, sub(q, exit, source));
    const auto midpoint = normalized(q, scale(q, add(q, near, far), Fixed::from_raw(Fixed::scale / 2)));
    // Wrapped antipodal atan2 is negative, so reject its zero seed before the angular stop.
    if (!q.ok() || midpoint == Vec3{}) return core::Result<void>::failure(
        refusal(projectile, work, "degenerate spherical seed midpoint"));
    const auto quarter = normalized(q, scale(q, add(q, near, midpoint), Fixed::from_raw(Fixed::scale / 2)));
    const auto three_quarters = normalized(q, scale(q, add(q, far, midpoint), Fixed::from_raw(Fixed::scale / 2)));
    const auto eighth = normalized(q, scale(q, add(q, near, quarter), Fixed::from_raw(Fixed::scale / 2)));
    std::list<Vec3> directions{eighth, three_quarters};
    bool bounded = sphere_points(q, near, three_quarters, directions, directions.begin(),
        radius, flight.profile.curve_distance, work, policy);
    if (directions.size() > 2) { directions.pop_front(); directions.pop_front(); }
    std::vector<Vec3> points{projectile.position};
    bounded = bounded && append_segment(q, points, projectile.position, first, flight.profile.curve_distance, work, policy, true);
    Vec3 last = first;
    if (!bounded || !room(points, directions.size(), work, policy)) return core::Result<void>::failure(
        refusal(projectile, work, "spherical subdivision cannot progress or storage refused"));
    points.reserve(points.size() + directions.size());
    for (const auto& direction : directions) {
        last = add(q, source, scale(q, direction, radius));
        points.push_back(last);
    }
    auto planar = heading; planar.z = {};
    planar = normalized(q, planar);
    const auto remaining = q.sub(flight.shield_allowance, flight.path_distance);
    const auto endpoint = add(q, last, scale(q, planar, q.sub(remaining, whole(30))));
    bounded = bounded && append_segment(q, points, last, endpoint, flight.profile.curve_distance, work, policy, true);
    if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket detour construction").message));
    if (!bounded) return core::Result<void>::failure(
        refusal(projectile, work, "detour storage refused"));
    const auto authored = flight.profile.authored_distance;
    auto built = build_path(q, flight, points, &work);
    if (!built) return core::Result<void>::failure(refusal(projectile, work, built.error().message));
    // RFL-07: trim at the type's authored distance before resetting its retained allowance.
    Fixed passed{};
    for (std::size_t i = 0; i < flight.path.size(); ++i) {
        const auto end = q.add(passed, flight.path[i].length);
        if (authored < end && flight.path[i].length.raw() > 0) {
            const auto endpoint_at_limit = evaluate(q, flight.path[i], q.div(q.sub(authored, passed), flight.path[i].length));
            points.resize(i + 1); points.push_back(endpoint_at_limit);
            built = build_path(q, flight, points, &work);
            break;
        }
        passed = end;
    }
    if (!built) return core::Result<void>::failure(refusal(projectile, work, built.error().message));
    flight.shield_allowance = remaining;
    if (!q.ok()) return core::Result<void>::failure(refusal(projectile, work, q.error("rocket detour trimming").message));
    *projectile.flight = std::move(flight);
    projectile.target = invalid_entity_id; projectile.locked = false;
    return core::Result<void>::success();
}

core::Result<std::optional<Vec3>> rocket_point(const FlightState& flight, const Fixed distance) {
    Calc q;
    Fixed passed{};
    for (const auto& segment : flight.path) {
        const auto end = q.add(passed, segment.length);
        if (distance < end && segment.length.raw() > 0) {
            const auto point = evaluate(q, segment, q.div(q.sub(distance, passed), segment.length));
            if (!q.ok()) return core::Result<std::optional<Vec3>>::failure(q.error("rocket path lookup"));
            return core::Result<std::optional<Vec3>>::success(point);
        }
        passed = end;
    }
    if (!q.ok()) return core::Result<std::optional<Vec3>>::failure(q.error("rocket path lookup"));
    return core::Result<std::optional<Vec3>>::success(std::nullopt);
}
} // namespace eawr::sim::tactical::detail
