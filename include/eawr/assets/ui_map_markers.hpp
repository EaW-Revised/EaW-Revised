#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>

namespace eawr::assets {

// GPD-07: the map record identity is shared with the later land-entry consumer.
enum class UiMarkerKind { in_base, out_base, defender, specific, reinforcement, bunker, build_pad };
struct UiMapMarker {
    std::uint32_t record{};
    UiMarkerKind kind{};
    SourceVec3 position{};
    float x{}, y{};
    std::string specific_types;
    float map_width{}, map_height{};
};

// WGT-79: increasing squared 3D distance from the mean. Equal-distance order is unspecified.
inline void prioritize_in_base_markers(std::vector<UiMapMarker>& markers) {
    if (markers.empty()) return;
    double x = 0, y = 0, z = 0;
    for (const auto& marker : markers) { x += marker.position.x; y += marker.position.y; z += marker.position.z; }
    const auto n = static_cast<double>(markers.size()); x /= n; y /= n; z /= n;
    const auto distance = [=](const UiMapMarker& marker) {
        const double dx = marker.position.x - x, dy = marker.position.y - y, dz = marker.position.z - z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::sort(markers.begin(), markers.end(), [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
}

// Deterministic parse-work receipt for performance tests; includes nested chunks.
[[nodiscard]] inline std::size_t ui_map_chunk_count(const Map& map) {
    const auto count = [](const auto& self, const std::vector<RawChunk>& chunks) -> std::size_t {
        std::size_t total = chunks.size();
        for (const auto& chunk : chunks) total += self(self, chunk.children);
        return total;
    };
    return count(count, map.chunks);
}

// GPD-05..10: only uniquely resolved eligible records enter the table. Invalid map dimensions
// or positions do not produce guessed positions. The preview orientation still needs capture.
[[nodiscard]] inline std::vector<UiMapMarker> ui_map_markers(const Map& map, const data::Catalog& catalog) {
    std::vector<UiMapMarker> inside, outside, defenders, specific, reinforcements, bunkers, pads;
    if (!map.declared_extents || !(map.declared_extents->first > 0) || !(map.declared_extents->second > 0)) return {};
    for (const auto& placement : map.placements) {
        if (!placement.position || placement.type_resolution != TypeResolution::unique || placement.type_candidates.size() != 1) continue;
        const auto& pos = *placement.position;
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) continue;
        const auto& type = placement.type_candidates.front().logical_name;
        const auto resolved = catalog.resolve(type, data::Category::game_object);
        if (!resolved || !data::retail::boolean(resolved.value().single_text("Include_In_UI_Map_Header")).value_or(false)) continue;
        std::string name = type;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto targets = resolved.value().single_text("Marker_For_Specific_Object_Type");
        UiMarkerKind kind;
        if (name == "IN BASE SPECIAL STRUCTURE POSITION") kind = UiMarkerKind::in_base;
        else if (name == "OUT BASE SPECIAL STRUCTURE POSITION") kind = UiMarkerKind::out_base;
        else if (name == "DEFENDING FORCES POSITION") kind = UiMarkerKind::defender;
        else if (!targets.empty()) kind = UiMarkerKind::specific;
        else if (name.starts_with("REINFORCEMENT_POINT")) kind = UiMarkerKind::reinforcement;
        else if (name == "GARRISON_BUNKER_CAPTURABLE") kind = UiMarkerKind::bunker;
        else if (name == "SKIRMISH_BUILD_PAD") kind = UiMarkerKind::build_pad;
        else continue;
        UiMapMarker marker{placement.key.record_ordinal, kind, pos,
            pos.x / map.declared_extents->first + 0.5F, pos.y / map.declared_extents->second + 0.5F, targets,
            map.declared_extents->first, map.declared_extents->second};
        switch (kind) {
        case UiMarkerKind::in_base: inside.push_back(marker); break;
        case UiMarkerKind::out_base: outside.push_back(marker); break;
        case UiMarkerKind::defender: defenders.push_back(marker); break;
        case UiMarkerKind::specific: specific.push_back(marker); break;
        case UiMarkerKind::reinforcement: reinforcements.push_back(marker); break;
        case UiMarkerKind::bunker: bunkers.push_back(marker); break;
        case UiMarkerKind::build_pad: pads.push_back(marker); break;
        }
    }
    prioritize_in_base_markers(inside);
    std::vector<UiMapMarker> result;
    const auto append = [&](const auto& source, const std::size_t cap) {
        for (const auto& marker : source) { if (result.size() >= cap) break; result.push_back(marker); }
    };
    append(inside, 80); append(specific, 80); append(outside, 80);
    const auto unit_cap = result.size() + 10; append(defenders, unit_cap);
    result.insert(result.end(), reinforcements.begin(), reinforcements.end());
    result.insert(result.end(), bunkers.begin(), bunkers.end());
    result.insert(result.end(), pads.begin(), pads.end());
    return result;
}
} // namespace eawr::assets
