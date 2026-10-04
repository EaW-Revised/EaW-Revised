#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/motion.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Q24 helpers shared by the single-ship planner (motion.cpp) and the path finder
// (pathfind.cpp).
namespace eawr::sim::tactical::motion_detail {

[[nodiscard]] constexpr math::Fixed whole(const std::int64_t value) noexcept {
    return math::Fixed::from_raw(value * math::Fixed::scale);
}

// 2 pi rounded once to Q24 (6.283185307...).
inline constexpr math::Fixed two_pi = math::Fixed::from_raw(105414357);
// The planner's alignment tolerance in degrees (MV-16), rounded once to Q24.
inline constexpr math::Fixed aligned_degrees = math::Fixed::from_raw(167772); // 0.01
inline constexpr math::Fixed min_expansion_distance = whole(50);
inline constexpr std::int64_t coordinate_limit_raw = max_motion_coordinate * math::Fixed::scale;

// Q24 arithmetic whose first failure is sticky; callers check it once at the end.
class Calc final {
public:
    explicit Calc(math::TrigCache* trig = nullptr) noexcept : trig_(trig) {}
    // The hot operations skip the Result on success (#503); a failure takes the Result form for
    // its diagnostic, out of line (motion.cpp) so that the success path inlines (#520).
    math::Fixed add(math::Fixed left, math::Fixed right) {
        math::Fixed result;
        return math::try_add(left, right, result) ? result : failed_add(left, right);
    }
    math::Fixed sub(math::Fixed left, math::Fixed right) {
        math::Fixed result;
        return math::try_subtract(left, right, result) ? result : failed_sub(left, right);
    }
    math::Fixed mul(math::Fixed left, math::Fixed right) {
        math::Fixed result;
        return math::try_multiply(left, right, result) ? result : failed_mul(left, right);
    }
    math::Fixed div(math::Fixed left, math::Fixed right) {
        math::Fixed result;
        return math::try_divide(left, right, result) ? result : failed_div(left, right);
    }
    math::Fixed sqrt(math::Fixed value) {
        math::Fixed result;
        return math::try_sqrt(value, result) ? result : failed_sqrt(value);
    }
    math::Fixed neg(math::Fixed value) {
        return value.raw() != std::numeric_limits<std::int64_t>::min() ? math::Fixed::from_raw(-value.raw())
                                                                        : failed_neg(value);
    }
    math::Fixed abs(math::Fixed value) { return value.raw() < 0 ? neg(value) : value; }
    math::Fixed length(math::Fixed x, math::Fixed y) {
        math::Fixed result;
        return math::try_length(math::Vec2{x, y}, result) ? result : failed_length(x, y);
    }
    math::Fixed length(const math::Vec3& value) { return take(math::length(value)); }
    math::Fixed dot(math::Fixed ax, math::Fixed ay, math::Fixed bx, math::Fixed by) {
        math::Fixed result;
        return math::try_dot(math::Vec2{ax, ay}, math::Vec2{bx, by}, result) ? result : failed_dot(ax, ay, bx, by);
    }

    // Degrees in and out; the math library works in turns.
    math::Fixed sin_deg(math::Fixed degrees) {
        const auto turns = div(degrees, whole(360));
        return trig_ != nullptr ? trig_->sample(turns).sine : math::sin_turn(turns);
    }
    math::Fixed cos_deg(math::Fixed degrees) {
        const auto turns = div(degrees, whole(360));
        return trig_ != nullptr ? trig_->sample(turns).cosine : math::cos_turn(turns);
    }
    math::Fixed atan2_deg(math::Fixed y, math::Fixed x) { return mul(take(math::atan2_turn(y, x)), whole(360)); }

    [[nodiscard]] bool ok() const noexcept { return !error_; }
    [[nodiscard]] core::Diagnostic error(std::string_view what) const;

    // The rule or step that the operations in its scope belong to (#615). The first failure's
    // diagnostic names the innermost site and the failed operation's raw operands.
    class Site final {
    public:
        Site(Calc& calc, const char* name) noexcept : calc_(calc), previous_(calc.site_) { calc.site_ = name; }
        ~Site() { calc_.site_ = previous_; }
        Site(const Site&) = delete;
        Site& operator=(const Site&) = delete;

    private:
        Calc& calc_;
        const char* previous_;
    };

private:
    math::Fixed failed_add(math::Fixed left, math::Fixed right);
    math::Fixed failed_sub(math::Fixed left, math::Fixed right);
    math::Fixed failed_mul(math::Fixed left, math::Fixed right);
    math::Fixed failed_div(math::Fixed left, math::Fixed right);
    math::Fixed failed_sqrt(math::Fixed value);
    math::Fixed failed_neg(math::Fixed value);
    math::Fixed failed_length(math::Fixed x, math::Fixed y);
    math::Fixed failed_dot(math::Fixed ax, math::Fixed ay, math::Fixed bx, math::Fixed by);
    math::Fixed take(core::Result<math::Fixed> result) {
        if (!result) {
            if (!error_) {
                record(result.error(), {});
            }
            return math::Fixed{};
        }
        return result.value();
    }
    // Keeps the first failure, prefixed with the site and followed by the operation (#615).
    void record(const core::Diagnostic& failure, std::string_view operation);
    std::optional<core::Diagnostic> error_;
    const char* site_{};
    math::TrigCache* trig_{};
};

// Get_Nearest_Open_Position (AV-19): 40 rings of (int)(2 pi r / occupation) + 1 points; a zero
// occupation gives FoC one point. FoC bounds the count by nothing but the soft radius, so
// validate_motion rejects a footprint whose outer ring would exceed max_ring_points (not a
// retail limit; a point costs a collision query) and the search caps the count for a
// footprint that bypassed validation. FoC's corvette puts 5000 there at the largest increment.
inline constexpr int destination_search_rings = 40;
inline constexpr std::int64_t max_ring_points = std::int64_t{1} << 13;

[[nodiscard]] inline std::int64_t ring_points(Calc& calc, const math::Fixed radius, const math::Fixed occupation) {
    std::int64_t count = 1;
    if (occupation.raw() > 0) count += calc.div(calc.mul(two_pi, radius), occupation).raw() / math::Fixed::scale;
    return count;
}

// Degrees wrapped to [-180, 180).
[[nodiscard]] math::Fixed clamp180(math::Fixed degrees) noexcept;

struct Heading {
    math::Fixed x;
    math::Fixed y;
};

[[nodiscard]] Heading heading(Calc& calc, math::Fixed yaw);

// Adds the end-of-move speed and the braking or acceleration helper nodes (MV-18, MV-19);
// `theta` is the search's arc angle, the straightness bound of a helper leg.
void finish_path(Calc& calc, const MotionProfile& profile, math::Fixed theta, std::vector<PathNode>& nodes);
// MV-19 (and MV-18's braking node) alone: the helper nodes of straight speed-change legs.
void insert_helpers(Calc& calc, const MotionProfile& profile, math::Fixed theta, std::vector<PathNode>& nodes);

// MV-12: the path finder's trivial two-node path when the target lies in the start cell of a
// forward step of `step`; nullopt when it does not.
[[nodiscard]] std::optional<std::vector<PathNode>> trivial_path(Calc& calc, math::Fixed max_speed, math::Fixed step,
    math::Fixed start_frame, math::Vec3 position, math::Fixed yaw, math::Fixed speed, math::Vec3 target);

// The forward step of a move (research E71-03): the expansion distance, or half the move
// length (at least 50) for moves shorter than twice it; at least one frame at `max_speed`.
[[nodiscard]] math::Fixed forward_step(Calc& calc, math::Fixed expansion, math::Fixed length, math::Fixed max_speed);

[[nodiscard]] bool within_coordinates(const math::Vec3& value) noexcept;

} // namespace eawr::sim::tactical::motion_detail
