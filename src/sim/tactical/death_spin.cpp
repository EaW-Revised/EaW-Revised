#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <string>

// Spin-away deaths (#447): docs/behaviour/space-fighter-deaths.md SP-01 to SP-08.
namespace eawr::sim::tactical {

namespace {

using math::Fixed;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::clamp180;
using motion_detail::whole;

// Slot keys of the spin's keyed draws (SP-02, SP-05); no other draw uses them.
constexpr std::uint32_t spin_chance_slot = 0xfffd0001U;
constexpr std::uint32_t spin_path_slot = 0xfffd0002U;

// SP-06: the roll a spinning craft adds each frame, in degrees.
constexpr Fixed roll_per_frame = whole(20);
// SP-05: Simpson intervals per path segment (the remake's fixed rule, see the note).
constexpr std::int64_t simpson_intervals = 32;

// One axis of a natural cubic segment: a + b u + c u^2 + d u^3 for u in [0, 1].
struct Cubic {
    Fixed a;
    Fixed b;
    Fixed c;
    Fixed d;
};

using Axis = std::array<Cubic, 3>; // the three segments of one axis

class Path final {
public:
    explicit Path(Calc& calc) : calc_(calc) {}

    // SP-05: the natural cubic spline through four points, per axis.
    void fit(const std::array<Vec3, 4>& points) {
        const auto axis = [&](const auto component) {
            std::array<Fixed, 4> x{};
            for (std::size_t index = 0; index < x.size(); ++index) x[index] = component(points[index]);
            return fit_axis(x);
        };
        axes_ = {axis([](const Vec3& value) { return value.x; }), axis([](const Vec3& value) { return value.y; }),
            axis([](const Vec3& value) { return value.z; })};
    }

    [[nodiscard]] Vec3 point(const std::size_t segment, const Fixed u) {
        return {eval(axes_[0][segment], u), eval(axes_[1][segment], u), eval(axes_[2][segment], u)};
    }

    // A segment's length: composite Simpson over the speed |dP/du|.
    [[nodiscard]] Fixed length(const std::size_t segment) {
        const Fixed step = Fixed::from_raw(Fixed::scale / simpson_intervals);
        Fixed sum{};
        for (std::int64_t index = 0; index <= simpson_intervals; ++index) {
            const Fixed u = Fixed::from_raw(index * Fixed::scale / simpson_intervals);
            const Fixed speed = calc_.length(Vec3{slope(axes_[0][segment], u), slope(axes_[1][segment], u),
                slope(axes_[2][segment], u)});
            const std::int64_t weight = index == 0 || index == simpson_intervals ? 1 : index % 2 == 1 ? 4 : 2;
            sum = calc_.add(sum, calc_.mul(speed, whole(weight)));
        }
        return calc_.div(calc_.mul(sum, step), whole(3));
    }

private:
    [[nodiscard]] Axis fit_axis(const std::array<Fixed, 4>& x) {
        constexpr std::size_t last = 3;
        std::array<Fixed, 4> gamma{};
        std::array<Fixed, 4> delta{};
        std::array<Fixed, 4> slopes{};
        gamma[0] = Fixed::from_raw(Fixed::scale / 2);
        for (std::size_t index = 1; index < last; ++index) {
            gamma[index] = calc_.div(whole(1), calc_.sub(whole(4), gamma[index - 1]));
        }
        gamma[last] = calc_.div(whole(1), calc_.sub(whole(2), gamma[last - 1]));
        delta[0] = calc_.mul(calc_.mul(calc_.sub(x[1], x[0]), whole(3)), gamma[0]);
        for (std::size_t index = 1; index < last; ++index) {
            delta[index] = calc_.mul(
                calc_.sub(calc_.mul(calc_.sub(x[index + 1], x[index - 1]), whole(3)), delta[index - 1]), gamma[index]);
        }
        delta[last] = calc_.mul(
            calc_.sub(calc_.mul(calc_.sub(x[last], x[last - 1]), whole(3)), delta[last - 1]), gamma[last]);
        slopes[last] = delta[last];
        for (std::size_t index = last; index-- > 0;) {
            slopes[index] = calc_.sub(delta[index], calc_.mul(gamma[index], slopes[index + 1]));
        }
        Axis axis{};
        for (std::size_t index = 0; index < last; ++index) {
            const Fixed rise = calc_.sub(x[index + 1], x[index]);
            axis[index] = Cubic{x[index], slopes[index],
                calc_.sub(calc_.sub(calc_.mul(rise, whole(3)), calc_.mul(slopes[index], whole(2))), slopes[index + 1]),
                calc_.add(calc_.add(calc_.mul(calc_.neg(rise), whole(2)), slopes[index]), slopes[index + 1])};
        }
        return axis;
    }

    [[nodiscard]] Fixed eval(const Cubic& cubic, const Fixed u) {
        return calc_.add(calc_.mul(calc_.add(calc_.mul(calc_.add(calc_.mul(cubic.d, u), cubic.c), u), cubic.b), u), cubic.a);
    }

    [[nodiscard]] Fixed slope(const Cubic& cubic, const Fixed u) {
        return calc_.add(calc_.mul(calc_.add(calc_.mul(calc_.mul(cubic.d, whole(3)), u), calc_.mul(cubic.c, whole(2))), u),
            cubic.b);
    }

    Calc& calc_;
    std::array<Axis, 3> axes_{};
};

[[nodiscard]] Vec3 add(Calc& calc, const Vec3& left, const Vec3& right) {
    return {calc.add(left.x, right.x), calc.add(left.y, right.y), calc.add(left.z, right.z)};
}
[[nodiscard]] Vec3 sub(Calc& calc, const Vec3& left, const Vec3& right) {
    return {calc.sub(left.x, right.x), calc.sub(left.y, right.y), calc.sub(left.z, right.z)};
}
[[nodiscard]] Vec3 scale(Calc& calc, const Vec3& value, const Fixed factor) {
    return {calc.mul(value.x, factor), calc.mul(value.y, factor), calc.mul(value.z, factor)};
}
// The vector over its length; the zero vector stays zero.
[[nodiscard]] Vec3 unit(Calc& calc, const Vec3& value) {
    const Fixed length = calc.length(value);
    if (length.raw() == 0) return Vec3{};
    return {calc.div(value.x, length), calc.div(value.y, length), calc.div(value.z, length)};
}
[[nodiscard]] Vec3 cross(Calc& calc, const Vec3& left, const Vec3& right) {
    return {calc.sub(calc.mul(left.y, right.z), calc.mul(left.z, right.y)),
        calc.sub(calc.mul(left.z, right.x), calc.mul(left.x, right.z)),
        calc.sub(calc.mul(left.x, right.y), calc.mul(left.y, right.x))};
}

[[nodiscard]] Fixed frames_of(Calc& calc, const SpinAwayProfile& profile) {
    return calc.mul(profile.time, whole(logical_frames_per_second));
}

// SP-05: the path from the craft's position along its velocity for the spin time, bowed
// sideways (never downward) at a random angle by 0.3 of its length.
void build_path(Calc& calc, DeathSpin& spin, const SpinAwayProfile& profile, const std::uint64_t seed,
    const std::uint64_t frame) {
    spin.travelled = Fixed{};
    spin.path = calc.length(spin.velocity).raw() != 0;
    if (!spin.path) return;
    const Vec3 start = spin.position;
    const Vec3 run = scale(calc, spin.velocity, frames_of(calc, profile));
    const Vec3 end = add(calc, start, run);
    const Fixed half = Fixed::from_raw(Fixed::scale / 2);
    const Vec3 middle = scale(calc, add(calc, start, end), half);
    // The side axes: the middle point (a position) plus one on each axis, crossed with the run.
    // Both are taken as directions, so they are scaled to unit length first; that changes none
    // of the crossed directions.
    const Vec3 lean = unit(calc, add(calc, middle, Vec3{whole(1), whole(1), whole(1)}));
    const Vec3 along = unit(calc, run);
    const Vec3 side = unit(calc, cross(calc, lean, along));
    const Vec3 lift = unit(calc, cross(calc, side, along));
    CombatRandom draw(seed, frame, spin.unit, spin_path_slot);
    const Fixed angle = Fixed::from_raw(static_cast<std::int64_t>(draw.uniform(0, Fixed::scale - 1)));
    Vec3 bow = add(calc, scale(calc, lift, math::sin_turn(angle)), scale(calc, side, math::cos_turn(angle)));
    if (bow.z.raw() < 0) bow.z = calc.neg(bow.z);
    const Fixed reach = calc.mul(calc.length(run), Fixed::from_raw(Fixed::scale * 3 / 10));
    spin.points = {start, middle, add(calc, end, scale(calc, bow, reach)), end};
    Path path(calc);
    path.fit(spin.points);
    for (std::size_t segment = 0; segment < spin.lengths.size(); ++segment) spin.lengths[segment] = path.length(segment);
}

// SP-07: the facing turned toward `target` by at most `rate` degrees in pitch and in yaw.
void turn_toward(Calc& calc, DeathSpin& spin, const Vec3& target, const Fixed rate) {
    const Vec3 to = sub(calc, target, spin.position);
    const Fixed yaw = to.x.raw() == 0 && to.y.raw() == 0 ? Fixed{} : calc.atan2_deg(to.y, to.x);
    const Fixed pitch = to.z.raw() == 0 && to.x.raw() == 0 ? Fixed{}
                                                           : calc.neg(calc.atan2_deg(to.z, calc.length(to.x, to.y)));
    const auto limited = [&](const Fixed wanted, const Fixed current) {
        const Fixed change = clamp180(calc.sub(wanted, current));
        return std::clamp(change, calc.neg(rate), rate);
    };
    spin.pitch = clamp180(calc.add(spin.pitch, limited(pitch, spin.pitch)));
    spin.yaw = clamp180(calc.add(spin.yaw, limited(yaw, spin.yaw)));
}

} // namespace

bool spins_away(const SpinAwayProfile& profile, const std::uint64_t seed, const std::uint64_t frame,
    const EntityId unit) noexcept {
    CombatRandom draw(seed, frame, unit, spin_chance_slot);
    const auto raw = static_cast<std::int64_t>(draw.uniform(0, static_cast<std::uint32_t>(Fixed::scale)));
    return Fixed::from_raw(raw) <= profile.chance;
}

DeathSpin start_spin(const EntityId unit, const TypeId type, const PlayerId owner, const Vec3 position,
    const CraftState& state) {
    DeathSpin spin;
    spin.unit = unit;
    spin.type = type;
    spin.owner = owner;
    spin.position = position;
    spin.roll = state.roll;
    spin.pitch = state.pitch;
    spin.yaw = state.yaw;
    spin.velocity = state.velocity;
    return spin;
}

core::Result<bool> step_spin(DeathSpin& spin, const Fixed rate_of_turn, const SpinAwayProfile& profile,
    const std::uint64_t seed, const std::uint64_t frame) {
    Calc calc;
    // SP-04: a spin whose accumulated roll is zero builds its path from where it is.
    if (spin.spin_roll.raw() == 0) {
        build_path(calc, spin, profile, seed, frame);
        spin.spin_roll = spin.roll;
    }
    // SP-06: the craft runs its speed along the path each frame; it ends once it has run the
    // straight distance of its time, or at the path's end.
    const Fixed speed = calc.length(spin.velocity);
    spin.travelled = calc.add(spin.travelled, speed);
    bool ended = !spin.path || speed.raw() == 0 || spin.travelled >= calc.mul(speed, frames_of(calc, profile));
    Vec3 target{};
    if (!ended) {
        Path path(calc);
        path.fit(spin.points);
        Fixed start{};
        bool found = false;
        for (std::size_t segment = 0; segment < spin.lengths.size() && !found; ++segment) {
            const Fixed length = spin.lengths[segment];
            if (length.raw() > 0 && spin.travelled < calc.add(start, length)) {
                target = path.point(segment, calc.div(calc.sub(spin.travelled, start), length));
                found = true;
            }
            start = calc.add(start, length);
        }
        ended = !found;
    }
    if (!ended) {
        turn_toward(calc, spin, target, rate_of_turn);
        spin.spin_roll = calc.add(spin.spin_roll, roll_per_frame);
        spin.roll = clamp180(spin.spin_roll);
        spin.position = target;
    }
    if (!calc.ok()) {
        return core::Result<bool>::failure(calc.error("spinning craft " + std::to_string(spin.unit)));
    }
    return core::Result<bool>::success(ended);
}

core::Result<math::Quat> spin_rotation(const DeathSpin& spin) {
    return craft_rotation(CraftState{spin.roll, spin.pitch, spin.yaw, {}, false});
}

} // namespace eawr::sim::tactical
