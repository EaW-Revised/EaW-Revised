#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// WGUI-73: component names author a two-digit row and column after the '_'.
// Neighbours change one coordinate, and exist only when that cell is authored.
struct FleetCellCoordinate final { int row{}; int column{}; };
inline std::optional<FleetCellCoordinate> fleet_cell_coordinate(std::string_view name) {
    const auto separator = name.rfind('_');
    if (separator == std::string_view::npos || name.size() - separator != 5) return std::nullopt;
    const auto digits = name.substr(separator + 1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return FleetCellCoordinate{(digits[0] - '0') * 10 + digits[1] - '0',
                               (digits[2] - '0') * 10 + digits[3] - '0'};
}

struct FleetCellIcon final {
    float width{};
    float height{};
    std::optional<std::size_t> previous;
    // GHUD-S69: `previous` is a drop target. When that cell is not free the group takes the nearest
    // free cell below it, then the nearest above (debug build slot collision rule).
    bool nearest{};
};
struct FleetCellPlacement final {
    std::size_t cell{};
    bool wide{};
    bool high{};
    // Shell Y points up; both portrait and count move by this offset.
    [[nodiscard]] float x_shift() const { return wide ? 22.0F : 0.0F; }
    [[nodiscard]] float y_shift() const { return high ? -22.0F : 0.0F; }
    [[nodiscard]] std::size_t hover_variant() const { return (wide ? 2U : 0U) + (high ? 1U : 0U); }
};
struct FleetCellLayout final {
    std::vector<std::optional<FleetCellPlacement>> groups;
    // Every reserved neighbour retains its owning observed group.
    std::vector<std::optional<std::size_t>> owners;
    bool compact{};
};

// Presentation admission only. The supplied observed groups are never regrouped.
// WGUI-73: preserve valid slots, retry doubled first-fit, then compact if needed.
inline FleetCellLayout fleet_cell_layout(std::span<const std::optional<FleetCellCoordinate>> cells,
                                         std::span<const FleetCellIcon> icons) {
    const auto attempt = [&](bool double_cells, bool preserve) {
        FleetCellLayout out{std::vector<std::optional<FleetCellPlacement>>(icons.size()),
                            std::vector<std::optional<std::size_t>>(cells.size()), !double_cells};
        const auto neighbour = [&](std::size_t cell, int row, int column) -> std::optional<std::size_t> {
            if (!cells[cell]) return std::nullopt;
            for (std::size_t at = 0; at < cells.size(); ++at)
                if (cells[at] && cells[at]->row == cells[cell]->row + row &&
                    cells[at]->column == cells[cell]->column + column) return at;
            return std::nullopt;
        };
        const auto admit = [&](std::size_t group, std::size_t cell) {
            if (cell >= cells.size() || out.owners[cell]) return false;
            const bool wide = double_cells && icons[group].width >= 52.0F;
            const bool high = double_cells && icons[group].height >= 52.0F;
            std::array<std::optional<std::size_t>, 4> footprint{cell, std::nullopt, std::nullopt, std::nullopt};
            if (wide) footprint[1] = neighbour(cell, 0, 1);
            if (high) footprint[2] = neighbour(cell, 1, 0);
            if (wide && high && footprint[1]) footprint[3] = neighbour(*footprint[1], 1, 0);
            if ((wide && !footprint[1]) || (high && !footprint[2]) || (wide && high && !footprint[3])) return false;
            for (const auto at : footprint) if (at && out.owners[*at]) return false;
            for (const auto at : footprint) if (at) out.owners[*at] = group;
            out.groups[group] = FleetCellPlacement{cell, wide, high};
            return true;
        };
        // A dropped group claims its target only after every kept group holds its own cell.
        if (preserve) for (std::size_t group = 0; group < icons.size(); ++group)
            if (icons[group].previous && !icons[group].nearest) static_cast<void>(admit(group, *icons[group].previous));
        if (preserve) for (std::size_t group = 0; group < icons.size(); ++group) {
            if (out.groups[group] || !icons[group].nearest || !icons[group].previous) continue;
            const auto wanted = *icons[group].previous;
            bool placed = admit(group, wanted);
            for (std::size_t below = std::min(wanted, cells.size()); below-- > 0 && !placed;) placed = admit(group, below);
            for (std::size_t above = wanted + 1; above < cells.size() && !placed; ++above) placed = admit(group, above);
        }
        for (std::size_t group = 0; group < icons.size(); ++group) {
            if (out.groups[group]) continue;
            for (std::size_t cell = 0; cell < cells.size(); ++cell) if (admit(group, cell)) break;
        }
        return out;
    };
    const auto complete = [](const FleetCellLayout& layout) {
        return std::all_of(layout.groups.begin(), layout.groups.end(), [](const auto& group) { return group.has_value(); });
    };
    auto result = attempt(true, true);
    if (complete(result)) return result;
    result = attempt(true, false);
    if (complete(result)) return result;
    return attempt(false, false);
}

} // namespace eawr::presentation::ui
