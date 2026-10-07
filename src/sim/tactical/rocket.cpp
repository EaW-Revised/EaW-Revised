#include "rocket_internal.hpp"

#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <list>
#include <vector>

namespace eawr::sim::tactical::detail {
namespace {
using math::Fixed;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::whole;
constexpr std::size_t point_limit = 2048;
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
Fixed arc_length(Calc& q, const RocketSegment& segment, const Fixed low,
    const Fixed high, const std::uint32_t depth, bool& bounded) {
    const auto first = evaluate(q, segment, low);
    const auto last = evaluate(q, segment, high);
    const auto chord = q.length(sub(q, last, first));
    if (chord < arc_tolerance || !q.ok()) return chord;
    const auto mid = Fixed::from_raw(low.raw() + (high.raw() - low.raw()) / 2);
    const auto middle = evaluate(q, segment, mid);
    const auto halves = q.add(q.length(sub(q, middle, first)), q.length(sub(q, last, middle)));
    if (!q.ok() || halves.raw() == 0) return halves;
    if (q.abs(q.sub(q.div(chord, halves), whole(1))) <= arc_tolerance
        || q.abs(q.sub(chord, halves)) <= arc_tolerance) return halves;
    if (depth == 32 || mid == low || mid == high) {
        bounded = false;
        return Fixed{};
    }
    return q.add(arc_length(q, segment, low, mid, depth + 1, bounded),
        arc_length(q, segment, mid, high, depth + 1, bounded));
}

bool append_segment(Calc& q, std::vector<Vec3>& points, const Vec3& from,
    const Vec3& to, const Fixed curve_distance, const bool detour = false) {
    const auto delta = sub(q, to, from);
    const auto length = q.length(delta);
    const auto spacing = detour ? curve_distance
        : q.mul(curve_distance, Fixed::from_raw(Fixed::scale * 11 / 10));
    const auto count = q.div(length, spacing).ceil_to_integer();
    if (!q.ok() || count < 0 || count > static_cast<std::int64_t>(point_limit - points.size())) return false;
    for (std::int64_t i = 1; i <= count; ++i) {
        // Preserve the exact authored endpoint despite Q24 fraction rounding.
        points.push_back(i == count ? to : add(q, from, scale(q, delta, q.div(whole(i), whole(count)))));
    }
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
    const std::uint32_t depth = 0) {
    const auto cosine = std::clamp(dot(q, from, to), whole(-1), whole(1));
    const auto sine = q.sqrt(std::max(Fixed{}, q.sub(whole(1), q.mul(cosine, cosine))));
    const auto angle = q.mul(q.div(q.atan2_deg(sine, cosine), whole(360)), motion_detail::two_pi);
    if (!q.ok()) return false;
    if (q.mul(angle, radius) <= spacing) return true;
    if (depth == 32 || points.size() == point_limit) return false;
    const auto middle = normalized(q, scale(q, add(q, from, to), Fixed::from_raw(Fixed::scale / 2)));
    const auto inserted = points.insert(std::next(before), middle);
    return sphere_points(q, from, middle, points, before, radius, spacing, depth + 1)
        && sphere_points(q, middle, to, points, inserted, radius, spacing, depth + 1);
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

core::Result<void> build_path(Calc& q, FlightState& flight, const std::vector<Vec3>& points) {
    bool bounded = true;
    flight.path_initialized = true;
    flight.path.clear();
    flight.path_distance = Fixed{};
    // RFL-06: a retained unevaluable route fails lookup at the current pose.
    if (points.size() < 3) return core::Result<void>::success();
    const std::array<std::vector<Fixed>, 3> coordinates = [&] {
        std::array<std::vector<Fixed>, 3> result;
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
        segment.length = arc_length(q, segment, Fixed{}, whole(1), 0, bounded);
        flight.path.push_back(segment);
    }
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket spline evaluation"));
    if (!bounded) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "rocket arc evaluation exceeds its subdivision bound"));
    return core::Result<void>::success();
}
} // namespace

bool valid_flight(const FlightProfile& profile) noexcept {
    const auto bounded = [](const Fixed value) {
        return value.raw() >= 0 && value.raw() <= max_motion_coordinate * Fixed::scale;
    };
    if (!bounded(profile.authored_distance) || !bounded(profile.curve_distance)
        || !bounded(profile.straight_distance) || !bounded(profile.curve_offset)
        || (profile.lifetime && (profile.lifetime->raw() < 0 || profile.lifetime->raw() > 3600 * Fixed::scale))) return false;
    if (profile.kind != FlightKind::rocket) return true;
    // G4 supplies the stock endpoint route. RFL-03/05/07 broader constructions stay gated.
    return profile.curve_distance.raw() > 0 && profile.curve_offset.raw() == 0 && profile.target_radius;
}

core::Result<void> prepare_rocket_path(Projectile& projectile) {
    auto& flight = *projectile.flight;
    if (!valid_flight(flight.profile)) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "unsupported rocket path profile (RFL-03/05)"));
    Calc q;
    const auto delta = sub(q, flight.aim, projectile.position);
    const auto initial = std::min(flight.profile.straight_distance,
        q.div(q.length(delta.x, delta.y), whole(2)));
    std::vector<Vec3> points{projectile.position};
    Vec3 start = projectile.position;
    bool bounded = true;
    if (initial.raw() != 0) {
        const auto direction = scale(q, projectile.step, q.div(whole(1), projectile.speed));
        start = add(q, projectile.position, scale(q, direction, initial));
        bounded = append_segment(q, points, projectile.position, start, flight.profile.curve_distance);
    }
    bounded = bounded && append_segment(q, points, start, flight.aim, flight.profile.curve_distance);
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket path construction"));
    if (!bounded) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "rocket path exceeds the control-point bound"));
    return build_path(q, flight, points);
}

core::Result<void> repath_rocket(Projectile& projectile, const Vec3& source, const Vec3& previous_step) {
    auto& flight = *projectile.flight;
    if (!flight.shield_redirected) flight.shield_allowance = flight.profile.authored_distance;
    flight.shield_redirected = true;
    Calc q;
    const auto heading = normalized(q, previous_step);
    if (dot(q, heading, sub(q, source, projectile.position)).raw() < 0) return core::Result<void>::success();
    const auto first = add(q, projectile.position, scale(q, heading, whole(30)));
    const auto lead = add(q, first, scale(q, heading, whole(30)));
    const auto offset = sub(q, lead, source);
    const auto radius = q.length(offset);
    // The forward ray starts half a unit beyond a point already on this sphere.
    const auto intersection = q.neg(q.mul(whole(2), dot(q, offset, heading)));
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket detour ray"));
    if (intersection < Fixed::from_raw(Fixed::scale / 2) || intersection > whole(1000000)) {
        return core::Result<void>::success();
    }
    const auto exit = add(q, lead, scale(q, heading, intersection));
    const auto near = normalized(q, offset);
    const auto far = normalized(q, sub(q, exit, source));
    const auto midpoint = normalized(q, scale(q, add(q, near, far), Fixed::from_raw(Fixed::scale / 2)));
    const auto quarter = normalized(q, scale(q, add(q, near, midpoint), Fixed::from_raw(Fixed::scale / 2)));
    const auto three_quarters = normalized(q, scale(q, add(q, far, midpoint), Fixed::from_raw(Fixed::scale / 2)));
    const auto eighth = normalized(q, scale(q, add(q, near, quarter), Fixed::from_raw(Fixed::scale / 2)));
    std::list<Vec3> directions{eighth, three_quarters};
    bool bounded = sphere_points(q, near, three_quarters, directions, directions.begin(),
        radius, flight.profile.curve_distance);
    if (directions.size() > 2) { directions.pop_front(); directions.pop_front(); }
    std::vector<Vec3> points{projectile.position};
    bounded = bounded && append_segment(q, points, projectile.position, first, flight.profile.curve_distance, true);
    Vec3 last = first;
    for (const auto& direction : directions) {
        last = add(q, source, scale(q, direction, radius));
        points.push_back(last);
    }
    auto planar = heading; planar.z = {};
    planar = normalized(q, planar);
    const auto remaining = q.sub(flight.shield_allowance, flight.path_distance);
    const auto endpoint = add(q, last, scale(q, planar, q.sub(remaining, whole(30))));
    bounded = bounded && append_segment(q, points, last, endpoint, flight.profile.curve_distance, true);
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket detour construction"));
    if (!bounded || points.size() > point_limit) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "rocket detour exceeds the control-point bound"));
    const auto authored = flight.profile.authored_distance;
    auto built = build_path(q, flight, points);
    if (!built) return built;
    // RFL-07: trim at the type's authored distance before resetting its retained allowance.
    Fixed passed{};
    for (std::size_t i = 0; i < flight.path.size(); ++i) {
        const auto end = q.add(passed, flight.path[i].length);
        if (authored < end && flight.path[i].length.raw() > 0) {
            const auto endpoint_at_limit = evaluate(q, flight.path[i], q.div(q.sub(authored, passed), flight.path[i].length));
            points.resize(i + 1); points.push_back(endpoint_at_limit);
            built = build_path(q, flight, points);
            break;
        }
        passed = end;
    }
    if (!built) return built;
    flight.shield_allowance = remaining;
    projectile.target = invalid_entity_id; projectile.locked = false;
    if (!q.ok()) return core::Result<void>::failure(q.error("rocket detour trimming"));
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
