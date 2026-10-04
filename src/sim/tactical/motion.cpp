#include "eawr/sim/tactical/motion.hpp"

#include "eawr/sim/math/math.hpp"
#include "../math/wide.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace eawr::sim::tactical {

namespace motion_detail {

using math::Fixed;

core::Diagnostic Calc::error(const std::string_view what) const {
    return detail::diagnostic(diagnostic_codes::worker_failure,
        std::string(what) + ": " + (error_ ? error_->message : std::string("arithmetic failure")));
}

void Calc::record(const core::Diagnostic& failure, const std::string_view operation) {
    error_ = failure;
    if (site_ != nullptr) error_->message = std::string(site_) + ": " + error_->message;
    if (!operation.empty()) error_->message += " (" + std::string(operation) + ")";
}

namespace {

// The raw Q24 operands of a failed operation, for its diagnostic (#615).
[[nodiscard]] std::string operands(const Fixed left, const std::string_view operation, const Fixed right) {
    return "raw " + std::to_string(left.raw()) + " " + std::string(operation) + " raw " + std::to_string(right.raw());
}

} // namespace

Fixed Calc::failed_add(const Fixed left, const Fixed right) {
    auto result = math::add(left, right);
    if (!result && !error_) record(result.error(), operands(left, "+", right));
    return result ? result.value() : Fixed{};
}
Fixed Calc::failed_sub(const Fixed left, const Fixed right) {
    auto result = math::subtract(left, right);
    if (!result && !error_) record(result.error(), operands(left, "-", right));
    return result ? result.value() : Fixed{};
}
Fixed Calc::failed_mul(const Fixed left, const Fixed right) {
    auto result = math::multiply(left, right);
    if (!result && !error_) record(result.error(), operands(left, "*", right));
    return result ? result.value() : Fixed{};
}
Fixed Calc::failed_div(const Fixed left, const Fixed right) {
    auto result = math::divide(left, right);
    if (!result && !error_) record(result.error(), operands(left, "/", right));
    return result ? result.value() : Fixed{};
}
Fixed Calc::failed_sqrt(const Fixed value) { return take(math::sqrt(value)); }
Fixed Calc::failed_neg(const Fixed value) { return take(math::negate(value)); }
Fixed Calc::failed_length(const Fixed x, const Fixed y) { return take(math::length(math::Vec2{x, y})); }
Fixed Calc::failed_dot(const Fixed ax, const Fixed ay, const Fixed bx, const Fixed by) {
    return take(math::dot(math::Vec2{ax, ay}, math::Vec2{bx, by}));
}

Fixed clamp180(const Fixed degrees) noexcept {
    constexpr std::int64_t full = 360 * Fixed::scale;
    constexpr std::int64_t half = 180 * Fixed::scale;
    std::int64_t raw = degrees.raw() % full;
    if (raw >= half) {
        raw -= full;
    } else if (raw < -half) {
        raw += full;
    }
    return Fixed::from_raw(raw);
}

// cos_deg and sin_deg of the same turn, from one CORDIC run (#520).
Heading heading(Calc& calc, const Fixed yaw) {
    const auto both = math::sin_cos_turn(calc.div(yaw, whole(360)));
    return {both.cosine, both.sine};
}

bool within_coordinates(const math::Vec3& value) noexcept {
    const auto inside = [](const Fixed component) {
        return component.raw() >= -coordinate_limit_raw && component.raw() <= coordinate_limit_raw;
    };
    return inside(value.x) && inside(value.y) && inside(value.z);
}

Fixed forward_step(Calc& calc, const Fixed expansion, const Fixed length, const Fixed max_speed) {
    Fixed step = expansion;
    if (length < calc.mul(whole(2), step)) {
        step = std::max(min_expansion_distance, calc.div(length, whole(2)));
    }
    return std::max(step, max_speed);
}

std::optional<std::vector<PathNode>> trivial_path(Calc& calc, const Fixed max_speed, const Fixed step,
    const Fixed start_frame, const math::Vec3 position, const Fixed yaw, const Fixed speed, const math::Vec3 target) {
    const Fixed dx = calc.sub(target.x, position.x);
    const Fixed dy = calc.sub(target.y, position.y);
    const Fixed cell_limit = calc.mul(whole(2), step);
    if (!(dx.raw() >= 0 && dy.raw() >= 0 && calc.mul(whole(3), dx) < cell_limit && calc.mul(whole(3), dy) < cell_limit)) {
        return std::nullopt;
    }
    std::vector<PathNode> nodes;
    nodes.push_back({start_frame, position, clamp180(yaw), speed});
    nodes.push_back({calc.add(start_frame, calc.div(calc.length(dx, dy), max_speed)), {target.x, target.y, position.z},
        calc.atan2_deg(dy, dx), Fixed{}});
    return nodes;
}

} // namespace motion_detail

namespace {

using math::Fixed;
using math::Vec2;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::aligned_degrees;
using motion_detail::clamp180;
using motion_detail::coordinate_limit_raw;
using motion_detail::heading;
using motion_detail::min_expansion_distance;
using motion_detail::two_pi;
using motion_detail::whole;
using motion_detail::within_coordinates;

// The speed below which a sampled path keeps its yaw (MV-31), rounded once to Q24.
constexpr Fixed still_speed = Fixed::from_raw(1678); // 0.0001
// How far outside the turn circle a remake-only straight leg ends (MV-17): 1/16 unit.
constexpr Fixed circle_margin = Fixed::from_raw(Fixed::scale / 16);
// The least share of the roll rate a banking frame uses (BK-02): 0.1 rounded once to Q24.
constexpr Fixed min_roll_share = Fixed::from_raw(1677722);
// Arc steps a plan may take before it flies straight to the target.
constexpr int max_arc_steps = 64;
constexpr std::int64_t rate_limit_raw = max_motion_rate * Fixed::scale;
// Numeric limits, not movement rules (MV-01). The slowest authored super capital turns
// at 0.01 * 1.2 > 1/128 degrees/frame. Including Q24 rounding, their speeds are in
// [0.35,1), a,d > 0.02, and vmax/rate < 31. At any accepted endpoints and starting speed
// in [0,vmax], R < 2048, acceleration travel < 25, and 64 arcs/inside-circle exits travel < 64 * 4R.
// Endpoint separation is < 2^20, so each leg (including the final straight) is < 2^21
// units and < 2^23 frames. A straight tangent is < leg + 2*vmax^2/min(a,d) < 2^22 units;
// arc tangents are <= 180*vmax/rate < 5580. Raw spans are < 2^47, deltas/tangents < 2^46.
// Thus the sum of Hermite position terms is < 5 * 2^(3*47+46) < 2^190; velocity
// terms are < 12 * 2^(2*47+46+24) < 2^168. Both fit the existing exact 192 bits.
// Helpers subdivide legs. Face turns are <= 180/rate * slowdown and need no Hermite.
// Keep the speed/acceleration bounds: every authored value already exceeds them.
constexpr std::int64_t min_speed_raw = Fixed::scale / 16;
constexpr std::int64_t min_acceleration_raw = Fixed::scale >> 12;
constexpr std::int64_t min_turn_raw = Fixed::scale / 128;

namespace wide = math::detail;

// The signed product of int64 factors in 192 bits; false on overflow.
[[nodiscard]] bool product(std::initializer_list<std::int64_t> factors, wide::SignedWide& result) noexcept {
    result = {wide::from_u64(1), false};
    for (const auto factor : factors) {
        if (wide::multiply_by_u64(result.magnitude, wide::unsigned_magnitude(factor), result.magnitude)) {
            return false;
        }
        result.negative = result.negative != (factor < 0);
    }
    if (result.magnitude.is_zero()) {
        result.negative = false;
    }
    return true;
}

// MV-30: a cubic Hermite from p0 to p0 + span_delta with end tangents m0 and m1 (raw), at
// `elapsed` of `span` (raw frames, 0 <= elapsed < span), evaluated exactly and rounded once:
// the offset from p0 and the velocity per frame. With tau = elapsed / span and rest = span -
// elapsed, offset = (e^2 (S + 2 rest) delta + e rest^2 m0 - e^2 rest m1) / S^3 and velocity =
// (6 e rest delta + rest (S - 3e) m0 + e (3e - 2S) m1) 2^24 / S^3.
[[nodiscard]] bool hermite(const std::int64_t elapsed, const std::int64_t span, const std::int64_t delta,
    const std::int64_t m0, const std::int64_t m1, std::int64_t& offset, std::int64_t& velocity) noexcept {
    const std::int64_t e = elapsed;
    const std::int64_t rest = span - elapsed;
    wide::UInt192 cube{};
    if (wide::multiply_by_u64(wide::multiply_u64(static_cast<std::uint64_t>(span), static_cast<std::uint64_t>(span)),
            static_cast<std::uint64_t>(span), cube)) {
        return false;
    }
    wide::SignedWide term;
    wide::SignedWide position{};
    if (!product({e, e, span + 2 * rest, delta}, term)) return false;
    position.add(term);
    if (!product({e, rest, rest, m0}, term)) return false;
    position.add(term);
    if (!product({-e, e, rest, m1}, term)) return false;
    position.add(term);
    wide::SignedWide rate{};
    if (!product({6, e, rest, delta, Fixed::scale}, term)) return false;
    rate.add(term);
    if (!product({rest, span - 3 * e, m0, Fixed::scale}, term)) return false;
    rate.add(term);
    if (!product({e, 3 * e - 2 * span, m1, Fixed::scale}, term)) return false;
    rate.add(term);
    return wide::rounded_divide_to_raw(position, cube, offset) && wide::rounded_divide_to_raw(rate, cube, velocity);
}

[[nodiscard]] bool within(const Fixed value, const std::int64_t low) noexcept {
    return value.raw() >= low && value.raw() <= rate_limit_raw;
}

[[nodiscard]] core::Diagnostic invalid(const std::string& message) {
    return detail::diagnostic(diagnostic_codes::invalid_setup, "motion table: " + message);
}

} // namespace

void motion_detail::finish_path(Calc& calc, const MotionProfile& profile, const Fixed theta, std::vector<PathNode>& nodes) {
    auto& last = nodes.back();
    const auto& previous = nodes[nodes.size() - 2];
    const Fixed arrival = last.speed;
    if (arrival.raw() == 0) {
        insert_helpers(calc, profile, theta, nodes);
        return;
    }
    const Fixed leg = calc.length(calc.sub(last.position.x, previous.position.x),
        calc.sub(last.position.y, previous.position.y));
    Fixed end_speed{};
    const Fixed spare = calc.sub(calc.mul(arrival, arrival), calc.mul(calc.mul(whole(2), profile.deceleration), leg));
    if (spare.raw() > 0) {
        end_speed = std::min(arrival, calc.sqrt(spare));
    }
    last.speed = end_speed;
    const Fixed braking = calc.div(calc.abs(calc.sub(calc.mul(arrival, arrival), calc.mul(end_speed, end_speed))),
        calc.mul(whole(2), profile.deceleration));
    const Fixed extra = calc.sub(calc.div(calc.sub(arrival, end_speed), profile.deceleration), calc.div(braking, arrival));
    if (extra.raw() > 0) {
        last.frame = calc.add(last.frame, extra);
    }

    insert_helpers(calc, profile, theta, nodes);
}

void motion_detail::insert_helpers(Calc& calc, const MotionProfile& profile, const Fixed theta, std::vector<PathNode>& nodes) {
    const Fixed straight = calc.cos_deg(theta);
    for (std::size_t index = 0; index + 1 < nodes.size() && calc.ok();) {
        const PathNode from = nodes[index];
        const PathNode to = nodes[index + 1];
        if (calc.abs(calc.sub(from.speed, to.speed)) <= still_speed) {
            ++index;
            continue;
        }
        const auto a = heading(calc, from.yaw);
        const auto b = heading(calc, to.yaw);
        if (calc.dot(a.x, a.y, b.x, b.y) < straight) {
            ++index;
            continue;
        }
        const bool faster = to.speed >= from.speed;
        const Fixed rate = faster ? profile.acceleration : profile.deceleration;
        const Fixed segment = calc.length(calc.sub(to.position.x, from.position.x), calc.sub(to.position.y, from.position.y));
        const Fixed change = calc.div(calc.abs(calc.sub(calc.mul(to.speed, to.speed), calc.mul(from.speed, from.speed))),
            calc.mul(whole(2), rate));
        if (!(calc.add(change, profile.acceleration) < segment)) {
            ++index;
            continue;
        }
        PathNode helper;
        helper.yaw = from.yaw;
        const Fixed remaining = calc.sub(segment, change);
        if (faster) {
            // Speed changes over the first `change` units, then holds (MV-19).
            const Fixed t = calc.div(change, segment);
            helper.position = {calc.add(from.position.x, calc.mul(calc.sub(to.position.x, from.position.x), t)),
                calc.add(from.position.y, calc.mul(calc.sub(to.position.y, from.position.y), t)), from.position.z};
            helper.speed = to.speed;
            helper.frame = calc.sub(to.frame, calc.div(remaining, to.speed));
        } else {
            // Speed holds, then changes over the last `change` units (MV-18).
            const Fixed t = calc.div(change, segment);
            helper.position = {calc.add(to.position.x, calc.mul(calc.sub(from.position.x, to.position.x), t)),
                calc.add(to.position.y, calc.mul(calc.sub(from.position.y, to.position.y), t)), from.position.z};
            helper.speed = from.speed;
            helper.frame = calc.add(from.frame, calc.div(remaining, from.speed));
        }
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(index + 1), helper);
        index += 2;
    }
}


const MotionProfile* MotionTable::find(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(profiles.begin(), profiles.end(), type_id,
        [](const MotionProfile& profile, const TypeId id) { return profile.type_id < id; });
    return found != profiles.end() && found->type_id == type_id ? &*found : nullptr;
}

const Footprint* MotionTable::footprint(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(footprints.begin(), footprints.end(), type_id,
        [](const Footprint& footprint, const TypeId id) { return footprint.type_id < id; });
    return found != footprints.end() && found->type_id == type_id ? &*found : nullptr;
}

core::Result<void> validate_motion(const MotionTable& table) {
    if (table.nebula_disable_seconds.raw() < 0 || table.nebula_disable_seconds.raw() > 3600 * math::Fixed::scale
        || !std::is_sorted(table.nebula_service_types.begin(), table.nebula_service_types.end())
        || std::adjacent_find(table.nebula_service_types.begin(), table.nebula_service_types.end()) != table.nebula_service_types.end()) {
        return core::Result<void>::failure(invalid("invalid nebula service content"));
    }
    if (!(table.rules.arc_degrees.raw() > 0 && table.rules.arc_degrees <= whole(180))) {
        return core::Result<void>::failure(invalid("the arc angle must be in (0, 180] degrees"));
    }
    if (!(table.rules.expansion_distance.raw() > 0 && table.rules.expansion_distance.raw() <= coordinate_limit_raw)) {
        return core::Result<void>::failure(invalid("the expansion distance is out of range"));
    }
    if (table.rules.reevaluation_frames == 0 || table.rules.reevaluation_frames > (1U << 16U)) {
        return core::Result<void>::failure(invalid("the reevaluation interval must be 1 to 65536 frames"));
    }
    if (table.rules.guard_range.raw() < 0 || table.rules.guard_range.raw() > coordinate_limit_raw) {
        return core::Result<void>::failure(invalid("the guard range is out of range"));
    }
    for (std::size_t index = 0; index < table.profiles.size(); ++index) {
        const auto& profile = table.profiles[index];
        const auto context = "type " + std::to_string(profile.type_id) + ": ";
        if (index != 0 && profile.type_id <= table.profiles[index - 1].type_id) {
            return core::Result<void>::failure(invalid(context + "type IDs must strictly increase"));
        }
        if (!within(profile.max_speed, min_speed_raw) || !within(profile.acceleration, min_acceleration_raw)
            || !within(profile.deceleration, min_acceleration_raw) || !within(profile.rate_of_turn, min_turn_raw)) {
            return core::Result<void>::failure(invalid(context
                + "speed must be in [1/16, 1024], rate of turn in [1/128, 1024], "
                  "acceleration and deceleration in [2^-12, 1024]"));
        }
        if (profile.turn_in_place_slowdown < whole(1) || profile.turn_in_place_slowdown.raw() > rate_limit_raw) {
            return core::Result<void>::failure(invalid(context + "the turn-in-place slowdown must be in [1, 1024]"));
        }
        if (!within(profile.roll_rate, 0) || profile.bank_angle.raw() < 0 || profile.bank_angle > whole(90)) {
            return core::Result<void>::failure(
                invalid(context + "the roll rate must be in [0, 1024] and the bank angle in [0, 90] degrees"));
        }
    }
    for (std::size_t index = 0; index < table.footprints.size(); ++index) {
        const auto& footprint = table.footprints[index];
        const auto context = "footprint " + std::to_string(footprint.type_id) + ": ";
        if (index != 0 && footprint.type_id <= table.footprints[index - 1].type_id) {
            return core::Result<void>::failure(invalid(context + "type IDs must strictly increase"));
        }
        const auto in_range = [](const Fixed value) { return value.raw() >= 0 && value.raw() <= coordinate_limit_raw; };
        if (!in_range(footprint.x_extent) || !in_range(footprint.y_extent) || !in_range(footprint.radius)) {
            return core::Result<void>::failure(invalid(context + "extents and radius must be in [0, 262144]"));
        }
        if (footprint.obstacle_offset.x.raw() < -coordinate_limit_raw || footprint.obstacle_offset.x.raw() > coordinate_limit_raw
            || footprint.obstacle_offset.y.raw() < -coordinate_limit_raw || footprint.obstacle_offset.y.raw() > coordinate_limit_raw) {
            return core::Result<void>::failure(invalid(context + "obstacle offsets must be in [-262144, 262144]"));
        }
    }
    if (const auto& rules = table.avoidance) {
        const auto positive = [](const Fixed value) { return value.raw() > 0 && value.raw() <= rate_limit_raw; };
        if (!(rules->max_rotations >= whole(1) && rules->max_rotations <= whole(360)) || !positive(rules->wait_speed)
            || rules->wait_speed > whole(1) || !positive(rules->wait_frames) || !positive(rules->wait_cost)
            || !positive(rules->min_obstacle_cost) || !positive(rules->path_cost_coefficient)
            || rules->path_cost_coefficient > whole(1) || !positive(rules->occupation_radius)
            || rules->failure_cutoff.raw() < 0 || rules->failure_cutoff.raw() > rate_limit_raw
            || !positive(rules->failure_expansions) || rules->failure_rotation.raw() < 0
            || rules->failure_rotation.raw() > rate_limit_raw || rules->failure_forward.raw() < 0
            || rules->failure_forward.raw() > rate_limit_raw || rules->max_expansions == 0
            || rules->max_expansions > (1U << 20) || rules->tries == 0 || rules->tries > 16
            || rules->tracking_interval == 0 || rules->tracking_interval > (1U << 16) || rules->tracking_windows == 0
            || rules->tracking_windows > 1024 || !positive(rules->destination_search_increment)
            || rules->search_budget == 0 || rules->search_delay == 0 || rules->search_delay > 1024
            || rules->search_slice == 0) {
            return core::Result<void>::failure(invalid("avoidance rules are out of range"));
        }
        // PC-08: a sliced search must land before the approach can be reevaluated.
        if (rules->search_delay >= table.rules.reevaluation_frames) {
            return core::Result<void>::failure(invalid("search delay must be less than the reevaluation interval"));
        }
        // FoC floors neither the soft radius nor OccupationRadiusCoefficientSpace: a one-raw-unit
        // radius would put billions of points on a destination search ring. A footprint whose outer
        // ring would exceed max_ring_points is rejected. FoC's smallest soft radius (the corvette's
        // 41.822) puts 245 points there at the 50-unit increment; a radius under about 1.25 is rejected.
        const Fixed outer = Fixed::from_raw(rules->destination_search_increment.raw() * (motion_detail::destination_search_rings - 1));
        for (const auto& footprint : table.footprints) {
            Calc calc;
            const auto points = motion_detail::ring_points(calc, outer, calc.mul(footprint.radius, rules->occupation_radius));
            if (!calc.ok() || points > motion_detail::max_ring_points) {
                return core::Result<void>::failure(invalid("footprint " + std::to_string(footprint.type_id)
                    + ": the radius is too small for the destination search"));
            }
        }
    }
    return validate_squadron_table(table.squadrons);
}

core::Result<MotionProfile> scaled_profile(const MotionProfile& profile, const Fixed speed_factor) {
    if (speed_factor == whole(1)) {
        return core::Result<MotionProfile>::success(profile);
    }
    Calc calc;
    MotionProfile scaled = profile;
    scaled.max_speed = calc.mul(profile.max_speed, speed_factor);
    scaled.acceleration = calc.mul(profile.acceleration, speed_factor);
    scaled.deceleration = calc.mul(profile.deceleration, speed_factor);
    if (!calc.ok()) {
        return core::Result<MotionProfile>::failure(calc.error("motion speed factor"));
    }
    return core::Result<MotionProfile>::success(scaled);
}

core::Result<MotionState> plan_move(const MotionProfile& profile, const MotionRules& rules, const std::uint64_t tick,
    const Vec3 position, const Fixed yaw, const Fixed speed, const Vec3 target) {
    MotionState state;
    if (!within_coordinates(position) || !within_coordinates(target) || !within(profile.max_speed, min_speed_raw)
        || !within(profile.acceleration, min_acceleration_raw) || !within(profile.deceleration, min_acceleration_raw)
        || !within(profile.rate_of_turn, min_turn_raw) || !within(speed, 0) || tick > max_ticks) {
        return core::Result<MotionState>::success(state);
    }
    Calc calc;
    const Fixed tx = target.x;
    const Fixed ty = target.y;
    const Fixed z = position.z; // a move stays in the unit's layer (MV-11)
    const Fixed dx = calc.sub(tx, position.x);
    const Fixed dy = calc.sub(ty, position.y);
    if (dx.raw() == 0 && dy.raw() == 0) {
        return core::Result<MotionState>::success(state);
    }
    const Fixed start_yaw = clamp180(yaw);
    state.kind = MotionKind::path;
    state.start_tick = tick;
    state.start_position = position;
    state.start_yaw = start_yaw;
    state.start_speed = speed;
    state.target = target;
    auto& nodes = state.nodes;
    nodes.push_back({whole(static_cast<std::int64_t>(tick)), position, start_yaw, speed});

    const Fixed vmax = profile.max_speed;
    const Fixed length = calc.length(dx, dy);

    // MV-12: a target inside the planner's start cell gets a direct two-node path.
    Fixed cell = rules.expansion_distance;
    if (length < calc.mul(whole(2), cell)) {
        cell = std::max(min_expansion_distance, calc.div(length, whole(2)));
    }
    cell = std::max(cell, vmax);
    const Fixed cell_limit = calc.mul(whole(2), cell);
    if (dx.raw() >= 0 && dy.raw() >= 0 && calc.mul(whole(3), dx) < cell_limit && calc.mul(whole(3), dy) < cell_limit) {
        nodes.push_back({calc.add(nodes.front().frame, calc.div(length, vmax)), {tx, ty, z}, calc.atan2_deg(dy, dx), Fixed{}});
        if (!calc.ok()) {
            return core::Result<MotionState>::failure(calc.error("move plan"));
        }
        return core::Result<MotionState>::success(std::move(state));
    }

    Fixed frame = nodes.front().frame;
    Fixed x = position.x;
    Fixed y = position.y;
    Fixed psi = start_yaw;
    Fixed v = speed;

    // MV-13: reach full speed on a straight line along the current yaw first.
    if (v != vmax) {
        const bool faster = v < vmax;
        const Fixed rate = faster ? profile.acceleration : profile.deceleration;
        const Fixed change = calc.abs(calc.sub(vmax, v));
        const Fixed distance = calc.div(calc.mul(change, calc.add(vmax, v)), calc.mul(whole(2), rate));
        const auto u = heading(calc, psi);
        x = calc.add(x, calc.mul(u.x, distance));
        y = calc.add(y, calc.mul(u.y, distance));
        frame = calc.add(frame, calc.div(change, rate));
        v = vmax;
        nodes.push_back({frame, {x, y, z}, psi, v});
    }

    // MV-14 to MV-17: arcs of the rule angle toward the target, then the tangent match.
    const Fixed theta = rules.arc_degrees;
    const Fixed rot = profile.rate_of_turn;
    const Fixed radius = calc.div(calc.mul(vmax, whole(360)), calc.mul(two_pi, rot));
    const Fixed arc_frames = calc.div(theta, rot);
    const Fixed chord = calc.mul(calc.mul(whole(2), radius), calc.sin_deg(calc.div(theta, whole(2))));
    for (int step = 0; step < max_arc_steps && calc.ok(); ++step) {
        const Fixed to_x = calc.sub(tx, x);
        const Fixed to_y = calc.sub(ty, y);
        if (to_x.raw() == 0 && to_y.raw() == 0) {
            break;
        }
        const Fixed diff = clamp180(calc.sub(calc.atan2_deg(to_y, to_x), psi));
        if (calc.abs(diff) < aligned_degrees) {
            break; // MV-16: dead ahead, fly straight
        }
        const std::int64_t side = diff.raw() >= 0 ? 1 : -1;
        const auto u = heading(calc, psi);
        const Fixed normal_x = side > 0 ? calc.neg(u.y) : u.y;
        const Fixed normal_y = side > 0 ? u.x : calc.neg(u.x);
        const Fixed centre_x = calc.add(x, calc.mul(normal_x, radius));
        const Fixed centre_y = calc.add(y, calc.mul(normal_y, radius));
        const Fixed off_x = calc.sub(centre_x, tx);
        const Fixed off_y = calc.sub(centre_y, ty);
        const Fixed centre_distance = calc.length(off_x, off_y);
        if (centre_distance <= radius) {
            // MV-17: the target is inside the turn circle; fly straight until it is outside.
            const Fixed reach = calc.add(radius, circle_margin);
            const Fixed along = calc.dot(off_x, off_y, u.x, u.y);
            const Fixed inside = calc.mul(calc.sub(centre_distance, reach), calc.add(centre_distance, reach));
            const Fixed leg = calc.add(calc.neg(along), calc.sqrt(calc.sub(calc.mul(along, along), inside)));
            x = calc.add(x, calc.mul(u.x, leg));
            y = calc.add(y, calc.mul(u.y, leg));
            frame = calc.add(frame, calc.div(leg, v));
            nodes.push_back({frame, {x, y, z}, psi, v});
            continue;
        }
        if (calc.abs(diff) <= theta) {
            // MV-15: turn on the circle to the tangent that points at the target.
            const Fixed ratio = calc.div(radius, centre_distance);
            const Fixed opening = calc.atan2_deg(calc.sqrt(calc.sub(whole(1), calc.mul(ratio, ratio))), ratio);
            const Fixed toward = calc.atan2_deg(calc.neg(off_y), calc.neg(off_x));
            const Fixed phi = side > 0 ? calc.sub(toward, opening) : calc.add(toward, opening);
            const Fixed touch_x = calc.add(centre_x, calc.mul(radius, calc.cos_deg(phi)));
            const Fixed touch_y = calc.add(centre_y, calc.mul(radius, calc.sin_deg(phi)));
            const Fixed head = calc.atan2_deg(calc.sub(ty, touch_y), calc.sub(tx, touch_x));
            frame = calc.add(frame, calc.div(calc.abs(clamp180(calc.sub(head, psi))), rot));
            x = touch_x;
            y = touch_y;
            psi = head;
            nodes.push_back({frame, {x, y, z}, psi, v});
            break;
        }
        // MV-14: one arc step of the rule angle toward the target.
        const Fixed half_turn = side > 0 ? calc.add(psi, calc.div(theta, whole(2))) : calc.sub(psi, calc.div(theta, whole(2)));
        x = calc.add(x, calc.mul(chord, calc.cos_deg(half_turn)));
        y = calc.add(y, calc.mul(chord, calc.sin_deg(half_turn)));
        psi = clamp180(side > 0 ? calc.add(psi, theta) : calc.sub(psi, theta));
        frame = calc.add(frame, arc_frames);
        nodes.push_back({frame, {x, y, z}, psi, v});
    }
    // MV-16: the final straight leg to the target.
    const Fixed leg = calc.length(calc.sub(tx, x), calc.sub(ty, y));
    frame = calc.add(frame, calc.div(leg, v));
    nodes.push_back({frame, {tx, ty, z}, psi, v});
    motion_detail::finish_path(calc, profile, rules.arc_degrees, nodes);
    if (!calc.ok()) {
        return core::Result<MotionState>::failure(calc.error("move plan"));
    }
    return core::Result<MotionState>::success(std::move(state));
}

core::Result<MotionState> plan_face(const MotionProfile& profile, const std::uint64_t tick, const Vec3 position,
    const Fixed yaw, const Vec3 target) {
    MotionState state;
    if (!within_coordinates(position) || !within_coordinates(target) || !within(profile.rate_of_turn, min_turn_raw)
        || tick > max_ticks) {
        return core::Result<MotionState>::success(state);
    }
    Calc calc;
    const Fixed dx = calc.sub(target.x, position.x);
    const Fixed dy = calc.sub(target.y, position.y);
    if (dx.raw() == 0 && dy.raw() == 0) {
        return core::Result<MotionState>::success(state);
    }
    const Fixed start_yaw = clamp180(yaw);
    const Fixed head = clamp180(calc.atan2_deg(dy, dx));
    const Fixed turn = calc.abs(clamp180(calc.sub(head, start_yaw)));
    const Fixed start = whole(static_cast<std::int64_t>(tick));
    // MV-20: turning in place takes the turn over the rate of turn, times the layer's slowdown.
    const Fixed duration = calc.mul(calc.div(turn, profile.rate_of_turn), profile.turn_in_place_slowdown);
    if (!calc.ok()) {
        return core::Result<MotionState>::failure(calc.error("turn plan"));
    }
    state.kind = MotionKind::turn;
    state.start_tick = tick;
    state.start_position = position;
    state.start_yaw = start_yaw;
    state.target = target;
    state.nodes.push_back({start, position, start_yaw, Fixed{}});
    state.nodes.push_back({calc.add(start, duration), position, head, Fixed{}});
    if (!calc.ok()) {
        return core::Result<MotionState>::failure(calc.error("turn plan"));
    }
    return core::Result<MotionState>::success(std::move(state));
}

core::Result<MotionSample> sample_motion(
    const MotionState& state, const std::uint64_t tick, const Vec3 position, const Fixed yaw) {
    MotionSample sample{position, yaw, Fixed{}, true};
    if (state.kind == MotionKind::none || state.nodes.size() < 2) {
        return core::Result<MotionSample>::success(sample);
    }
    Calc calc;
    const Fixed now = whole(static_cast<std::int64_t>(tick));
    const auto& nodes = state.nodes;
    if (state.kind == MotionKind::turn) {
        const auto& from = nodes.front();
        const auto& to = nodes.back();
        if (now >= to.frame) {
            sample.yaw = to.yaw; // MV-21: the turn ends exactly on the target yaw
            return core::Result<MotionSample>::success(sample);
        }
        const Fixed span = calc.sub(to.frame, from.frame);
        const Fixed t = std::max(Fixed{}, calc.div(calc.sub(now, from.frame), span));
        sample.yaw = clamp180(calc.add(from.yaw, calc.mul(clamp180(calc.sub(to.yaw, from.yaw)), t)));
        sample.finished = false;
        if (!calc.ok()) {
            return core::Result<MotionSample>::failure(calc.error("turn sample"));
        }
        return core::Result<MotionSample>::success(sample);
    }
    std::size_t index = 0;
    while (index + 1 < nodes.size() && nodes[index + 1].frame <= now) {
        ++index;
    }
    if (index + 1 >= nodes.size()) {
        return core::Result<MotionSample>::success(sample); // MV-32: no snap to the last node
    }
    const auto& a = nodes[index];
    const auto& b = nodes[index + 1];
    const Fixed span = calc.sub(b.frame, a.frame);
    const Fixed t = std::max(Fixed{}, calc.div(calc.sub(now, a.frame), span));
    sample.finished = false;
    if (a.position.x == b.position.x && a.position.y == b.position.y) {
        sample.position = {a.position.x, a.position.y, position.z};
        sample.yaw = clamp180(calc.add(a.yaw, calc.mul(clamp180(calc.sub(b.yaw, a.yaw)), t)));
    } else {
        // MV-30: cubic Hermite between the nodes, tangents yaw * speed * frame span.
        const auto ha = heading(calc, a.yaw);
        const auto hb = heading(calc, b.yaw);
        const Fixed ma = calc.mul(a.speed, span);
        const Fixed mb = calc.mul(b.speed, span);
        const Fixed span_x = calc.sub(b.position.x, a.position.x);
        const Fixed span_y = calc.sub(b.position.y, a.position.y);
        const std::int64_t elapsed = std::max<std::int64_t>(0, calc.sub(now, a.frame).raw());
        std::int64_t offset_x{};
        std::int64_t offset_y{};
        std::int64_t rate_x{};
        std::int64_t rate_y{};
        if (!hermite(elapsed, span.raw(), span_x.raw(), calc.mul(ha.x, ma).raw(), calc.mul(hb.x, mb).raw(), offset_x, rate_x)
            || !hermite(elapsed, span.raw(), span_y.raw(), calc.mul(ha.y, ma).raw(), calc.mul(hb.y, mb).raw(), offset_y,
                rate_y)) {
            return core::Result<MotionSample>::failure(calc.error("path sample exceeds the exact arithmetic range"));
        }
        sample.position.x = calc.add(a.position.x, Fixed::from_raw(offset_x));
        sample.position.y = calc.add(a.position.y, Fixed::from_raw(offset_y));
        sample.position.z = position.z;
        const Fixed vx = Fixed::from_raw(rate_x);
        const Fixed vy = Fixed::from_raw(rate_y);
        sample.speed = calc.length(vx, vy);
        if (sample.speed > still_speed) {
            sample.yaw = calc.atan2_deg(vy, vx); // MV-31
        }
    }
    if (!calc.ok()) {
        return core::Result<MotionSample>::failure(calc.error("path sample"));
    }
    return core::Result<MotionSample>::success(sample);
}

core::Result<Fixed> bank_roll(const MotionProfile& profile, const Fixed roll, const Fixed yaw_before, const Fixed yaw_after) {
    if (profile.bank_angle.raw() == 0) {
        return core::Result<Fixed>::success(Fixed{});
    }
    Calc calc;
    // BK-02: the share of the rate of turn this frame used sets the bank; half the rate or more
    // banks fully. A left turn (yaw growing) lowers the left side: the roll goes negative.
    const Fixed turn = clamp180(calc.sub(yaw_after, yaw_before));
    const Fixed share = std::min(whole(1), calc.div(calc.abs(turn), profile.rate_of_turn));
    const Fixed depth = share < Fixed::from_raw(Fixed::scale / 2) ? calc.mul(whole(2), share) : whole(1);
    Fixed target = calc.mul(profile.bank_angle, depth);
    if (turn.raw() >= 0) {
        target = calc.neg(target);
    }
    // BK-03: the roll eases in, at the roll rate times the remaining gap over the bank angle,
    // never below a tenth of the rate, and stops on the target.
    const Fixed gap = calc.abs(calc.sub(target, roll));
    const Fixed step = calc.mul(profile.roll_rate, std::clamp(calc.div(gap, profile.bank_angle), min_roll_share, whole(1)));
    Fixed result = roll;
    if (roll < target) {
        result = std::min(calc.add(roll, step), target);
    } else if (target < roll) {
        result = std::max(calc.sub(roll, step), target);
    }
    if (!calc.ok()) {
        return core::Result<Fixed>::failure(calc.error("bank roll"));
    }
    return core::Result<Fixed>::success(result);
}

core::Result<Fixed> level_roll(const MotionProfile& profile, const Fixed roll) {
    Calc calc;
    Fixed result = roll;
    if (roll.raw() > 0) {
        result = std::max(calc.sub(roll, profile.roll_rate), Fixed{});
    } else if (roll.raw() < 0) {
        result = std::min(calc.add(roll, profile.roll_rate), Fixed{});
    }
    if (!calc.ok()) {
        return core::Result<Fixed>::failure(calc.error("level roll"));
    }
    return core::Result<Fixed>::success(result);
}

core::Result<math::Quat> banked_rotation(const math::Quat heading, const Fixed roll_degrees) {
    if (roll_degrees.raw() == 0) {
        return core::Result<math::Quat>::success(heading);
    }
    Calc calc;
    const Fixed half = calc.div(clamp180(roll_degrees), whole(720));
    if (!calc.ok()) {
        return core::Result<math::Quat>::failure(calc.error("bank rotation"));
    }
    const auto roll = math::normalize(math::Quat{math::sin_turn(half), Fixed{}, Fixed{}, math::cos_turn(half)});
    if (!roll) {
        return core::Result<math::Quat>::failure(roll.error());
    }
    // BK-05: roll about the unit's own forward axis, after its heading (FoC's Rz * Rx).
    const auto rolled = math::compose(heading, roll.value());
    if (!rolled) {
        return core::Result<math::Quat>::failure(rolled.error());
    }
    return math::normalize(rolled.value());
}

core::Result<Fixed> yaw_degrees(const math::Quat rotation) {
    const auto matrix = math::to_matrix(rotation, Vec3{});
    if (!matrix) {
        return core::Result<Fixed>::failure(matrix.error());
    }
    const auto turns = math::atan2_turn(matrix.value().rows[1][0], matrix.value().rows[0][0]);
    if (!turns) {
        return core::Result<Fixed>::failure(turns.error());
    }
    return core::Result<Fixed>::success(clamp180(Fixed::from_raw(turns.value().raw() * 360)));
}

core::Result<math::Quat> yaw_rotation(const Fixed yaw) {
    Calc calc;
    const Fixed half = calc.div(clamp180(yaw), whole(720));
    if (!calc.ok()) {
        return core::Result<math::Quat>::failure(calc.error("yaw rotation"));
    }
    return math::normalize(math::Quat{Fixed{}, Fixed{}, math::sin_turn(half), math::cos_turn(half)});
}

std::string_view to_string(const MotionKind kind) noexcept {
    switch (kind) {
    case MotionKind::none:
        return "none";
    case MotionKind::path:
        return "path";
    case MotionKind::turn:
        return "turn";
    }
    return "unknown";
}

} // namespace eawr::sim::tactical
