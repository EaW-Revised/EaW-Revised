#pragma once

#include "eawr/sim/tactical/land_path.hpp"

namespace eawr::sim::tactical {

enum class LandContactKind : std::uint8_t { terrain, stopped_stopped, moving_stopped, moving_moving };
// LR3-08 / WLPA-56: state dispatch is retained independently of geometry and path outcomes.
enum class LandContactState : std::uint8_t {
    initial, resolving, resolved, unresolvable, retry_later, path_around, repulsion,
    existing_path, no_action, attack, pass, follower_speed, infantry_push,
    same_direction_speed, offset_exchange, deadlocked, recovery
};
enum class LandContactSearch : std::uint8_t { none, quota_denied, no_route, found, install_rejected, installed };
enum class LandContactRoute : std::uint8_t { ordinary, pass, escape_aligned, escape_opposed };

struct LandContactSettings {
    std::uint64_t complexity_quota{};
    std::uint32_t reset_interval{};
    math::Fixed prediction_seconds{}, minimum_prediction_distance{};
    friend bool operator==(const LandContactSettings&, const LandContactSettings&) = default;
};

struct LandContact {
    // Canonical member IDs are the stable contact identity; second==0 means terrain.
    EntityId first{}, second{}, primary{}, route_actor{}, recorded_member{};
    LandContactKind kind{LandContactKind::terrain};
    LandContactState state{LandContactState::initial};
    LandContactSearch search{LandContactSearch::none};
    LandContactRoute route{LandContactRoute::ordinary};
    math::Fixed score{};
    math::Vec2 terrain_target{};
    std::optional<math::Fixed> first_speed_limit, second_speed_limit;
    std::uint8_t deadlock_remaining{30}, search_passes{};
    std::uint64_t first_progress{}, second_progress{}, charged_nodes{};
    bool first_stopped{}, second_stopped{}, search_succeeded{}, installed{}, motion_observed{};
    bool escape_retry_pending{}; // first escape is ordinary service; failed retries rejoin quota admission
    friend bool operator==(const LandContact&, const LandContact&) = default;
};

// LR3-11: no elapsed-time input. Nonqualifying visits preserve the counter.
[[nodiscard]] bool land_deadlock_visit(LandContact& contact, math::Fixed first_assigned,
    math::Fixed second_assigned, math::Fixed first_collision, math::Fixed second_collision,
    math::Fixed first_acceleration, math::Fixed second_acceleration, bool pair_valid, bool progressed) noexcept;

} // namespace eawr::sim::tactical
