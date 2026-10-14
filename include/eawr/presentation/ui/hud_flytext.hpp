#pragma once

#include "eawr/presentation/ui/hud_shell.hpp"
#include <cmath>

namespace eawr::presentation::ui {

// HCB-64: presentation metadata only. The bounty producer owns eligibility,
// accounting and the retained victim position; callers supply logical frames.
struct HudFlyTextView {
    std::string text;
    std::uint64_t created_frame{};
    assets::Vec2f projected_position; // screen pixels, camera projection supplied by caller
    bool behind_camera{};
};

struct HudFlyTextPresentation {
    std::string_view text; // borrows the caller's view; renderers retain their own copy
    assets::Vec2f position;
    std::uint8_t alpha{};
};

[[nodiscard]] inline std::optional<HudIconText> hud_flytext_style(const data::ui::CommandBarCatalog& catalog) {
    const auto* component = catalog.find("generic_flytext");
    if (!component || component->type != data::ui::ComponentType::text_button
        || !component->flag(data::ui::Field::no_shell)) return std::nullopt;
    return hud_readout_style(catalog, "generic_flytext");
}

[[nodiscard]] inline std::optional<HudFlyTextPresentation> hud_flytext_presentation(
    const HudFlyTextView& view, const std::uint64_t frame) noexcept {
    if (view.text.empty() || view.behind_camera || frame < view.created_frame
        || !std::isfinite(view.projected_position.x) || !std::isfinite(view.projected_position.y)) return std::nullopt;
    const auto age = frame - view.created_frame;
    // HCB-64: these constants are debug-build presentation values, not XML timers.
    if (age > 60) return std::nullopt;
    const auto faded_frames = age > 30 ? age - 30 : 0;
    return HudFlyTextPresentation{view.text,
        {view.projected_position.x, view.projected_position.y - static_cast<float>(age) * (70.0F / 60.0F)},
        static_cast<std::uint8_t>(255.0 * static_cast<double>(30 - faded_frames) / 30.0)};
}

} // namespace eawr::presentation::ui
