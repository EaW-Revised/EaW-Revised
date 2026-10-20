#include "space_populate.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "frame_timer.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

namespace eawr::presentation::godot_backend {

bool SpacePopulation::compose(GodotRenderer& renderer, const assets::Map& map, const vfs::Vfs& filesystem,
                              const FixedCamera& camera, const std::span<const std::uint32_t> environment_records,
                              const sim::AssetId first_asset_id, const sim::EntityId first_entity_id) {
    core::load_profile::Scope scope(core::load_profile::Phase::scene_population);
    if (options_.catalog == nullptr) {
        failure_ = "populate requires the XML catalog, which did not load";
        return false;
    }
    const data::Catalog& catalog = *options_.catalog;
    map_placements_ = map.placements.size();
    composed_ = false;
    scene::VfsAssetCache cache(filesystem);
    scene::BuildInput input;
    input.map = &map;
    input.map_sha256 = options_.map_sha256;
    input.catalog = &catalog;
    input.access = cache.access();
    scene_ = scene::build(input);
    const scene::ObjectResolver resolve = [&](const std::string_view id, const data::Category category) -> std::optional<data::EffectiveObject> {
        auto resolved = catalog.resolve(id, category);
        if (!resolved) return std::nullopt;
        return std::move(resolved.value());
    };
    decisions_ = scene::classify_space_placements(
        *scene_, [&](const std::string_view object_id) -> std::optional<scene::SpaceObjectTags> {
            auto resolved = resolve(object_id, data::Category::game_object);
            if (!resolved) return std::nullopt;
            auto tags = scene::space_object_tags(*resolved, resolve);
            // Only records claimed by EnvironmentView are excluded below. Other
            // background models (AVC-06) use ordinary surfaces and particle owners.
            tags.background = false;
            return tags;
        });
    for (scene::SpacePlacementDecision& decision : decisions_) {
        const auto record = scene_->placements[decision.scene_ordinal].record_ordinal;
        if (std::find(options_.session_records.begin(), options_.session_records.end(), record)
            == options_.session_records.end()) continue;
        decision.role = scene::SpaceRole::not_drawable;
        decision.drawn_surfaces.clear();
        decision.hardpoints.clear();
        decision.damage_decals.clear();
        decision.missing = {"drawn by the live session from its snapshot (#80)"};
        ++session_records_skipped_;
        skipped_session_records_.push_back(record);
    }
    if (options_.debug_ship) {
        const auto& ship = *options_.debug_ship;
        const auto marker = std::find_if(map.placements.begin(), map.placements.end(), [&](const auto& placement) {
            return placement.key.record_ordinal == ship.spawn_record;
        });
        if (marker == map.placements.end() || !marker->position) {
            failure_ = "--eawr-space-place-object: spawn record " + std::to_string(ship.spawn_record)
                + " has no position on this map";
            return false;
        }
        const auto marker_decision = std::find_if(decisions_.begin(), decisions_.end(), [&](const auto& decision) {
            return scene_->placements[decision.scene_ordinal].record_ordinal == ship.spawn_record;
        });
        const auto marker_named = [&](const std::string_view suffix) {
            return marker_decision != decisions_.end() && marker_decision->object_id.size() >= suffix.size()
                && ieq(std::string_view(marker_decision->object_id).substr(
                    marker_decision->object_id.size() - suffix.size()), suffix);
        };
        // A skirmish start station stands on its Team_NN_Space_Station marker
        // and a unit on a spawn marker, each with the marker's own pose, as the
        // skirmish start places them (#199 station evidence, R-ROT-04).
        const bool spawn_marker = marker_named("Spawn_Point_Marker");
        const bool station_marker = marker_named("_Space_Station");
        if ((!spawn_marker && !station_marker) || marker_decision->role != scene::SpaceRole::marker) {
            failure_ = "--eawr-space-place-object: record " + std::to_string(ship.spawn_record)
                + " is not a catalog spawn or station marker";
            return false;
        }
        const data::Definition* definition = catalog.find(ship.object_id, data::Category::game_object);
        auto effective = catalog.resolve(ship.object_id, data::Category::game_object);
        const std::string_view expected_type = station_marker ? "StarBase" : "SpaceUnit";
        if (definition == nullptr || !effective || effective.value().type_name != expected_type) {
            failure_ = "--eawr-space-place-object: " + ship.object_id + " is not a FoC " + std::string(expected_type)
                + (station_marker ? " (a station marker takes a star base)" : "");
            return false;
        }
        assets::Map debug_map;
        debug_map.source = map.source;
        debug_map.kind = assets::MapKind::space;
        assets::Placement source;
        source.key = {map.source, ship.spawn_record};
        source.type_crc = 0U;  // A synthetic placement; the XML identity is explicit.
        source.type_resolution = assets::TypeResolution::unique;
        source.type_candidates.push_back({.logical_name = definition->id, .source = definition->root.source});
        source.position = marker->position;
        source.orientation_degrees = marker->orientation_degrees;
        source.orientation_status = marker->orientation_status;
        debug_ship_type_ = std::string(expected_type);
        debug_ship_yaw_ = source.orientation_degrees ? source.orientation_degrees->z : 0.0F;
        debug_map.placements.push_back(std::move(source));
        scene::BuildInput debug_input = input;
        debug_input.map = &debug_map;
        scene::Scene debug_scene = scene::build(debug_input);
        if (debug_scene.placements.size() != 1 || !debug_scene.placements.front().drawable()) {
            failure_ = "--eawr-space-place-object: " + ship.object_id + " has no drawable FoC model";
            return false;
        }
        scene::Placement placed = std::move(debug_scene.placements.front());
        placed.scene_ordinal = scene_->placements.size();
        placed.entity_id = static_cast<sim::EntityId>(placed.scene_ordinal + 1);
        scene_->placements.push_back(std::move(placed));
        auto decision = classify_composed_unit(scene_->placements.back(), effective.value(), resolve);
        decision.scene_ordinal = scene_->placements.size() - 1;
        if (decision.role != scene::SpaceRole::drawn) {
            failure_ = "--eawr-space-place-object: " + ship.object_id + " has no supported visible hull";
            return false;
        }
        decisions_.push_back(std::move(decision));
        debug_ship_status_ = "composed";
    }
    compose_source_ = map.source;
    compose_camera_ = camera;
    ComposeContext context{renderer, map, filesystem, camera, environment_records, cache, input, resolve};
    return compose_range(context, 0, 0, first_asset_id, first_entity_id);
}
} // namespace eawr::presentation::godot_backend
