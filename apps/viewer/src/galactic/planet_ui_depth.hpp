#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eawr::viewer::galactic {

// #7809: the galaxy map draws each planet's controls (name, income, pips, fleet badges, status icons) as one group.
// Groups are stacked back to front by the planet's depth along the camera forward axis, so a nearer planet's group
// covers a farther planet's group. A planet body is drawn by the 3D scene beneath the whole 2D layer, so the layer
// also hides the part of a farther group that lies behind a nearer planet body: the body is a screen disc.
struct PlanetUiDepthEntry final {
    std::uint64_t id{};
    float depth{};       // distance along the camera forward axis
    float x{}, y{};      // projected planet centre in HUD pixels
    float radius{};      // projected body radius in HUD pixels; 0 when the planet draws no body
};

struct PlanetUiOccluder final { float x{}, y{}, radius{}; };

inline constexpr std::size_t max_planet_ui_occluders = 6;

struct PlanetUiDepthPlan final {
    // Planet ids, farthest first. A planet's group sits above every group before it.
    std::vector<std::uint64_t> order;
    struct Hidden final {
        std::uint64_t id{};
        std::array<PlanetUiOccluder, max_planet_ui_occluders> discs{};
        std::size_t count{};
    };
    // Planets whose controls may reach behind a nearer body, with that body's disc.
    std::vector<Hidden> hidden;
};

// `reach` is how far a planet's controls can extend from its centre, in HUD pixels: a disc farther than
// radius + reach from a planet cannot cover any of its controls.
[[nodiscard]] inline PlanetUiDepthPlan plan_planet_ui_depth(std::vector<PlanetUiDepthEntry> entries, const float reach) {
    std::sort(entries.begin(), entries.end(), [](const PlanetUiDepthEntry& a, const PlanetUiDepthEntry& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.id < b.id;
    });
    PlanetUiDepthPlan plan;
    plan.order.reserve(entries.size());
    for (const auto& entry : entries) plan.order.push_back(entry.id);
    // `entries` is farthest first, so every nearer planet comes after the planet it may hide.
    for (std::size_t rear = 0; rear < entries.size(); ++rear) {
        PlanetUiDepthPlan::Hidden hidden;
        hidden.id = entries[rear].id;
        // Nearest bodies first: they matter most when more than the limit overlap.
        for (std::size_t front = entries.size(); front-- > rear + 1 && hidden.count < max_planet_ui_occluders;) {
            const auto& body = entries[front];
            if (body.radius <= 0.0F || body.depth >= entries[rear].depth) continue;
            const float dx = body.x - entries[rear].x, dy = body.y - entries[rear].y;
            const float limit = body.radius + reach;
            if (dx * dx + dy * dy >= limit * limit) continue;
            hidden.discs[hidden.count++] = {body.x, body.y, body.radius};
        }
        if (hidden.count) plan.hidden.push_back(hidden);
    }
    return plan;
}

}
