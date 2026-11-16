#include "eawr/sim/tactical/tsw.hpp"

#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/tactical/motion.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::tactical {

namespace {

using math::Fixed;
using math::Vec3;

core::Result<Vec3> delta(const Vec3& to, const Vec3& from) {
    const auto dx = math::subtract(to.x, from.x);
    if (!dx) return core::Result<Vec3>::failure(dx.error());
    const auto dy = math::subtract(to.y, from.y);
    if (!dy) return core::Result<Vec3>::failure(dy.error());
    const auto dz = math::subtract(to.z, from.z);
    if (!dz) return core::Result<Vec3>::failure(dz.error());
    return core::Result<Vec3>::success(Vec3{dx.value(), dy.value(), dz.value()});
}

} // namespace

core::Result<std::uint32_t> tsw_select_target(const TswSetup& setup, const FactionId own_faction, const Vec3& from,
    const std::span<const std::uint32_t> destroyed) {
    std::uint32_t best = 0;
    Fixed best_distance{};
    for (std::size_t index = 0; index < setup.targets.size(); ++index) {
        const auto& target = setup.targets[index];
        if (own_faction != 0 && target.faction == own_faction) continue;
        if (std::find(destroyed.begin(), destroyed.end(), static_cast<std::uint32_t>(index + 1)) != destroyed.end()) continue;
        const auto offset = delta(target.position, from);
        if (!offset) return core::Result<std::uint32_t>::failure(offset.error());
        const auto distance = math::length(offset.value());
        if (!distance) return core::Result<std::uint32_t>::failure(distance.error());
        // The lowest record wins a tie, like the placement's own selection.
        if (best == 0 || distance.value() < best_distance) {
            best = static_cast<std::uint32_t>(index + 1);
            best_distance = distance.value();
        }
    }
    return core::Result<std::uint32_t>::success(best);
}

core::Result<TswApproachStep> tsw_approach_step(
    const TswProfile& profile, const Vec3& position, const Fixed speed, const Vec3& target) {
    using StepResult = core::Result<TswApproachStep>;
    const auto offset = delta(target, position);
    if (!offset) return StepResult::failure(offset.error());
    const auto distance = math::length(offset.value());
    if (!distance) return StepResult::failure(distance.error());
    if (distance.value() <= profile.attack_distance) return StepResult::success(TswApproachStep{position, speed, true});
    auto next_speed = math::add(speed, profile.max_thrust);
    if (!next_speed) return StepResult::failure(next_speed.error());
    Fixed capped = next_speed.value();
    if (capped > profile.max_speed) capped = profile.max_speed;
    const auto heading = math::normalize(offset.value());
    if (!heading) return StepResult::failure(heading.error());
    const auto sx = math::multiply(heading.value().x, capped);
    const auto sy = math::multiply(heading.value().y, capped);
    const auto sz = math::multiply(heading.value().z, capped);
    if (!sx) return StepResult::failure(sx.error());
    if (!sy) return StepResult::failure(sy.error());
    if (!sz) return StepResult::failure(sz.error());
    const auto x = math::add(position.x, sx.value());
    const auto y = math::add(position.y, sy.value());
    const auto z = math::add(position.z, sz.value());
    if (!x) return StepResult::failure(x.error());
    if (!y) return StepResult::failure(y.error());
    if (!z) return StepResult::failure(z.error());
    return StepResult::success(TswApproachStep{Vec3{x.value(), y.value(), z.value()}, capped, false});
}

Fixed tsw_decelerate(const TswProfile& profile, const Fixed speed) noexcept {
    const auto lower = speed.raw() - profile.max_thrust.raw();
    return Fixed::from_raw(lower > 0 ? lower : 0);
}

core::Result<TswApproachStep> tsw_ready_step(
    const TswProfile& profile, const Vec3& position, const Fixed speed, const Vec3& target) {
    using StepResult = core::Result<TswApproachStep>;
    const Fixed slower = tsw_decelerate(profile, speed);
    if (slower.raw() == 0) return StepResult::success(TswApproachStep{position, slower, false});
    const auto offset = delta(target, position);
    if (!offset) return StepResult::failure(offset.error());
    if (offset.value().x.raw() == 0 && offset.value().y.raw() == 0 && offset.value().z.raw() == 0)
        return StepResult::success(TswApproachStep{position, slower, false});
    const auto heading = math::normalize(offset.value());
    if (!heading) return StepResult::failure(heading.error());
    const auto sx = math::multiply(heading.value().x, slower);
    const auto sy = math::multiply(heading.value().y, slower);
    const auto sz = math::multiply(heading.value().z, slower);
    if (!sx) return StepResult::failure(sx.error());
    if (!sy) return StepResult::failure(sy.error());
    if (!sz) return StepResult::failure(sz.error());
    const auto x = math::add(position.x, sx.value());
    const auto y = math::add(position.y, sy.value());
    const auto z = math::add(position.z, sz.value());
    if (!x) return StepResult::failure(x.error());
    if (!y) return StepResult::failure(y.error());
    if (!z) return StepResult::failure(z.error());
    return StepResult::success(TswApproachStep{Vec3{x.value(), y.value(), z.value()}, slower, false});
}

core::Result<std::optional<math::Quat>> tsw_facing(const Vec3& from, const Vec3& to) {
    using FaceResult = core::Result<std::optional<math::Quat>>;
    const auto offset = delta(to, from);
    if (!offset) return FaceResult::failure(offset.error());
    const auto& d = offset.value();
    if (d.x.raw() == 0 && d.y.raw() == 0 && d.z.raw() == 0) return FaceResult::success(std::nullopt);
    const auto horizontal = math::length(math::Vec2{d.x, d.y});
    if (!horizontal) return FaceResult::failure(horizontal.error());
    // Heading (degrees) about +Z, then elevation about the local +Y axis: positive elevation (target above) tips +X
    // up, which is a negative turn about +Y.
    Fixed yaw_degrees{};
    if (horizontal.value().raw() != 0) {
        const auto turn = math::atan2_turn(d.y, d.x);
        if (!turn) return FaceResult::failure(turn.error());
        const auto degrees = math::multiply(turn.value(), Fixed::from_raw(360 * Fixed::scale));
        if (!degrees) return FaceResult::failure(degrees.error());
        yaw_degrees = degrees.value();
    }
    const auto yaw = yaw_rotation(yaw_degrees);
    if (!yaw) return FaceResult::failure(yaw.error());
    const auto elevation = math::atan2_turn(d.z, horizontal.value());
    if (!elevation) return FaceResult::failure(elevation.error());
    const Fixed half = Fixed::from_raw(-elevation.value().raw() / 2);
    const math::Quat pitch{Fixed{}, math::sin_turn(half), Fixed{}, math::cos_turn(half)};
    const auto composed = math::compose(yaw.value(), pitch);
    if (!composed) return FaceResult::failure(composed.error());
    const auto normal = math::normalize(composed.value());
    if (!normal) return FaceResult::failure(normal.error());
    return FaceResult::success(normal.value());
}

core::Result<std::uint64_t> tsw_eta_frames(const TswProfile& profile, const Vec3& position, const Vec3& target) {
    using EtaResult = core::Result<std::uint64_t>;
    const auto offset = delta(target, position);
    if (!offset) return EtaResult::failure(offset.error());
    const auto distance = math::length(offset.value());
    if (!distance) return EtaResult::failure(distance.error());
    if (distance.value() <= profile.attack_distance) return EtaResult::success(0);
    // A type that cannot move never arrives: no finite ETA, so no warning ever fires.
    if (profile.max_speed.raw() <= 0) return EtaResult::success(std::numeric_limits<std::uint64_t>::max());
    const auto remaining = math::subtract(distance.value(), profile.attack_distance);
    if (!remaining) return EtaResult::failure(remaining.error());
    const auto frames = math::divide(remaining.value(), profile.max_speed);
    if (!frames) return EtaResult::failure(frames.error());
    return EtaResult::success(static_cast<std::uint64_t>(frames.value().raw() / Fixed::scale));
}

std::uint32_t tsw_speech_crossings(
    const std::uint64_t eta_frames, std::uint32_t speech_mask, std::vector<std::uint32_t>& announce) {
    for (std::uint32_t index = 0; index < tsw_speech_threshold_count; ++index) {
        const std::uint32_t bit = 1U << index;
        if ((speech_mask & bit) != 0U) continue;
        if (eta_frames > static_cast<std::uint64_t>(tsw_speech_thresholds[index]) * logical_frames_per_second) continue;
        speech_mask |= bit;
        announce.push_back(tsw_speech_thresholds[index]);
    }
    return speech_mask;
}

} // namespace eawr::sim::tactical
