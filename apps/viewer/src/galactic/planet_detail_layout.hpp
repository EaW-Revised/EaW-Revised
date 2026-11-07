#pragma once

#include "eawr/assets/ui_map_markers.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/types.hpp"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

enum class PlanetDetailAccess { closed, view_only, edit };
[[nodiscard]] inline bool detail_pads_visible(PlanetDetailAccess access) { return access != PlanetDetailAccess::closed; }
[[nodiscard]] inline bool detail_can_edit(PlanetDetailAccess access) { return access == PlanetDetailAccess::edit; }
[[nodiscard]] inline PlanetDetailAccess planet_detail_access(bool owner, bool ground_intel,
    bool corrupted, bool benefits_from_corruption, bool surface_accessible) {
    if (!surface_accessible) return PlanetDetailAccess::closed;
    if (owner) return PlanetDetailAccess::edit;
    if (ground_intel || (corrupted && benefits_from_corruption)) return PlanetDetailAccess::view_only;
    return PlanetDetailAccess::closed;
}

struct DetailStructure {
    sim::EntityId id{};
    sim::galactic::TypeId type{};
    std::string name, base_position, every_marker_type, additional_type;
    float shield_radius{};
    std::optional<std::uint32_t> remembered;
};
struct DetailUnit {
    sim::EntityId id{};
    sim::galactic::TypeId type{};
    std::optional<std::uint32_t> remembered;
};
struct DetailPad {
    assets::UiMapMarker marker;
    std::optional<sim::EntityId> object;
    sim::galactic::TypeId type{};
    std::string specific_type;
    float shield_radius{};
    bool fixed{};
};
struct PlanetDetailLayout { std::vector<DetailPad> builds, units; };

inline std::string detail_fold(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}
inline bool detail_marker_accepts(const assets::UiMapMarker& marker, const std::string& name) {
    const auto target = detail_fold(name);
    auto values = detail_fold(marker.specific_types);
    std::size_t at = 0;
    while (at < values.size()) {
        const auto end = values.find_first_of(", \t\r\n", at);
        if (values.substr(at, end == std::string::npos ? end : end - at) == target && !target.empty()) return true;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return false;
}
// GPD-16/17/20: compute a display layout without mutating the snapshot.
inline PlanetDetailLayout planet_detail_layout(const std::vector<assets::UiMapMarker>& markers,
    const std::vector<DetailStructure>& structures, const std::vector<DetailUnit>& units) {
    PlanetDetailLayout result;
    for (const auto& marker : markers) {
        if (marker.kind == assets::UiMarkerKind::in_base || marker.kind == assets::UiMarkerKind::out_base
            || marker.kind == assets::UiMarkerKind::specific)
            result.builds.push_back({marker, {}, {}, {}, {}, marker.kind == assets::UiMarkerKind::specific});
        else if (marker.kind == assets::UiMarkerKind::defender) result.units.push_back({marker, {}, {}, {}, {}, false});
    }
    for (const auto& structure : structures) {
        for (auto& pad : result.builds) {
            if (!pad.fixed || pad.object || !detail_marker_accepts(pad.marker,
                    structure.every_marker_type.empty() ? structure.name : structure.every_marker_type)) continue;
            pad.object = structure.id; pad.type = structure.type;
            pad.specific_type = structure.every_marker_type; pad.shield_radius = structure.shield_radius;
            if (structure.every_marker_type.empty()) break;
        }
        if (!structure.additional_type.empty()) for (auto& pad : result.builds) {
            if (!pad.fixed || pad.object || !detail_marker_accepts(pad.marker, structure.additional_type)) continue;
            pad.object = structure.id; pad.specific_type = structure.additional_type;
        }
    }
    const auto assigned_structure = [&](sim::EntityId id) {
        return std::any_of(result.builds.begin(), result.builds.end(), [&](const auto& pad) { return pad.object == id && pad.type != 0; });
    };
    // Remembered identities reserve their pads before default fill.
    for (const auto& structure : structures) {
        if (!structure.remembered || assigned_structure(structure.id)) continue;
        auto chosen = std::find_if(result.builds.begin(), result.builds.end(), [&](const auto& pad) {
            return !pad.fixed && !pad.object && pad.marker.record == *structure.remembered;
        });
        if (chosen != result.builds.end()) { chosen->object = structure.id; chosen->type = structure.type; chosen->shield_radius = structure.shield_radius; }
    }
    for (const auto& structure : structures) {
        if (assigned_structure(structure.id) || !structure.every_marker_type.empty()) continue;
        const auto base = detail_fold(structure.base_position);
        if (base != "IN_BASE" && base != "OUT_BASE") continue;
        const auto preferred = base == "OUT_BASE" ? assets::UiMarkerKind::out_base : assets::UiMarkerKind::in_base;
        auto chosen = std::find_if(result.builds.begin(), result.builds.end(), [&](const auto& pad) {
            return !pad.fixed && !pad.object && pad.marker.kind == preferred;
        });
        if (chosen == result.builds.end()) chosen = std::find_if(result.builds.begin(), result.builds.end(), [&](const auto& pad) {
            return !pad.fixed && !pad.object;
        });
        if (chosen != result.builds.end()) { chosen->object = structure.id; chosen->type = structure.type; chosen->shield_radius = structure.shield_radius; }
    }
    for (const auto& unit : units) {
        if (!unit.remembered) continue;
        auto chosen = std::find_if(result.units.begin(), result.units.end(), [&](const auto& pad) {
            return !pad.object && pad.marker.record == *unit.remembered;
        });
        if (chosen != result.units.end()) { chosen->object = unit.id; chosen->type = unit.type; }
    }
    for (const auto& unit : units) {
        if (std::any_of(result.units.begin(), result.units.end(), [&](const auto& pad) { return pad.object == unit.id; })) continue;
        auto chosen = std::find_if(result.units.begin(), result.units.end(), [](const auto& pad) { return !pad.object; });
        if (chosen != result.units.end()) { chosen->object = unit.id; chosen->type = unit.type; }
    }
    return result;
}
// GPD-18/21: one atomic command moves or swaps same-class pads.
[[nodiscard]] inline std::vector<sim::galactic::GroundLayoutChange> detail_move(
    const PlanetDetailLayout& layout, bool building, std::size_t source, std::size_t target, bool editable) {
    const auto& pads = building ? layout.builds : layout.units;
    if (!editable || source >= pads.size() || target >= pads.size() || source == target
        || !pads[source].object || pads[source].fixed || pads[target].fixed) return {};
    std::vector<sim::galactic::GroundLayoutChange> entries{{*pads[source].object, pads[target].marker.record}};
    if (pads[target].object) entries.push_back({*pads[target].object, pads[source].marker.record});
    return entries;
}
[[nodiscard]] inline std::vector<sim::galactic::GroundLayoutChange> detail_defaults(
    const PlanetDetailLayout& layout, const std::vector<sim::galactic::GroundLayoutEntry>& remembered) {
    std::vector<sim::galactic::GroundLayoutChange> entries;
    const auto collect = [&](const auto& pads) {
        for (const auto& pad : pads) {
            if (!pad.object || pad.fixed) continue;
            const auto known = std::find_if(remembered.begin(), remembered.end(), [&](const auto& entry) { return entry.object == *pad.object; });
            if (known == remembered.end() || known->marker != pad.marker.record)
                entries.push_back({*pad.object, pad.marker.record});
        }
    };
    collect(layout.builds); collect(layout.units);
    return entries;
}
} // namespace eawr::viewer::galactic
