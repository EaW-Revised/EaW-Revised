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
namespace {

using TeamColour = std::optional<std::array<std::uint8_t, 3>>;

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

[[nodiscard]] std::array<float, 3> apply(const sim::math::Mat3x4& matrix, const std::array<float, 3>& point) {
    std::array<float, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        result[row] = to_float(matrix.rows[row][0]) * point[0] + to_float(matrix.rows[row][1]) * point[1]
            + to_float(matrix.rows[row][2]) * point[2] + to_float(matrix.rows[row][3]);
    }
    return result;
}

// Distance from the eye to a render-basis point, and whether the point lies in
// a slightly widened view frustum of the camera.
struct ViewTest final {
    std::array<float, 3> eye{};
    std::array<float, 3> forward{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
    float tan_half_fov{};
    float aspect{};

    explicit ViewTest(const FixedCamera& camera) : eye(camera.eye) {
        const auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        };
        const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const auto normal = [](std::array<float, 3> v) {
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 0.0F) for (float& c : v) c /= length;
            return v;
        };
        forward = normal(sub(camera.target, camera.eye));
        right = normal(cross(forward, camera.up));
        up = cross(right, forward);
        tan_half_fov = std::tan(camera.vertical_fov_degrees * 0.5F * 3.14159265F / 180.0F);
        aspect = static_cast<float>(camera.width) / static_cast<float>(std::max<std::uint32_t>(camera.height, 1));
    }
    [[nodiscard]] float distance(const std::array<float, 3>& point) const {
        const float dx = point[0] - eye[0], dy = point[1] - eye[1], dz = point[2] - eye[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    [[nodiscard]] bool visible(const std::array<float, 3>& point) const {
        const std::array<float, 3> d{point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
        const float depth = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
        if (depth <= 0.0F) return false;
        const float x = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (depth * tan_half_fov * aspect);
        const float y = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (depth * tan_half_fov);
        return std::abs(x) <= 1.25F && std::abs(y) <= 1.25F;
    }
};




} // namespace

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
        scene::Scene only_ship;
        only_ship.placements.push_back(scene_->placements.back());
        auto decision = scene::classify_space_placements(only_ship, [&](const std::string_view object_id)
            -> std::optional<scene::SpaceObjectTags> {
            auto object = resolve(object_id, data::Category::game_object);
            return object ? std::optional(scene::space_object_tags(*object, resolve)) : std::nullopt;
        }).front();
        decision.scene_ordinal = scene_->placements.size() - 1;
        if (decision.role != scene::SpaceRole::drawn) {
            failure_ = "--eawr-space-place-object: " + ship.object_id + " has no supported visible hull";
            return false;
        }
        decisions_.push_back(std::move(decision));
        debug_ship_status_ = "composed";
    }
    live_decisions_.assign(options_.placed_ships.size(), std::nullopt);
    live_shown_.assign(options_.placed_ships.size(), true);
    live_defend_.assign(options_.placed_ships.size(), false);
    live_clips_.assign(options_.placed_ships.size(), std::nullopt);
    live_alternate_clips_.assign(options_.placed_ships.size(), std::nullopt);
    live_retired_.assign(options_.placed_ships.size(), false);
    live_alternates_.assign(options_.placed_ships.size(), 0);
    live_alternate_counts_.assign(options_.placed_ships.size(), 0);
    live_bounds_.assign(options_.placed_ships.size(), std::nullopt);
    live_transforms_.assign(options_.placed_ships.size(), std::nullopt);
    for (std::size_t ship_index = 0; ship_index < options_.placed_ships.size(); ++ship_index) {
        const auto& ship = options_.placed_ships[ship_index];
        const bool live = ship.live_entity != 0;
        const std::string live_name = "live unit " + std::to_string(ship.live_entity) + " (" + ship.object_id + ")";
        const data::Definition* definition = catalog.find(ship.object_id, data::Category::game_object);
        auto effective = catalog.resolve(ship.object_id, data::Category::game_object);
        if (live && (definition == nullptr || !effective)) {
            (ship.projectile_slot ? projectile_slots_undrawn_ : ship.launch_slot ? launch_slots_undrawn_ : live_undrawn_).push_back(live_name + ": not in the catalog");
            continue;
        }
        if (!live && (definition == nullptr || !effective || effective.value().type_name != "SpaceUnit")) {
            failure_ = "--eawr-space-place-at: " + ship.object_id + " is not a FoC SpaceUnit";
            return false;
        }
        assets::Map placed_map;
        placed_map.source = map.source;
        placed_map.kind = assets::MapKind::space;
        assets::Placement source;
        // A live unit takes a record past the map's, so no map record rule
        // (environment, session) applies to it.
        source.key = {map.source, live ? static_cast<std::uint32_t>(map.placements.size() + ship_index) : 0U};
        source.type_crc = 0U; // A synthetic placement; the XML identity is explicit.
        source.type_resolution = assets::TypeResolution::unique;
        source.type_candidates.push_back({.logical_name = definition->id, .source = definition->root.source});
        source.position = ship.position;
        source.orientation_degrees = assets::SourceVec3{0.0F, 0.0F, ship.yaw_degrees};
        source.orientation_status = assets::OrientationStatus::yaw_only;
        placed_map.placements.push_back(std::move(source));
        scene::BuildInput placed_input = input;
        placed_input.map = &placed_map;
        scene::Scene placed_scene = scene::build(placed_input);
        if (placed_scene.placements.size() != 1
            || (!placed_scene.placements.front().drawable()
                && !(ship.projectile_slot && scene::drawable_projectile_effects(placed_scene.placements.front())))) {
            if (live) {
                (ship.projectile_slot ? projectile_slots_undrawn_ : ship.launch_slot ? launch_slots_undrawn_ : live_undrawn_).push_back(live_name + ": no drawable FoC model");
                continue;
            }
            failure_ = "--eawr-space-place-at: " + ship.object_id + " has no drawable FoC model";
            return false;
        }
        scene::Placement placed = std::move(placed_scene.placements.front());
        if (ship.construction) placed.surfaces = scene::construction_surfaces(cache.access(), placed.model_path);
        if (ship.placement_preview) placed.effects.clear(); // WR-12: preview clones never emit particles
        if (live) {
            placed.team_colour = ship.team_colour;
            placed.team_colour_status = ship.team_colour ? "live_session_owner" : "owner_absent";
            for (std::size_t index = 0; !ship.team_colour && ship.colour_record && index < scene_->placements.size(); ++index) {
                const scene::Placement& mapped = scene_->placements[index];
                if (mapped.record_ordinal != *ship.colour_record) continue;
                placed.team_colour = mapped.team_colour;
                placed.team_colour_status = mapped.team_colour_status;
            }
        }
        placed.scene_ordinal = scene_->placements.size();
        placed.entity_id = static_cast<sim::EntityId>(placed.scene_ordinal + 1);
        scene_->placements.push_back(std::move(placed));
        scene::Scene only_ship;
        only_ship.placements.push_back(scene_->placements.back());
        auto decision = scene::classify_space_placements(only_ship, [&](const std::string_view object_id)
            -> std::optional<scene::SpaceObjectTags> {
            auto object = resolve(object_id, data::Category::game_object);
            return object ? std::optional(scene::space_object_tags(*object, resolve)) : std::nullopt;
        }).front();
        decision.scene_ordinal = scene_->placements.size() - 1;
        if (decision.role != scene::SpaceRole::drawn) {
            if (live) {
                (ship.projectile_slot ? projectile_slots_undrawn_ : ship.launch_slot ? launch_slots_undrawn_ : live_undrawn_).push_back(live_name + ": " + std::string(scene::to_string(decision.role)));
                scene_->placements.pop_back();
                continue;
            }
            failure_ = "--eawr-space-place-at: " + ship.object_id + " has no supported visible hull";
            return false;
        }
        if (live) live_decisions_[ship_index] = decisions_.size();
        decisions_.push_back(std::move(decision));
    }
    for (scene::SpacePlacementDecision& decision : decisions_) {
        const auto record = scene_->placements[decision.scene_ordinal].record_ordinal;
        if (std::find(environment_records.begin(), environment_records.end(), record) != environment_records.end()) {
            decision.role = scene::SpaceRole::environment;
            decision.drawn_surfaces.clear();
            decision.hardpoints.clear();
            decision.damage_decals.clear();
            decision.missing.clear();
        }
    }

    // The light exists before any hull upload, so with shadows on every hull
    // compiles the shadow-receiving variant. The distance is refined below.
    const std::size_t receiving_before = renderer.shadow_receiving_materials();
    const std::size_t variant_failures_before = renderer.shadow_variant_failures();
    if (options_.policy != lighting::Policy::off) {
        lighting_ = space_populate_detail::lighting_state(options_, camera.far_plane);
        renderer.set_lighting(lighting_);
    }

    std::map<std::string, assets::Texture> textures;
    sim::AssetId next_asset = std::max(first_asset, first_asset_id);
    // One visible submesh through its legacy selector with its authored
    // values; nullopt (recorded) when the renderer refuses it.


    // #427: a SHIELD submesh with its MeshShield.fx material and its three textures (BP-22).
    // Nullopt with `status` set when a texture does not resolve or the renderer refuses it.

    std::map<std::pair<std::string, std::uint32_t>, std::optional<SurfaceUpload>> shell_uploads;

    // Everything drawn: one upload (shared) at one source-basis matrix.
    struct Piece final {
        const SurfaceUpload* upload{};
        sim::math::Mat3x4 source{};
        std::optional<std::size_t> idle;    // index into idle_placements_
        std::optional<HardpointGate> gate;  // hardpoint art, drawn per its state
        std::optional<std::size_t> live;    // a live ship's piece (#80)
        sim::math::Mat3x4 local{sim::math::identity_matrix()};  // its frame in the ship's model space
        bool live_clip{};                   // posed by its ship's PlacedShip::clip (#81)
        bool shield{};                      // its ship's shield shell (#427)
        std::optional<std::uint32_t> alternate{};
    };
    std::vector<Piece> pieces;
    // Keyed by model path, mesh and submesh: a model shared by placements or
    // hardpoints is uploaded once.
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t, TeamColour>, std::optional<SurfaceUpload>> uploads;
    std::map<std::string, std::optional<std::vector<animation::BonePose>>> bind_poses;
    struct ClipBinding final { std::optional<std::size_t> clip; std::string status; };
    std::map<std::pair<std::string, std::string>, ClipBinding> clip_bindings;
    std::vector<std::string> failed_placements;
    std::optional<std::pair<std::size_t, std::size_t>> debug_piece_range;
    const auto bind_idle = [&](const scene::Placement& placement, const assets::Model& model)
        -> std::optional<std::size_t> {
        if (placement.idle_animation_status != "corpus_naming_observed" || placement.idle_animation.empty()) {
            ++animation_status_["clip_absent"];
            static_hulls_[placement.object_id] = "clip_absent";
            return std::nullopt;
        }
        const auto key = std::make_pair(placement.model_path, placement.idle_animation);
        auto found = clip_bindings.find(key);
        if (found == clip_bindings.end()) {
            ClipBinding binding{std::nullopt, "bound"};
            auto decoded = assets::load_animation(filesystem, placement.idle_animation);
            if (!decoded) binding.status = "clip_decode_failed";
            else if (auto player = animation::Player::create(model, &decoded.value()); !player) {
                binding.status = "clip_binding_failed";
            } else if (!particles::map_owner_sample(player.value(), options_.animation_frames - 1U)) {
                binding.status = "clip_rate_unsupported";
            } else {
                binding.clip = clips_.size();
                clips_.push_back(std::make_shared<const animation::Player>(std::move(player.value())));
            }
            found = clip_bindings.emplace(key, std::move(binding)).first;
        }
        ++animation_status_[found->second.status];
        if (found->second.clip) bound_idle_hulls_[placement.object_id] = placement.idle_animation;
        else static_hulls_[placement.object_id] = found->second.status;
        return found->second.clip;
    };
    // #81: a live ship's own clip (a death clone's DIE clip), bound like an
    // idle clip but posed by pose_live_clips() on the live session's clock.
    const auto bind_live_clip = [&](const scene::Placement& placement, const assets::Model& model,
                                    const std::string& path) -> std::optional<std::size_t> {
        const std::string key = placement.object_id + " " + path;
        std::optional<std::size_t> clip;
        std::string status = "bound";
        auto decoded = assets::load_animation(filesystem, path);
        if (!decoded) status = "clip_decode_failed: " + core::format_diagnostic(decoded.error());
        else if (auto player = animation::Player::create(model, &decoded.value()); !player) {
            status = "clip_binding_failed: " + core::format_diagnostic(player.error());
        } else if (const float rate = player.value().frames_per_second();
                   !(rate >= 1.0F) || std::floor(rate) != rate) {
            status = "clip_rate_unsupported";
        } else {
            clip = clips_.size();
            clips_.push_back(std::make_shared<const animation::Player>(std::move(player.value())));
        }
        live_clip_status_[key] = status;
        return clip;
    };
    const auto shared_upload = [&](const std::string& path, const assets::Model& model, const std::uint32_t mesh_index,
                                   const std::uint32_t submesh_index, const scene::LegacySelector& selector,
                                   const std::string& texture_path, const TeamColour& colour) -> const SurfaceUpload* {
        const auto key = std::make_tuple(path, mesh_index, submesh_index, colour);
        auto found = uploads.find(key);
        if (found == uploads.end()) {
            found = uploads.emplace(key, upload_surface(renderer, filesystem, cache, textures, next_asset, model, mesh_index, submesh_index, selector, texture_path, colour,
                path + " mesh " + std::to_string(mesh_index) + " submesh " + std::to_string(submesh_index))).first;
        }
        return found->second ? &*found->second : nullptr;
    };

    // Retail starts a created object's Idle_Anim_00 at Idle_Anim_00_Rate_Mod
    // from a random frame, looping when Loop_Idle_Anim_00 is set (#145). The
    // random frame is taken from the placement's identity, so captures repeat.
    const auto idle_placement = [&](const scene::Placement& placement, const std::size_t clip) {
        IdlePlacement idle{.clip = clip};
        if (auto object = resolve(placement.object_id, data::Category::game_object)) {
            const DeclaredIdle declared = declared_idle(scene::space_object_tags(*object).idle);
            idle.playback = declared.playback;
            if (declared.rejected_rate) idle_rate_rejected_[placement.object_id] = *declared.rejected_rate;
        }
        idle.start_frame = placement_start_frame(placement, clips_[clip]->playable_frames());
        idle_playbacks_[placement.object_id] = idle.playback;
        return idle;
    };

    hardpoint_states_.assign(decisions_.size(), {});
    for (std::size_t decision_index = 0; decision_index < decisions_.size(); ++decision_index) {
        scene::SpacePlacementDecision& decision = decisions_[decision_index];
        if (decision.role != scene::SpaceRole::drawn) continue;
        hardpoint_states_[decision_index].assign(decision.hardpoints.size(), scene::HardpointState::intact);
        const scene::Placement& placement = scene_->placements[decision.scene_ordinal];
        const TeamColour colour = options_.team_colour ? options_.team_colour(placement) : TeamColour{};
        std::optional<std::size_t> live_ship;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (live_decisions_[ship] == decision_index) live_ship = ship;
        }
        ++team_colour_status_[placement.team_colour_status];
        const assets::Model* model = cache.model(placement.model_path);
        if (model == nullptr) continue;  // drawn implies loaded; kept defensive
        const std::size_t first_piece = pieces.size();
        const auto add_surface = [&](const std::size_t index, const std::optional<HardpointGate> gate) {
            const scene::Surface& surface = placement.surfaces[index];
            std::string texture_path;
            for (const scene::TextureBinding& binding : surface.textures) {
                if (ieq(binding.parameter, "BaseTexture")) texture_path = binding.resolved;
            }
            const SurfaceUpload* upload = shared_upload(placement.model_path, *model, surface.mesh_index,
                surface.submesh_index, *scene::find_legacy_selector(surface.shader), texture_path,
                scene::colorizes(surface.shader) ? colour : TeamColour{});
            if (upload != nullptr) {
                pieces.push_back({upload, placement.transform->matrix, std::nullopt, gate, live_ship,
                                  sim::math::identity_matrix()});
                if (live_ship && options_.placed_ships[*live_ship].construction) {
                    const auto alternate = scene::mesh_alternate(surface.mesh_name);
                    pieces.back().alternate = alternate;
                    if (alternate) live_alternate_counts_[*live_ship] = std::max(live_alternate_counts_[*live_ship], *alternate);
                }
            }
            else {
                const std::string where = "placement " + std::to_string(decision.scene_ordinal) + " ("
                    + decision.object_id + ") surface " + std::to_string(index);
                decision.missing.push_back(where + " upload_failed " + surface.shader);
                failed_placements.push_back(where);
            }
        };
        const scene::HardpointOwnerArt owner_art = scene::hardpoint_owner_art(*model, decision.hardpoints);
        for (const std::size_t index : decision.drawn_surfaces) {
            const std::size_t mesh = placement.surfaces[index].mesh_index;
            const std::size_t hardpoint = mesh < owner_art.engine_mesh_hardpoint.size()
                ? owner_art.engine_mesh_hardpoint[mesh] : scene::HardpointOwnerArt::none;
            if (hardpoint == scene::HardpointOwnerArt::none) add_surface(index, std::nullopt);
            else add_surface(index, HardpointGate{decision_index, hardpoint, HardpointGate::Art::engine_particle});
        }
        // Each Damage_Decal surface is uploaded as well and drawn once its
        // hardpoint is destroyed.
        for (const auto& decal : decision.damage_decals) {
            add_surface(decal.surface, HardpointGate{decision_index, decal.hardpoint, HardpointGate::Art::damage_decal});
        }
        // #427: the SHIELD sub-object of a live ship with a DEFEND ability, drawn while the
        // ability runs (BP-22). The ALO hides it; only MeshShield.fx submeshes are drawn.
        if (live_ship && options_.placed_ships[*live_ship].defend_shell) {
            std::string& status = shield_shells_[decision.object_id];
            const std::optional<std::uint32_t> shield = shield_mesh_index(*model);
            if (!shield) status = "no SHIELD sub-object";
            for (std::uint32_t submesh = 0; shield && submesh < model->meshes[*shield].submeshes.size(); ++submesh) {
                const std::string& shader = model->meshes[*shield].submeshes[submesh].shader;
                if (!ieq(shader, meshshield_program)) {
                    status = "SHIELD submesh " + std::to_string(submesh) + " shader " + shader + " not drawn";
                    continue;
                }
                auto found = shell_uploads.find({placement.model_path, submesh});
                if (found == shell_uploads.end()) {
                    std::string cause;
                    found = shell_uploads.emplace(std::pair{placement.model_path, submesh},
                                                  upload_shell(renderer, filesystem, cache, next_asset, *model, *shield, submesh, cause)).first;
                    if (!found->second) status = cause;
                }
                if (!found->second) continue;
                if (status.empty()) status = "composed";
                Piece piece{&*found->second, placement.transform->matrix, std::nullopt, std::nullopt, live_ship,
                            sim::math::identity_matrix()};
                piece.shield = true;
                pieces.push_back(piece);
            }
        }
        if (std::any_of(pieces.begin() + static_cast<std::ptrdiff_t>(first_piece), pieces.end(),
                [](const Piece& piece) { return piece.upload->pose.has_value(); })) {
            const std::string live_clip_path = live_ship ? options_.placed_ships[*live_ship].clip : std::string{};
            const auto clip = live_clip_path.empty() ? bind_idle(placement, *model) : std::nullopt;
            if (!live_clip_path.empty()) {
                if (const auto bound = bind_live_clip(placement, *model, live_clip_path)) {
                    live_clips_[*live_ship] = *bound;
                    if (const std::string& alternate = options_.placed_ships[*live_ship].alternate_clip; !alternate.empty()) {
                        live_alternate_clips_[*live_ship] = bind_live_clip(placement, *model, alternate);
                    }
                    for (std::size_t index = first_piece; index < pieces.size(); ++index) {
                        if (pieces[index].upload->pose) pieces[index].live_clip = true;
                    }
                }
            } else if (clip) {
                ++animated_placements_;
                const std::size_t idle = idle_placements_.size();
                idle_placements_.push_back(idle_placement(placement, *clip));
                for (std::size_t index = first_piece; index < pieces.size(); ++index) {
                    if (pieces[index].upload->pose) pieces[index].idle = idle;
                }
            }
        }

        // Model_To_Attach at the owner's Attachment_Bone in its bind pose,
        // drawn while the hardpoint's state shows it. Anything that cannot
        // attach is listed, not guessed.
        for (std::size_t hardpoint_index = 0; hardpoint_index < decision.hardpoints.size(); ++hardpoint_index) {
            const scene::HardpointAttachment& hardpoint = decision.hardpoints[hardpoint_index];
            const std::string who = "hardpoint " + hardpoint.hardpoint + ": ";
            if (!hardpoint.resolved) {
                decision.missing.push_back(who + "not in the catalog");
                continue;
            }
            if (hardpoint.model.empty()) continue;  // a hardpoint without a model draws nothing
            const auto bone = std::find_if(model->bones.begin(), model->bones.end(),
                [&](const assets::Bone& entry) { return ieq(entry.name, hardpoint.bone); });
            if (bone == model->bones.end()) {
                decision.missing.push_back(who + "attachment bone " + hardpoint.bone + " is not in the model");
                continue;
            }
            const std::string path = probe(cache, "data/art/models/", hardpoint.model, model_suffixes);
            const assets::Model* attached = path.empty() ? nullptr : cache.model(path);
            if (attached == nullptr) {
                decision.missing.push_back(who + (path.empty() ? "model_not_in_vfs " : "model_failed_to_load ")
                                           + hardpoint.model);
                continue;
            }
            auto pose = bind_poses.find(placement.model_path);
            if (pose == bind_poses.end()) {
                std::optional<std::vector<animation::BonePose>> bones;
                if (auto player = animation::Player::create(*model)) {
                    if (auto sampled = player.value().sample({})) bones = std::move(sampled.value().bones);
                }
                pose = bind_poses.emplace(placement.model_path, std::move(bones)).first;
            }
            const std::size_t bone_index = static_cast<std::size_t>(bone - model->bones.begin());
            // The attached model's root sits at the bone's bind frame. Every
            // FoC Model_To_Attach is authored about its own root, also those
            // whose skeleton repeats owner bone names; an identity frame
            // stacked them at the hull origin (#136; docs/asset-formats.md,
            // "Hardpoint state art").
            std::optional<sim::math::Mat3x4> frame;
            if (pose->second && bone_index < pose->second->size()) {
                frame = particles::fixed_model_frame((*pose->second)[bone_index].model_asset, scene::fixed_from_binary32);
            }
            const auto placed = frame ? sim::math::compose(placement.transform->matrix, *frame)
                                      : core::Result<sim::math::Mat3x4>::failure({});
            if (!placed) {
                decision.missing.push_back(who + "attachment bone " + hardpoint.bone + " has no bind frame");
                continue;
            }
            for (std::uint32_t mesh_index = 0; mesh_index < attached->meshes.size(); ++mesh_index) {
                const assets::Mesh& mesh = attached->meshes[mesh_index];
                if (!mesh.visible) continue;
                for (std::uint32_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
                    const assets::Submesh& submesh = mesh.submeshes[submesh_index];
                    if (scene::is_shadow_volume_shader(submesh.shader)) continue;
                    const scene::LegacySelector* selector = scene::find_legacy_selector(submesh.shader);
                    if (selector == nullptr) {
                        decision.missing.push_back(who + "shader_unsupported " + submesh.shader);
                        continue;
                    }
                    std::string texture_path;
                    for (const assets::MaterialParameter& parameter : submesh.parameters) {
                        const auto* name = std::get_if<std::string>(&parameter.value);
                        if (!ieq(parameter.name, "BaseTexture") || name == nullptr) continue;
                        texture_path = probe(cache, "data/art/textures/", *name, texture_suffixes);
                        if (texture_path.empty()) decision.missing.push_back(who + "texture_unresolved " + *name);
                    }
                    const SurfaceUpload* upload
                        = shared_upload(path, *attached, mesh_index, submesh_index, *selector, texture_path,
                                        scene::colorizes(submesh.shader) ? colour : TeamColour{});
                    if (upload == nullptr) {
                        const std::string where = "placement " + std::to_string(decision.scene_ordinal) + " ("
                            + decision.object_id + ") hardpoint " + hardpoint.hardpoint + " mesh "
                            + std::to_string(mesh_index) + " submesh " + std::to_string(submesh_index);
                        decision.missing.push_back(where + " upload_failed " + submesh.shader);
                        failed_placements.push_back(where);
                        continue;
                    }
                    pieces.push_back({upload, placed.value(), std::nullopt,
                                      HardpointGate{decision_index, hardpoint_index, HardpointGate::Art::attached_model},
                                      live_ship, *frame});
                }
            }
        }
        // One line per distinct reason.
        std::sort(decision.missing.begin(), decision.missing.end());
        decision.missing.erase(std::unique(decision.missing.begin(), decision.missing.end()), decision.missing.end());
        if (options_.debug_ship && decision.scene_ordinal == scene_->placements.size() - 1)
            debug_piece_range = std::pair{first_piece, pieces.size()};
    }

    if (!failed_placements.empty()) {
        failure_ = std::to_string(failed_placements.size()) + " supported surface placement uploads failed";
        for (const std::string& where : failed_placements) failure_ += "; " + where;
        for (const std::string& detail : upload_failures_) failure_ += "; " + detail;
        return false;
    }

    const ViewTest view(camera);
    float debug_farthest = 0.0F;
    sim::EntityId next_entity = std::max(first_entity, first_entity_id);
    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        const Piece& piece = pieces[piece_index];
        const sim::EntityId entity = next_entity++;
        pieces_.push_back({{entity, piece.upload->renderer_asset, source_to_render(piece.source)}, piece.gate,
                           piece.live, piece.local, piece.shield, piece.alternate});
        if (piece.upload->pose) {
            if (!piece.shield) ++skinned_instances_;
            const auto posed = renderer.set_skin_pose(entity, piece.upload->renderer_asset, *piece.upload->pose);
            if (!posed) upload_failures_.push_back("entity " + std::to_string(entity) + " bind pose: "
                                                   + core::format_diagnostic(posed.error()));
            if (piece.idle) animated_instances_.push_back({entity, piece.upload->renderer_asset, *piece.idle});
            if (piece.live_clip) live_clip_instances_.push_back({*piece.live, entity, piece.upload->renderer_asset});
        }
        const auto& low = piece.upload->minimum;
        const auto& high = piece.upload->maximum;
        if (low[0] > high[0]) continue;
        if (piece.live && *piece.live < live_bounds_.size()) {
            // #82: the ship's pick box grows by the piece's corners in the ship's model space.
            auto& bounds = live_bounds_[*piece.live];
            for (int corner = 0; corner < 8; ++corner) {
                const std::array<float, 3> local = apply(piece.local, {(corner & 1) ? high[0] : low[0],
                    (corner & 2) ? high[1] : low[1], (corner & 4) ? high[2] : low[2]});
                if (!bounds) bounds = std::pair{local, local};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    bounds->first[axis] = std::min(bounds->first[axis], local[axis]);
                    bounds->second[axis] = std::max(bounds->second[axis], local[axis]);
                }
            }
        }
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<float, 3> source_corner{(corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1],
                                                     (corner & 4) ? high[2] : low[2]};
            const std::array<float, 3> world = apply(piece.source, source_corner);
            const std::array<float, 3> render{world[0], world[2], -world[1]};
            const float distance = view.distance(render);
            if (debug_piece_range && piece_index >= debug_piece_range->first && piece_index < debug_piece_range->second)
                debug_farthest = std::max(debug_farthest, distance);
        }
    }
    rebuild_piece_index();
    attachment_marks_.configure(hardpoint_states_);
    instances_.reserve(pieces_.size());
    refresh_instances();
    shadow_receiving_ = renderer.shadow_receiving_materials() - receiving_before;
    shadow_variant_failures_ = renderer.shadow_variant_failures() - variant_failures_before;
    if (options_.policy != lighting::Policy::off) {
        // A fixed tactical reach preserves Godot's snapped cascade scale when
        // the camera moves. Only the labelled debug ship view fits its hull.
        const float reach = debug_farthest > 0.0F ? std::max(debug_farthest * 1.1F, 1.0F)
            : shadow_settings(active_render_profile(), true).max_distance;
        shadow_max_distance_ = std::min(reach, camera.far_plane);
        lighting_.shadow_max_distance = shadow_max_distance_;
        renderer.set_lighting(lighting_);
    }
    // The first attached-effect plan: every placement, the debug ship
    // included, with every hardpoint intact (#136).
    if (options_.attached_effects && !options_.attached_effects(*this)) {
        failure_ = "space attached effect preparation failed";
        return false;
    }
    composed_ = true;
    // The debug ship's states then go through the hook #72 drives, so each
    // one starts or stops emitters as a live change would, before the first
    // frame.
    if (options_.debug_ship) {
        for (const auto& [hardpoint, state] : options_.debug_ship->hardpoint_states) {
            if (!set_hardpoint_state(scene_->placements.size() - 1, hardpoint, state)) {
                if (failure_.empty()) {
                    failure_ = "--eawr-space-hardpoint-state: " + hardpoint + " is not a hardpoint of "
                        + options_.debug_ship->object_id;
                }
                return false;
            }
        }
    }
    return true;
}
} // namespace eawr::presentation::godot_backend
