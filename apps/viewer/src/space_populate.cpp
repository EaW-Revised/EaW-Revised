#include "space_populate.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
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

[[nodiscard]] std::string json(const std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character < 0x20U || character >= 0x7fU) {
            result += "\\u00";
            result.push_back(hex[character >> 4U]);
            result.push_back(hex[character & 15U]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result + "\"";
}

[[nodiscard]] std::string strings(const std::vector<std::string>& values) {
    std::string text{"["};
    for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
    return text + "]";
}

// The documented conversion (x, y, z) -> (x, z, -y) is a signed permutation,
// so conjugating the source matrix by it moves raw Q24 values exactly (the
// same conversion the land populate path applies).
[[nodiscard]] sim::math::Mat3x4 source_to_render(const sim::math::Mat3x4& source) {
    using Fixed = sim::math::Fixed;
    constexpr std::array<std::size_t, 3> axis{0, 2, 1};
    constexpr std::array<std::int64_t, 3> sign{1, 1, -1};
    sim::math::Mat3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result.rows[row][column] = Fixed::from_raw(
                sign[row] * sign[column] * source.rows[axis[row]][axis[column]].raw());
        }
        result.rows[row][3] = Fixed::from_raw(sign[row] * source.rows[axis[row]][3].raw());
    }
    return result;
}

// A live unit's model transform from the simulation's position, facing yaw
// and bank roll (#351), and a breakoff prop's pitch (#391). It is the
// placement conversion every placed object goes through
// (scene::placement_transform), which carries FoC's fixed +90 degree model
// turn (#288), so live units and placed objects turn together and the
// simulation's facing needs no compensation.
[[nodiscard]] core::Result<sim::math::Mat3x4> live_unit_transform(const sim::math::Vec3& position,
    const sim::math::Fixed yaw_degrees, const sim::math::Fixed pitch_degrees, const sim::math::Fixed roll_degrees,
    const sim::math::Fixed scale) {
    return scene::placement_transform(position.x, position.y, position.z, yaw_degrees, pitch_degrees, roll_degrees,
                                      scale);
}

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

// The land path's lighting policy (MapMode::State::lighting_state) applied to
// the space scene: SH from the environment's three lights and ambient, or the
// frozen P0 hemisphere constants.
[[nodiscard]] GodotRenderer::LightingState lighting_state(const SpacePopulation::Options& options,
                                                          const float max_distance) {
    GodotRenderer::LightingState state;
    state.shadows = options.shadows;
    state.shadow_floor = {options.environment.shadow.r, options.environment.shadow.g, options.environment.shadow.b};
    state.shadow_max_distance = max_distance;
    const auto quality = shadow_settings(active_render_profile(), true);
    state.shadow_atlas_size = quality.atlas_size;
    state.shadow_filter = quality.high_filter ? GodotRenderer::ShadowFilter::soft_high
                                            : GodotRenderer::ShadowFilter::soft_medium;
    // Four blended cascades keep the profile's near split depths while the
    // last cascade covers space's longer reach (#231). Preserve the #150
    // bias that clears hull and asteroid acne.
    state.shadow_layout = GodotRenderer::ShadowLayout::parallel_4_splits;
    state.shadow_split_offsets = quality.split_offsets;
    state.shadow_blend_splits = true;
    state.shadow_bias = 2.0F;
    state.shadow_normal_bias = 5.0F;
    lighting::IrradianceMatrices matrices;
    lighting::IrradianceMatrices fill;
    lighting::Vec3 toward{};
    if (options.policy == lighting::Policy::sh) {
        matrices = lighting::source_to_render(lighting::sph_light_all(options.environment));
        fill = lighting::source_to_render(lighting::sph_light_fill(options.environment));
        state.sun_diffuse = lighting::sun_diffuse(options.environment);
        const lighting::Vec3 sun = options.environment.lights[0].direction;
        toward = lighting::source_to_render(lighting::Vec3{-sun.x, -sun.y, -sun.z});
        state.specular = lighting::sun_specular(options.environment);
    } else {
        matrices = lighting::hemisphere_matrices();
        fill = lighting::hemisphere_fill_matrices();
        state.sun_diffuse = lighting::hemisphere_directional;
        const auto& raw = lighting::hemisphere_light_direction;
        toward = {raw[0], raw[1], raw[2]};
        state.specular = {2.0F, 1.88F, 1.72F};
    }
    const float length = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
    state.toward_light = {toward.x / length, toward.y / length, toward.z / length};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        state.sph[channel] = matrices.rgb[channel];
        state.sph_fill[channel] = fill.rgb[channel];
    }
    return state;
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

struct SurfaceUpload final {
    sim::AssetId renderer_asset{};
    std::optional<std::vector<animation::BonePose>> pose;
    // Source-basis bounds of the posed surface in model space.
    std::array<float, 3> minimum{1e30F, 1e30F, 1e30F};
    std::array<float, 3> maximum{-1e30F, -1e30F, -1e30F};
};

} // namespace

SpacePopulation::SpacePopulation(Options options) : options_(std::move(options)) {}

std::optional<lighting::Environment> SpacePopulation::environment(
    const assets::Map& map, const std::string_view choice, std::string& failure) {
    if (choice != "map") return lighting::alo_viewer_default_environment();
    if (map.environments.empty()) {
        failure = "--eawr-environment map: the map declares no environment record";
        return std::nullopt;
    }
    std::vector<lighting::RawField> fields;
    for (const auto& field : map.environments.front().fields) fields.push_back({field.id, field.bytes});
    auto candidate = lighting::candidate_environment(fields);
    if (!candidate) failure = "--eawr-environment map: environment 0 does not decode under the candidate mapping";
    return candidate;
}

std::optional<RenderPass> SpacePopulation::pass(const sim::AssetId asset) const {
    const auto found = passes_.find(asset);
    if (found == passes_.end()) return std::nullopt;
    return found->second;
}

bool SpacePopulation::compose(GodotRenderer& renderer, const assets::Map& map, const vfs::Vfs& filesystem,
                              const FixedCamera& camera, const std::span<const std::uint32_t> environment_records,
                              const sim::AssetId first_asset_id, const sim::EntityId first_entity_id) {
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
    const scene::ObjectResolver resolve = [&](const std::string_view id) -> std::optional<data::EffectiveObject> {
        auto resolved = catalog.resolve(id);
        if (!resolved) return std::nullopt;
        return std::move(resolved.value());
    };
    decisions_ = scene::classify_space_placements(
        *scene_, [&](const std::string_view object_id) -> std::optional<scene::SpaceObjectTags> {
            auto resolved = resolve(object_id);
            if (!resolved) return std::nullopt;
            return scene::space_object_tags(*resolved, resolve);
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
        const data::Definition* definition = catalog.find(ship.object_id);
        auto effective = catalog.resolve(ship.object_id);
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
            auto object = resolve(object_id);
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
    live_bounds_.assign(options_.placed_ships.size(), std::nullopt);
    live_transforms_.assign(options_.placed_ships.size(), std::nullopt);
    for (std::size_t ship_index = 0; ship_index < options_.placed_ships.size(); ++ship_index) {
        const auto& ship = options_.placed_ships[ship_index];
        const bool live = ship.live_entity != 0;
        const std::string live_name = "live unit " + std::to_string(ship.live_entity) + " (" + ship.object_id + ")";
        const data::Definition* definition = catalog.find(ship.object_id);
        auto effective = catalog.resolve(ship.object_id);
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
        if (placed_scene.placements.size() != 1 || !placed_scene.placements.front().drawable()) {
            if (live) {
                (ship.projectile_slot ? projectile_slots_undrawn_ : ship.launch_slot ? launch_slots_undrawn_ : live_undrawn_).push_back(live_name + ": no drawable FoC model");
                continue;
            }
            failure_ = "--eawr-space-place-at: " + ship.object_id + " has no drawable FoC model";
            return false;
        }
        scene::Placement placed = std::move(placed_scene.placements.front());
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
            auto object = resolve(object_id);
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
        lighting_ = lighting_state(options_, camera.far_plane);
        renderer.set_lighting(lighting_);
    }

    std::map<std::string, assets::Texture> textures;
    sim::AssetId next_asset = std::max(first_asset, first_asset_id);
    // One visible submesh through its legacy selector with its authored
    // values; nullopt (recorded) when the renderer refuses it.
    const auto upload_surface = [&](const assets::Model& model, const std::uint32_t mesh_index,
                                    const std::uint32_t submesh_index, const scene::LegacySelector& selector,
                                    const std::string& texture_path, const TeamColour& colour,
                                    const std::string& identity) -> std::optional<SurfaceUpload> {
        const assets::Mesh& source_mesh = model.meshes[mesh_index];
        assets::Model single;
        single.source = model.source;
        single.bones = model.bones;
        assets::Mesh mesh = source_mesh;
        mesh.submeshes = {source_mesh.submeshes[submesh_index]};
        single.meshes.push_back(std::move(mesh));
        const assets::Submesh& submesh = single.meshes.front().submeshes.front();
        assets::Texture texture = placeholder_texture();
        if (!texture_path.empty()) {
            auto found = textures.find(texture_path);
            if (found == textures.end()) {
                auto decoded = assets::load_texture(filesystem, texture_path);
                found = textures.emplace(texture_path, decoded ? std::move(decoded.value()) : placeholder_texture()).first;
            }
            texture = found->second;
        }
        MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::legacy_effect,
            .pass = selector.transparent ? RenderPass::transparent : RenderPass::opaque,
            .program = std::string(selector.program),
            .technique = std::string(selector.technique),
            .pass_name = std::string(selector.pass),
            .bindings = {},
        };
        for (const assets::MaterialParameter& parameter : submesh.parameters) {
            material.bindings.push_back({parameter.name, parameter.value});
        }
        if (colour && scene::colorizes(submesh.shader)) {
            std::erase_if(material.bindings, [](const MaterialBinding& binding) { return ieq(binding.name, "Colorization"); });
            material.bindings.push_back({"Colorization", scene::colorization_binding(material.program, *colour)});
        }
        const std::vector<GodotRenderer::BindingTexture> family_textures = family_binding_textures(material,
            [&](const std::string_view declared) -> std::optional<assets::Texture> {
                const std::string path = probe(cache, "data/art/textures/", declared, texture_suffixes);
                if (path.empty()) return std::nullopt;
                auto found = textures.find(path);
                if (found == textures.end()) {
                    auto decoded = assets::load_texture(filesystem, path);
                    if (!decoded) return std::nullopt;
                    found = textures.emplace(path, std::move(decoded.value())).first;
                }
                return found->second;
            });
        const auto uploaded = options_.reject_upload_for_test && upload_failures_.empty()
            ? core::Result<void>::failure({.code = "EAWR-SPACE-POPULATE-TEST",
                                           .message = "injected supported surface upload failure"})
            : renderer.upload(next_asset, single, texture, material, family_textures);
        if (!uploaded) {
            upload_failures_.push_back(identity + " shader " + submesh.shader + ": "
                                       + core::format_diagnostic(uploaded.error()));
            return std::nullopt;
        }
        SurfaceUpload upload;
        upload.renderer_asset = next_asset++;
        uploaded_.push_back(upload.renderer_asset);
        passes_.emplace(upload.renderer_asset, material.pass);
        ++surfaces_uploaded_;
        if (colour) ++team_colour_variants_;
        const std::int32_t bone = single.meshes.front().bone;
        if (!single.bones.empty() && (bone >= 0 || !submesh.skin_bones.empty())) {
            // Draw the bind pose until this placement's idle clip is sampled.
            if (auto player = animation::Player::create(single)) {
                if (auto pose = player.value().sample({})) upload.pose = std::move(pose.value().bones);
            }
        }
        for (const assets::Vertex& vertex : submesh.vertices) {
            std::array<float, 3> point{vertex.position.x, vertex.position.y, vertex.position.z};
            if (upload.pose && bone >= 0 && static_cast<std::size_t>(bone) < upload.pose->size()) {
                const animation::BonePose& posed = (*upload.pose)[static_cast<std::size_t>(bone)];
                const animation::Matrix& m = submesh.skin_bones.empty() ? posed.model_asset : posed.skin_asset;
                point = {m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12],
                         m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13],
                         m[2] * point[0] + m[6] * point[1] + m[10] * point[2] + m[14]};
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                upload.minimum[axis] = std::min(upload.minimum[axis], point[axis]);
                upload.maximum[axis] = std::max(upload.maximum[axis], point[axis]);
            }
        }
        return upload;
    };

    // #427: a SHIELD submesh with its MeshShield.fx material and its three textures (BP-22).
    // Nullopt with `status` set when a texture does not resolve or the renderer refuses it.
    const auto upload_shell = [&](const assets::Model& model, const std::uint32_t mesh_index,
                                  const std::uint32_t submesh_index, std::string& status) -> std::optional<SurfaceUpload> {
        assets::Model single;
        single.source = model.source;
        single.bones = model.bones;
        assets::Mesh mesh = model.meshes[mesh_index];
        mesh.submeshes = {model.meshes[mesh_index].submeshes[submesh_index]};
        mesh.visible = true;
        single.meshes.push_back(std::move(mesh));
        const assets::Submesh& submesh = single.meshes.front().submeshes.front();
        const MaterialDescription material = meshshield_material(submesh);
        std::optional<assets::Texture> base;
        std::vector<GodotRenderer::BindingTexture> extra;
        for (const std::string_view binding : {std::string_view("BaseTexture"), std::string_view("WaveTexture"),
                                               std::string_view("DistortionTexture")}) {
            const MaterialBinding* found = nullptr;
            for (const MaterialBinding& entry : material.bindings) {
                if (ieq(entry.name, binding)) found = &entry;
            }
            const auto* name = found == nullptr ? nullptr : std::get_if<std::string>(&found->value);
            const std::string path = name == nullptr ? std::string{}
                                                     : probe(cache, "data/art/textures/", *name, texture_suffixes);
            if (path.empty()) {
                status = std::string(binding) + " " + (name == nullptr ? std::string("missing") : *name + " unresolved");
                return std::nullopt;
            }
            auto decoded = assets::load_texture(filesystem, path);
            if (!decoded) {
                status = std::string(binding) + " " + path + ": " + core::format_diagnostic(decoded.error());
                return std::nullopt;
            }
            if (binding == "BaseTexture") base = std::move(decoded.value());
            else extra.push_back({std::string(binding), std::move(decoded.value())});
        }
        const auto uploaded = renderer.upload(next_asset, single, *base, material, extra);
        if (!uploaded) {
            status = "upload_failed: " + core::format_diagnostic(uploaded.error());
            return std::nullopt;
        }
        SurfaceUpload upload;
        upload.renderer_asset = next_asset++;
        uploaded_.push_back(upload.renderer_asset);
        passes_.emplace(upload.renderer_asset, material.pass);
        shield_assets_.push_back(upload.renderer_asset);
        const std::int32_t bone = single.meshes.front().bone;
        if (!single.bones.empty() && (bone >= 0 || !submesh.skin_bones.empty())) {
            if (auto player = animation::Player::create(single)) {
                if (auto pose = player.value().sample({})) upload.pose = std::move(pose.value().bones);
            }
        }
        return upload;  // no bounds: the shell is not part of the pick box
    };
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
            found = uploads.emplace(key, upload_surface(model, mesh_index, submesh_index, selector, texture_path, colour,
                path + " mesh " + std::to_string(mesh_index) + " submesh " + std::to_string(submesh_index))).first;
        }
        return found->second ? &*found->second : nullptr;
    };

    // Retail starts a created object's Idle_Anim_00 at Idle_Anim_00_Rate_Mod
    // from a random frame, looping when Loop_Idle_Anim_00 is set (#145). The
    // random frame is taken from the placement's identity, so captures repeat.
    const auto idle_placement = [&](const scene::Placement& placement, const std::size_t clip) {
        IdlePlacement idle{.clip = clip};
        if (auto object = resolve(placement.object_id)) {
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
                                                  upload_shell(*model, *shield, submesh, cause)).first;
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
                           piece.live, piece.local, piece.shield});
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

std::size_t SpacePopulation::hidden_decals(const std::size_t decision) const {
    if (decision >= decisions_.size() || decision >= hardpoint_states_.size()) return 0;
    const auto& states = hardpoint_states_[decision];
    return static_cast<std::size_t>(std::count_if(decisions_[decision].damage_decals.begin(),
        decisions_[decision].damage_decals.end(), [&](const auto& decal) {
            return decal.hardpoint >= states.size() || !scene::hardpoint_art(states[decal.hardpoint]).damage_decal;
        }));
}

bool SpacePopulation::set_hardpoint_state(const std::uint64_t scene_ordinal, const std::string_view hardpoint,
                                          const scene::HardpointState state) {
    bool found = false;
    bool changed = false;
    for (std::size_t index = 0; index < decisions_.size() && index < hardpoint_states_.size(); ++index) {
        const scene::SpacePlacementDecision& decision = decisions_[index];
        if (decision.scene_ordinal != scene_ordinal) continue;
        for (std::size_t entry = 0; entry < decision.hardpoints.size() && entry < hardpoint_states_[index].size(); ++entry) {
            if (!ieq(decision.hardpoints[entry].hardpoint, hardpoint)) continue;
            changed = changed || hardpoint_states_[index][entry] != state;
            hardpoint_states_[index][entry] = state;
            found = true;
        }
    }
    if (!found) return false;
    refresh_instances();
    if (changed && composed_ && options_.attached_effects && !options_.attached_effects(*this)) {
        failure_ = "hardpoint " + std::string(hardpoint) + ": the attached effects could not follow its state";
        return false;
    }
    return true;
}

std::span<const scene::HardpointState> SpacePopulation::hardpoint_states(const std::uint64_t scene_ordinal) const {
    for (std::size_t index = 0; index < decisions_.size() && index < hardpoint_states_.size(); ++index) {
        if (decisions_[index].scene_ordinal == scene_ordinal) return hardpoint_states_[index];
    }
    return {};
}

bool SpacePopulation::is_marker(const std::uint64_t scene_ordinal) const {
    return std::any_of(decisions_.begin(), decisions_.end(), [&](const scene::SpacePlacementDecision& decision) {
        return decision.scene_ordinal == scene_ordinal && decision.role == scene::SpaceRole::marker;
    });
}

void SpacePopulation::refresh_instances() {
    instances_.clear();
    std::uint64_t shells = 0;
    std::set<std::pair<std::size_t, std::size_t>> attached;
    for (const GatedInstance& piece : pieces_) {
        if (piece.live && *piece.live < live_shown_.size() && !live_shown_[*piece.live]) continue;
        if (piece.shield) {
            if (!piece.live || *piece.live >= live_defend_.size() || !live_defend_[*piece.live]) continue;
            ++shells;
        }
        if (piece.gate) {
            const HardpointGate& gate = *piece.gate;
            const scene::HardpointArt art = scene::hardpoint_art(hardpoint_states_[gate.decision][gate.hardpoint]);
            const bool visible = gate.art == HardpointGate::Art::attached_model ? art.attached_model
                : gate.art == HardpointGate::Art::damage_decal ? art.damage_decal
                : hardpoint_states_[gate.decision][gate.hardpoint] != scene::HardpointState::destroyed;
            if (!visible) continue;
            if (gate.art == HardpointGate::Art::attached_model) attached.emplace(gate.decision, gate.hardpoint);
        }
        instances_.push_back(piece.instance);
    }
    instance_count_ = instances_.size() - shells;
    attachments_drawn_ = attached.size();
    shield_shells_shown_ = std::max(shield_shells_shown_, shells);
}

void SpacePopulation::pose_units(GodotRenderer& renderer, const std::uint32_t sample) {
    const std::uint32_t held_sample = options_.live_clock ? sample : std::min(sample, options_.animation_frames - 1U);
    if (idle_placements_.empty() || animation_sample_ == held_sample) return;
    animation_sample_ = held_sample;
    const std::uint64_t tick = static_cast<std::uint64_t>(held_sample) + options_.idle_offset;
    std::vector<std::optional<animation::Pose>> poses(idle_placements_.size());
    for (std::size_t index = 0; index < idle_placements_.size(); ++index) {
        const IdlePlacement& idle = idle_placements_[index];
        auto pose = animation::sample_idle(*clips_[idle.clip], idle.playback, idle.start_frame, tick,
                                           particles::map_owner_ticks_per_second);
        if (pose) poses[index] = std::move(pose.value());
        else ++idle_sample_failures_;
    }
    for (const AnimatedInstance& instance : animated_instances_) {
        if (!poses[instance.idle]) continue;
        const auto posed = renderer.set_skin_pose(instance.entity, instance.asset, poses[instance.idle]->bones);
        if (!posed) upload_failures_.push_back("entity " + std::to_string(instance.entity) + " idle pose: "
                                               + core::format_diagnostic(posed.error()));
    }
}

bool SpacePopulation::live_ship_drawn(const std::size_t ship) const noexcept {
    return ship < live_decisions_.size() && live_decisions_[ship].has_value();
}

const animation::Player* SpacePopulation::live_clip(const std::size_t ship, const bool alternate) const noexcept {
    const auto& bound = alternate ? live_alternate_clips_ : live_clips_;
    return ship < bound.size() && bound[ship] ? clips_[*bound[ship]].get() : nullptr;
}

bool SpacePopulation::pose_live_clips(GodotRenderer& renderer, const std::span<const LiveClipPose> poses) {
    for (const LiveClipPose& request : poses) {
        const animation::Player* player = live_clip(request.ship, request.alternate);
        if (player == nullptr) continue;
        auto pose = animation::sample_death_frame(*player, {request.position, request.blend_from});
        if (!pose) {
            failure_ = "live clip of ship " + std::to_string(request.ship) + ": " + core::format_diagnostic(pose.error());
            return false;
        }
        // Bone visibility is part of a clip (a death clip hides the pieces it
        // breaks off); the palette has no visibility, so a hidden bone's
        // geometry collapses to the bone's origin.
        std::vector<animation::BonePose> bones = std::move(pose.value().bones);
        for (animation::BonePose& bone : bones) {
            if (bone.visible) continue;
            ++live_clip_hidden_bones_;
            for (animation::Matrix* matrix : {&bone.skin_asset, &bone.model_asset}) {
                for (std::size_t index = 0; index < 12; ++index) (*matrix)[index] = 0.0F;
            }
        }
        for (const LiveClipInstance& instance : live_clip_instances_) {
            if (instance.ship != request.ship) continue;
            const auto posed = renderer.set_skin_pose(instance.entity, instance.asset, bones);
            if (!posed) {
                failure_ = "entity " + std::to_string(instance.entity) + " live clip pose: "
                    + core::format_diagnostic(posed.error());
                return false;
            }
        }
        ++live_clip_poses_;
    }
    return true;
}

std::optional<SpacePopulation::LiveShipBox> SpacePopulation::live_ship_box(const std::size_t ship) const {
    if (ship >= live_bounds_.size() || ship >= live_transforms_.size() || !live_bounds_[ship] || !live_transforms_[ship]
        || !live_ship_drawn(ship)) {
        return std::nullopt;
    }
    return LiveShipBox{*live_transforms_[ship], live_bounds_[ship]->first, live_bounds_[ship]->second};
}

std::optional<SpacePopulation::LiveShipEmitterView> SpacePopulation::live_ship_emitter_view(const std::size_t ship) const {
    if (!live_ship_drawn(ship) || live_retired_[ship]) return std::nullopt;
    const std::size_t decision = *live_decisions_[ship];
    LiveShipEmitterView view;
    view.placement = &scene_->placements[decisions_[decision].scene_ordinal];
    view.hardpoints = decisions_[decision].hardpoints;
    if (decision < hardpoint_states_.size()) view.states = hardpoint_states_[decision];
    if (live_shown_[ship] && ship < live_transforms_.size()) view.model_to_world = live_transforms_[ship];
    view.death_clone = options_.placed_ships[ship].death_clone;
    view.projectile = options_.placed_ships[ship].projectile_slot;
    view.entity = options_.placed_ships[ship].live_entity;
    return view;
}

std::optional<sim::math::Mat3x4> SpacePopulation::live_transform(const LivePose& pose) const {
    if (!live_ship_drawn(pose.ship) || live_retired_[pose.ship]) return std::nullopt;
    const scene::Placement& placement = scene_->placements[decisions_[*live_decisions_[pose.ship]].scene_ordinal];
    auto transform = live_unit_transform(pose.position, pose.yaw_degrees, pose.pitch_degrees, pose.roll_degrees,
                                         sim::math::Fixed::from_raw(placement.scale_raw));
    if (!transform) return std::nullopt;
    return transform.value();
}

bool SpacePopulation::pose_live(const std::span<const LivePose> poses) {
    std::fill(live_shown_.begin(), live_shown_.end(), false);
    std::fill(live_defend_.begin(), live_defend_.end(), false);
    std::vector<std::optional<sim::math::Mat3x4>> transforms(live_shown_.size());
    for (const LivePose& pose : poses) {
        if (!live_ship_drawn(pose.ship) || live_retired_[pose.ship]) continue;
        const std::size_t decision = *live_decisions_[pose.ship];
        const auto transform = live_transform(pose);
        if (!transform) {
            failure_ = "live unit " + std::to_string(options_.placed_ships[pose.ship].live_entity)
                + ": its transform leaves the Q24 range";
            return false;
        }
        transforms[pose.ship] = *transform;
        live_shown_[pose.ship] = true;
        live_defend_[pose.ship] = pose.defend_active;
        auto& states = hardpoint_states_[decision];
        for (std::size_t index = 0; index < pose.hardpoints.size() && index < states.size(); ++index) {
            states[index] = pose.hardpoints[index];
        }
    }
    live_transforms_ = transforms;
    for (GatedInstance& piece : pieces_) {
        if (!piece.live || !transforms[*piece.live]) continue;
        auto placed = sim::math::compose(*transforms[*piece.live], piece.local);
        if (!placed) {
            failure_ = "live unit " + std::to_string(options_.placed_ships[*piece.live].live_entity)
                + ": a piece transform leaves the Q24 range";
            return false;
        }
        piece.instance.fixed_transform = source_to_render(placed.value());
    }
    refresh_instances();
    return true;
}

std::vector<sim::EntityId> SpacePopulation::live_hull_entities(const std::size_t ship) const {
    std::vector<sim::EntityId> result;
    for (const GatedInstance& piece : pieces_) {
        if (piece.live != ship || piece.shield) continue;
        if (piece.gate && piece.gate->art == HardpointGate::Art::attached_model) continue;
        result.push_back(piece.instance.entity_id);
    }
    return result;
}

std::vector<sim::EntityId> SpacePopulation::live_ship_entities(const std::size_t ship) const {
    std::vector<sim::EntityId> result;
    for (const GatedInstance& piece : pieces_) {
        if (piece.live != ship) continue;
        result.push_back(piece.instance.entity_id);
    }
    return result;
}

void SpacePopulation::set_shield_time(GodotRenderer& renderer, const float seconds) {
    for (const sim::AssetId asset : shield_assets_) {
        if (std::find(uploaded_.begin(), uploaded_.end(), asset) == uploaded_.end()) continue;
        if (auto set = renderer.set_material_scalar(asset, meshshield_time_binding, seconds); !set) {
            upload_failures_.push_back("shield shell " + std::to_string(asset) + " clock: "
                                       + core::format_diagnostic(set.error()));
        }
    }
}

void SpacePopulation::retire_live_ship(GodotRenderer& renderer, const std::size_t ship) {
    if (ship >= live_retired_.size() || live_retired_[ship]) return;
    live_retired_[ship] = true;
    std::set<sim::AssetId> dropped;
    std::set<sim::EntityId> entities;
    for (const GatedInstance& piece : pieces_) {
        if (piece.live != ship) continue;
        renderer.clear_skin_pose(piece.instance.entity_id);
        dropped.insert(piece.instance.asset_id);
        entities.insert(piece.instance.entity_id);
    }
    std::erase_if(pieces_, [&](const GatedInstance& piece) { return piece.live == ship; });
    std::erase_if(live_clip_instances_, [&](const LiveClipInstance& instance) { return instance.ship == ship; });
    std::erase_if(animated_instances_, [&](const AnimatedInstance& instance) { return entities.contains(instance.entity); });
    if (live_clips_[ship]) clips_[*live_clips_[ship]].reset();
    live_clips_[ship].reset();
    if (live_alternate_clips_[ship]) clips_[*live_alternate_clips_[ship]].reset();
    live_alternate_clips_[ship].reset();
    live_shown_[ship] = false;
    // Uploads are shared by model, surface and colour: another clone of the same type keeps them.
    for (const GatedInstance& piece : pieces_) dropped.erase(piece.instance.asset_id);
    for (const sim::AssetId asset : dropped) {
        static_cast<void>(renderer.release(asset));
        std::erase(uploaded_, asset);
        passes_.erase(asset);
        ++live_released_assets_;
    }
    refresh_instances();
}

void SpacePopulation::release(GodotRenderer& renderer) {
    composed_ = false;
    for (const GatedInstance& piece : pieces_) renderer.clear_skin_pose(piece.instance.entity_id);
    for (const sim::AssetId asset : uploaded_) static_cast<void>(renderer.release(asset));
    uploaded_.clear();
    pieces_.clear();
    instances_.clear();
    live_clip_instances_.clear();
}

void SpacePopulation::write_report(std::ostream& output) const {
    std::map<scene::SpaceRole, std::size_t> roles;
    for (const auto& decision : decisions_) ++roles[decision.role];
    // Objects not drawn, or drawn with something left out, grouped by object
    // and reason so the list stays readable.
    std::map<std::pair<std::string, std::string>, std::vector<std::uint64_t>> groups;
    for (const auto& decision : decisions_) {
        if (decision.role == scene::SpaceRole::drawn && decision.missing.empty()) continue;
        std::string reason(scene::to_string(decision.role));
        for (const std::string& item : decision.missing) reason += "; " + item;
        groups[{decision.object_id.empty() ? std::string("(uncatalogued)") : decision.object_id, reason}]
            .push_back(decision.scene_ordinal);
    }
    output << "  \"populate\": {\"requested\": true, \"declared_placements\": "
           << map_placements_ << ", \"composed\": " << roles[scene::SpaceRole::drawn]
           << ", \"scene_sha256\": " << json(scene_ ? scene_->scene_sha256 : "");
    if (!options_.placed_ships.empty()) output << ", \"placed_ships\": " << options_.placed_ships.size();
    if (!live_decisions_.empty() && std::any_of(options_.placed_ships.begin(), options_.placed_ships.end(),
            [](const Options::PlacedShip& ship) { return ship.live_entity != 0; })) {
        // #81, #391: death clones and breakoff props are live ships too, counted apart from
        // the session's units.
        std::size_t drawn = 0;
        std::size_t clones_drawn = 0;
        std::size_t breakoffs_drawn = 0;
        std::size_t launch_slots_drawn = 0;
        std::size_t projectile_slots_drawn = 0;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (!live_decisions_[ship]) continue;
            const Options::PlacedShip& placed = options_.placed_ships[ship];
            ++(placed.death_clone ? clones_drawn
               : placed.breakoff  ? breakoffs_drawn
               : placed.launch_slot ? launch_slots_drawn
               : placed.projectile_slot ? projectile_slots_drawn
                                    : drawn);
        }
        output << ", \"live_units\": {\"drawn\": " << drawn << ", \"not_drawn\": " << strings(live_undrawn_)
               << ", \"session_records_skipped\": " << session_records_skipped_
               << ", \"clones_drawn\": " << clones_drawn << ", \"breakoffs_drawn\": " << breakoffs_drawn
               << ", \"launch_slots_drawn\": " << launch_slots_drawn
               << ", \"launch_slots_not_drawn\": " << strings(launch_slots_undrawn_)
               << ", \"projectile_slots_drawn\": " << projectile_slots_drawn
               << ", \"projectile_slots_not_drawn\": " << strings(projectile_slots_undrawn_)
               << ", \"ships_retired\": " << std::count(live_retired_.begin(), live_retired_.end(), true)
               << ", \"released_assets\": " << live_released_assets_;
        if (!live_clip_status_.empty()) {
            output << ", \"clips\": {";
            bool first = true;
            for (const auto& [key, status] : live_clip_status_) {
                output << (first ? "" : ", ") << json(key) << ": " << json(status);
                first = false;
            }
            output << "}, \"clip_poses\": " << live_clip_poses_ << ", \"clip_hidden_bones\": " << live_clip_hidden_bones_;
        }
        // #427: each DEFEND type's shield shell and the most shells one frame drew.
        output << ", \"shield_shells\": {\"types\": {";
        bool first_shell = true;
        for (const auto& [object, status] : shield_shells_) {
            output << (first_shell ? "" : ", ") << json(object) << ": " << json(status);
            first_shell = false;
        }
        output << "}, \"max_shown\": " << shield_shells_shown_ << "}";
        output << "}";
    }
    if (options_.debug_ship) {
        output << ", \"debug_ship\": {\"status\": " << json(debug_ship_status_)
               << ", \"object\": " << json(options_.debug_ship->object_id)
               << ", \"spawn_record\": " << options_.debug_ship->spawn_record
               << ", \"yaw_degrees\": " << debug_ship_yaw_ << ", \"pose\": \"idle_bind\", \"source\": "
               << json("FoC " + debug_ship_type_ + " XML")
               << ", \"hardpoint_states\": {";
        // The last value given for an id is the one applied.
        const auto& states = options_.debug_ship->hardpoint_states;
        bool first_state = true;
        for (std::size_t index = 0; index < states.size(); ++index) {
            if (std::any_of(states.begin() + static_cast<std::ptrdiff_t>(index) + 1, states.end(),
                            [&](const auto& later) { return later.first == states[index].first; })) continue;
            output << (first_state ? "" : ", ") << json(states[index].first) << ": "
                   << json(scene::to_string(states[index].second));
            first_state = false;
        }
        output << "}}";
    }
    output
           << ",\n    \"classification\": \"scene::classify_space_placements: catalog markers (Is_Marker or the Marker "
              "element) are never drawn; nebula and background objects (Is_Nebula, In_Background) are left to the "
              "space environment; every other placement with a supported visible surface is drawn. Each HardPoints "
              "entry's state selects its art (scene::hardpoint_art): an intact hardpoint draws its Model_To_Attach "
              "at the bind frame of its Attachment_Bone and hides its Damage_Decal mesh and the damage emitters at "
              "and below its Damage_Particles bone. MeshShadowVolume surfaces are stencil-shadow geometry and are "
              "not drawn in colour\""
           << ",\n    \"roles\": {\"drawn\": " << roles[scene::SpaceRole::drawn]
           << ", \"environment\": " << roles[scene::SpaceRole::environment]
           << ", \"marker\": " << roles[scene::SpaceRole::marker]
           << ", \"not_drawable\": " << roles[scene::SpaceRole::not_drawable] << "}"
           << ", \"surfaces_uploaded\": " << surfaces_uploaded_ << ", \"instances\": " << instance_count_
           << ", \"hardpoints_attached\": " << attachments_drawn_
           << ", \"skinned_instances\": " << skinned_instances_
            << ",\n    \"team_colour\": {\"colour_variants\": " << team_colour_variants_
            << ", \"source\": \"TED owner to catalog faction <Color>\", \"drawn_by_status\": {";
    bool first_status = true;
    for (const auto& [status, count] : team_colour_status_) {
        output << (first_status ? "" : ", ") << json(status) << ": " << count;
        first_status = false;
    }
    output << "}}, \"unit_animation\": {\"clips\": " << clips_.size()
            << ", \"placements_animated\": " << animated_placements_
            << ", \"instances_animated\": " << animated_instances_.size()
            << ", \"sample_at_capture\": " << (animation_sample_ ? std::to_string(*animation_sample_) : "null")
            << ", \"clock\": " << json(options_.live_clock ? "live" : "held")
            << ", \"idle_offset\": " << options_.idle_offset
            << ", \"idle_sample_failures\": " << idle_sample_failures_
            << ", \"idle_rule\": " << json(idle_rule)
            << ", \"by_status\": {";
    first_status = true;
    for (const auto& [status, count] : animation_status_) {
        output << (first_status ? "" : ", ") << json(status) << ": " << count;
        first_status = false;
    }
    output << "}, \"bound_idle_hulls\": [";
    bool first_hull = true;
    for (const auto& [object, clip] : bound_idle_hulls_) {
        output << (first_hull ? "" : ", ") << "{\"object\": " << json(object)
               << ", \"clip\": " << json(clip);
        if (const auto playback = idle_playbacks_.find(object); playback != idle_playbacks_.end()) {
            const animation::IdlePlayback& idle = playback->second;
            output << ", \"loop\": " << (idle.loop ? "true" : "false")
                   << ", \"restarts\": " << (idle.restarts ? "true" : "false")
                   << ", \"random_start\": " << (idle.random_start ? "true" : "false")
                   << ", \"rate_mod\": [" << idle.rate.numerator << ", " << idle.rate.denominator << ']';
        }
        if (const auto rejected = idle_rate_rejected_.find(object); rejected != idle_rate_rejected_.end())
            output << ", \"rate_mod_rejected\": " << json(rejected->second);
        output << '}';
        first_hull = false;
    }
    output << "], \"unbound_skinned_hulls\": [";
    first_hull = true;
    for (const auto& [object, reason] : static_hulls_) {
        output << (first_hull ? "" : ", ") << "{\"object\": " << json(object)
               << ", \"reason\": " << json(reason) << '}';
        first_hull = false;
    }
    output << "]}, \"attached_effects\": {\"composed\": " << (attached_effects_.composed ? "true" : "false")
           << ", \"records\": " << attached_effects_.records
           << ", \"hidden_by_hardpoint_state\": " << attached_effects_.hardpoint_hidden
           << ", \"admitted\": " << attached_effects_.admitted
           << ", \"capacity_exhausted\": " << attached_effects_.capacity_exhausted
           << ", \"report\": " << (attached_effects_.composed ? "\"map_particles\"" : "null")
           << ", \"cause\": " << json(attached_effects_.cause) << '}'
           << ",\n    \"upload_failures\": " << strings(upload_failures_) << ",\n    \"not_drawn_or_partial\": [";
    bool first = true;
    for (const auto& [key, ordinals] : groups) {
        std::string list;
        for (std::size_t index = 0; index < ordinals.size(); ++index) {
            list += (index == 0 ? "" : ",") + std::to_string(ordinals[index]);
        }
        output << (first ? "\n      " : ",\n      ") << "{\"object\": " << json(key.first)
               << ", \"placements\": " << ordinals.size() << ", \"ordinals\": " << json(list)
               << ", \"reason\": " << json(key.second) << "}";
        first = false;
    }
    output << (groups.empty() ? "" : "\n    ") << "],\n    \"placements\": [";
    for (std::size_t index = 0; index < decisions_.size(); ++index) {
        const auto& decision = decisions_[index];
        const scene::Placement& placement = scene_->placements[decision.scene_ordinal];
        output << (index == 0 ? "\n      " : ",\n      ") << "{\"ordinal\": " << decision.scene_ordinal
               << ", \"object\": " << json(decision.object_id) << ", \"type\": " << json(decision.type_name)
               << ", \"role\": " << json(scene::to_string(decision.role)) << ", \"model\": " << json(placement.model_path)
               << ", \"drawn_surfaces\": " << decision.drawn_surfaces.size()
               << ", \"stencil_shadow_surfaces\": " << decision.shadow_volume_surfaces
               << ", \"hidden_damage_decal_surfaces\": " << hidden_decals(index)
               << ", \"hardpoints\": " << decision.hardpoints.size()
               << ", \"missing\": " << strings(decision.missing) << "}";
    }
    output << (decisions_.empty() ? "" : "\n    ") << "]},\n";

    output << "  \"lighting\": {\"policy\": " << json(lighting::to_string(options_.policy))
           << ", \"composed\": " << (options_.policy != lighting::Policy::off ? "true" : "false")
           << ", \"shadows\": " << (options_.shadows ? "true" : "false")
           << ", \"environment\": " << json(options_.environment_source)
           << ", \"shadow_max_distance\": " << shadow_max_distance_;
    if (options_.policy != lighting::Policy::off) {
        output << ", \"shadow_mode\": " << json(to_string(lighting_.shadow_layout))
               << ", \"shadow_atlas_size\": " << lighting_.shadow_atlas_size
               << ", \"shadow_filter\": " << json(to_string(lighting_.shadow_filter))
               << ", \"shadow_stabilization\": \"godot_sphere_texel_snap\""
               << ", \"shadow_split_offsets\": [" << lighting_.shadow_split_offsets[0] << ", "
               << lighting_.shadow_split_offsets[1] << ", " << lighting_.shadow_split_offsets[2] << ']'
               << ", \"shadow_bias\": " << lighting_.shadow_bias.value_or(0.0F)
               << ", \"shadow_normal_bias\": " << lighting_.shadow_normal_bias.value_or(0.0F)
               << ", \"shadow_floor\": [" << lighting_.shadow_floor[0] << ", " << lighting_.shadow_floor[1] << ", "
               << lighting_.shadow_floor[2] << ']';
    }
    if (options_.debug_ship) output << ", \"shadow_scope\": \"scene directional light (debug evidence only)\"";
    output << ", \"shadow_receiving_materials\": " << shadow_receiving_
           << ", \"shadow_variant_failures\": " << shadow_variant_failures_
           << ", \"cause\": " << json(options_.policy == lighting::Policy::off
               ? "no lighting policy requested: hulls keep the P0 legacy material constants"
               : "the land lighting policy applied to the populated space scene: one directional sun from the "
                 "environment and its SH irradiance on every hull; the sky adapter stays unshaded and casts no shadow")
           << "},\n";
}

} // namespace eawr::presentation::godot_backend
