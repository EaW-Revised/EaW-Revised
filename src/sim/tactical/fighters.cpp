#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace eawr::sim::tactical {

namespace {

using math::Fixed;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::clamp180;
using motion_detail::two_pi;
using motion_detail::whole;

// Slot keys of the spawner's keyed draws (FL-01, FL-03, FL-06).
constexpr std::uint32_t service_slot = 0xfffe0001U;
constexpr std::uint32_t entry_slot = 0xfffe0002U;
constexpr std::uint32_t bay_slot = 0xfffe0003U;

constexpr std::uint64_t service_interval = 30; // SPAWN_SQUADRON's service interval base (FL-01)
constexpr std::size_t max_members = 64;
constexpr std::size_t max_entries = 64;
constexpr std::size_t max_bays = 255;

template <typename T>
[[nodiscard]] const T* find_by_type(const std::vector<T>& rows, const TypeId type_id) noexcept {
    const auto found = std::lower_bound(rows.begin(), rows.end(), type_id,
        [](const T& row, const TypeId id) { return row.type_id < id; });
    return found != rows.end() && found->type_id == type_id ? &*found : nullptr;
}

template <typename T>
[[nodiscard]] bool increasing(const std::vector<T>& rows) noexcept {
    for (std::size_t index = 1; index < rows.size(); ++index) {
        if (rows[index].type_id <= rows[index - 1].type_id) return false;
    }
    return true;
}

[[nodiscard]] bool rate(const Fixed value, const bool positive) noexcept {
    return (positive ? value.raw() > 0 : value.raw() >= 0) && value.raw() <= max_motion_rate * Fixed::scale;
}

[[nodiscard]] bool distance(const Fixed value) noexcept {
    return value.raw() >= 0 && value.raw() <= max_motion_coordinate * Fixed::scale;
}

[[nodiscard]] bool point(const Vec3& value) noexcept {
    const auto inside = [](const Fixed component) {
        return component.raw() >= -max_motion_coordinate * Fixed::scale
            && component.raw() <= max_motion_coordinate * Fixed::scale;
    };
    return inside(value.x) && inside(value.y) && inside(value.z);
}

[[nodiscard]] core::Result<void> invalid(const std::string& message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "squadron table: " + message));
}

[[nodiscard]] Fixed sign(const Fixed value) noexcept {
    return value.raw() > 0 ? whole(1) : value.raw() < 0 ? whole(-1) : Fixed{};
}

// Compare distances in raw Q24 units. The accepted coordinates can make a Q24 square overflow,
// while three squared raw differences fit in the same wide accumulator used by combat queries.
[[nodiscard]] math::detail::UInt192 squared_components(const Fixed x, const Fixed y, const Fixed z = Fixed{}) {
    math::detail::UInt192 total{};
    const auto add_square = [&](const Fixed component) {
        const auto raw = component.raw();
        const auto magnitude = raw < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(raw)
                                       : static_cast<std::uint64_t>(raw);
        static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(magnitude, magnitude)));
    };
    add_square(x);
    add_square(y);
    add_square(z);
    return total;
}

// FD-10 (#615): the share of a look-ahead whose square rounds to zero in Q24 though it is not
// zero (a craft all but stopped): -offset.look over look.look from the exact raw products,
// clamped to [0, 1]. FoC's float division has a tiny positive denominator there, so it gives
// this clamped ratio (nearly always 0 or 1), never a trap.
[[nodiscard]] Fixed short_look_share(const Vec3& offset, const Vec3& look, const math::detail::UInt192& look_squared) {
    auto along = math::detail::SignedWide::product(offset.x.raw(), look.x.raw());
    along.add(math::detail::SignedWide::product(offset.y.raw(), look.y.raw()));
    along.add(math::detail::SignedWide::product(offset.z.raw(), look.z.raw()));
    if (along.magnitude.is_zero() || !along.negative) return Fixed{};
    if (math::detail::compare(along.magnitude, look_squared) >= 0) return whole(1);
    // Below the look-ahead's square, under 3 * 2^23 raw^2 when its Q24 square rounds to zero.
    std::int64_t raw{};
    const auto scaled = math::detail::SignedWide::scaled(static_cast<std::int64_t>(along.magnitude.limb[0]), Fixed::fractional_bits);
    return math::detail::rounded_divide_to_raw(scaled, look_squared, raw) ? Fixed::from_raw(raw) : whole(1);
}

// The yaw and pitch that point a craft's nose at `offset` (FM-02): the offset in the craft's
// frame, unyawed then unpitched.
void local_angles(Calc& calc, const CraftState& state, const Vec3& offset, Fixed& yaw_delta, Fixed& pitch_delta) {
    const Fixed cy = calc.cos_deg(state.yaw);
    const Fixed sy = calc.sin_deg(state.yaw);
    const Fixed x1 = calc.add(calc.mul(offset.x, cy), calc.mul(offset.y, sy));
    const Fixed y1 = calc.sub(calc.mul(offset.y, cy), calc.mul(offset.x, sy));
    const Fixed cp = calc.cos_deg(state.pitch);
    const Fixed sp = calc.sin_deg(state.pitch);
    const Fixed lx = calc.sub(calc.mul(x1, cp), calc.mul(offset.z, sp));
    const Fixed lz = calc.add(calc.mul(x1, sp), calc.mul(offset.z, cp));
    yaw_delta = lx.raw() == 0 && y1.raw() == 0 ? Fixed{} : clamp180(calc.atan2_deg(y1, lx));
    const Fixed across = calc.length(lx, y1);
    pitch_delta = across.raw() == 0 && lz.raw() == 0 ? Fixed{} : clamp180(calc.neg(calc.atan2_deg(lz, across)));
}

// FD-05: `target` within `range` of `position` and 90 degrees of yaw and of pitch of the nose.
bool followable(Calc& calc, const Vec3& position, const CraftState& state, const Vec3& target, const Fixed range,
    Fixed& yaw, Fixed& pitch) {
    const auto squared = squared_components(calc.sub(target.x, position.x), calc.sub(target.y, position.y),
        calc.sub(target.z, position.z));
    if (math::detail::compare(squared_components(range, Fixed{}), squared) < 0) return false;
    local_angles(calc, state, {calc.sub(target.x, position.x), calc.sub(target.y, position.y),
        calc.sub(target.z, position.z)}, yaw, pitch);
    return calc.abs(yaw) <= whole(90) && calc.abs(pitch) <= whole(90);
}

// FD-10: frames a craft flies straight on after steering away from a ship.
constexpr std::uint32_t avoidance_relaxation = 15;

// --- The locomotor of one craft for one frame (FM, FA rules) ------------------------------------

class Locomotor final {
public:
    explicit Locomotor(const CraftFrame& frame)
        : frame_(frame), self_(*frame.self), profile_(*frame.self->profile), state_(frame.self->state),
          position_(frame.self->position) {
        // AB-24: an active ability's speed multiplier scales the craft's maximum speed only.
        const Calc::Site site(calc_, "AB-24 speed factor");
        if (frame.speed_factor.raw() != Fixed::scale) {
            profile_.max_speed = calc_.mul(profile_.max_speed, frame.speed_factor);
            // #615: validation keeps the speed and the factor positive, so a product that rounds to
            // zero in Q24 is a tiny positive float in FoC. Keep the least positive speed: FM-11's
            // budget ratio over it then stays finite and huge, as FoC's float one is.
            if (profile_.max_speed.raw() == 0) profile_.max_speed = Fixed::from_raw(1);
        }
    }

    core::Result<CraftStep> run() {
        handle_flip();
        // DG-26: idle, and the approach in formation (FA-07), are out of combat.
        bool out_of_combat = true;
        if (frame_.attacking) {
            out_of_combat = directed_combat();
        } else if (frame_.moving) {
            move(); // DG-26: a squadron on a move order has no target, so it stays out of combat.
        } else {
            idle();
        }
        {
            const Calc::Site site(calc_, "FM-01 position");
            position_ = {calc_.add(position_.x, state_.velocity.x), calc_.add(position_.y, state_.velocity.y),
                calc_.add(position_.z, state_.velocity.z)};
        }
        if (!calc_.ok()) {
            return core::Result<CraftStep>::failure(calc_.error("craft " + std::to_string(self_.id)));
        }
        auto rotation = craft_rotation(state_);
        if (!rotation) {
            return core::Result<CraftStep>::failure(rotation.error());
        }
        return core::Result<CraftStep>::success(CraftStep{position_, state_, rotation.value(),
            out_of_combat ? profile_.out_of_combat_defense : Fixed{}, frame_.attacking && out_of_combat});
    }

private:
    struct Budget {
        Fixed turn;
        Fixed thrust;
        Fixed lift;
    };

    [[nodiscard]] bool leading() const noexcept { return frame_.leader == nullptr || frame_.leader == frame_.self; }
    [[nodiscard]] const CraftView& leader() const noexcept { return frame_.leader != nullptr ? *frame_.leader : self_; }

    [[nodiscard]] Budget budget() const noexcept { return {profile_.rate_of_turn, profile_.thrust, profile_.lift}; }

    [[nodiscard]] Fixed length(const Vec3& value) { return calc_.length(value); }
    [[nodiscard]] Fixed length_xy(const Fixed x, const Fixed y) { return calc_.length(x, y); }
    [[nodiscard]] Vec3 sub(const Vec3& left, const Vec3& right) {
        return {calc_.sub(left.x, right.x), calc_.sub(left.y, right.y), calc_.sub(left.z, right.z)};
    }
    [[nodiscard]] Vec3 add(const Vec3& left, const Vec3& right) {
        return {calc_.add(left.x, right.x), calc_.add(left.y, right.y), calc_.add(left.z, right.z)};
    }
    [[nodiscard]] Vec3 scale(const Vec3& value, const Fixed factor) {
        return {calc_.mul(value.x, factor), calc_.mul(value.y, factor), calc_.mul(value.z, factor)};
    }
    // The vector over its length, or zero for the zero vector (FoC's Normalize).
    [[nodiscard]] Vec3 unit(const Vec3& value, Fixed& length_out) {
        length_out = length(value);
        if (length_out.raw() == 0) return Vec3{};
        return {calc_.div(value.x, length_out), calc_.div(value.y, length_out), calc_.div(value.z, length_out)};
    }
    // FM-14: the craft's slot less the leader's, turned by the leader's whole facing (the roll
    // about the nose, then the pitch, then the yaw: the frame craft_rotation draws), so the
    // formation banks, climbs and loops with its leader. Built once per frame.
    [[nodiscard]] const Vec3& slot() {
        if (slot_) return *slot_;
        const Calc::Site site(calc_, "FM-14 formation slot");
        const CraftState& facing = leader().state;
        const Vec3 offset = sub(frame_.own_offset, frame_.leader_offset);
        const Fixed cr = calc_.cos_deg(facing.roll);
        const Fixed sr = calc_.sin_deg(facing.roll);
        const Fixed cp = calc_.cos_deg(facing.pitch);
        const Fixed sp = calc_.sin_deg(facing.pitch);
        const Fixed cy = calc_.cos_deg(facing.yaw);
        const Fixed sy = calc_.sin_deg(facing.yaw);
        const Fixed rolled_y = calc_.sub(calc_.mul(offset.y, cr), calc_.mul(offset.z, sr));
        const Fixed rolled_z = calc_.add(calc_.mul(offset.y, sr), calc_.mul(offset.z, cr));
        const Fixed pitched_x = calc_.add(calc_.mul(offset.x, cp), calc_.mul(rolled_z, sp));
        const Fixed pitched_z = calc_.sub(calc_.mul(rolled_z, cp), calc_.mul(offset.x, sp));
        slot_ = Vec3{calc_.sub(calc_.mul(pitched_x, cy), calc_.mul(rolled_y, sy)),
            calc_.add(calc_.mul(pitched_x, sy), calc_.mul(rolled_y, cy)), pitched_z};
        return *slot_;
    }

    // FM-06: a craft pitched through the vertical rolls over and turns about.
    void handle_flip() {
        const Calc::Site site(calc_, "FM-06 flip");
        const Fixed pitch = clamp180(state_.pitch);
        state_.pitch = pitch;
        const Fixed magnitude = calc_.abs(pitch);
        if (magnitude < whole(90)) return;
        state_.roll = clamp180(calc_.add(state_.roll, whole(180)));
        state_.pitch = clamp180(calc_.mul(sign(pitch), calc_.sub(calc_.sub(whole(180), magnitude), profile_.lift)));
        state_.yaw = clamp180(calc_.add(state_.yaw, whole(180)));
    }

    // FM-03: yaw toward the turn while rolling into it; no yaw until the roll leans the right way.
    void adjust_turn(const Fixed angle, Fixed& turn) {
        const Calc::Site site(calc_, "FM-03 turn");
        const Fixed wanted = clamp180(angle);
        const Fixed target_roll = calc_.neg(std::clamp(wanted, calc_.neg(profile_.bank_angle), profile_.bank_angle));
        Fixed roll = clamp180(state_.roll);
        const Fixed gap = clamp180(calc_.sub(target_roll, roll));
        const Fixed roll_budget = std::clamp(calc_.mul(calc_.div(turn, profile_.rate_of_turn), profile_.roll_rate),
            Fixed{}, whole(180));
        if (roll_budget < calc_.abs(gap)) {
            roll = gap.raw() > 0 ? calc_.add(roll, roll_budget) : calc_.sub(roll, roll_budget);
        } else {
            roll = calc_.add(roll, gap);
        }
        if (roll.raw() != 0 && sign(target_roll) != sign(roll)) {
            turn = Fixed{};
        } else if (turn < calc_.abs(wanted)) {
            state_.yaw = wanted.raw() > 0 ? calc_.add(state_.yaw, turn) : calc_.sub(state_.yaw, turn);
            turn = Fixed{};
        } else {
            state_.yaw = calc_.add(state_.yaw, wanted);
            turn = calc_.sub(turn, calc_.abs(wanted));
        }
        state_.roll = clamp180(roll);
        state_.yaw = clamp180(state_.yaw);
    }

    // FM-04: pitch by at most the lift.
    void adjust_pitch(const Fixed angle, Fixed& lift) {
        const Calc::Site site(calc_, "FM-04 pitch");
        Fixed pitch = clamp180(state_.pitch);
        if (lift < calc_.abs(angle)) {
            pitch = angle.raw() > 0 ? calc_.add(pitch, lift) : calc_.sub(pitch, lift);
            lift = Fixed{};
        } else {
            pitch = calc_.add(pitch, angle);
            lift = calc_.sub(lift, calc_.abs(angle));
        }
        state_.pitch = clamp180(pitch);
    }

    // FM-05: the speed moves toward `target` by at most the thrust; the velocity follows the facing.
    void adjust_speed(const Fixed target, Fixed& thrust) {
        const Calc::Site site(calc_, "FM-05 speed");
        Fixed speed = length(state_.velocity);
        const Fixed gap = calc_.sub(target, speed);
        if (thrust <= calc_.abs(gap)) {
            speed = gap.raw() >= 0 ? calc_.add(speed, thrust) : calc_.sub(speed, thrust);
            thrust = Fixed{};
        } else {
            speed = calc_.add(speed, gap);
            thrust = calc_.sub(thrust, calc_.abs(gap));
        }
        const Fixed level = calc_.cos_deg(state_.pitch);
        state_.velocity = {calc_.mul(speed, calc_.mul(calc_.cos_deg(state_.yaw), level)),
            calc_.mul(speed, calc_.mul(calc_.sin_deg(state_.yaw), level)),
            calc_.mul(speed, calc_.sin_deg(calc_.neg(state_.pitch)))};
    }

    void local_angles(const Vec3& offset, Fixed& yaw_delta, Fixed& pitch_delta) {
        tactical::local_angles(calc_, state_, offset, yaw_delta, pitch_delta);
    }

    // FM-02, FM-06: head for `target` at `speed` within the budget.
    void head_for(const Vec3& target, const Fixed speed, Budget budget, const bool idle_state) {
        const Calc::Site site(calc_, "FM-02 head for");
        if (budget.thrust.raw() <= 0 || budget.turn.raw() <= 0) return;
        Fixed yaw_delta;
        Fixed pitch_delta;
        local_angles(sub(target, position_), yaw_delta, pitch_delta);
        if (calc_.abs(yaw_delta) < whole(90)) state_.flipping = false;
        bool flip = state_.flipping;
        if (!flip && !idle_state && calc_.abs(yaw_delta) > whole(170) && profile_.lift.raw() > 0) flip = true;
        if (flip) {
            pitch_delta = pitch_delta.raw() <= 0 ? whole(-180) : whole(180);
            yaw_delta = Fixed{};
            state_.flipping = true;
        }
        adjust_turn(yaw_delta, budget.turn);
        adjust_pitch(pitch_delta, budget.lift);
        adjust_speed(speed, budget.thrust);
    }

    // FM-12: the speed that closes a follower's formation error.
    [[nodiscard]] Fixed target_speed(const Fixed speed, const Fixed low, const Fixed high) {
        const Calc::Site site(calc_, "FM-12 formation speed");
        if (leading() || frame_.leader == nullptr) return speed;
        const Fixed slow = std::min(low, high);
        const Fixed fast = std::max(slow, high);
        Fixed leader_speed;
        const Vec3 direction = unit(leader().state.velocity, leader_speed);
        const Vec3 error = sub(sub(position_, leader().position), slot());
        const Fixed along = calc_.add(calc_.add(calc_.mul(direction.x, error.x), calc_.mul(direction.y, error.y)),
            calc_.mul(direction.z, error.z));
        const Fixed own = length(state_.velocity);
        const Fixed high_speed = std::max(fast, own);
        const Fixed low_speed = std::min(slow, own);
        const Fixed twice_thrust = calc_.mul(whole(2), profile_.thrust);
        const Fixed braking = calc_.div(calc_.sub(calc_.mul(high_speed, high_speed), calc_.mul(speed, speed)), twice_thrust);
        const Fixed accelerating = calc_.div(calc_.sub(calc_.mul(speed, speed), calc_.mul(low_speed, low_speed)), twice_thrust);
        const Fixed tolerance = frame_.formation_tolerance;
        if (calc_.abs(along) <= tolerance) return speed;
        if (along > tolerance) {
            if (accelerating.raw() <= 0) return slow;
            const Fixed share = std::clamp(calc_.div(along, accelerating), Fixed{}, whole(1));
            return calc_.add(calc_.mul(share, slow), calc_.mul(calc_.sub(whole(1), share), leader_speed));
        }
        if (braking.raw() <= 0) return fast;
        // Behind its slot FoC bounds the share below only, so a far-behind follower extrapolates past `fast`.
        const Fixed share = std::max(calc_.div(calc_.neg(along), braking), Fixed{});
        return calc_.add(calc_.mul(share, fast), calc_.mul(calc_.sub(whole(1), share), leader_speed));
    }

    // FM-11: fly to the craft's slot a turn radius ahead of the leader, easing toward `height`. On
    // the approach (FA-07) and a group move (FO-10) the leader looks along `path` instead of its
    // velocity, and on a group move aims `shift` units to the left of that point.
    void form_up(const Fixed speed, const Fixed height, const std::optional<Vec3>& path = std::nullopt,
        const Fixed shift = Fixed{}) {
        const Calc::Site site(calc_, "FM-11 form up");
        const Fixed wanted = target_speed(speed, calc_.mul(speed, Fixed::from_raw(Fixed::scale / 2)),
            calc_.mul(speed, Fixed::from_raw(Fixed::scale * 3 / 2)));
        Budget limits = budget();
        if (profile_.max_speed < wanted) {
            const Fixed factor = calc_.div(wanted, profile_.max_speed);
            limits = {calc_.mul(limits.turn, factor), calc_.mul(limits.thrust, factor), calc_.mul(limits.lift, factor)};
        }
        Vec3 direction = leading() && path ? *path : leader().state.velocity;
        if (direction.x.raw() == 0 && direction.y.raw() == 0 && direction.z.raw() == 0) {
            direction = {calc_.cos_deg(leader().state.yaw), calc_.sin_deg(leader().state.yaw), Fixed{}};
        }
        Fixed ignored;
        direction = unit(direction, ignored);
        const Fixed own = std::max(wanted, length(state_.velocity));
        const Fixed radius = calc_.div(calc_.mul(calc_.div(whole(360), profile_.rate_of_turn), own), two_pi);
        Vec3 waypoint = add(leader().position, scale(direction, radius));
        if (leading() && path && shift.raw() != 0) {
            // FO-10: the left of the path's direction, in the plane.
            waypoint = add(waypoint, Vec3{calc_.neg(calc_.mul(direction.y, shift)), calc_.mul(direction.x, shift), Fixed{}});
        }
        Fixed z = height;
        if (profile_.lift.raw() > 0) {
            const Fixed ease = calc_.mul(calc_.div(calc_.abs(clamp180(state_.pitch)), profile_.lift), own);
            if (ease.raw() > 0) {
                const Fixed across = length_xy(calc_.sub(waypoint.x, position_.x), calc_.sub(waypoint.y, position_.y));
                if (across < ease) {
                    z = calc_.add(position_.z, calc_.div(calc_.mul(calc_.sub(height, position_.z), across), ease));
                }
            }
        }
        waypoint.z = z;
        if (!leading()) waypoint = add(waypoint, slot());
        if (!avoid(waypoint, limits, false)) head_for(waypoint, wanted, limits, false); // FD-10
    }

    // FD-10: steer away from a ship whose box the craft is inside when its look-ahead segment
    // hits the ship; for a while after, fly straight on (the aim becomes ten frames ahead).
    // True when the craft steered away this frame.
    bool avoid(Vec3& aim, const Budget& limits, const bool idle_state) {
        const Calc::Site site(calc_, "FD-10 avoidance");
        if (!frame_.obstacles.empty()) {
            const Fixed speed = length(state_.velocity);
            // Two over the turn rate in radians, doubled for the leader.
            Fixed frames = calc_.div(whole(720), calc_.mul(two_pi, profile_.rate_of_turn));
            if (leading()) frames = calc_.mul(frames, whole(2));
            const Vec3 look = scale(state_.velocity, frames);
            const auto look_squared = squared_components(look.x, look.y, look.z);
            for (const auto& obstacle : frame_.obstacles) {
                const Fixed reach = calc_.add(obstacle.radius, speed);
                const auto inside_box = [&](const Fixed own, const Fixed other) {
                    return math::detail::unsigned_magnitude(own.raw() - other.raw())
                        <= static_cast<std::uint64_t>(reach.raw());
                };
                if (!inside_box(position_.x, obstacle.position.x) || !inside_box(position_.y, obstacle.position.y)
                    || !inside_box(position_.z, obstacle.position.z)) {
                    continue;
                }
                const Vec3 offset = sub(position_, obstacle.position);
                const Fixed radius_squared = calc_.mul(obstacle.radius, obstacle.radius);
                const auto dot = [&](const Vec3& a, const Vec3& b) {
                    return calc_.add(calc_.add(calc_.mul(a.x, b.x), calc_.mul(a.y, b.y)), calc_.mul(a.z, b.z));
                };
                if (dot(offset, offset) < radius_squared) continue; // it starts inside the ship
                Fixed share{};
                if (math::detail::compare(look_squared, math::detail::from_u64(0)) > 0) {
                    const Calc::Site share_site(calc_, "FD-10 look-ahead share");
                    const Fixed look_dot = dot(look, look);
                    share = look_dot.raw() != 0
                        ? std::clamp(calc_.div(calc_.neg(dot(offset, look)), look_dot), Fixed{}, whole(1))
                        : short_look_share(offset, look, look_squared);
                }
                const Vec3 nearest = add(offset, scale(look, share));
                if (dot(nearest, nearest) > radius_squared) continue;
                Vec3 away = offset;
                if (away.x.raw() == 0 && away.y.raw() == 0 && away.z.raw() == 0) away = {whole(1), Fixed{}, Fixed{}};
                head_for(add(position_, away), profile_.min_speed, limits, idle_state);
                state_.relaxation = avoidance_relaxation;
                return true;
            }
        }
        if (state_.relaxation > 0) {
            --state_.relaxation;
            aim = add(position_, scale(state_.velocity, whole(10)));
        }
        return false;
    }

    // FD-05: follow the chased craft: turn and pitch toward it and match its speed, slowing to
    // the minimum inside the follow distance.
    void follow(const CraftView& chased, const Fixed yaw, const Fixed pitch) {
        const Calc::Site site(calc_, "FD-05 follow");
        Budget limits = budget();
        if (limits.thrust.raw() == 0 || limits.turn.raw() == 0) return;
        adjust_turn(yaw, limits.turn);
        adjust_pitch(pitch, limits.lift);
        const Vec3 gap = sub(chased.position, position_);
        const bool far = math::detail::compare(squared_components(gap.x, gap.y, gap.z),
                             squared_components(profile_.follow_distance, Fixed{})) >= 0;
        adjust_speed(far ? length(chased.state.velocity) : profile_.min_speed, limits.thrust);
    }

    // FD-02 to FD-07: a dogfight against a squadron.
    void dogfight() {
        const Calc::Site site(calc_, "FD-02 dogfight");
        if (frame_.dogfight == DogfightFlight::find_cell) {
            // FD-02: the leader heads for the target squadron, the others form up at its height.
            if (leading()) {
                head_for(frame_.target_position, profile_.max_speed, budget(), false);
            } else {
                form_up(profile_.max_speed, frame_.target_position.z);
            }
            return;
        }
        if (within_combat_cell_reach(position_, frame_.cell) && frame_.chase != nullptr) {
            Fixed yaw;
            Fixed pitch;
            if (followable(calc_, position_, state_, frame_.chase->position, profile_.attack_distance, yaw, pitch)) {
                follow(*frame_.chase, yaw, pitch);
                return;
            }
            Vec3 aim = frame_.chase->position;
            if (!avoid(aim, budget(), false)) head_for(aim, profile_.max_speed, budget(), false);
            return;
        }
        // FD-03, FD-07: the leader heads for the cell point, the others form up at the layer.
        if (leading()) {
            head_for(combat_cell_point(frame_.cell, profile_.layer_z), profile_.max_speed, budget(), false);
        } else {
            form_up(profile_.max_speed, profile_.layer_z);
        }
    }

    // FM-20, FM-22: hold a point; the leader circles it, the others trail the leader.
    void idle() {
        const Calc::Site site(calc_, "FM-20 idle");
        const Vec3 hold = frame_.hold;
        Fixed speed = profile_.min_speed;
        Vec3 away = sub(position_, hold);
        away.z = Fixed{};
        if (away.x.raw() == 0 && away.y.raw() == 0) away = {whole(1), Fixed{}, Fixed{}};
        Fixed own_distance;
        away = unit(away, own_distance);
        const Fixed leader_distance = std::max(whole(1),
            length_xy(calc_.sub(leader().position.x, hold.x), calc_.sub(leader().position.y, hold.y)));
        Vec3 target = hold;
        if (leader_distance < whole(90)) {
            speed = calc_.mul(calc_.div(own_distance, leader_distance), Fixed::from_raw(Fixed::scale / 2));
            if (leading()) {
                // Ten over the distance in radians, as a turn. FM-20 (#664, debug build): over the
                // distance itself, which is never zero (a zero offset became a whole unit above, and
                // the exact length of any other offset is one raw unit at least). Clamping it to one
                // unit turned the aim of a leader within a unit of the point by the same 10 radians
                // every frame, across the point, and it came to rest there.
                const Fixed turn = calc_.div(calc_.div(whole(10), own_distance), two_pi);
                const Fixed c = math::cos_turn(turn);
                const Fixed s = math::sin_turn(turn);
                const Vec3 rotated{calc_.sub(calc_.mul(away.x, c), calc_.mul(away.y, s)),
                    calc_.add(calc_.mul(away.x, s), calc_.mul(away.y, c)), Fixed{}};
                target = add(target, scale(rotated, whole(60)));
            }
        }
        if (!leading()) {
            target = add(add(leader().position, scale(leader().state.velocity, whole(40))),
                scale(slot(), Fixed::from_raw(Fixed::scale * 3 / 4)));
        }
        if (!avoid(target, budget(), true)) head_for(target, speed, budget(), true); // FD-10
    }

    // FO-01 (#424): a player move. The leader heads for the destination at full speed and the
    // others form up on it, as FoC's fighter locomotor flies a team along its path.
    void move() {
        const Calc::Site site(calc_, "FO-01 move");
        if (frame_.lane) {
            // FO-10: a squadron of a group move flies its formation's path at the formation's
            // speed, its leader steering toward its lane.
            form_up(frame_.lane->speed, frame_.hold.z, frame_.lane->direction, frame_.lane->shift);
            return;
        }
        if (leading()) {
            Vec3 aim = frame_.hold;
            if (!avoid(aim, budget(), false)) head_for(aim, profile_.max_speed, budget(), false); // FD-10
            return;
        }
        form_up(profile_.max_speed, frame_.hold.z);
    }

    // FA-01 to FA-07: close on the target, then attack runs. True on the FA-07 approach (DG-26):
    // FoC's directed combat clears the out-of-combat defense, and only its path approach sets it.
    bool directed_combat() {
        const Calc::Site site(calc_, "FA-01 directed combat");
        if (frame_.dogfight != DogfightFlight::none) {
            dogfight();
            return false;
        }
        const Vec3 target = frame_.target_position;
        const Fixed reach = calc_.add(profile_.strafe_distance, frame_.target_radius);
        const Fixed dx = calc_.sub(target.x, leader().position.x);
        const Fixed dy = calc_.sub(target.y, leader().position.y);
        const auto across = squared_components(dx, dy);
        const auto reach_squared = squared_components(reach, Fixed{});
        if (math::detail::compare(reach_squared, across) < 0) {
            if (frame_.approach) {
                // FA-07: the approach in formation at the layer height, the leader along its path
                // (the planar line to the target, unverified G-F8).
                form_up(profile_.max_speed, profile_.layer_z, Vec3{dx, dy, Fixed{}});
                return true;
            }
            Vec3 aim = target;
            if (!avoid(aim, budget(), false)) head_for(aim, profile_.max_speed, budget(), false); // FA-01, FD-10
            return false;
        }
        if (frame_.target_craft) {
            // FA-06: a craft target outside a squadron: the leader chases, the others form up.
            if (leading()) {
                head_for(target, profile_.max_speed, budget(), false);
            } else {
                form_up(profile_.max_speed, target.z);
            }
            return false;
        }
        if (leading()) {
            // FA-02 to FA-04: a run at the target while it stays in the nose cone, else pass on.
            if (in_cone(target, reach_squared)) {
                Vec3 aim = target;
                if (!avoid(aim, budget(), false)) head_for(aim, profile_.max_speed, budget(), false); // FD-10
                return false;
            }
        }
        form_up(profile_.max_speed, target.z);
        return false;
    }

    // FA-03: the target within `range_squared` and 45 degrees of yaw and pitch of the leader's nose.
    [[nodiscard]] bool in_cone(const Vec3& target, const math::detail::UInt192 range_squared) {
        const Calc::Site site(calc_, "FA-03 nose cone");
        const Vec3 offset = sub(target, position_);
        const auto squared = squared_components(offset.x, offset.y, offset.z);
        if (math::detail::compare(range_squared, squared) < 0) return false;
        Fixed yaw_delta;
        Fixed pitch_delta;
        local_angles(offset, yaw_delta, pitch_delta);
        return calc_.abs(yaw_delta) <= whole(45) && calc_.abs(pitch_delta) <= whole(45);
    }

    const CraftFrame& frame_;
    const CraftView& self_;
    CraftProfile profile_;
    CraftState state_;
    Vec3 position_;
    std::optional<Vec3> slot_;
    Calc calc_;
};

} // namespace

const CraftProfile* SquadronTable::find_craft(const TypeId type_id) const noexcept { return find_by_type(craft, type_id); }
const SquadronProfile* SquadronTable::find_squadron(const TypeId type_id) const noexcept {
    return find_by_type(squadrons, type_id);
}
const SpawnerProfile* SquadronTable::find_spawner(const TypeId type_id) const noexcept {
    return find_by_type(spawners, type_id);
}

core::Result<void> validate_squadron_table(const SquadronTable& table) {
    if (!increasing(table.craft) || !increasing(table.squadrons) || !increasing(table.spawners)) {
        return invalid("type IDs must strictly increase");
    }
    if (!rate(table.side_error_min, false) || !rate(table.side_error_max, false)) {
        return invalid("a formation side error is out of range");
    }
    for (const auto& craft : table.craft) {
        const auto label = "craft type " + std::to_string(craft.type_id);
        if (!rate(craft.max_speed, true) || !rate(craft.min_speed, false) || !rate(craft.rate_of_turn, true)
            || !rate(craft.lift, false) || !rate(craft.thrust, true) || !rate(craft.roll_rate, false)
            || craft.bank_angle.raw() < 0 || craft.bank_angle.raw() > 90 * Fixed::scale || !distance(craft.strafe_distance)
            || !distance(craft.follow_distance) || !distance(craft.attack_distance)
            || craft.out_of_combat_defense.raw() > Fixed::scale
            || craft.out_of_combat_defense.raw() < -max_out_of_combat_defense * Fixed::scale
            || craft.layer_z.raw() < -max_motion_coordinate * Fixed::scale
            || craft.layer_z.raw() > max_motion_coordinate * Fixed::scale) {
            return invalid(label + ": a rate or distance is out of range");
        }
        if (craft.spin_away && (craft.spin_away->chance.raw() < 0 || craft.spin_away->chance > whole(1)
                                || craft.spin_away->time.raw() < 0 || craft.spin_away->time > whole(max_spin_seconds))) {
            return invalid(label + ": a spin-away chance is in [0, 1] and its time in [0, 60] seconds");
        }
    }
    for (const auto& squadron : table.squadrons) {
        const auto label = "squadron type " + std::to_string(squadron.type_id);
        if (squadron.members.empty() || squadron.members.size() > max_members
            || squadron.offsets.size() != squadron.members.size()) {
            return invalid(label + ": 1 to 64 members, one offset each");
        }
        for (const auto member : squadron.members) {
            if (find_by_type(table.craft, member) == nullptr) {
                return invalid(label + ": member type " + std::to_string(member) + " is not a craft of the table");
            }
        }
        if (!std::all_of(squadron.offsets.begin(), squadron.offsets.end(), point) || !distance(squadron.guard_chase_range)
            || !distance(squadron.idle_chase_range) || !distance(squadron.attack_move_response_range)
            || !distance(squadron.formation_tolerance)) {
            return invalid(label + ": an offset or range is out of range");
        }
    }
    for (const auto& spawner : table.spawners) {
        const auto label = "spawner type " + std::to_string(spawner.type_id);
        if (spawner.entries.size() > max_entries || spawner.bays.size() > max_bays) {
            return invalid(label + ": at most 64 entries and 255 bays");
        }
        for (const auto& entry : spawner.entries) {
            if (entry.starting < 0 || entry.reserve < unlimited_reserve) {
                return invalid(label + ": negative spawn count");
            }
            if (entry.reserve >= 0 && entry.starting > std::numeric_limits<std::int32_t>::max() - entry.reserve) {
                return invalid(label + ": starting plus reserve exceeds int32");
            }
        }
        for (const auto& bay : spawner.bays) {
            if (!point(bay.position) || !point(bay.spawn_vector)) return invalid(label + ": a bay point is out of range");
        }
        for (const auto& entry : spawner.entries) {
            if (find_by_type(table.squadrons, entry.squadron) == nullptr) {
                return invalid(label + ": entry squadron type " + std::to_string(entry.squadron) + " is not in the table");
            }
        }
        if (spawner.delay_frames > 30U * 3600U) return invalid(label + ": the delay exceeds an hour");
    }
    return core::Result<void>::success();
}

SpawnerState initial_spawner(const std::uint64_t seed, const std::uint64_t frame, const EntityId unit) {
    CombatRandom draw(seed, frame, unit, service_slot);
    SpawnerState state;
    state.next_service_frame = frame + draw.uniform(0, static_cast<std::uint32_t>(service_interval - 1));
    return state;
}

std::optional<SpawnDecision> service_spawner(const SpawnerProfile& profile, SpawnerState& state, const std::uint64_t seed,
    const std::uint64_t frame, const EntityId unit, const std::vector<bool>& bay_intact) {
    if (frame != state.next_service_frame) return std::nullopt;
    state.next_service_frame = frame + service_interval;
    if (!state.ready) {
        // FL-02: the first service builds the entries and may launch at once.
        state.ready = true;
        state.entries.clear();
        for (const auto& entry : profile.entries) {
            const auto remaining = entry.reserve < 0 ? unlimited_reserve : entry.reserve + entry.starting;
            state.entries.push_back(SpawnEntryState{0, remaining});
        }
        state.next_spawn_frame = frame;
    }
    if (state.next_spawn_frame > frame) return std::nullopt;
    // FL-03: a space spawner needs a standing fighter bay.
    std::vector<std::uint32_t> bays;
    for (std::uint32_t index = 0; index < profile.bays.size(); ++index) {
        if (index < bay_intact.size() && bay_intact[index]) bays.push_back(index);
    }
    if (bays.empty() || state.entries.empty()) return std::nullopt;
    const auto eligible = [&](const std::size_t index) {
        const auto& entry = state.entries[index];
        return entry.alive < profile.entries[index].starting
            && (entry.remaining > 0 || entry.remaining == unlimited_reserve);
    };
    if (std::none_of(state.entries.begin(), state.entries.end(),
            [&](const SpawnEntryState& entry) { return eligible(static_cast<std::size_t>(&entry - state.entries.data())); })) {
        return std::nullopt;
    }
    // FL-04, FL-05: a random start, then the first eligible entry in cyclic order.
    const auto count = static_cast<std::uint32_t>(state.entries.size());
    CombatRandom entry_draw(seed, frame, unit, entry_slot);
    const auto start = entry_draw.uniform(0, count - 1);
    std::optional<SpawnDecision> decision;
    for (std::uint32_t step = 0; step < count; ++step) {
        const auto index = (start + step) % count;
        if (!eligible(index)) continue;
        auto& entry = state.entries[index];
        if (entry.remaining > 0) --entry.remaining;
        ++entry.alive;
        CombatRandom bay_draw(seed, frame, unit, bay_slot);
        decision = SpawnDecision{index, bays[bay_draw.uniform(0, static_cast<std::uint32_t>(bays.size() - 1))]};
        break;
    }
    state.next_spawn_frame = frame + profile.delay_frames;
    return decision;
}

void squadron_lost(const SpawnerProfile& profile, SpawnerState& state, const std::uint32_t entry, const std::uint64_t frame) {
    // FL-08: the entries up to the lost one were all full: the next launch waits a full delay.
    bool full = true;
    for (std::uint32_t index = 0; index < state.entries.size() && index < profile.entries.size(); ++index) {
        auto& current = state.entries[index];
        if (current.alive < profile.entries[index].starting) full = false;
        if (index == entry) {
            if (current.alive > 0) --current.alive;
            break;
        }
    }
    if (full) state.next_spawn_frame = frame + profile.delay_frames;
}

bool squadron_move_arrived(const math::Vec3& origin, const math::Vec3& destination, const math::Vec3& leader) noexcept {
    // The sign of (leader - destination) . (destination - origin) in the plane, from the sums of
    // the positive and the negative products (raw differences fit 64 bits for accepted positions).
    math::detail::UInt192 positive{};
    math::detail::UInt192 negative{};
    const auto add_product = [&](const Fixed a0, const Fixed a1, const Fixed b0, const Fixed b1) {
        const auto a = static_cast<std::int64_t>(static_cast<std::uint64_t>(a0.raw()) - static_cast<std::uint64_t>(a1.raw()));
        const auto b = static_cast<std::int64_t>(static_cast<std::uint64_t>(b0.raw()) - static_cast<std::uint64_t>(b1.raw()));
        if (a == 0 || b == 0) return;
        const auto magnitude = [](const std::int64_t value) {
            return value < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        };
        auto& sum = (a < 0) == (b < 0) ? positive : negative;
        static_cast<void>(math::detail::add_magnitude(sum, math::detail::multiply_u64(magnitude(a), magnitude(b))));
    };
    add_product(leader.x, destination.x, destination.x, origin.x);
    add_product(leader.y, destination.y, destination.y, origin.y);
    return math::detail::compare(positive, negative) >= 0;
}

core::Result<CraftStep> step_craft(const CraftFrame& frame) {
    if (frame.self == nullptr || frame.self->profile == nullptr) {
        return core::Result<CraftStep>::failure(
            detail::diagnostic(diagnostic_codes::worker_failure, "craft step without a craft profile"));
    }
    return Locomotor(frame).run();
}

namespace {

[[nodiscard]] std::int64_t floor_divide(const std::int64_t value, const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient * divisor != value && value < 0 ? quotient - 1 : quotient;
}

constexpr std::int64_t cell_raw = combat_cell_size * Fixed::scale;

} // namespace

CombatCell combat_cell_of(const Vec3& point) noexcept {
    CombatCell cell;
    cell.y = static_cast<std::int32_t>(floor_divide(point.y.raw(), cell_raw));
    auto x = point.x.raw();
    if ((cell.y & 1) != 0) x -= cell_raw / 2;
    cell.x = static_cast<std::int32_t>(floor_divide(x, cell_raw));
    return cell;
}

Vec3 combat_cell_point(const CombatCell cell, const Fixed z) noexcept {
    auto x = static_cast<std::int64_t>(cell.x) * cell_raw + cell_raw / 2;
    if ((cell.y & 1) != 0) x += cell_raw / 2;
    return {Fixed::from_raw(x), Fixed::from_raw(static_cast<std::int64_t>(cell.y) * cell_raw + cell_raw / 2), z};
}

namespace {

constexpr std::int64_t idle_raw = idle_cell_size * Fixed::scale;

} // namespace

CombatCell idle_cell_of(const Vec3& point) noexcept {
    CombatCell cell;
    cell.y = static_cast<std::int32_t>(floor_divide(point.y.raw(), idle_raw));
    auto x = point.x.raw();
    if ((cell.y & 1) != 0) x -= idle_raw / 2;
    cell.x = static_cast<std::int32_t>(floor_divide(x, idle_raw));
    return cell;
}

Vec3 idle_cell_point(const CombatCell cell, const Fixed z) noexcept {
    auto x = static_cast<std::int64_t>(cell.x) * idle_raw + idle_raw / 2;
    if ((cell.y & 1) != 0) x += idle_raw / 2;
    return {Fixed::from_raw(x), Fixed::from_raw(static_cast<std::int64_t>(cell.y) * idle_raw + idle_raw / 2), z};
}

std::optional<CombatCell> idle_cell_claim(const Vec3& desired, const std::span<const CombatCell> taken) {
    const auto held = [&taken](const CombatCell cell) { return std::find(taken.begin(), taken.end(), cell) != taken.end(); };
    const CombatCell start = idle_cell_of(desired);
    if (!held(start)) return start;
    // FM-24: rings around the start cell, row by row from the low corner; the start cell counts
    // as the first cell examined.
    std::uint32_t examined = 1;
    for (std::int32_t ring = 1; examined < idle_cell_search_limit; ++ring) {
        std::optional<CombatCell> best;
        math::detail::UInt192 best_score{};
        for (std::int32_t y = start.y - ring; y <= start.y + ring && examined < idle_cell_search_limit; ++y) {
            for (std::int32_t x = start.x - ring; x <= start.x + ring && examined < idle_cell_search_limit; ++x) {
                const std::int32_t across = x > start.x ? x - start.x : start.x - x;
                const std::int32_t along = y > start.y ? y - start.y : start.y - y;
                if (std::max(across, along) != ring) continue;
                ++examined;
                const CombatCell cell{x, y};
                if (held(cell)) continue;
                const Vec3 point = idle_cell_point(cell, Fixed{});
                const auto dx = math::detail::unsigned_magnitude(point.x.raw() - desired.x.raw());
                const auto dy = math::detail::unsigned_magnitude(point.y.raw() - desired.y.raw());
                auto score = math::detail::multiply_u64(dx, dx);
                static_cast<void>(math::detail::add_magnitude(score, math::detail::multiply_u64(dy, dy)));
                if (!best || math::detail::compare(score, best_score) < 0) {
                    best = cell;
                    best_score = score;
                }
            }
        }
        if (best) return best;
    }
    return std::nullopt;
}

bool within_combat_cell_reach(const Vec3& point, const CombatCell cell) noexcept {
    // FD-03: 400 / sqrt(2), compared squared (80000 square units) in raw units.
    const Vec3 centre = combat_cell_point(cell, Fixed{});
    const auto dx = math::detail::unsigned_magnitude(point.x.raw() - centre.x.raw());
    const auto dy = math::detail::unsigned_magnitude(point.y.raw() - centre.y.raw());
    auto across = math::detail::multiply_u64(dx, dx);
    static_cast<void>(math::detail::add_magnitude(across, math::detail::multiply_u64(dy, dy)));
    const auto half = static_cast<std::uint64_t>(combat_cell_size * combat_cell_size / 2);
    return math::detail::compare(across,
               math::detail::multiply_u64(half * static_cast<std::uint64_t>(Fixed::scale), static_cast<std::uint64_t>(Fixed::scale)))
        <= 0;
}

core::Result<bool> in_follow_cone(const CraftView& self, const Vec3& target, Fixed& yaw, Fixed& pitch) {
    if (self.profile == nullptr) {
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "follow test without a craft profile"));
    }
    Calc calc;
    const bool result = followable(calc, self.position, self.state, target, self.profile->attack_distance, yaw, pitch);
    if (!calc.ok()) {
        return core::Result<bool>::failure(calc.error("follow test of craft " + std::to_string(self.id)));
    }
    return core::Result<bool>::success(result);
}

core::Result<std::vector<GroupSlot>> squadron_group_slots(const std::span<const GroupSquadron> squadrons,
    const Vec3& destination) {
    Calc calc;
    std::vector<GroupSlot> slots(squadrons.size(), GroupSlot{destination, std::nullopt});
    std::vector<Fixed> distance(squadrons.size());
    std::vector<std::size_t> pool(squadrons.size());
    for (std::size_t index = 0; index < squadrons.size(); ++index) {
        pool[index] = index;
        const auto& position = squadrons[index].position;
        distance[index] = calc.length(Vec3{calc.sub(destination.x, position.x), calc.sub(destination.y, position.y),
            calc.sub(destination.z, position.z)});
    }
    const Fixed third = Fixed::from_raw(Fixed::scale * 33 / 100);
    const Fixed ratio = Fixed::from_raw(Fixed::scale * 9 / 10);
    const Fixed cell = whole(20);
    // FO-07: the squadron farthest from the destination (input order on ties) starts a formation.
    // Its box is its radius doubled, and in the plane at least a third of its distance to the
    // destination; every later squadron whose own radius overlaps that box, and whose speeds
    // suit it (each one's minimum at most 0.9 of the other's maximum), joins it.
    while (!pool.empty() && calc.ok()) {
        auto farthest = pool.begin();
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (distance[*it] > distance[*farthest]) farthest = it;
        }
        const auto self = *farthest;
        pool.erase(farthest);
        const auto& own = squadrons[self];
        const Fixed reach = std::max(calc.mul(own.radius, whole(2)), calc.mul(distance[self], third));
        std::vector<std::size_t> formation{self};
        std::vector<std::size_t> rest;
        for (const auto index : pool) {
            const auto& other = squadrons[index];
            const Fixed limit = calc.add(reach, other.radius);
            const bool overlaps = calc.abs(calc.sub(other.position.x, own.position.x)) <= limit
                && calc.abs(calc.sub(other.position.y, own.position.y)) <= limit;
            const bool speeds = own.max_speed.raw() > 0 && other.max_speed.raw() > 0
                && calc.div(other.min_speed, own.max_speed) <= ratio && calc.div(own.min_speed, other.max_speed) <= ratio;
            (overlaps && speeds ? formation : rest).push_back(index);
        }
        pool = std::move(rest);
        if (formation.size() < 2) continue;

        // FO-08: the formation faces from its centre to the destination (project: +x when they
        // meet), along its one straight path (FO-10). In cells of 20 units a squadron is two
        // radii wide and deep, and a row is the square root of the squadrons' summed areas times
        // 1.1 wide.
        Fixed cx{};
        Fixed cy{};
        for (const auto index : formation) {
            cx = calc.add(cx, squadrons[index].position.x);
            cy = calc.add(cy, squadrons[index].position.y);
        }
        const Fixed count = whole(static_cast<std::int64_t>(formation.size()));
        const Vec3 origin{calc.div(cx, count), calc.div(cy, count), Fixed{}};
        const Fixed dx = calc.sub(destination.x, origin.x);
        const Fixed dy = calc.sub(destination.y, origin.y);
        const Fixed span = calc.length(dx, dy);
        const Fixed fx = span.raw() != 0 ? calc.div(dx, span) : whole(1);
        const Fixed fy = span.raw() != 0 ? calc.div(dy, span) : Fixed{};
        const Fixed sx = calc.neg(fy); // the left of the facing
        const Fixed sy = fx;
        const auto footprint = [&](const std::size_t index) { return calc.div(calc.mul(squadrons[index].radius, whole(2)), cell); };
        Fixed area{};
        for (const auto index : formation) area = calc.add(area, calc.mul(footprint(index), footprint(index)));
        const Fixed width = calc.mul(calc.sqrt(area), Fixed::from_raw(Fixed::scale * 11 / 10));
        const auto project = [&](const std::size_t index, const Fixed ax, const Fixed ay) {
            return calc.add(calc.mul(squadrons[index].position.x, ax), calc.mul(squadrons[index].position.y, ay));
        };
        // FO-09: a row takes squadrons of one order class, one type; the classes go by their
        // attack distance, shortest first (project: the craft's distance and then the type ID,
        // where FoC reads the team container's distance and then its type name).
        const auto order = [&](const std::size_t a, const std::size_t b) {
            if (squadrons[a].type == squadrons[b].type) return 0;
            if (squadrons[a].attack_distance != squadrons[b].attack_distance) {
                return squadrons[a].attack_distance < squadrons[b].attack_distance ? -1 : 1;
            }
            return squadrons[a].type < squadrons[b].type ? -1 : 1;
        };
        // Within its class the frontmost squadrons (along the facing; input order on ties) fill
        // a row while their widths fit, the first always; a row runs from the right of the move
        // to its left in the order its squadrons stand, spread with equal gaps. Each row lies
        // half its depth behind the last, and the block is centred on the destination.
        std::vector<std::size_t> waiting = formation;
        std::stable_sort(waiting.begin(), waiting.end(), [&](const std::size_t a, const std::size_t b) {
            const int by_class = order(a, b);
            return by_class != 0 ? by_class < 0 : project(a, fx, fy) > project(b, fx, fy);
        });
        std::vector<std::pair<std::size_t, std::pair<Fixed, Fixed>>> placed;
        Fixed x{};
        Fixed front_half{};
        Fixed depth{};
        bool first = true;
        while (!waiting.empty() && calc.ok()) {
            const auto leading = waiting.front();
            std::vector<std::size_t> row;
            std::vector<std::size_t> later;
            Fixed used{};
            Fixed deepest{};
            for (const auto index : waiting) {
                const Fixed size = footprint(index);
                if (order(index, leading) == 0 && (row.empty() || calc.add(used, size) <= width)) {
                    row.push_back(index);
                    used = calc.add(used, size);
                    deepest = std::max(deepest, size);
                } else {
                    later.push_back(index);
                }
            }
            waiting = std::move(later);
            std::stable_sort(row.begin(), row.end(),
                [&](const std::size_t a, const std::size_t b) { return project(a, sx, sy) < project(b, sx, sy); });
            const Fixed half = calc.div(deepest, whole(2));
            if (x.raw() < 0) x = calc.sub(x, half);
            const Fixed gap = calc.div(calc.sub(width, used), whole(static_cast<std::int64_t>(row.size() + 1)));
            Fixed along = gap;
            for (const auto index : row) {
                const Fixed own_half = calc.div(footprint(index), whole(2));
                along = calc.add(along, own_half);
                placed.push_back({index, {x, calc.sub(along, calc.div(width, whole(2)))}});
                along = calc.add(along, calc.add(own_half, gap));
            }
            if (first) front_half = half;
            first = false;
            if (waiting.empty()) depth = calc.add(calc.neg(calc.sub(x, half)), front_half);
            x = calc.sub(x, half);
        }
        const Fixed shift = calc.sub(calc.div(depth, whole(2)), front_half);
        for (const auto& [index, offset] : placed) {
            const Fixed ahead = calc.mul(calc.add(offset.first, shift), cell);
            const Fixed aside = calc.mul(offset.second, cell);
            slots[index].point = {calc.add(destination.x, calc.add(calc.mul(fx, ahead), calc.mul(sx, aside))),
                calc.add(destination.y, calc.add(calc.mul(fy, ahead), calc.mul(sy, aside))), destination.z};
            slots[index].lane = SquadronLane{static_cast<EntityId>(self), origin, Vec3{fx, fy, Fixed{}}, ahead, aside};
        }
    }
    if (!calc.ok()) {
        return core::Result<std::vector<GroupSlot>>::failure(calc.error("squadron group move"));
    }
    return core::Result<std::vector<GroupSlot>>::success(std::move(slots));
}

core::Result<std::vector<LaneFlight>> formation_lane_flight(const std::span<const LaneMember> members,
    const Fixed side_error_min, const Fixed side_error_max) {
    Calc calc;
    std::vector<LaneFlight> flights(members.size());
    if (members.empty()) return core::Result<std::vector<LaneFlight>>::success(std::move(flights));
    // FO-11: the formation flies at its slowest member's `Max_Speed` (project: FoC's formation
    // maximum was not read), and a squadron ahead of its place slows toward the largest
    // `Min_Speed` of the formation (debug build: the formation's minimum speed).
    Fixed top = members.front().max_speed;
    Fixed floor = members.front().min_speed;
    for (const auto& member : members) {
        top = std::min(top, member.max_speed);
        floor = std::max(floor, member.min_speed);
    }
    std::vector<Fixed> forward(members.size());
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& member = members[index];
        const auto& lane = member.lane;
        const Fixed rx = calc.sub(member.position.x, lane.origin.x);
        const Fixed ry = calc.sub(member.position.y, lane.origin.y);
        forward[index] = calc.add(calc.mul(rx, lane.direction.x), calc.mul(ry, lane.direction.y));
        // FO-10: the side error is the lane's offset less how far left of the path the squadron
        // flies; within the minimum there is no steer, beyond it the rest over the maximum.
        const Fixed side = calc.sub(calc.mul(ry, lane.direction.x), calc.mul(rx, lane.direction.y));
        const Fixed error = calc.sub(lane.aside, side);
        auto& flight = flights[index];
        flight.direction = lane.direction;
        if (side_error_max.raw() > 0 && calc.abs(error) > side_error_min) {
            const Fixed beyond = error.raw() > 0 ? calc.sub(error, side_error_min) : calc.add(error, side_error_min);
            flight.shift = calc.div(beyond, side_error_max);
        }
    }
    // FO-11: a squadron's deviance is the mean, over the formation's other squadrons at least 20
    // units off (debug build: the space threshold), of how far each stands ahead of it less how
    // far its slot lies ahead.
    std::vector<Fixed> deviance(members.size());
    Fixed most{};
    Fixed least{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        Fixed sum{};
        std::int64_t counted = 0;
        for (std::size_t other = 0; other < members.size(); ++other) {
            if (other == index) continue;
            const Fixed wanted = calc.sub(members[other].lane.ahead, members[index].lane.ahead);
            const Fixed term = calc.sub(calc.sub(forward[other], forward[index]), wanted);
            if (calc.abs(term) < whole(20)) continue;
            sum = calc.add(sum, term);
            ++counted;
        }
        deviance[index] = counted != 0 ? calc.div(sum, whole(counted)) : Fixed{};
        most = std::max(most, deviance[index]);
        least = std::min(least, deviance[index]);
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        const Fixed own = members[index].max_speed;
        const Fixed off = deviance[index];
        Fixed speed = top;
        // FO-11: behind its place a squadron speeds up toward its own `Max_Speed`, ahead of it it
        // slows toward the formation's minimum, by its deviance over the largest one (at least
        // 12 units, debug build).
        if (off.raw() > 0 && most.raw() > 0) {
            const Fixed share = calc.div(off, std::max(whole(12), most));
            speed = calc.add(calc.mul(share, own), calc.mul(calc.sub(whole(1), share), top));
        } else if (off.raw() < 0 && least.raw() < 0) {
            const Fixed share = calc.div(off, std::min(calc.neg(whole(12)), least));
            speed = calc.add(calc.mul(share, floor), calc.mul(calc.sub(whole(1), share), top));
        }
        // FO-11: steering aside it keeps its pace along the path, never faster than its own
        // `Max_Speed`.
        const Fixed shift = flights[index].shift;
        speed = calc.mul(speed, calc.sqrt(calc.add(whole(1), calc.mul(shift, shift))));
        flights[index].speed = std::clamp(speed, Fixed{}, own);
    }
    if (!calc.ok()) {
        return core::Result<std::vector<LaneFlight>>::failure(calc.error("squadron formation flight"));
    }
    return core::Result<std::vector<LaneFlight>>::success(std::move(flights));
}

core::Result<CraftState> launch_state(const Vec3 direction, const Fixed speed) {
    Calc calc;
    CraftState state;
    const Fixed across = calc.length(direction.x, direction.y);
    if (direction.x.raw() != 0 || direction.y.raw() != 0) state.yaw = clamp180(calc.atan2_deg(direction.y, direction.x));
    if (across.raw() != 0 || direction.z.raw() != 0) state.pitch = clamp180(calc.neg(calc.atan2_deg(direction.z, across)));
    const Fixed level = calc.cos_deg(state.pitch);
    state.velocity = {calc.mul(speed, calc.mul(calc.cos_deg(state.yaw), level)),
        calc.mul(speed, calc.mul(calc.sin_deg(state.yaw), level)), calc.mul(speed, calc.sin_deg(calc.neg(state.pitch)))};
    if (!calc.ok()) {
        return core::Result<CraftState>::failure(calc.error("launch facing"));
    }
    return core::Result<CraftState>::success(state);
}

core::Result<math::Quat> craft_rotation(const CraftState& state) {
    // FM-02: yaw about Z, then pitch about the craft's Y (positive lowers the nose), then the
    // roll about its forward axis (BK-05's sign: negative lowers the left side).
    Calc calc;
    const Fixed half_yaw = calc.div(clamp180(state.yaw), whole(720));
    const Fixed half_pitch = calc.div(clamp180(state.pitch), whole(720));
    if (!calc.ok()) {
        return core::Result<math::Quat>::failure(calc.error("craft rotation"));
    }
    const math::Quat yaw{Fixed{}, Fixed{}, math::sin_turn(half_yaw), math::cos_turn(half_yaw)};
    const math::Quat pitch{Fixed{}, math::sin_turn(half_pitch), Fixed{}, math::cos_turn(half_pitch)};
    auto heading = math::compose(yaw, pitch);
    if (!heading) {
        return core::Result<math::Quat>::failure(heading.error());
    }
    auto level = math::normalize(heading.value());
    if (!level) {
        return level;
    }
    return banked_rotation(level.value(), state.roll);
}

namespace detail {

namespace {

// Squared distance of two points as an exact wide integer of raw units (ties compare exactly).
[[nodiscard]] math::detail::UInt192 squared(const Vec3& left, const Vec3& right, const bool planar) {
    math::detail::UInt192 total = math::detail::from_u64(0);
    const auto term = [&total](const Fixed a, const Fixed b) {
        const auto magnitude = math::detail::unsigned_magnitude(a.raw() - b.raw());
        static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(magnitude, magnitude)));
    };
    term(left.x, right.x);
    term(left.y, right.y);
    if (!planar) term(left.z, right.z);
    return total;
}

} // namespace

SquadronScan squadron_target(const CombatWorld& world, const SquadronState& state, const SquadronProfile& squadron,
    const CombatUnit& leader, const Vec3 anchor) {
    const auto visible = [&leader](const CombatUnit& unit) {
        return unit.team == leader.team || (unit.visible_to & (std::uint64_t{1} << leader.player_index)) != 0U;
    };
    // FT-01: a live target the owner sees is kept, and so is a fogged one while the squadron still
    // flies the approach its attack planned (FA-07, #633).
    if (state.target != invalid_entity_id) {
        // #424: a squadron target (a player's attack order) is kept while one of its craft is.
        const auto* current = world.resolve_target(world.find(state.target), leader.position);
        if (current != nullptr && (visible(*current) || state.approach)) return SquadronScan{state.target, state.next_scan_frame};
    }
    SquadronScan result{invalid_entity_id, state.next_scan_frame};
    if (world.frame < state.next_scan_frame || leader.profile == nullptr || !leader.profile->max_attack_distance) {
        return result;
    }
    // FT-02, FT-03: one scan a second (project cadence) within the chase range plus the leader's
    // attack distance of the anchor. FO-05, FO-06: a player attack-move or guard sets the range.
    result.next_scan_frame = world.frame + logical_frames_per_second;
    Fixed chase = squadron.idle_chase_range;
    if (state.diversion == SquadronDiversion::attack_move) {
        chase = squadron.attack_move_response_range;
    } else if (state.mode == SquadronMode::escort || state.diversion == SquadronDiversion::guard) {
        chase = squadron.guard_chase_range;
    }
    const auto reach_raw = static_cast<std::uint64_t>(chase.raw() + leader.profile->max_attack_distance->raw());
    const auto reach = math::detail::multiply_u64(reach_raw, reach_raw);
    const PrioritySet* set = leader.profile->priority_set ? &world.table->priority_sets[*leader.profile->priority_set] : nullptr;
    const CombatUnit* best = nullptr;
    Fixed best_priority;
    math::detail::UInt192 best_distance{};
    for (const auto& unit : world.units) {
        if (unit.team == leader.team || unit.durability_profile == nullptr || !visible(unit)) continue;
        if (math::detail::compare(squared(unit.position, anchor, true), reach) > 0) continue;
        std::optional<Fixed> priority = Fixed::from_raw(Fixed::scale);
        if (set != nullptr) {
            const auto found = std::lower_bound(set->rows.begin(), set->rows.end(), unit.type_id,
                [](const PriorityRow& row, const TypeId id) { return row.type_id < id; });
            priority = found != set->rows.end() && found->type_id == unit.type_id ? found->priority : set->unlisted;
        }
        if (!priority) continue;
        const auto distance = squared(unit.position, leader.position, false);
        if (best == nullptr || *priority < best_priority
            || (*priority == best_priority && math::detail::compare(distance, best_distance) < 0)) {
            best = &unit;
            best_priority = *priority;
            best_distance = distance;
        }
    }
    if (best != nullptr) result.target = best->id;
    return result;
}

} // namespace detail

} // namespace eawr::sim::tactical
