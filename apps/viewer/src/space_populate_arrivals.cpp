#include "space_populate.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "frame_timer.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/text.hpp"
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

struct SpacePopulation::ComposeBatch final {
    std::map<std::string, assets::Texture> textures;
    sim::AssetId next_asset{};
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
        bool cloak{};                       // its ship's STEALTH sub-object (WST-12)
        bool cloak_hidden{};                // hidden while its ship is cloaked (WST-12)
        std::optional<bool> blast_charged{};
        // GM-03: its team colour, set on its instance; the upload is colour-free.
        TeamColour colour{};
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
};

void SpacePopulation::prepare_compose_slots(bool appending) {
    if (!appending) {
        live_decisions_.clear(); hull_surface_assets_.clear(); live_models_.clear(); live_shown_.clear();
        live_defend_.clear(); live_stealth_.clear(); live_blast_charging_.clear(); live_blast_charged_.clear();
        live_clips_.clear(); turret_bindings_.clear(); live_alternate_clips_.clear(); live_retired_.clear();
        live_death_clone_.clear(); live_alternates_.clear(); live_alternate_counts_.clear();
        live_bounds_.clear(); live_transforms_.clear(); backdrop_ships_.clear();
        hardpoint_states_.clear();
        shadow_receiving_ = shadow_variant_failures_ = 0;
    }
    live_decisions_.resize(options_.placed_ships.size(), std::nullopt);
    hull_surface_assets_.resize(options_.placed_ships.size(), {});
    live_models_.resize(options_.placed_ships.size(), std::nullopt);
    live_shown_.resize(options_.placed_ships.size(), !appending);
    live_defend_.resize(options_.placed_ships.size(), false);
    live_stealth_.resize(options_.placed_ships.size(), false);
    live_blast_charging_.resize(options_.placed_ships.size(), false);
    live_blast_charged_.resize(options_.placed_ships.size(), false);
    live_clips_.resize(options_.placed_ships.size(), std::nullopt);
    contact_bones_.resize(options_.placed_ships.size());
    turret_bindings_.resize(options_.placed_ships.size(), std::nullopt);
    live_alternate_clips_.resize(options_.placed_ships.size(), std::nullopt);
    live_retired_.resize(options_.placed_ships.size(), false);
    live_death_clone_.resize(options_.placed_ships.size(), false);
    live_alternates_.resize(options_.placed_ships.size(), 0);
    live_alternate_counts_.resize(options_.placed_ships.size(), 0);
    live_bounds_.resize(options_.placed_ships.size(), std::nullopt);
    live_transforms_.resize(options_.placed_ships.size(), std::nullopt);
    backdrop_ships_.resize(options_.placed_ships.size(), false);
}

bool SpacePopulation::classify_arrivals(ComposeContext& context, std::size_t first_ship, std::size_t first_decision) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    const data::Catalog& catalog = *options_.catalog;
    for (std::size_t ship_index = first_ship; ship_index < options_.placed_ships.size(); ++ship_index) {
        const auto& ship = options_.placed_ships[ship_index];
        live_death_clone_[ship_index] = ship.death_clone;
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
        source.key = {map.source, live ? static_cast<std::uint32_t>(map_placements_ + ship_index) : 0U};
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
        if (placed_scene.placements.size() == 1) {
            // MM-17: no model path is a known model-free placement. A model that
            // is currently hidden or has no drawable surface still has a model identity.
            live_models_[ship_index] = !placed_scene.placements.front().model_path.empty();
        }
        if (placed_scene.placements.size() != 1
            || (!placed_scene.placements.front().drawable()
                && !((ship.projectile_slot || ship.ability_proxy_owner)
                    && scene::drawable_projectile_effects(placed_scene.placements.front())))) {
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
        // BP-75: backdrop reporting is separate from the unit draw policy. The
        // background-scene pass, depth clear and far plane remain unimplemented.
        backdrop_ships_[ship_index] = scene::space_object_tags(effective.value()).background;
        auto decision = classify_composed_unit(scene_->placements.back(), effective.value(), resolve, ship.ability_proxy_owner);
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
    for (auto& decision : std::span(decisions_).subspan(first_decision)) {
        const auto record = scene_->placements[decision.scene_ordinal].record_ordinal;
        if (std::find(environment_records.begin(), environment_records.end(), record) != environment_records.end()) {
            decision.role = scene::SpaceRole::environment;
            decision.drawn_surfaces.clear();
            decision.hardpoints.clear();
            decision.damage_decals.clear();
            decision.missing.clear();
        }
    }

    return true;
}

std::optional<std::size_t> SpacePopulation::bind_composed_idle(ComposeContext& context, ComposeBatch& batch,
    const scene::Placement& placement, const assets::Model& model) {
    const auto& filesystem = context.filesystem;
    auto& clip_bindings = batch.clip_bindings;
    using ClipBinding = ComposeBatch::ClipBinding;
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
}

// #81: a live ship's own clip (a death clone's DIE clip), bound like an
// idle clip but posed by pose_live_clips() on the live session's clock.
std::optional<std::size_t> SpacePopulation::bind_composed_clip(ComposeContext& context,
    const scene::Placement& placement, const assets::Model& model, const std::string& path) {
    const auto& filesystem = context.filesystem;
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
}

const SpacePopulation::SurfaceUpload* SpacePopulation::upload_composed_surface(ComposeContext& context,
    ComposeBatch& batch, const std::string& path, const assets::Model& model, std::uint32_t mesh_index,
    std::uint32_t submesh_index, const scene::LegacySelector& selector, const std::string& texture_path) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& uploads = batch.uploads;
    auto& textures = batch.textures;
    auto& next_asset = batch.next_asset;
    // GM-03: one upload per surface for every team colour; the colour is per instance.
    const auto key = std::make_tuple(path, mesh_index, submesh_index, TeamColour{});
    auto found = uploads.find(key);
    if (found == uploads.end()) {
        found = uploads.emplace(key, upload_surface(renderer, filesystem, cache, textures, next_asset, model, mesh_index, submesh_index, selector, texture_path, TeamColour{},
            path + " mesh " + std::to_string(mesh_index) + " submesh " + std::to_string(submesh_index))).first;
    }
    return found->second ? &*found->second : nullptr;
}

// Retail starts a created object's Idle_Anim_00 at Idle_Anim_00_Rate_Mod
// from a random frame, looping when Loop_Idle_Anim_00 is set (#145). The
// random frame is taken from the placement's identity, so captures repeat.
IdlePlacement SpacePopulation::composed_idle_placement(ComposeContext& context, const scene::Placement& placement,
    std::size_t clip) {
    const auto& resolve = context.resolve;
    IdlePlacement idle{.clip = clip};
    if (auto object = resolve(placement.object_id, data::Category::game_object)) {
        const DeclaredIdle declared = declared_idle(scene::space_object_tags(*object).idle);
        idle.playback = declared.playback;
        if (declared.rejected_rate) idle_rate_rejected_[placement.object_id] = *declared.rejected_rate;
    }
    idle.start_frame = placement_start_frame(placement, clips_[clip]->playable_frames());
    idle_playbacks_[placement.object_id] = idle.playback;
    return idle;
}

void SpacePopulation::compose_hull_pieces(ComposeContext& context, ComposeBatch& batch,
    scene::SpacePlacementDecision& decision, std::size_t decision_index, const scene::Placement& placement,
    const assets::Model* model, const std::optional<std::size_t>& live_ship,
    const std::optional<std::array<std::uint8_t, 3>>& colour, bool cloak_ship) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& pieces = batch.pieces;
    auto& failed_placements = batch.failed_placements;
    const auto add_surface = [&](const std::size_t index, const std::optional<HardpointGate> gate) {
        const scene::Surface& surface = placement.surfaces[index];
        // WST-12: the STEALTH sub-object is drawn only as the cloak below.
        if (cloak_ship && ieq(surface.mesh_name, stealth_mesh_name)) return;
        std::string texture_path;
        for (const scene::TextureBinding& binding : surface.textures) {
            if (ieq(binding.parameter, "BaseTexture")) texture_path = binding.resolved;
        }
        const SurfaceUpload* upload = upload_composed_surface(context, batch, placement.model_path, *model, surface.mesh_index,
            surface.submesh_index, *scene::material_selector(surface), texture_path);
        if (upload != nullptr) {
            pieces.push_back({upload, placement.transform->matrix, std::nullopt, gate, live_ship,
                              sim::math::identity_matrix()});
            if (!surface.effect_missing && scene::colorizes(surface.shader)) pieces.back().colour = colour;
            pieces.back().cloak_hidden = cloak_ship && ieq(surface.mesh_name, cloak_base_mesh_name);
            if (ieq(surface.mesh_name, "buildup") || ieq(surface.mesh_name, "charged")) {
                pieces.back().blast_charged = ieq(surface.mesh_name, "charged");
            }
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
    // BP-74/WHE-35/36: hidden charge meshes are admitted once, then gated by the
    // published BLAST phase, including FD-55's delayed charged cleanup.
    if (live_ship) for (std::uint32_t mesh_index = 0; mesh_index < model->meshes.size(); ++mesh_index) {
        const auto& mesh = model->meshes[mesh_index];
        if (!ieq(mesh.name, "buildup") && !ieq(mesh.name, "charged")) continue;
        if (std::any_of(decision.drawn_surfaces.begin(), decision.drawn_surfaces.end(), [&](const auto index) {
            return placement.surfaces[index].mesh_index == mesh_index;
        })) continue;
        for (std::uint32_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const auto& submesh = mesh.submeshes[submesh_index];
            const bool effect_missing = scene::missing_material_effect(cache.access(), submesh.shader);
            const auto* selector = scene::material_selector(submesh.shader, effect_missing);
            if (selector == nullptr) {
                decision.missing.push_back("charge mesh " + mesh.name + " unsupported shader " + submesh.shader);
                continue;
            }
            std::string texture_path;
            if (!effect_missing) for (const auto& parameter : submesh.parameters) {
                if (!ieq(parameter.name, "BaseTexture")) continue;
                if (const auto* name = std::get_if<std::string>(&parameter.value))
                    texture_path = probe(cache, "data/art/textures/", *name, texture_suffixes);
            }
            const auto* upload = upload_composed_surface(context, batch, placement.model_path, *model, mesh_index, submesh_index,
                *selector, texture_path);
            if (upload == nullptr) continue;
            pieces.push_back({upload, placement.transform->matrix, std::nullopt, std::nullopt, live_ship,
                              sim::math::identity_matrix()});
            pieces.back().blast_charged = ieq(mesh.name, "charged");
            pieces.back().cloak_hidden = cloak_ship;
        }
    }
    // Each Damage_Decal surface is uploaded as well and drawn once its
    // hardpoint is destroyed.
    for (const auto& decal : decision.damage_decals) {
        add_surface(decal.surface, HardpointGate{decision_index, decal.hardpoint, HardpointGate::Art::damage_decal});
    }
}

void SpacePopulation::compose_shell_pieces(ComposeContext& context, ComposeBatch& batch,
    scene::SpacePlacementDecision& decision, const scene::Placement& placement, const assets::Model* model,
    const std::optional<std::size_t>& live_ship, bool cloak_ship) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& pieces = batch.pieces;
    auto& shell_uploads = batch.shell_uploads;
    auto& next_asset = batch.next_asset;
    using Piece = ComposeBatch::Piece;
    // #427: the SHIELD sub-object of a live ship with a DEFEND ability, drawn while the
    // ability runs (BP-22). The ALO hides it; qualified shield submeshes are drawn.
    if (live_ship && options_.placed_ships[*live_ship].defend_shell) {
        std::string& status = shield_shells_[decision.object_id];
        const std::optional<std::uint32_t> shield = shield_mesh_index(*model);
        if (!shield) status = "no SHIELD sub-object";
        for (std::uint32_t submesh = 0; shield && submesh < model->meshes[*shield].submeshes.size(); ++submesh) {
            const std::string& shader = model->meshes[*shield].submeshes[submesh].shader;
            if (!shield_program(shader)) {
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
    // WST-12: the STEALTH sub-object of a live ship with a STEALTH ability, drawn with its
    // qualified shield material while the cloak is on.
    if (cloak_ship) {
        std::string& status = stealth_shells_[decision.object_id];
        const std::optional<std::uint32_t> cloak = stealth_mesh_index(*model);
        if (!cloak) status = "no STEALTH sub-object";
        for (std::uint32_t submesh = 0; cloak && submesh < model->meshes[*cloak].submeshes.size(); ++submesh) {
            const std::string& shader = model->meshes[*cloak].submeshes[submesh].shader;
            if (!shield_program(shader)) {
                status = "STEALTH submesh " + std::to_string(submesh) + " shader " + shader + " not drawn";
                continue;
            }
            auto found = shell_uploads.find({placement.model_path, ((*cloak + 1U) << 16U) | submesh});
            if (found == shell_uploads.end()) {
                std::string cause;
                found = shell_uploads.emplace(std::pair{placement.model_path, ((*cloak + 1U) << 16U) | submesh},
                                              upload_shell(renderer, filesystem, cache, next_asset, *model, *cloak, submesh, cause, true)).first;
                if (!found->second || !cause.empty()) status = cause;
            }
            if (!found->second) continue;
            if (status.empty()) status = "composed";
            Piece piece{&*found->second, placement.transform->matrix, std::nullopt, std::nullopt, live_ship,
                        sim::math::identity_matrix()};
            piece.cloak = true;
            pieces.push_back(piece);
        }
    }
}

void SpacePopulation::compose_piece_animation(ComposeContext& context, ComposeBatch& batch,
    const scene::Placement& placement, const assets::Model* model, const std::optional<std::size_t>& live_ship,
    std::size_t first_piece) {
    const auto& resolve = context.resolve;
    auto& pieces = batch.pieces;
    using Piece = ComposeBatch::Piece;
    if (std::any_of(pieces.begin() + static_cast<std::ptrdiff_t>(first_piece), pieces.end(),
            [](const Piece& piece) { return piece.upload->pose.has_value(); })) {
        const std::string live_clip_path = live_ship ? options_.placed_ships[*live_ship].clip : std::string{};
        const auto clip = live_clip_path.empty() ? bind_composed_idle(context, batch, placement, *model) : std::nullopt;
        if (!live_clip_path.empty()) {
            if (const auto bound = bind_composed_clip(context, placement, *model, live_clip_path)) {
                live_clips_[*live_ship] = *bound;
                if (const std::string& alternate = options_.placed_ships[*live_ship].alternate_clip; !alternate.empty()) {
                    live_alternate_clips_[*live_ship] = bind_composed_clip(context, placement, *model, alternate);
                }
                for (std::size_t index = first_piece; index < pieces.size(); ++index) {
                    if (pieces[index].upload->pose) pieces[index].live_clip = true;
                }
            }
        } else if (clip) {
            ++animated_placements_;
            const std::size_t idle = idle_placements_.size();
            idle_placements_.push_back(composed_idle_placement(context, placement, *clip));
            if (live_ship) idle_live_ships_.emplace(idle, *live_ship);
            for (std::size_t index = first_piece; index < pieces.size(); ++index) {
                if (pieces[index].upload->pose) pieces[index].idle = idle;
            }
        }
    }

    if (live_ship) {
        const auto object = resolve(placement.object_id, data::Category::game_object);
        const auto text = [&](std::string_view tag) -> std::string_view {
            const auto* value = object ? object->value(tag) : nullptr;
            auto result = value ? std::string_view(value->value.raw_text) : std::string_view{};
            const auto trimmed = core::trim_view(result, core::xml_whitespace);
            return trimmed.empty() ? std::string_view{} : trimmed;
        };
        if (!text("Turret_Bone_Name").empty()) {
            const auto bone_index = [&](std::string_view name) -> std::optional<std::size_t> {
                for (std::size_t index = 0; index < model->bones.size(); ++index)
                    if (ieq(model->bones[index].name, name)) return index;
                return std::nullopt;
            };
            if (const auto turret = bone_index(text("Turret_Bone_Name"))) {
                TurretBinding binding;
                binding.turret = *turret;
                binding.barrel = bone_index(text("Barrel_Bone_Name"));
                for (const auto& [idle, ship] : idle_live_ships_) {
                    if (ship == *live_ship) {
                        binding.idle = idle;
                        binding.player = clips_[idle_placements_[idle].clip];
                        break;
                    }
                }
                if (!binding.player) {
                    if (auto player = animation::Player::create(*model))
                        binding.player = std::make_shared<const animation::Player>(std::move(player.value()));
                }
                if (binding.player) {
                    if (auto sampled = binding.player->sample({})) {
                        binding.pose = std::move(sampled.value());
                        if (!binding.idle) binding.bind_bones = binding.pose.bones;
                        turret_bindings_[*live_ship] = std::move(binding);
                    }
                }
            }
        }
    }
}

void SpacePopulation::compose_hardpoint_pieces(ComposeContext& context, ComposeBatch& batch,
    scene::SpacePlacementDecision& decision, std::size_t decision_index, const scene::Placement& placement,
    const assets::Model* model, const std::optional<std::size_t>& live_ship,
    const std::optional<std::array<std::uint8_t, 3>>& colour, bool cloak_ship) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& pieces = batch.pieces;
    auto& bind_poses = batch.bind_poses;
    auto& failed_placements = batch.failed_placements;
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
                const bool effect_missing = scene::missing_material_effect(cache.access(), submesh.shader);
                const scene::LegacySelector* selector = scene::material_selector(submesh.shader, effect_missing);
                if (selector == nullptr) {
                    decision.missing.push_back(who + "shader_unsupported " + submesh.shader);
                    continue;
                }
                std::string texture_path;
                if (!effect_missing) for (const assets::MaterialParameter& parameter : submesh.parameters) {
                    const auto* name = std::get_if<std::string>(&parameter.value);
                    if (!ieq(parameter.name, "BaseTexture") || name == nullptr) continue;
                    texture_path = probe(cache, "data/art/textures/", *name, texture_suffixes);
                    if (texture_path.empty()) decision.missing.push_back(who + "texture_unresolved " + *name);
                }
                const SurfaceUpload* upload
                    = upload_composed_surface(context, batch, path, *attached, mesh_index, submesh_index, *selector, texture_path);
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
                pieces.back().cloak_hidden = cloak_ship; // WST-12: the cloak hides the hardpoints
                if (!effect_missing && scene::colorizes(submesh.shader)) pieces.back().colour = colour;
            }
        }
    }
}

void SpacePopulation::compose_draw_pieces(ComposeContext& context, ComposeBatch& batch, std::size_t first_decision,
    bool appending) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& pieces = batch.pieces;
    auto& bind_poses = batch.bind_poses;
    auto& debug_piece_range = batch.debug_piece_range;
    hardpoint_states_.resize(decisions_.size());
    for (std::size_t decision_index = first_decision; decision_index < decisions_.size(); ++decision_index) {
        if (!appending && options_.progress) options_.progress(decision_index, decisions_.size());
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
        if (live_ship) {
            auto pose = bind_poses.find(placement.model_path);
            if (pose == bind_poses.end()) {
                std::optional<std::vector<animation::BonePose>> bones;
                if (auto player = animation::Player::create(*model)) {
                    if (auto sampled = player.value().sample({})) bones = std::move(sampled.value().bones);
                }
                pose = bind_poses.emplace(placement.model_path, std::move(bones)).first;
            }
            if (pose->second) contact_bones_[*live_ship] = *pose->second;
        }
        const std::size_t first_piece = pieces.size();
        const bool cloak_ship = live_ship && options_.placed_ships[*live_ship].stealth_shell;
        compose_hull_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship);
        compose_shell_pieces(context, batch, decision, placement, model, live_ship, cloak_ship);
        compose_piece_animation(context, batch, placement, model, live_ship, first_piece);
        compose_hardpoint_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship);
        // One line per distinct reason.
        std::sort(decision.missing.begin(), decision.missing.end());
        decision.missing.erase(std::unique(decision.missing.begin(), decision.missing.end()), decision.missing.end());
        if (options_.debug_ship && decision.scene_ordinal == scene_->placements.size() - 1)
            debug_piece_range = std::pair{first_piece, pieces.size()};
    }
    if (!appending && options_.progress) options_.progress(decisions_.size(), decisions_.size());
}

float SpacePopulation::commit_composed_pieces(ComposeContext& context, ComposeBatch& batch, sim::EntityId& next_entity) {
    auto& renderer = context.renderer;
    const auto& camera = context.camera;
    auto& pieces = batch.pieces;
    auto& debug_piece_range = batch.debug_piece_range;
    using Piece = ComposeBatch::Piece;
    const ViewTest view(camera);
    float debug_farthest = 0.0F;

    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        const Piece& piece = pieces[piece_index];
        const sim::EntityId entity = next_entity++;
        pieces_.push_back({{entity, piece.upload->renderer_asset, source_to_render(piece.source)}, piece.gate,
                           piece.live, piece.local, piece.shield, piece.alternate, piece.cloak, piece.cloak_hidden,
                           piece.blast_charged, piece.colour});
        if (piece.live && !piece.shield && !piece.cloak && !piece.gate
            && (!piece.alternate || *piece.alternate == 0))
            hull_surface_assets_[*piece.live].push_back(piece.upload->renderer_asset);
        if (piece.colour) {
            // GM-03: the colour the shared upload's material would have carried, stored 0..1 as the
            // colorize shaders' instance colour expects (they linearise it where their material does).
            const auto& rgb = *piece.colour;
            renderer.set_unit_colorization(entity, {static_cast<float>(rgb[0]) / 255.0F,
                static_cast<float>(rgb[1]) / 255.0F, static_cast<float>(rgb[2]) / 255.0F});
            ++team_colour_variants_;
        }
        if (piece.upload->pose) {
            if (!piece.shield && !piece.cloak) ++skinned_instances_;
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
    return debug_farthest;
}

bool SpacePopulation::finish_composed_range(ComposeContext& context, bool appending, sim::AssetId next_asset,
    sim::EntityId next_entity, std::size_t receiving_before, std::size_t variant_failures_before, float debug_farthest) {
    auto& renderer = context.renderer;
    const auto& camera = context.camera;
    rebuild_piece_index();
    attachment_marks_.configure(hardpoint_states_);
    instances_.reserve(pieces_.size());
    refresh_instances();
    if (appending) {
        next_arrival_asset_ = next_asset;
        next_arrival_piece_ = next_entity;
        schedule_arrival_sample();
    }
    shadow_receiving_ += renderer.shadow_receiving_materials() - receiving_before;
    shadow_variant_failures_ += renderer.shadow_variant_failures() - variant_failures_before;
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
    if (!appending && options_.debug_ship) {
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

bool SpacePopulation::compose_range(ComposeContext& context, const std::size_t first_ship,
    const std::size_t first_decision, const sim::AssetId first_asset_id, const sim::EntityId first_entity_id) {
    auto& renderer = context.renderer;
    const auto& camera = context.camera;
    const bool appending = composed_;
    prepare_compose_slots(appending);
    if (!classify_arrivals(context, first_ship, first_decision)) return false;
    // The light exists before any hull upload, so with shadows on every hull
    // compiles the shadow-receiving variant. The distance is refined below.
    const std::size_t receiving_before = renderer.shadow_receiving_materials();
    const std::size_t variant_failures_before = renderer.shadow_variant_failures();
    if (options_.policy != lighting::Policy::off) {
        lighting_ = space_populate_detail::lighting_state(options_, camera.far_plane);
        renderer.set_lighting(lighting_);
    }

    ComposeBatch batch;
    batch.next_asset = std::max(first_asset, first_asset_id);
    compose_draw_pieces(context, batch, first_decision, appending);
    auto& failed_placements = batch.failed_placements;
    auto& next_asset = batch.next_asset;
    if (!failed_placements.empty()) {
        if (appending) next_arrival_asset_ = next_asset;
        failure_ = std::to_string(failed_placements.size()) + " supported surface placement uploads failed";
        for (const std::string& where : failed_placements) failure_ += "; " + where;
        for (const std::string& detail : upload_failures_) failure_ += "; " + detail;
        return false;
    }

    sim::EntityId next_entity = std::max(first_entity, first_entity_id);
    const float debug_farthest = commit_composed_pieces(context, batch, next_entity);
    return finish_composed_range(context, appending, next_asset, next_entity, receiving_before,
        variant_failures_before, debug_farthest);
}
bool SpacePopulation::append_live_ships(GodotRenderer& renderer, const vfs::Vfs& filesystem,
    const std::span<const Options::PlacedShip> ships) {
    if (ships.empty()) return true;
    // The initial compose establishes the scene and stable ship/decision indices.
    if (!composed_ || !scene_ || !options_.catalog) {
        failure_ = "arrival slots require a composed population";
        return false;
    }
    failure_.clear(); // The caller retains any prior failed-type diagnostic separately.
    const auto first_ship = options_.placed_ships.size();
    const auto first_decision = decisions_.size();
    options_.placed_ships.insert(options_.placed_ships.end(), ships.begin(), ships.end());
    assets::Map map;
    map.source = compose_source_;
    map.kind = assets::MapKind::space;
    scene::VfsAssetCache cache(filesystem);
    scene::BuildInput input{&map, options_.map_sha256, options_.catalog, cache.access()};
    const scene::ObjectResolver resolve = [&](const std::string_view id, const data::Category category)
        -> std::optional<data::EffectiveObject> {
        auto object = options_.catalog->resolve(id, category);
        return object ? std::optional(std::move(object).value()) : std::nullopt;
    };
    ComposeContext context{renderer, map, filesystem, compose_camera_, {}, cache, input, resolve};
    return compose_range(context, first_ship, first_decision, next_arrival_asset_, next_arrival_piece_);
}

} // namespace eawr::presentation::godot_backend
