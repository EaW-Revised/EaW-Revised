#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace eawr::presentation::godot_backend {

enum class BattlecamAdvance { exit, hold, update, next };

// BCAM-16: pause freezes the shot; ending the battle restores the tactical view.
[[nodiscard]] inline BattlecamAdvance battlecam_advance(bool playing, bool paused, bool expired, bool held) noexcept {
    if (!playing) return BattlecamAdvance::exit;
    if (paused) return BattlecamAdvance::hold;
    return expired && !held ? BattlecamAdvance::next : BattlecamAdvance::update;
}

// Restore every presentation surface together, even when the battle is ending.
template<class Camera, class Fog, class Hud>
inline void restore_battlecam(Camera camera, Fog fog, Hud hud, std::uint64_t& subject, bool& key, bool& tab) {
    camera();
    fog();
    hud();
    subject = 0;
    key = false;
    tab = false;
}

// BCAM-10: presentation-local random stream, never shared with the simulation.
class BattlecamRandom final {
public:
    explicit BattlecamRandom(std::uint32_t seed) : state_(seed) {}
    [[nodiscard]] std::uint32_t next() noexcept {
        state_ = state_ * 1664525U + 1013904223U;
        return state_;
    }
    [[nodiscard]] float fraction() noexcept {
        return static_cast<float>(next() >> 8U) / 16777215.0F;
    }
    [[nodiscard]] float between(float low, float high) noexcept { return low + (high - low) * fraction(); }
private:
    std::uint32_t state_;
};

struct BattlecamChase final {
    std::array<float, 3> start{}, end{};
    float duration{};

    [[nodiscard]] static BattlecamChase make(float radius, BattlecamRandom& random) noexcept {
        const bool side = (random.next() & 0x80000000U) != 0U;
        const float degrees = random.between(side ? 135.0F : -45.0F, side ? 225.0F : 45.0F);
        const float angle = degrees * std::numbers::pi_v<float> / 180.0F;
        // BCAM-10: the 50/50 variant choice is provisional pending a trace.
        const bool passing = (random.next() & 0x80000000U) != 0U;
        const float radial = (passing ? 3.0F : 6.0F) * radius;
        const float x = radial * std::cos(angle);
        const float z = radial * std::sin(angle);
        // BCAM-10: axis assignment is provisional; drift and spiral are disabled
        // pending the trace/capture recorded in the battlecam walk.
        if (passing) return {{x, -random.between(6.0F, 8.0F) * radius, z},
                             {x, random.between(2.0F, 4.0F) * radius, z}, 4.0F};
        return {{x, random.between(3.0F, 7.0F) * radius, z},
                {x, random.between(2.0F, 9.0F) * radius, z}, random.between(2.0F, 4.0F)};
    }
    [[nodiscard]] std::array<float, 3> at(float elapsed) const noexcept {
        const float t = std::clamp(elapsed / duration, 0.0F, 1.0F);
        std::array<float, 3> result{};
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = start[i] + (end[i] - start[i]) * t;
        return result;
    }
};
} // namespace eawr::presentation::godot_backend
