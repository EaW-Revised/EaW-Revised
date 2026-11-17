#pragma once

// The galactic cinematic tour (#6291): what the camera does while the player's cinematic camera runs on the galaxy map
// (the camera button, or the battle-camera key). The planets are visited in a shuffled order, three eased camera keys
// each, seven seconds apart: a far view from a low angle, a pass close to the planet, the far view from the opposite
// side; the target stays on the planet. Engine-free pure maths: the host feeds it the planets and the elapsed game time
// and draws the pose. Rules: docs/behaviour/walks/gc-cinematic-tour.md (GCT-n). The shape of the keys comes from the
// debug build; the eased velocity profile and the distances' scale are not observed (GCT-12).

#include "galaxy_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace eawr::viewer::galactic {

// Deterministic given its seed; the host seeds it from the wall clock, so a tour differs each time.
class TourRandom final {
public:
    explicit TourRandom(std::uint32_t seed) : state_(seed) {}
    [[nodiscard]] std::uint32_t next() noexcept {
        state_ = state_ * 1664525U + 1013904223U;
        return state_ >> 8U;
    }
    // 0 .. count - 1
    [[nodiscard]] std::uint32_t below(std::uint32_t count) noexcept { return count == 0U ? 0U : next() % count; }
    // low .. high, both included
    [[nodiscard]] std::int32_t between(std::int32_t low, std::int32_t high) noexcept {
        return low + static_cast<std::int32_t>(below(static_cast<std::uint32_t>(high - low + 1)));
    }

private:
    std::uint32_t state_;
};

struct TourPlanet final {
    ObjectId id{};
    Vec3 position;
    float extent{}; // the length of the planet model's half extents
};

struct TourPose final {
    Vec3 eye;
    Vec3 target;
    friend constexpr bool operator==(const TourPose&, const TourPose&) noexcept = default;
};

// Every window or menu that takes the galaxy map's input while it is open. ONE set: the tour's start and the pointer confinement
// both ask it (GalacticMode::State::galaxy_modal_open), so a new modal is added here once. The size check below forces whoever
// adds a field to update `any` and its contract case.
struct GalaxyModals final {
    bool retreat_dialog{}, save_menu{}, campaign_score{}, battle_prompt{}, story_summary{}, reference_pages{};
    bool hero_menu{}, black_market{}, weapon_menu{}, corruption_menu{}, action_menu{};
    [[nodiscard]] constexpr bool any() const noexcept {
        return retreat_dialog || save_menu || campaign_score || battle_prompt || story_summary || reference_pages || hero_menu
            || black_market || weapon_menu || corruption_menu || action_menu;
    }
};
static_assert(sizeof(GalaxyModals) == 11U, "a modal was added to GalaxyModals: update any() and the contract case");

// GCT-01: when the tour may play. The original's start refuses while a battle is being resolved (a pending battle prompt);
// the remake also refuses while the game is paused, because the bands hide the GUI and with it the pause banner and its
// Resume Game button (**unverified**: the original's paused look under the cinematic GUI is not known).
struct TourGates final {
    bool map_showing{}; // the galaxy map is the mode on screen (no battle, no loading screen)
    bool paused{};
    bool prompt_up{};   // any galaxy modal is open (GalaxyModals::any)
};
[[nodiscard]] constexpr bool tour_may_play(const TourGates& gates) noexcept {
    return gates.map_showing && !gates.paused && !gates.prompt_up;
}

// DEV-13 (#6291 follow-up): the Space binding, the player's Options > Tweaks choice ("Space pauses the galactic game", on by
// default). Off is the original's: Space starts the tour on its release and never pauses. On: Space only pauses (on its press)
// and never starts the tour; the tour stays on the camera button. Exactly one of the two, whatever the setting.
struct SpaceKey final {
    bool pauses{};
    bool starts_tour{};
};
[[nodiscard]] constexpr SpaceKey space_key(const bool space_pauses_setting) noexcept {
    return {space_pauses_setting, !space_pauses_setting};
}

class GalacticTour final {
public:
    // GCT-04: one key every seven seconds of game time (the original counts logical frames).
    static constexpr double seconds_per_key = 7.0;
    // GCT-05: the far views sit between 15 and 30 degrees above the galaxy plane.
    static constexpr std::int32_t far_elevation_min_degrees = 15;
    static constexpr std::int32_t far_elevation_max_degrees = 30;
    // GCT-05: the close pass rides a ring at this many extent lengths around the planet.
    static constexpr float close_pass_extents = 5.0F;

    // GCT-02/03: nullopt when there is nothing to visit. `selected` is the selected planet, if any: it is moved to the
    // front of the shuffled list and, as in the original, the visit starts with the planet after it (GCT-12).
    // `galaxy_extent` is the length of the half extents of the union of all planet boxes: the far views' distance.
    [[nodiscard]] static std::optional<GalacticTour> begin(std::vector<TourPlanet> planets, std::optional<ObjectId> selected,
                                                           float galaxy_extent, TourRandom& random) {
        if (planets.size() < 2U || !(galaxy_extent > 0.0F)) return std::nullopt;
        // The original's shuffle: swap every element with a random one at or before it.
        for (std::size_t i = planets.size() - 1U; i > 0U; --i)
            std::swap(planets[i], planets[random.below(static_cast<std::uint32_t>(i + 1U))]);
        if (selected) {
            const auto at = std::find_if(planets.begin(), planets.end(), [&](const TourPlanet& p) { return p.id == *selected; });
            if (at != planets.end()) std::iter_swap(planets.begin(), at);
        }
        GalacticTour tour;
        float sign = 1.0F;
        std::size_t key = 0U;
        for (std::size_t i = 1U; i < planets.size(); ++i, key += 3U) {
            const TourPlanet& planet = planets[i];
            const float ring_angle = radians(static_cast<float>(random.between(0, 360)));
            const float azimuth = ring_angle + radians(15.0F);
            const float elevation = radians(static_cast<float>(random.between(far_elevation_min_degrees, far_elevation_max_degrees)));
            const Vec3 pullback{sign * galaxy_extent * std::cos(azimuth) * std::cos(elevation),
                           sign * galaxy_extent * std::sin(azimuth) * std::cos(elevation),
                           sign * galaxy_extent * std::sin(elevation)};
            const Vec3 ring{planet.extent * close_pass_extents * std::cos(ring_angle),
                            planet.extent * close_pass_extents * std::sin(ring_angle), 0.0F};
            const double at = static_cast<double>(key) * seconds_per_key;
            tour.eye_.push_back({at, add(planet.position, pullback), 0.5F, 0.5F});
            tour.eye_.push_back({at + seconds_per_key, add(planet.position, ring), 0.0F, 0.0F});
            tour.eye_.push_back({at + 2.0 * seconds_per_key, sub(planet.position, pullback), 0.0F, 0.0F});
            tour.target_.push_back({at, planet.position, 0.25F, 0.25F});
            tour.target_.push_back({at + 2.0 * seconds_per_key, planet.position, 0.25F, 0.25F});
            tour.visited_.push_back(planet.id);
            sign = -sign;
        }
        tour.duration_ = static_cast<double>(key) * seconds_per_key;
        return tour;
    }

    // GCT-06: how long the tour plays before a new one starts.
    [[nodiscard]] double duration() const noexcept { return duration_; }
    // The planets visited, in order (the first of the shuffled list is not one of them: GCT-12).
    [[nodiscard]] const std::vector<ObjectId>& visited() const noexcept { return visited_; }
    // The planet the camera is looking at `seconds` into the tour.
    [[nodiscard]] ObjectId visiting(double seconds) const noexcept {
        if (visited_.empty()) return 0;
        const double slot = std::clamp(seconds, 0.0, duration_) / (3.0 * seconds_per_key);
        return visited_[std::min(visited_.size() - 1U, static_cast<std::size_t>(slot))];
    }
    [[nodiscard]] TourPose pose(double seconds) const {
        return {evaluate(eye_, seconds), evaluate(target_, seconds)};
    }

private:
    struct Key final {
        double seconds{};
        Vec3 position;
        float ease_out{}; // the share of the segment after this key spent speeding up
        float ease_in{};  // the share of the segment before this key spent slowing down
    };
    GalacticTour() = default;

    [[nodiscard]] static float radians(float degrees) noexcept { return degrees * std::numbers::pi_v<float> / 180.0F; }
    [[nodiscard]] static Vec3 add(const Vec3& a, const Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    [[nodiscard]] static Vec3 sub(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

    // GCT-12: a trapezoid speed profile over a segment: speeding up over `start`, slowing down over `end` (shares of the
    // segment), steady between. 0 and 0 is a straight line at constant speed.
    [[nodiscard]] static double eased(double u, double start, double end) noexcept {
        u = std::clamp(u, 0.0, 1.0);
        const double total = start + end;
        if (total > 1.0) { start /= total; end /= total; }
        const double top = 1.0 / (1.0 - 0.5 * (start + end));
        if (start > 0.0 && u < start) return top * u * u / (2.0 * start);
        if (end > 0.0 && u > 1.0 - end) return 1.0 - top * (1.0 - u) * (1.0 - u) / (2.0 * end);
        return top * (0.5 * start + (u - start));
    }

    [[nodiscard]] static Vec3 evaluate(const std::vector<Key>& keys, double seconds) {
        if (keys.empty()) return {};
        if (seconds <= keys.front().seconds) return keys.front().position;
        if (seconds >= keys.back().seconds) return keys.back().position;
        const auto next = std::upper_bound(keys.begin(), keys.end(), seconds, [](double s, const Key& k) { return s < k.seconds; });
        const Key& b = *next;
        const Key& a = *(next - 1);
        const double span = b.seconds - a.seconds;
        const float s = span > 0.0 ? static_cast<float>(eased((seconds - a.seconds) / span, a.ease_out, b.ease_in)) : 1.0F;
        return {a.position.x + (b.position.x - a.position.x) * s, a.position.y + (b.position.y - a.position.y) * s,
                a.position.z + (b.position.z - a.position.z) * s};
    }

    std::vector<Key> eye_;
    std::vector<Key> target_;
    std::vector<ObjectId> visited_;
    double duration_{};
};

} // namespace eawr::viewer::galactic
