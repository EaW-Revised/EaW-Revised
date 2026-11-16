#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

// A removed map record has no simulation object and must not be presented.
[[nodiscard]] inline bool removed_space_record(const battle::BattleStart* start, std::uint32_t record) {
    return start && std::any_of(start->removed.begin(), start->removed.end(),
        [record](const battle::RemovedObject& removed) { return removed.record == record; });
}

inline bool suppress_removed_space_placement(const battle::BattleStart* start, std::uint32_t record,
                                             scene::SpacePlacementDecision& decision) {
    if (!removed_space_record(start, record)) return false;
    decision.role = scene::SpaceRole::not_drawable;
    decision.drawn_surfaces.clear();
    decision.hardpoints.clear();
    decision.damage_decals.clear();
    decision.missing = {"removed at battle start"};
    return true;
}

// WGT-57/101: the props that stand in place of a removed destroyed-planet backdrop, as map placements the scene builds
// like any other record (their ordinals lie past the map's, see battle::replacement_prop_record_base). The type is looked
// up by name, so an unresolvable replacement type only draws nothing (the scene reports it as an issue).
[[nodiscard]] inline std::vector<assets::Placement> replacement_placements(const assets::Map& map, const battle::BattleStart* start) {
    std::vector<assets::Placement> result;
    if (!start) return result;
    const auto binary32 = [](const sim::math::Fixed value) {
        return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
    };
    for (const battle::ReplacementProp& prop : start->props) {
        assets::Placement placement;
        placement.key = {map.source, prop.record};
        placement.type_crc = assets::object_type_crc(prop.type);
        placement.type_resolution = assets::TypeResolution::unique;
        assets::ObjectTypeRef type;
        type.logical_name = prop.type;
        placement.type_candidates.push_back(std::move(type));
        placement.position = assets::SourceVec3{binary32(prop.position.x), binary32(prop.position.y), binary32(prop.position.z)};
        placement.orientation_degrees = assets::SourceVec3{binary32(prop.orientation_degrees.x),
            binary32(prop.orientation_degrees.y), binary32(prop.orientation_degrees.z)};
        placement.orientation_status = assets::OrientationStatus::three_axis;
        result.push_back(std::move(placement));
    }
    return result;
}

inline void empty_placement(scene::Placement& placement) {
    placement.asset_id = 0;
    placement.model_path.clear();
    placement.model_declared.clear();
    placement.transform.reset();
    placement.surfaces.clear();
    placement.effects.clear();
    placement.issues.clear();
    placement.capture_point = false;
    placement.particle_root = false;
}

// #6144: a land map's static composition keeps no placement whose record the start removed. The placement is emptied
// (no model, asset, transform, surfaces, effects or issues), so it is not drawable, uploads nothing, plans no attached
// effect and is not a map particle root; its record ordinal and scene ordinal stay. Returns the number emptied.
inline std::size_t strip_removed_placements(scene::Scene& scene, const battle::BattleStart* start) {
    if (!start || start->removed.empty()) return 0;
    std::size_t stripped = 0;
    for (scene::Placement& placement : scene.placements) {
        if (!removed_space_record(start, placement.record_ordinal)) continue;
        empty_placement(placement);
        ++stripped;
    }
    return stripped;
}

// #6992: a land start replaces a map marker record with an owned station unit (a build pad, a pre-built structure) that the
// live session draws and picks. `records` are the map records of the units the session actually bound a ship to; the record
// is not composed statically either, so the object is drawn once. A start unit the session does not draw (no table row, not
// admitted) keeps its map record. Returns the number emptied.
inline std::size_t strip_session_owned_placements(scene::Scene& scene, const std::vector<std::uint32_t>& records) {
    if (records.empty()) return 0;
    std::size_t stripped = 0;
    for (scene::Placement& placement : scene.placements) {
        if (std::find(records.begin(), records.end(), placement.record_ordinal) == records.end()) continue;
        empty_placement(placement);
        ++stripped;
    }
    return stripped;
}

} // namespace eawr::presentation::godot_backend
