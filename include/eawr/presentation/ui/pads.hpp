#pragma once

#include "eawr/presentation/ui/production.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace eawr::presentation::ui {

// WBP-35, WSU-10: the hidden parent resolves to UC first, then its completed child.
[[nodiscard]] inline sim::EntityId pad_pick_target(const sim::EntityId pad, const bool hidden,
    const sim::EntityId constructing, const sim::EntityId constructed) noexcept {
    if (!hidden) return pad;
    return constructing != sim::invalid_entity_id ? constructing : constructed;
}

// WBP-37: neutralization shows remaining owner fill; the capture timer is never hull.
[[nodiscard]] inline double capture_fill(const double progress, const bool neutralizing) noexcept {
    return neutralizing ? 1.0 - std::clamp(progress, 0.0, 1.0) : std::clamp(progress, 0.0, 1.0);
}

// WBP-52: retain WBP-37's fade value, but bind it to the colourise constant.
// At rest the current owner supplies the colour; a transition uses remaining
// owner fill when neutralizing, and target fill when claiming a neutral pad.
[[nodiscard]] inline std::array<float, 3> capture_colorization(const double progress,
    const bool neutralizing, const std::array<std::uint8_t, 3>& colour) noexcept {
    const double fill = progress > 0 ? capture_fill(progress, neutralizing) : 1.0;
    std::array<float, 3> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<float>(1.0 - fill + fill * colour[index] / 255.0);
    }
    return result;
}

[[nodiscard]] inline double pad_time_progress(const double frame, const std::uint64_t start,
    const std::uint64_t finish) noexcept {
    if (finish <= start) return 1.0;
    return std::clamp((frame - static_cast<double>(start)) / static_cast<double>(finish - start), 0.0, 1.0);
}

// WBP-17: damage alternates follow hull, independently of the fixed finish deadline.
[[nodiscard]] inline std::uint32_t construction_alternate(const double hull_fraction,
    const std::uint32_t alternates) noexcept {
    const auto value = std::floor((static_cast<double>(alternates) + 1.0)
        * (1.0 - std::clamp(hull_fraction, 0.0, 1.0)) + 0.5);
    return static_cast<std::uint32_t>(std::clamp(value, 0.0, static_cast<double>(alternates)));
}

// WBP-08/36: only opening uses proximity. Every button click closes build mode,
// even when disabled; refresh checks credits/cooldown/availability, not occupancy.
class PadPalette final {
public:
    bool open(sim::EntityId entity, bool action_allowed) noexcept {
        if (!action_allowed || entity == sim::invalid_entity_id) return false;
        entity_ = entity;
        return true;
    }
    void close() noexcept { entity_.reset(); }
    [[nodiscard]] std::optional<sim::EntityId> entity() const noexcept { return entity_; }
    [[nodiscard]] std::optional<std::pair<sim::EntityId, sim::tactical::TypeId>> click(
        std::size_t slot, std::span<const BuildButton> buttons) noexcept {
        const auto entity = std::exchange(entity_, std::nullopt);
        if (!entity) return std::nullopt;
        const auto button = std::find_if(buttons.begin(), buttons.end(), [slot](const auto& entry) {
            return entry.slot == slot;
        });
        if (button == buttons.end() || !button->enabled) return std::nullopt;
        return std::pair{*entity, button->type};
    }
private:
    std::optional<sim::EntityId> entity_;
};

} // namespace eawr::presentation::ui
