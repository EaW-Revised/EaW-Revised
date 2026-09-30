#include "eawr/presentation/space/projectiles.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eawr::presentation::space {
namespace {

[[nodiscard]] double to_double(const sim::math::Fixed value) noexcept {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::array<double, 3> vec(const sim::math::Vec3& value) noexcept {
    return {to_double(value.x), to_double(value.y), to_double(value.z)};
}

[[nodiscard]] double degrees(const double radians) noexcept { return radians * 180.0 / std::numbers::pi; }

// [0, 360).
[[nodiscard]] double wrap(const double value) noexcept {
    double wrapped = std::fmod(value, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped >= 360.0 ? 0.0 : wrapped;
}

// A fixed 64-bit finalizer (splitmix64's), so each key spreads over the whole range.
[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t bits(const sim::math::Fixed value) noexcept { return static_cast<std::uint64_t>(value.raw()); }

} // namespace

std::optional<std::array<double, 2>> projectile_facing(const sim::tactical::Projectile& projectile) noexcept {
    if (projectile.homing) return std::array<double, 2>{wrap(to_double(projectile.yaw)), to_double(projectile.pitch)};
    const auto step = vec(projectile.step);
    const double across = std::hypot(step[0], step[1]);
    if (across == 0.0 && step[2] == 0.0) return std::nullopt;
    const double yaw = across == 0.0 ? 0.0 : wrap(degrees(std::atan2(step[1], step[0])));
    return std::array<double, 2>{yaw, -degrees(std::atan2(step[2], across))};
}

std::optional<ProjectilePose> interpolate_projectile(const sim::tactical::Projectile* before,
                                                     const sim::tactical::Projectile& latest, const double alpha) noexcept {
    const auto facing = projectile_facing(latest);
    if (!facing) return std::nullopt;
    ProjectilePose pose{vec(latest.position), (*facing)[0], (*facing)[1]};
    if (before == nullptr) return pose;
    const auto start = projectile_facing(*before);
    const double t = std::clamp(alpha, 0.0, 1.0);
    const auto from = vec(before->position);
    for (std::size_t axis = 0; axis < 3; ++axis) pose.position[axis] = from[axis] + (pose.position[axis] - from[axis]) * t;
    if (start) {
        double turn = std::fmod(pose.yaw_degrees - (*start)[0], 360.0);
        if (turn > 180.0) turn -= 360.0;
        if (turn <= -180.0) turn += 360.0;
        pose.yaw_degrees = wrap((*start)[0] + turn * t);
        pose.pitch_degrees = (*start)[1] + (pose.pitch_degrees - (*start)[1]) * t;
    }
    return pose;
}

std::optional<std::uint64_t> hit_projectile(const std::span<const sim::tactical::Projectile> before,
                                            const sim::tactical::CombatEvent& event) noexcept {
    if (event.kind != sim::tactical::CombatEventKind::projectile_hit) return std::nullopt;
    for (const sim::tactical::Projectile& projectile : before) {
        if (projectile.shooter == event.shooter && projectile.weapon == event.weapon && projectile.position == event.origin) {
            return projectile.id;
        }
    }
    return std::nullopt;
}

std::uint64_t hit_event_key(const sim::tactical::CombatEvent& event) noexcept {
    std::uint64_t key = mix(event.tick);
    for (const std::uint64_t part : {static_cast<std::uint64_t>(event.shooter), static_cast<std::uint64_t>(event.weapon),
                                     static_cast<std::uint64_t>(event.target), bits(event.origin.x), bits(event.origin.y),
                                     bits(event.origin.z)}) {
        key = mix(key ^ part);
    }
    return key;
}

std::size_t hit_particle_pick(const std::uint64_t key, const HitParticleList list, const std::size_t count) noexcept {
    if (count <= 1U) return 0U;
    return static_cast<std::size_t>(mix(key ^ (static_cast<std::uint64_t>(list) << 56U)) % count);
}

ProjectileModelSlots::ProjectileModelSlots(const std::size_t count) : slots_(count), before_bind_(count) {}

std::vector<std::optional<std::size_t>> ProjectileModelSlots::bind(const std::span<const std::uint64_t> ids) {
    std::vector<std::optional<std::size_t>> result(ids.size());
    // Remembered before this call mutates the slots: a catch-up sample's historical tick always
    // predates this frame's own, so it must see the binding this frame started from, not the one
    // it is about to compute.
    for (std::size_t index = 0; index < slots_.size(); ++index) before_bind_[index] = slots_[index].projectile;
    // A slot freed last frame has rested now; one whose projectile is gone rests this frame.
    for (Slot& slot : slots_) {
        slot.resting = false;
        if (slot.projectile && !std::binary_search(ids.begin(), ids.end(), *slot.projectile)) {
            slot.projectile.reset();
            slot.resting = true;
        }
    }
    std::size_t bound = 0;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto held = std::find_if(slots_.begin(), slots_.end(),
                                       [&](const Slot& slot) { return slot.projectile == ids[index]; });
        if (held != slots_.end()) {
            result[index] = static_cast<std::size_t>(held - slots_.begin());
            ++bound;
        }
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (result[index]) continue;
        const auto free = std::find_if(slots_.begin(), slots_.end(),
                                       [](const Slot& slot) { return !slot.projectile && !slot.resting; });
        if (free == slots_.end()) {
            ++refused_;
            continue;
        }
        free->projectile = ids[index];
        result[index] = static_cast<std::size_t>(free - slots_.begin());
        ++bindings_;
        ++bound;
    }
    max_bound_ = std::max(max_bound_, bound);
    return result;
}

std::optional<std::uint64_t> ProjectileModelSlots::bound(const std::size_t slot) const noexcept {
    return slot < slots_.size() ? slots_[slot].projectile : std::nullopt;
}

std::optional<std::uint64_t> ProjectileModelSlots::bound_before(const std::size_t slot) const noexcept {
    return slot < before_bind_.size() ? before_bind_[slot] : std::nullopt;
}

} // namespace eawr::presentation::space
