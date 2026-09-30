#include "space_fog.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace eawr::presentation::godot_backend::space_fog {

std::string validate_admission(const std::span<const std::string> types) {
    if (types.empty()) return "space fog requires at least one --eawr-space-fog-admit <XML element type>";
    if (types.size() > max_admitted_types) {
        return "at most " + std::to_string(max_admitted_types) + " --eawr-space-fog-admit types are accepted";
    }
    for (std::size_t index = 0; index < types.size(); ++index) {
        const std::string& type = types[index];
        const bool valid = !type.empty() && std::all_of(type.begin(), type.end(), [](const char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '_';
        });
        if (!valid) return "--eawr-space-fog-admit needs an XML element name of letters, digits and underscores";
        if (std::find(types.begin(), types.begin() + static_cast<std::ptrdiff_t>(index), type)
            != types.begin() + static_cast<std::ptrdiff_t>(index)) {
            return "--eawr-space-fog-admit type " + type + " is declared twice";
        }
    }
    return {};
}

std::string_view to_string(const Decision decision) noexcept {
    switch (decision) {
    case Decision::admitted: return "admitted";
    case Decision::admitted_blocked: return "admitted_blocked";
    case Decision::not_admitted: return "not_admitted";
    case Decision::uncatalogued: return "uncatalogued";
    }
    return "uncatalogued";
}

std::vector<PlacementDecision> classify(const scene::Scene& scene,
    const std::function<std::optional<std::string>(std::string_view object_id)>& type_of,
    const std::span<const std::string> admitted_types) {
    std::vector<PlacementDecision> result;
    result.reserve(scene.placements.size());
    for (const scene::Placement& placement : scene.placements) {
        PlacementDecision entry;
        entry.scene_ordinal = placement.scene_ordinal;
        entry.object_id = placement.object_id;
        const auto type = placement.object_id.empty() ? std::nullopt : type_of(placement.object_id);
        if (!type) {
            entry.decision = Decision::uncatalogued;
            result.push_back(std::move(entry));
            continue;
        }
        entry.type_name = *type;
        if (std::find(admitted_types.begin(), admitted_types.end(), *type) == admitted_types.end()) {
            entry.decision = Decision::not_admitted;
            result.push_back(std::move(entry));
            continue;
        }
        // An admitted unit is composed whole or the run fails: a partly drawn
        // unit would make its fog evidence unattributable.
        if (!placement.drawable()) entry.reasons.push_back("placement is not drawable");
        for (const scene::Issue& issue : placement.issues) {
            if (scene::blocks_drawing(issue.cause) || issue.cause == scene::Cause::shader_unsupported) {
                entry.reasons.push_back(std::string(scene::to_string(issue.cause)) + " " + issue.detail);
            }
        }
        for (std::size_t index = 0; index < placement.surfaces.size(); ++index) {
            const scene::Surface& surface = placement.surfaces[index];
            if (!surface.supported || scene::find_legacy_selector(surface.shader) == nullptr) {
                entry.reasons.push_back("surface " + std::to_string(index) + " shader " + surface.shader
                                        + " has no supported legacy selector");
            }
        }
        if (placement.surfaces.empty()) entry.reasons.push_back("placement has no surface");
        entry.decision = entry.reasons.empty() ? Decision::admitted : Decision::admitted_blocked;
        result.push_back(std::move(entry));
    }
    return result;
}

namespace {

struct CellSpan final {
    std::uint32_t first{};
    std::uint32_t last{};
};

// Cells of one axis that the half-open extent [low, high) covers, clamped to
// the grid; a degenerate extent (low == high) is the single point low. The
// cell on the far side of an edge that high lands on is not covered.
[[nodiscard]] std::optional<CellSpan> cells_of(const double low, const double high, const double origin,
                                               const double size, const std::uint32_t dimension) noexcept {
    const double first = std::floor((low - origin) / size);
    const double last = high > low ? std::ceil((high - origin) / size) - 1.0 : first;
    const double top = static_cast<double>(dimension) - 1.0;
    if (last < 0.0 || first > top) return std::nullopt;
    return CellSpan{static_cast<std::uint32_t>(std::clamp(first, 0.0, top)),
                    static_cast<std::uint32_t>(std::clamp(last, 0.0, top))};
}

} // namespace

bool can_reveal(const sim::fog::FogGrid* grid, const SourceBounds& bounds) noexcept {
    if (grid == nullptr) return false;
    const auto& desc = grid->desc();
    constexpr double q24 = 16777216.0;
    const double origin_x = static_cast<double>(desc.origin_x_raw) / q24;
    const double origin_y = static_cast<double>(desc.origin_y_raw) / q24;
    const double cell_x = static_cast<double>(desc.cell_x_raw) / q24;
    const double cell_y = static_cast<double>(desc.cell_y_raw) / q24;
    if (!(bounds.min_x <= bounds.max_x) || !(bounds.min_y <= bounds.max_y)) return false;
    const auto columns = cells_of(bounds.min_x, bounds.max_x, origin_x, cell_x, desc.width);
    const auto rows = cells_of(bounds.min_y, bounds.max_y, origin_y, cell_y, desc.height);
    if (!columns || !rows) return false;
    for (std::uint32_t y = rows->first; y <= rows->last; ++y) {
        for (std::uint32_t x = columns->first; x <= columns->last; ++x) {
            if (grid->cell(x, y).value_or(0) != 0) return true;
        }
    }
    return false;
}

} // namespace eawr::presentation::godot_backend::space_fog
