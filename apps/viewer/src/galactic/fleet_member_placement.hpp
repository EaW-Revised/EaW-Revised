#pragma once

#include "eawr/presentation/ui/fleet_cells.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include <map>
#include <utility>

namespace eawr::viewer::galactic {
// The receive component is the visible button spanning linked footprint cells,
// so a hit anywhere in that rectangle names the button's anchor (GHUD-S87).
// Empty rectangles are hidden cells; a drop there retains its authored index.
[[nodiscard]] inline std::size_t fleet_member_target_cell(std::size_t authored,
    std::span<const presentation::ui::PixelRect> buttons, presentation::ui::ReferencePoint point) {
    for (std::size_t cell = 0; cell < buttons.size(); ++cell) {
        const auto& rect = buttons[cell];
        if (rect.width > 0 && rect.height > 0 && point.x >= rect.x && point.y >= rect.y
            && point.x < rect.right() && point.y < rect.bottom()) return cell;
    }
    return authored;
}

// GHUD-S87: a safe requested cell joins every destination unit of the true
// type. A same-type anchor is safe; its reserved footprint neighbours are not.
// Collision fallback moves only the incoming identities to the nearest free
// footprint, allowing an intentional split. Kept icons hold resolved cells.
[[nodiscard]] inline std::optional<std::pair<std::size_t, bool>> fleet_member_cell_drop(
    std::span<const std::optional<presentation::ui::FleetCellCoordinate>> cells,
    std::span<const std::optional<sim::galactic::TypeId>> types,
    std::span<const presentation::ui::FleetCellIcon> kept,
    presentation::ui::FleetCellIcon moving, sim::galactic::TypeId type, std::size_t wanted) {
    if (wanted >= cells.size() || kept.size() != types.size()) return std::nullopt;
    for (std::size_t group = 0; group < kept.size(); ++group)
        if (types[group] == type && kept[group].previous == wanted) return std::pair{wanted, true};
    std::vector<presentation::ui::FleetCellIcon> icons(kept.begin(), kept.end());
    moving.previous = wanted; moving.nearest = true; icons.push_back(moving);
    const auto layout = presentation::ui::fleet_cell_layout(cells, icons);
    if (layout.groups.empty() || !layout.groups.back()) return std::nullopt;
    const auto resolved = layout.groups.back()->cell;
    return std::pair{resolved, resolved == wanted};
}

// A successful cell drop updates the moved identities, and only a safe drop
// gathers matching destination identities. Other types and the source remainder
// retain their saved indices. No simulation state is changed.
inline void remember_fleet_member_drop(std::map<sim::EntityId, std::int64_t>& indices,
    std::span<const sim::EntityId> incoming, std::span<const sim::EntityId> matching,
    std::pair<std::size_t, bool> placement) {
    const auto index = static_cast<std::int64_t>(placement.first);
    for (const auto member : incoming) indices[member] = index;
    if (placement.second) for (const auto member : matching) indices[member] = index;
}
} // namespace eawr::viewer::galactic
