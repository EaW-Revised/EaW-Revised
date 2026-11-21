#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace godot { class Control; }
namespace eawr::sim::galactic { class GalacticSnapshot; }
namespace eawr::presentation::godot_backend {
// WGCI-3: a passive choice-list card, distinct from the actionable dialog.
struct CorruptionHovercardState {
    godot::Control* panel{};
    std::string backdrop, header, money_icon;
    std::string signature;
    float left_edge{}, spacing{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fixture;
    std::uint64_t planet{};
    // The publication, viewport and anchor the retained card was built for: while they are unchanged nothing is rebuilt
    // and no signature is computed. A weak owner compare cannot match a different publication at a reused address.
    std::weak_ptr<const sim::galactic::GalacticSnapshot> source;
    float key_width{}, key_height{}, key_scale{}, key_x{}, key_y{};
    // The one information icon whose plain tooltip is currently suppressed while its card is pending or shown.
    std::uint64_t suppressed_planet{};
};
} // namespace eawr::presentation::godot_backend
