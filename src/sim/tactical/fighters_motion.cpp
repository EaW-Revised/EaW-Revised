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

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

namespace {
// FD-10: frames a craft flies straight on after steering away from a ship.
constexpr std::uint32_t avoidance_relaxation = 15;

// --- The locomotor of one craft for one frame (FM, FA rules) ------------------------------------

class Locomotor final {
public:
    explicit Locomotor(const CraftFrame& frame)
        : frame_(frame), self_(*frame.self), profile_(*frame.self->profile), state_(frame.self->state),
          position_(frame.self->position), calc_(frame.trig_cache != nullptr ? frame.trig_cache : &local_trig_) {
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
        fighters_detail::local_angles(calc_, state_, offset, yaw_delta, pitch_delta);
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
        if (limits.lift.raw() > 0) {
            // FM-11/FM-13: height easing uses the same lift budget as the catching-up turn.
            const Fixed ease = calc_.mul(calc_.div(calc_.abs(clamp180(state_.pitch)), limits.lift), own);
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

    // WSQ-10/WSQ-17: every moving craft forms up, including the leader. Its path
    // look-ahead and height easing bring it back to the layer after a reversal.
    void move() {
        const Calc::Site site(calc_, "FO-01 move");
        if (frame_.lane) {
            // FO-10: a squadron of a group move flies its formation's path at the formation's
            // speed, its leader steering toward its lane.
            form_up(frame_.lane->individual_speed ? profile_.max_speed : frame_.lane->speed,
                frame_.hold.z, frame_.lane->direction, frame_.lane->shift);
            return;
        }
        // Standalone callers without a lane supply one straight segment to the goal.
        form_up(profile_.max_speed, frame_.hold.z,
            Vec3{calc_.sub(frame_.hold.x, leader().position.x),
                calc_.sub(frame_.hold.y, leader().position.y), Fixed{}});
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
    math::TrigCache local_trig_;
    Calc calc_;
};

} // namespace

core::Result<CraftStep> step_craft(const CraftFrame& frame) {
    if (frame.self == nullptr || frame.self->profile == nullptr) {
        return core::Result<CraftStep>::failure(
            detail::diagnostic(diagnostic_codes::worker_failure, "craft step without a craft profile"));
    }
    return Locomotor(frame).run();
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

} // namespace eawr::sim::tactical
