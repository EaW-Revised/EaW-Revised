#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Pure model of the drawn escape flight of the campaign space retreat cinematic: where each retreating ship stands
// when the cinematic starts, how far it has flown at cinematic frame k, how bright its engines are and when it jumps
// (docs/behaviour/gc-retreat-frame-schedule.md FS-R07, FC-R04, FC-R05, RC-07).
//
// Presentation only: a function of the simulation's cinematic frame k, the entry marker geometry and the two faction
// numbers. The simulation keeps every retreating ship where it stood (RC-07); this model only says where the viewer
// draws it. It owns no engine type, keeps no state and reads no clock. Coordinates are source coordinates (X, Y on
// the map plane, Z up).
namespace eawr::presentation::camera {

namespace retreat_flight_rules {
// FC-R04, debug build. Every member moves this many units per flight call below frame 30 and, outside the
// accelerated prefix, after it.
inline constexpr float base_step = 8.125F;
inline constexpr std::uint32_t accelerate_from_frame = 30; // FC-R04: the first accelerated call
inline constexpr std::uint32_t flight_end_frame = 90;      // FC-R04: no movement at k >= 90
inline constexpr std::uint32_t brighten_from_frame = 20;   // FC-R05: member n brightens at k = 20 + n
inline constexpr float brighten_seconds = 0.2F;            // FC-R05: the engines rise to full in 0.2 seconds
inline constexpr float logical_frames_per_second = 30.0F;
inline constexpr float rest_brightness = 0.2F;             // BP-45: a ship at rest, where the rise starts
inline constexpr float staging_radius = 2500.0F;           // FS-R07: free space within this distance of the marker
inline constexpr float order_probe_distance = 10000.0F;    // FS-R07: departure order is distance to this point
// The debug-build faction defaults of the two consumed tags (WR-62, debug build EFS-R10, in walks/reinforcements.md;
// FC-R04 states the tags, not these numbers): used by a faction that does not author them.
inline constexpr float default_increment = 250.0F;
inline constexpr std::int32_t default_wait_frames = 2;
} // namespace retreat_flight_rules

// The faction movement numbers: Space_Retreat_Flight_Move_Increment and Space_Retreat_Unit_Increment_Wait_Frames.
struct RetreatFlightRates final {
    float increment{retreat_flight_rules::default_increment};
    std::int32_t wait_frames{retreat_flight_rules::default_wait_frames};
};

struct RetreatFlightMember final {
    std::uint64_t id{};
    float radius{};     // the ship's footprint radius, for the staging search
    float max_health{}; // maximum tactical health: higher is staged first (FS-R07)
};

struct RetreatFlightGeometry final {
    std::array<float, 3> entry{};
    // FS-R07: the marker's local negative X turned by its pitch and yaw, unit length. The ships face along it.
    std::array<float, 3> departure_direction{1.0F, 0.0F, 0.0F};
};

struct RetreatFlightShip final {
    std::uint64_t id{};
    std::array<float, 3> staged{}; // where it stands at k = 0
};

namespace retreat_flight_detail {

[[nodiscard]] inline float planar_distance(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}

// The flight service at call j (1-based cinematic frame): the step each member takes, and the accelerated prefix
// used by it. Mirrors FC-R04: the prefix and wait initialize at call 30 (prefix 0, wait 0); a call moves with the
// existing prefix, then the wait drops by one frame and, once spent, the prefix grows by at most one member and the
// wait reloads (capped at the last member).
struct FlightClock final {
    std::int32_t prefix{};
    std::int32_t wait{};
};

inline void advance_clock(FlightClock& clock, const std::int32_t wait_frames, const std::size_t count) noexcept {
    --clock.wait;
    if (clock.wait <= 0) {
        clock.prefix = std::min<std::int32_t>(clock.prefix + 1, static_cast<std::int32_t>(count) - 1);
        clock.wait = wait_frames;
    }
}

} // namespace retreat_flight_detail

// FS-R07: stands every member within the staging radius of the marker without overlap (greedy hexagonal rings
// around it), the member with the higher maximum health first (ties keep the roster order), then orders the result
// for departure by increasing distance to the point 10000 units along the facing (ties keep the staging order).
// Members that cannot be fitted share the outermost ring. The result's order is the departure roster order n.
[[nodiscard]] inline std::vector<RetreatFlightShip> plan_retreat_flight(
    const std::span<const RetreatFlightMember> members, const RetreatFlightGeometry& geometry) {
    std::vector<RetreatFlightShip> plan;
    if (members.empty()) return plan;
    std::vector<std::size_t> order(members.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
        return members[a].max_health > members[b].max_health;
    });
    float widest = 1.0F;
    for (const auto& member : members) widest = std::max(widest, member.radius);
    const float spacing = std::max(2.0F * widest, 50.0F);
    const int rings = std::max(1, static_cast<int>(std::floor(retreat_flight_rules::staging_radius / spacing)));
    constexpr float two_pi = 6.283185307179586F;

    struct Placed { std::array<float, 3> at; float radius; };
    std::vector<Placed> placed;
    placed.reserve(members.size());
    plan.reserve(members.size());
    std::size_t overflow = 0;
    for (const std::size_t index : order) {
        const auto& member = members[index];
        const float radius = std::max(member.radius, 0.0F);
        std::optional<std::array<float, 3>> chosen;
        for (int ring = 0; ring <= rings && !chosen; ++ring) {
            const int points = ring == 0 ? 1 : 6 * ring;
            const float ring_radius = static_cast<float>(ring) * spacing;
            for (int point = 0; point < points && !chosen; ++point) {
                const float angle = two_pi * static_cast<float>(point) / static_cast<float>(points);
                const std::array<float, 3> candidate{geometry.entry[0] + ring_radius * std::cos(angle),
                    geometry.entry[1] + ring_radius * std::sin(angle), geometry.entry[2]};
                if (retreat_flight_detail::planar_distance(candidate, geometry.entry) > retreat_flight_rules::staging_radius) continue;
                const bool free = std::none_of(placed.begin(), placed.end(), [&](const Placed& other) {
                    return retreat_flight_detail::planar_distance(candidate, other.at) < radius + other.radius;
                });
                if (free) chosen = candidate;
            }
        }
        if (!chosen) {
            // No free spot left inside the radius: spread the remainder around the outermost ring.
            const float angle = two_pi * static_cast<float>(overflow++) * 0.381966F;
            chosen = std::array<float, 3>{geometry.entry[0] + retreat_flight_rules::staging_radius * std::cos(angle),
                geometry.entry[1] + retreat_flight_rules::staging_radius * std::sin(angle), geometry.entry[2]};
        }
        placed.push_back({*chosen, radius});
        plan.push_back({member.id, *chosen});
    }
    const std::array<float, 3> probe{
        geometry.entry[0] + geometry.departure_direction[0] * retreat_flight_rules::order_probe_distance,
        geometry.entry[1] + geometry.departure_direction[1] * retreat_flight_rules::order_probe_distance,
        geometry.entry[2] + geometry.departure_direction[2] * retreat_flight_rules::order_probe_distance};
    const auto distance_to_probe = [&](const RetreatFlightShip& ship) {
        const float dx = ship.staged[0] - probe[0];
        const float dy = ship.staged[1] - probe[1];
        const float dz = ship.staged[2] - probe[2];
        return dx * dx + dy * dy + dz * dz;
    };
    std::stable_sort(plan.begin(), plan.end(), [&](const RetreatFlightShip& a, const RetreatFlightShip& b) {
        return distance_to_probe(a) < distance_to_probe(b);
    });
    return plan;
}

namespace retreat_flight_detail {

// The step of member `n` at flight call `call` given the clock as that call finds it.
[[nodiscard]] inline float step_with(const std::size_t n, const std::uint32_t call, const FlightClock& clock,
    const RetreatFlightRates& rates) noexcept {
    using namespace retreat_flight_rules;
    if (call < 1 || call >= flight_end_frame) return 0.0F;
    if (call < accelerate_from_frame) return base_step;
    return static_cast<std::int32_t>(n) <= clock.prefix ? rates.increment : base_step;
}

} // namespace retreat_flight_detail

// The step member `n` of `count` takes at flight call `call` (1-based cinematic frame). Zero from call 90 on.
[[nodiscard]] inline float retreat_flight_step(const std::size_t n, const std::size_t count, const std::uint32_t call,
    const RetreatFlightRates& rates) noexcept {
    if (count == 0) return 0.0F;
    retreat_flight_detail::FlightClock clock;
    for (std::uint32_t at = retreat_flight_rules::accelerate_from_frame; at < call; ++at)
        retreat_flight_detail::advance_clock(clock, rates.wait_frames, count);
    return retreat_flight_detail::step_with(n, call, clock, rates);
}

// The first call at which member `n` takes an accelerated step (FC-R04: stock wait 1 gives 30 + n), or nullopt
// when the shared frame-90 gate ends the flight before the prefix reaches it. Also the frame of its jump sound
// (FC-R05).
[[nodiscard]] inline std::optional<std::uint32_t> retreat_flight_accelerates_at(const std::size_t n,
    const std::size_t count, const RetreatFlightRates& rates) noexcept {
    using namespace retreat_flight_rules;
    retreat_flight_detail::FlightClock clock;
    for (std::uint32_t call = accelerate_from_frame; call < flight_end_frame && count != 0; ++call) {
        if (static_cast<std::int32_t>(n) <= clock.prefix) return call;
        retreat_flight_detail::advance_clock(clock, rates.wait_frames, count);
    }
    return std::nullopt;
}

// The distance member `n` has flown along the departure direction at cinematic frame `k`, plus `fraction` of the
// next call's step (the presented time between two simulation frames), in units. The simulation does not move the
// ships (RC-07); this is the drawn displacement.
[[nodiscard]] inline float retreat_flight_distance(const std::size_t n, const std::size_t count, const std::uint32_t k,
    const RetreatFlightRates& rates, const float fraction = 0.0F) noexcept {
    using namespace retreat_flight_rules;
    if (count == 0) return 0.0F;
    const std::uint32_t last = std::min(k, flight_end_frame - 1);
    float distance = 0.0F;
    retreat_flight_detail::FlightClock clock;
    for (std::uint32_t call = 1; call <= last; ++call) {
        distance += retreat_flight_detail::step_with(n, call, clock, rates);
        if (call >= accelerate_from_frame) retreat_flight_detail::advance_clock(clock, rates.wait_frames, count);
    }
    if (k + 1 < flight_end_frame)
        distance += std::clamp(fraction, 0.0F, 1.0F) * retreat_flight_detail::step_with(n, k + 1, clock, rates);
    return distance;
}

// FC-R05: the engine brightness member `n` shows at frame `k` (plus `fraction` of a frame), or nullopt while the
// ship's own brightness stands. Member n starts to brighten at k = 20 + n, only while that frame is before 90, and
// rises from the at-rest value to full over 0.2 seconds, then holds.
[[nodiscard]] inline std::optional<float> retreat_engine_brightness(const std::size_t n, const std::uint32_t k,
    const float fraction = 0.0F) noexcept {
    using namespace retreat_flight_rules;
    const std::uint32_t start = brighten_from_frame + static_cast<std::uint32_t>(n);
    if (start >= flight_end_frame || k < start) return std::nullopt;
    const float elapsed_frames = static_cast<float>(k - start) + std::clamp(fraction, 0.0F, 1.0F);
    const float ramp = std::clamp(elapsed_frames / (brighten_seconds * logical_frames_per_second), 0.0F, 1.0F);
    return rest_brightness + (1.0F - rest_brightness) * ramp;
}

// The Unlocked camera's object fade (FS-R09): full opacity up to `fade_begin` from the camera, none from `fade_end`,
// linear between. A degenerate range (end <= begin) cuts at `fade_begin`.
[[nodiscard]] inline float retreat_object_fade(const float camera_distance, const float fade_begin,
    const float fade_end) noexcept {
    if (camera_distance <= fade_begin) return 1.0F;
    if (!(fade_end > fade_begin) || camera_distance >= fade_end) return 0.0F;
    return (fade_end - camera_distance) / (fade_end - fade_begin);
}

// The yaw, in degrees on the map plane, of a ship that faces along the departure direction.
[[nodiscard]] inline float retreat_flight_yaw_degrees(const std::array<float, 3>& direction) noexcept {
    return std::atan2(direction[1], direction[0]) * 57.29577951308232F;
}

} // namespace eawr::presentation::camera
