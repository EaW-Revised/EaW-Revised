#include "space_populate.hpp"
#include "menu_timeline.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/presentation/hull_surface_diagnostic.hpp"
#include "frame_timer.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"
#include "compose_texture_cache.hpp"
#include "eawr/scene/texture_prefetch.hpp"

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
#include <limits>
#include <map>
#include <new>
#include <memory>
#include <set>
#include <sstream>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {
namespace {
template<class Vector>
void reserve_geometric(Vector& values, const std::size_t wanted) {
    if (wanted > values.capacity()) values.reserve(std::max(wanted, values.capacity() * 2));
}

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




// One line per initial compose: what the model pins cost (and, with the release on, saved).
void report_compose_models(const scene::VfsAssetCache::ModelStats& stats, const bool release) {
    godot::UtilityFunctions::print("EAWR compose models: pin_release=", release ? "on" : "off",
        " calls=", static_cast<std::int64_t>(stats.calls), " hits=", static_cast<std::int64_t>(stats.hits),
        " misses=", static_cast<std::int64_t>(stats.misses), " unique_loads=", static_cast<std::int64_t>(stats.unique_loads),
        " cache_retained_bytes=", static_cast<std::int64_t>(stats.cache_retained_bytes),
        " cache_evictions=", static_cast<std::int64_t>(stats.cache_evictions),
        " pin_releases=", static_cast<std::int64_t>(stats.raw_pin_releases),
        " pins_released=", static_cast<std::int64_t>(stats.raw_pins_released),
        " release_refused=", static_cast<std::int64_t>(stats.raw_pin_release_refused));
}

// One line per initial compose: what the texture hold cost and what the budget cost in repeat decodes.
void report_compose_textures(const ComposeTextureCache::Stats& stats) {
    godot::UtilityFunctions::print("EAWR compose textures: budget_bytes=",
        static_cast<std::int64_t>(std::min(stats.budget_bytes,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
        " peak_held_bytes=", static_cast<std::int64_t>(stats.peak_held_bytes),
        " held_bytes=", static_cast<std::int64_t>(stats.held_bytes),
        " decodes=", static_cast<std::int64_t>(stats.decodes), " hits=", static_cast<std::int64_t>(stats.hits),
        " evictions=", static_cast<std::int64_t>(stats.evictions), " redecodes=", static_cast<std::int64_t>(stats.redecodes));
}

// One line per initial compose with a texture read-ahead: what the readers did and what the compose steps waited for.
void report_texture_prefetch(const scene::TexturePrefetchStats& stats) {
    std::ostringstream out;
    out << "readers=" << stats.readers << " readers_peak=" << stats.readers_peak << " planned=" << stats.planned
        << " decoded=" << stats.decoded << " failed=" << stats.failed << " served_ready=" << stats.served_ready
        << " served_inflight=" << stats.served_inflight << " served_unstarted=" << stats.served_unstarted
        << " unplanned=" << stats.unplanned << " unrequested=" << stats.unrequested << " skipped=" << stats.skipped
        << " window_bytes=" << stats.window_bytes << " held_bytes_peak=" << stats.held_bytes_peak
        << " reader_decode_ms=" << stats.reader_decode_ms << " wait_ms=" << stats.wait_ms
        << " chained=" << stats.chained << " chain_added_bytes=" << stats.chain_added_bytes << " chain_ms=" << stats.chain_ms
        << " chained_by_loader=" << stats.chained_by_loader;
    godot::UtilityFunctions::print(godot::String("EAWR compose texture prefetch: ") + godot::String::utf8(out.str().c_str()));
    MenuTimeline::note("menu_texture_prefetch", out.str());
}
} // namespace

struct SpacePopulation::ComposeBatch final {
    // Decoded textures held between the surfaces that name them; bounded for the initial load only.
    ComposeTextureCache textures;
    // The initial load's texture read-ahead; declared after `textures` so it is joined first when the batch goes.
    std::unique_ptr<scene::TexturePrefetch> prefetch;
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
        std::string diagnostic_model{};
        render_categories::HullPart diagnostic_part{render_categories::HullPart::unclassified};
    };
    std::vector<Piece> pieces;
    // Keyed by model path, mesh and submesh: a model shared by placements or
    // hardpoints is uploaded once.
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t, TeamColour>, std::optional<SurfaceUpload>> uploads;
    std::map<std::string, std::optional<std::vector<animation::BonePose>>> bind_poses;
    std::vector<std::string> failed_placements;
    std::optional<std::pair<std::size_t, std::size_t>> debug_piece_range;
};

void SpacePopulation::prepare_compose_slots(bool appending) {
    if (!appending) {
        bind_players_.clear();
        live_decisions_.clear(); hull_surface_assets_.clear(); live_models_.clear(); live_shown_.clear();
        live_defend_.clear(); live_stealth_.clear(); live_blast_charging_.clear(); live_blast_charged_.clear();
        live_clips_.clear(); land_clips_.clear(); land_idle_.clear(); land_clip_bindings_.clear(); turret_bindings_.clear(); live_alternate_clips_.clear(); live_retired_.clear();
        hardpoint_turret_bindings_.clear();
        parent_hardpoint_turret_bindings_.clear();
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
    land_clips_.resize(options_.placed_ships.size());
    land_idle_.resize(options_.placed_ships.size());
    contact_bones_.resize(options_.placed_ships.size());
    turret_bindings_.resize(options_.placed_ships.size(), std::nullopt);
    hardpoint_turret_bindings_.resize(options_.placed_ships.size());
    parent_hardpoint_turret_bindings_.resize(options_.placed_ships.size());
    live_alternate_clips_.resize(options_.placed_ships.size(), std::nullopt);
    live_retired_.resize(options_.placed_ships.size(), false);
    live_death_clone_.resize(options_.placed_ships.size(), false);
    live_alternates_.resize(options_.placed_ships.size(), 0);
    live_alternate_counts_.resize(options_.placed_ships.size(), 0);
    live_bounds_.resize(options_.placed_ships.size(), std::nullopt);
    live_transforms_.resize(options_.placed_ships.size(), std::nullopt);
    backdrop_ships_.resize(options_.placed_ships.size(), false);
}

bool SpacePopulation::classify_arrival(ComposeContext& context, const std::size_t ship_index) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    const data::Catalog& catalog = *options_.catalog;
    const auto& ship = options_.placed_ships[ship_index];
    live_death_clone_[ship_index] = ship.death_clone;
    const bool live = ship.live_entity != 0;
    const std::string live_name = "live unit " + std::to_string(ship.live_entity) + " (" + ship.object_id + ")";
    const data::Definition* definition = catalog.find(ship.object_id, data::Category::game_object);
    auto effective = catalog.resolve(ship.object_id, data::Category::game_object);
    if (live && (definition == nullptr || !effective)) {
        (ship.projectile_slot ? projectile_slots_undrawn_ : ship.launch_slot ? launch_slots_undrawn_ : live_undrawn_).push_back(live_name + ": not in the catalog");
        return true;
    }
    if (!live && (definition == nullptr || !effective || effective.value().type_name != "SpaceUnit")) {
        failure_ = "--eawr-space-place-at: " + ship.object_id + " is not a FoC SpaceUnit";
        return false;
    }
    assets::Map placed_map;
    placed_map.source = map.source;
    placed_map.kind = domain() == sim::tactical::BattleDomain::land ? assets::MapKind::land : assets::MapKind::space;
    assets::Placement source;
    // A live unit takes a record past the map's, so no map record rule
    // (environment, session) applies to it.
    source.key = {map.source, live ? static_cast<std::uint32_t>(map_placements_ + ship_index) : 0U};
    source.type_crc = 0U; // A synthetic placement; the XML identity is explicit.
    source.type_resolution = assets::TypeResolution::unique;
    source.type_candidates.push_back({.logical_name = definition->id, .source = definition->root.source, .model_name = {}, .land_model_name = {}, .space_model_name = {}});
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
            return true;
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
            return true;
        }
        failure_ = "--eawr-space-place-at: " + ship.object_id + " has no supported visible hull";
        return false;
    }
    if (live) live_decisions_[ship_index] = decisions_.size();
    decisions_.push_back(std::move(decision));
    return true;
}

bool SpacePopulation::finish_classify(ComposeContext& context, const std::size_t first_decision) {
    const auto& environment_records = context.environment_records;
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


bool SpacePopulation::classify_arrivals(ComposeContext& context, std::size_t first_ship, std::size_t first_decision) {
    for (std::size_t ship_index = first_ship; ship_index < options_.placed_ships.size(); ++ship_index) {
        if (!classify_arrival(context, ship_index)) return false;
    }
    return finish_classify(context, first_decision);
}

std::optional<std::size_t> SpacePopulation::bind_composed_idle(ComposeContext& context, ComposeBatch&,
    const scene::Placement& placement, const assets::Model& model) {
    const auto& filesystem = context.filesystem;
    auto& clip_bindings = idle_clip_bindings_;
    using ClipBinding = IdleClipBinding;
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
        // Failures are not cached: a later arrival retries the create.
        if (!binding.clip) {
            ++animation_status_[binding.status];
            static_hulls_[placement.object_id] = binding.status;
            return std::nullopt;
        }
        found = clip_bindings.emplace(key, std::move(binding)).first;
    }
    ++animation_status_[found->second.status];
    bound_idle_hulls_[placement.object_id] = placement.idle_animation;
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
    ComposeBatch& batch, const std::string& path, std::uint32_t mesh_index,
    std::uint32_t submesh_index, const scene::LegacySelector& selector, const std::string& texture_path) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& uploads = batch.uploads;
    auto& textures = batch.textures;
    auto& next_asset = batch.next_asset;
    // GM-03: one upload per surface for every team colour; the colour is per instance.
    const auto key = std::make_tuple(path, mesh_index, submesh_index, TeamColour{});
    auto found = uploads.find(key);
    if (found == uploads.end()) {
        found = uploads.emplace(key, upload_surface(renderer, filesystem, cache, textures, next_asset, path, mesh_index, submesh_index, selector, texture_path, TeamColour{},
            path + " mesh " + std::to_string(mesh_index) + " submesh " + std::to_string(submesh_index))).first;
    }
    return found->second ? &*found->second : nullptr;
}

void SpacePopulation::start_texture_prefetch(ComposeContext& context, ComposeBatch& batch) {
    if (!scene_) return;
    scene::TexturePrefetchOptions prefetch_options;
    prefetch_options.fixed_readers = options_.texture_prefetch_readers;
    prefetch_options.solid_state = options_.texture_prefetch_solid_state;
    batch.prefetch = std::make_unique<scene::TexturePrefetch>(context.filesystem, prefetch_options);
    batch.textures.set_prefetch(batch.prefetch.get());
    // The order the compose steps ask in: decisions in order, then each decision's drawn surfaces in order (the
    // prewarm unit of every surface decodes its base texture first). The same texture rule as compose_hull_pieces.
    for (std::size_t decision_index = 0; decision_index < decisions_.size(); ++decision_index) {
        const scene::SpacePlacementDecision& decision = decisions_[decision_index];
        if (decision.role != scene::SpaceRole::drawn) continue;
        const scene::Placement& placement = scene_->placements[decision.scene_ordinal];
        bool cloak_ship = false;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (live_decisions_[ship] == decision_index) cloak_ship = options_.placed_ships[ship].stealth_shell;
        }
        for (const std::size_t index : decision.drawn_surfaces) {
            const scene::Surface& surface = placement.surfaces[index];
            if (surface.effect_missing) continue;
            if (cloak_ship && ieq(surface.mesh_name, stealth_mesh_name)) continue;
            std::string texture_path;
            for (const scene::TextureBinding& binding : surface.textures) {
                if (ieq(binding.parameter, "BaseTexture")) texture_path = binding.resolved;
            }
            const scene::LegacySelector* const selector = scene::material_selector(surface);
            const scene::TextureProcessing processing =
                selector != nullptr ? texture_processing_for(*selector) : scene::TextureProcessing::none;
            batch.prefetch->offer(texture_path, decision_index, processing);
            // The same material's normal maps (the family's flat-normal bindings), asked for by upload_surface as family textures.
            if (selector != nullptr && processing != scene::TextureProcessing::none) {
                for (const std::string_view slot : normal_binding_names(*selector)) {
                    for (const scene::TextureBinding& binding : surface.textures)
                        if (ieq(binding.parameter, slot) && !binding.resolved.empty())
                            batch.prefetch->offer(binding.resolved, decision_index, scene::TextureProcessing::normal_mip_chain);
                }
            }
        }
    }
}

void SpacePopulation::prewarm_composed_texture(ComposeContext& context, ComposeBatch& batch, const std::string& path,
    std::uint32_t mesh_index, std::uint32_t submesh_index, const scene::LegacySelector& selector,
    const std::string& texture_path) {
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    // The same conditions under which upload_surface decodes the base texture, so the cache sees the same requests.
    if (texture_path.empty() || selector.program == scene::missing_effect_selector().program) return;
    if (batch.uploads.find(std::make_tuple(path, mesh_index, submesh_index, TeamColour{})) != batch.uploads.end()) return;
    // The decision's first step pinned this model with cache.model() (Cursor::model, raw_models_), and release_raw_pins()
    // only runs at the next decision's first step, so even under a finite load budget the entry is alive here and this
    // lookup is a hit; the discarded shared_ptr costs no extra decode or memory.
    if (!cache.shared_model(path)) return;
    core::load_profile::Scope scope(core::load_profile::Phase::surface_texture_prepare);
    if (MenuTimeline::enabled()) MenuTimeline::detail_add("prewarm texture " + texture_path);
    const scene::TextureProcessing processing = texture_processing_for(selector);
    static_cast<void>(batch.textures.get(ComposeTextureCache::key(texture_path, processing), [&]() -> std::optional<assets::Texture> {
        auto decoded = batch.textures.decode(filesystem, texture_path, processing);
        return decoded ? std::move(decoded.value()) : placeholder_texture();
    }));
}

// Retail starts a created object's Idle_Anim_00 at Idle_Anim_00_Rate_Mod
// from a random frame, looping when Loop_Idle_Anim_00 is set (#145). The
// random frame is taken from the placement's identity, so captures repeat.
IdlePlacement SpacePopulation::composed_idle_placement(ComposeContext& context, const scene::Placement& placement,
    std::size_t clip) {
    const auto& resolve = context.resolve;
    IdlePlacement idle{.clip = clip};
    if (auto object = resolve(placement.object_id, data::Category::game_object)) {
        const DeclaredIdle declared = declared_idle(domain() == sim::tactical::BattleDomain::land
            ? scene::idle_tags(*object, assets::MapKind::land) : scene::space_object_tags(*object).idle);
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
    const std::optional<std::array<std::uint8_t, 3>>& colour, bool cloak_ship, const EnqueueLoadStep* split,
    std::shared_ptr<void> keepalive) {
    // One unit = one surface upload, in the order the inline loop ran them. With `split` the units become load steps
    // (inserted before the caller's continuation), so the upload order, asset ids and results are unchanged and a
    // frame can run between surfaces; without it they run here at once.
    struct Plan final {
        std::vector<std::size_t> diagnostic_turret_roots;
        bool diagnostic_roots_complete{true};
        scene::HardpointOwnerArt owner_art;
        std::vector<std::function<void()>> units;
    };
    auto plan = std::make_shared<Plan>();
    auto& [renderer, map, filesystem, camera, environment_records, cache, input, resolve] = context;
    auto& pieces = batch.pieces;
    auto& failed_placements = batch.failed_placements;
    auto& diagnostic_turret_roots = plan->diagnostic_turret_roots;
    bool& diagnostic_roots_complete = plan->diagnostic_roots_complete;
    if (render_categories::ablation != "off") {
        const auto add_roots = [&](const auto& definition) {
            if (!definition) { diagnostic_roots_complete = false; return; }
            for (const auto tag : {"Turret_Bone_Name", "Barrel_Bone_Name"}) {
                const auto* value = definition->value(tag);
                const auto name = value ? core::trim_view(value->value.raw_text, core::xml_whitespace) : std::string_view{};
                if (name.empty()) continue;
                const auto found = std::find_if(model->bones.begin(), model->bones.end(),
                    [&](const auto& bone) { return ieq(bone.name, name); });
                if (found == model->bones.end()) diagnostic_roots_complete = false;
                else diagnostic_turret_roots.push_back(static_cast<std::size_t>(found - model->bones.begin()));
            }
        };
        add_roots(resolve(placement.object_id, data::Category::game_object));
        for (const auto& hardpoint : decision.hardpoints)
            if (hardpoint.model.empty()) add_roots(resolve(hardpoint.hardpoint, data::Category::hardpoint));
    }
    // `live_ship` and `colour` are held by reference: in split mode they refer to the decision's Cursor, which the
    // shell step (the decision's last step) still holds, so it outlives every unit. Inline they are the caller's.
    // `plan` is deliberately not captured here or by the unit lambdas: plan->units holds them, so a capture would be a
    // self-cycle. The step closures keep `plan` alive in split mode; the local shared_ptr does in inline mode.
    const auto add_surface = [&, cloak_ship, model](const std::size_t index, const std::optional<HardpointGate> gate) {
        const scene::Surface& surface = placement.surfaces[index];
        // WST-12: the STEALTH sub-object is drawn only as the cloak below.
        if (cloak_ship && ieq(surface.mesh_name, stealth_mesh_name)) return;
        std::string texture_path;
        for (const scene::TextureBinding& binding : surface.textures) {
            if (ieq(binding.parameter, "BaseTexture")) texture_path = binding.resolved;
        }
        const SurfaceUpload* upload = upload_composed_surface(context, batch, placement.model_path, surface.mesh_index,
            surface.submesh_index, *scene::material_selector(surface), texture_path);
        if (upload != nullptr) {
            pieces.push_back({upload, placement.transform->matrix, std::nullopt, gate, live_ship,
                              sim::math::identity_matrix()});
            if (render_categories::ablation != "off") {
                const auto& mesh = model->meshes.at(surface.mesh_index);
                pieces.back().diagnostic_part = render_categories::hull_part(mesh, mesh.submeshes.at(surface.submesh_index),
                    model->bones, diagnostic_turret_roots, diagnostic_roots_complete);
            }
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
    // In split mode each surface's base texture is decoded by its own unit first (the upload unit then hits the cache),
    // so a slow decode and the renderer upload no longer share one load step.
    const auto prewarm = [&, cloak_ship](const std::size_t index) {
        const scene::Surface& surface = placement.surfaces[index];
        if (cloak_ship && ieq(surface.mesh_name, stealth_mesh_name)) return;
        std::string texture_path;
        for (const scene::TextureBinding& binding : surface.textures) {
            if (ieq(binding.parameter, "BaseTexture")) texture_path = binding.resolved;
        }
        prewarm_composed_texture(context, batch, placement.model_path, surface.mesh_index, surface.submesh_index,
            *scene::material_selector(surface), texture_path);
    };
    const auto prewarm_unit = [&](const std::size_t index) {
        if (split != nullptr) plan->units.emplace_back([=] { prewarm(index); });
    };
    plan->owner_art = scene::hardpoint_owner_art(*model, decision.hardpoints);
    const scene::HardpointOwnerArt& owner_art = plan->owner_art;
    for (const std::size_t index : decision.drawn_surfaces) {
        const std::size_t mesh = placement.surfaces[index].mesh_index;
        const std::size_t hardpoint = mesh < owner_art.engine_mesh_hardpoint.size()
            ? owner_art.engine_mesh_hardpoint[mesh] : scene::HardpointOwnerArt::none;
        prewarm_unit(index);
        if (hardpoint == scene::HardpointOwnerArt::none) plan->units.emplace_back([=] { add_surface(index, std::nullopt); });
        else plan->units.emplace_back([=] {
            add_surface(index, HardpointGate{decision_index, hardpoint, HardpointGate::Art::engine_particle});
        });
    }
    // BP-74/WHE-35/36: hidden charge meshes are admitted once, then gated by the
    // published BLAST phase, including FD-55's delayed charged cleanup.
    if (live_ship) for (std::uint32_t mesh_index = 0; mesh_index < model->meshes.size(); ++mesh_index) {
        const auto& mesh = model->meshes[mesh_index];
        if (!ieq(mesh.name, "buildup") && !ieq(mesh.name, "charged")) continue;
        if (std::any_of(decision.drawn_surfaces.begin(), decision.drawn_surfaces.end(), [&](const auto index) {
            return placement.surfaces[index].mesh_index == mesh_index;
        })) continue;
        for (std::uint32_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index)
        plan->units.emplace_back([&, cloak_ship, model, mesh_index, submesh_index] {
            const auto& charge_mesh = model->meshes[mesh_index];
            const auto& submesh = charge_mesh.submeshes[submesh_index];
            const bool effect_missing = scene::missing_material_effect(cache.access(), submesh.shader);
            const auto* selector = scene::material_selector(submesh.shader, effect_missing,
                !effect_missing && !scene::find_legacy_selector(submesh.shader) && context.renderer.usable_shader(submesh));
            if (selector == nullptr) {
                decision.missing.push_back("charge mesh " + charge_mesh.name + " unsupported shader " + submesh.shader);
                return;
            }
            std::string texture_path;
            if (!effect_missing) for (const auto& parameter : submesh.parameters) {
                if (!ieq(parameter.name, "BaseTexture")) continue;
                if (const auto* name = std::get_if<std::string>(&parameter.value))
                    texture_path = probe(cache, "data/art/textures/", *name, texture_suffixes);
            }
            const auto* upload = upload_composed_surface(context, batch, placement.model_path, mesh_index, submesh_index,
                *selector, texture_path);
            if (upload == nullptr) return;
            pieces.push_back({upload, placement.transform->matrix, std::nullopt, std::nullopt, live_ship,
                              sim::math::identity_matrix()});
            pieces.back().blast_charged = ieq(charge_mesh.name, "charged");
            pieces.back().cloak_hidden = cloak_ship;
        });
    }
    // Each Damage_Decal surface is uploaded as well and drawn once its
    // hardpoint is destroyed.
    for (const auto& decal : decision.damage_decals) {
        prewarm_unit(decal.surface);
        plan->units.emplace_back([=] {
            add_surface(decal.surface, HardpointGate{decision_index, decal.hardpoint, HardpointGate::Art::damage_decal});
        });
    }
    if (split == nullptr) {
        for (auto& unit : plan->units) unit();
        return;
    }
    for (std::size_t unit = 0; unit < plan->units.size(); ++unit)
        (*split)([plan, keepalive, unit] { plan->units[unit](); return true; });
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
                                              upload_shell(renderer, filesystem, cache, next_asset, placement.model_path, *shield, submesh, cause)).first;
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
                                              upload_shell(renderer, filesystem, cache, next_asset, placement.model_path, *cloak, submesh, cause, true)).first;
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
        if (live_ship && options_.placed_ships[*live_ship].land_motion) {
            auto& land = land_clips_[*live_ship];
            land[animation::clip_types::idle] = clip;
            if (clip) land_idle_[*live_ship] = composed_idle_placement(context, placement, *clip);
            const auto object = resolve(placement.object_id, data::Category::game_object);
            const auto* override_tag = object ? object->value("Land_Model_Anim_Override_Name") : nullptr;
            const std::string override_name = override_tag
                ? std::string(core::trim_view(override_tag->value.raw_text, core::xml_whitespace)) : std::string{};
            const auto exists = [&](const std::string& path) { return static_cast<bool>(context.filesystem.stat(path)); };
            constexpr std::array<std::size_t, 18> types{2, 3, 4, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 66, 67};
            for (const auto type : types) {
                const auto paths = animation::model_clip_paths(placement.model_path, type, exists, override_name);
                if (paths.empty()) continue;
                const auto key = std::make_pair(placement.model_path, paths.front());
                auto found = land_clip_bindings_.find(key);
                if (found == land_clip_bindings_.end()) {
                    const auto bound = bind_composed_clip(context, placement, *model, paths.front());
                    if (!bound) continue;
                    found = land_clip_bindings_.emplace(key, *bound).first;
                }
                // All clips admitted for one placement must share a skeleton.
                const auto existing = std::find_if(land.begin(), land.end(), [](const auto& item) { return item.has_value(); });
                if (existing != land.end() && !clips_[**existing]->blend_compatible(*clips_[found->second])) continue;
                land[type] = found->second;
            }
            for (std::size_t index = first_piece; index < pieces.size(); ++index)
                if (pieces[index].upload->pose) pieces[index].live_clip = true;
        } else if (!live_clip_path.empty()) {
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
                if (!binding.player) binding.player = bind_players_.bind(placement.model_path, *model);
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
        if (hardpoint.model.empty()) {
            // HPT-07: inline hardpoints share one parent palette, with independently bound bones.
            if (live_ship) {
                const auto definition = resolve(hardpoint.hardpoint, data::Category::hardpoint);
                const auto text = [&](std::string_view tag) {
                    const auto* value = definition ? definition->value(tag) : nullptr;
                    return value ? core::trim_view(value->value.raw_text, core::xml_whitespace) : std::string_view{};
                };
                const auto bone_index = [&](std::string_view name) -> std::optional<std::size_t> {
                    if (name.empty()) return std::nullopt;
                    for (std::size_t index = 0; index < model->bones.size(); ++index)
                        if (ieq(model->bones[index].name, name)) return index;
                    return std::nullopt;
                };
                if (const auto turret = bone_index(text("Turret_Bone_Name"))) {
                    auto& parent = parent_hardpoint_turret_bindings_[*live_ship];
                    if (!parent) {
                        if (auto shared = bind_players_.bind(placement.model_path, *model)) {
                            TurretBinding binding;
                            binding.player = std::move(shared);
                            for (const auto& [idle, ship] : idle_live_ships_) {
                                if (ship != *live_ship) continue;
                                binding.idle = idle;
                                binding.player = clips_[idle_placements_[idle].clip];
                                break;
                            }
                            if (auto sampled = binding.player->sample({})) {
                                binding.pose = std::move(sampled.value());
                                binding.bind_bones = binding.pose.bones;
                                parent = std::move(binding);
                            }
                        }
                    }
                    if (parent) {
                        TurretBinding binding;
                        binding.parent_model = true;
                        binding.turret = *turret;
                        binding.barrel = bone_index(text("Barrel_Bone_Name"));
                        binding.muzzle = bone_index(text("Fire_Bone_A"));
                        hardpoint_turret_bindings_[*live_ship].emplace(static_cast<std::uint32_t>(hardpoint_index),
                            std::move(binding));
                    }
                }
            }
            continue;
        }
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
        const std::size_t attachment_index = static_cast<std::size_t>(bone - model->bones.begin());
        // The attached model's root sits at the bone's bind frame. Every
        // FoC Model_To_Attach is authored about its own root, also those
        // whose skeleton repeats owner bone names; an identity frame
        // stacked them at the hull origin (#136; docs/asset-formats.md,
        // "Hardpoint state art").
        std::optional<sim::math::Mat3x4> frame;
        if (pose->second && attachment_index < pose->second->size()) {
            frame = particles::fixed_model_frame((*pose->second)[attachment_index].model_asset, scene::fixed_from_binary32);
        }
        const auto placed = frame ? sim::math::compose(placement.transform->matrix, *frame)
                                  : core::Result<sim::math::Mat3x4>::failure({});
        if (!placed) {
            decision.missing.push_back(who + "attachment bone " + hardpoint.bone + " has no bind frame");
            continue;
        }
        // WAD-40: the manual cannon is an authored hardpoint, not an object turret.
        // Bind its own model skeleton once, reusing the ordinary procedural pose path.
        if (live_ship) {
            const auto definition = resolve(hardpoint.hardpoint, data::Category::hardpoint);
            const auto text = [&](std::string_view tag) {
                const auto* value = definition ? definition->value(tag) : nullptr;
                return value ? core::trim_view(value->value.raw_text, core::xml_whitespace) : std::string_view{};
            };
            const auto bone_index = [&](std::string_view name) -> std::optional<std::size_t> {
                if (name.empty()) return std::nullopt;
                for (std::size_t index = 0; index < attached->bones.size(); ++index)
                    if (ieq(attached->bones[index].name, name)) return index;
                return std::nullopt;
            };
            if (const auto turret = bone_index(text("Turret_Bone_Name"))) {
                if (auto shared = bind_players_.bind(path, *attached)) {
                    TurretBinding binding;
                    binding.turret = *turret;
                    binding.attachment_frame = *frame;
                    binding.barrel = bone_index(text("Barrel_Bone_Name"));
                    binding.muzzle = bone_index(text("Fire_Bone_A"));
                    binding.player = std::move(shared);
                    if (auto sampled = binding.player->sample({})) {
                        binding.pose = std::move(sampled.value());
                        binding.bind_bones = binding.pose.bones;
                        hardpoint_turret_bindings_[*live_ship].emplace(static_cast<std::uint32_t>(hardpoint_index),
                            std::move(binding));
                    }
                }
            }
        }
        for (std::uint32_t mesh_index = 0; mesh_index < attached->meshes.size(); ++mesh_index) {
            const assets::Mesh& mesh = attached->meshes[mesh_index];
            if (!mesh.visible) continue;
            for (std::uint32_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
                const assets::Submesh& submesh = mesh.submeshes[submesh_index];
                if (scene::is_shadow_volume_shader(submesh.shader)) continue;
                const bool effect_missing = scene::missing_material_effect(cache.access(), submesh.shader);
                const scene::LegacySelector* selector = scene::material_selector(submesh.shader, effect_missing,
                    !effect_missing && !scene::find_legacy_selector(submesh.shader) && context.renderer.usable_shader(submesh));
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
                    = upload_composed_surface(context, batch, path, mesh_index, submesh_index, *selector, texture_path);
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
    // Initial load only: the pointers cache.model() returns below are used inside one decision, so the pins of the
    // scene build and classification, and then of each finished decision, are dropped before the next one.
    const bool release_pins = !appending && options_.release_compose_pins;
    for (std::size_t decision_index = first_decision; decision_index < decisions_.size(); ++decision_index) {
        if (!appending && options_.progress) options_.progress(decision_index, decisions_.size());
        // As the attract load does at each decision: hints for the decisions before this one that nobody asked for (stealth
        // meshes, skipped surfaces) are dead; left held they would fill the read-ahead window and stall its readers.
        if (batch.prefetch) batch.prefetch->advance(decision_index);
        if (release_pins) static_cast<void>(cache.release_raw_pins());
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
            core::load_profile::Scope bind_scope(core::load_profile::Phase::compose_bind_pose);
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
        {
            core::load_profile::Scope scope(core::load_profile::Phase::compose_hull);
            const core::load_profile::DetailModelScope compose_timer(placement.model_path,
                core::load_profile::ModelField::compose);
            core::load_profile::detail_composed(placement.model_path);
            compose_hull_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship);
        }
        {
            core::load_profile::Scope scope(core::load_profile::Phase::compose_shell);
            compose_shell_pieces(context, batch, decision, placement, model, live_ship, cloak_ship);
        }
        {
            core::load_profile::Scope scope(core::load_profile::Phase::compose_animation);
            compose_piece_animation(context, batch, placement, model, live_ship, first_piece);
        }
        {
            core::load_profile::Scope scope(core::load_profile::Phase::compose_hardpoints);
            compose_hardpoint_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship);
        }
        if (render_categories::ablation != "off")
            for (std::size_t p = first_piece; p < pieces.size(); ++p) pieces[p].diagnostic_model = placement.model_path;
        // One line per distinct reason.
        std::sort(decision.missing.begin(), decision.missing.end());
        decision.missing.erase(std::unique(decision.missing.begin(), decision.missing.end()), decision.missing.end());
        if (options_.debug_ship && decision.scene_ordinal == scene_->placements.size() - 1)
            debug_piece_range = std::pair{first_piece, pieces.size()};
    }
    if (release_pins) static_cast<void>(cache.release_raw_pins());
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
        const auto* owner = piece.live ? &options_.placed_ships[*piece.live] : nullptr;
        if (render_categories::ablation != "off" && piece.live && owner && !owner->death_clone
            && !owner->breakoff && !owner->projectile_slot)
        {
            if (render_categories::ablation == "hull-census" || render_categories::fighter_experiment()) {
                const auto effective = options_.catalog->resolve(owner->object_id, data::Category::game_object);
                if (render_categories::fighter_experiment()) {
                    diagnostic_fighters_.resize(options_.placed_ships.size());
                    diagnostic_fighters_[*piece.live] = effective && render_categories::authored_fighter(effective.value().single_text("CategoryMask"));
                }
                renderer.set_diagnostic_owner(entity, *piece.live + 1, piece.diagnostic_model, piece.diagnostic_part,
                    owner->object_id, effective ? effective.value().single_text("Space_Layer") : "",
                    effective ? effective.value().single_text("CategoryMask") : "",
                    effective ? effective.value().type_name : "",
                    piece.shield ? "shield" : piece.cloak ? "cloak" : piece.blast_charged.has_value() ? "charged_weapon"
                        : piece.gate ? (piece.gate->art == HardpointGate::Art::damage_decal ? "damage_decal"
                            : piece.gate->art == HardpointGate::Art::engine_particle ? "engine_particle_mesh" : "attached_hardpoint")
                        : "hull");
            } else renderer.set_diagnostic_owner(entity, *piece.live + 1, piece.diagnostic_model, piece.diagnostic_part);
        }
        renderer.set_render_category(entity, owner && owner->death_clone ? render_categories::Role::wreck
            : owner && owner->breakoff ? render_categories::Role::debris
            : piece.gate ? render_categories::Role::hardpoint
            : (piece.shield || piece.cloak || piece.blast_charged.has_value()) ? render_categories::Role::effect
            : owner && owner->projectile_slot ? render_categories::Role::unknown
            : render_categories::Role::hull);
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

bool SpacePopulation::enqueue_compose_range(std::shared_ptr<ComposeOwner> owner,
    const sim::AssetId first_asset_id, const sim::EntityId first_entity_id, const EnqueueLoadStep& enqueue) {
    struct Range final {
        std::shared_ptr<ComposeOwner> owner;
        ComposeBatch batch;
        std::size_t receiving_before{};
        std::size_t variant_failures_before{};
    };
    struct Cursor final {
        TeamColour colour{};
        std::optional<std::size_t> live_ship{};
        const assets::Model* model{};
        std::size_t first_piece{};
        bool cloak_ship{};
        bool drawn{};
    };
    auto range = std::make_shared<Range>();
    range->owner = std::move(owner);
    prepare_compose_slots(false);
    // The classification is one step per ship and a closing pass; the rest of the set-up and the per-decision steps
    // follow in a step of their own, which queues them behind the classification. Same order and results as inline.
    for (std::size_t ship_index = 0; ship_index < options_.placed_ships.size(); ++ship_index) {
        enqueue([this, range, ship_index] {
            core::load_profile::Scope scope(core::load_profile::Phase::compose_classify);
            return classify_arrival(range->owner->context, ship_index);
        });
    }
    enqueue([this, range] {
        core::load_profile::Scope scope(core::load_profile::Phase::compose_classify);
        return finish_classify(range->owner->context, 0);
    });
    enqueue([this, range, first_asset_id, first_entity_id, enqueue] {
        auto& initial_context = range->owner->context;
        auto& initial_renderer = initial_context.renderer;
        range->receiving_before = initial_renderer.shadow_receiving_materials();
        range->variant_failures_before = initial_renderer.shadow_variant_failures();
        if (options_.policy != lighting::Policy::off) {
            lighting_ = space_populate_detail::lighting_state(options_, initial_context.camera.far_plane);
            initial_renderer.set_lighting(lighting_);
        }
        range->batch.next_asset = std::max(first_asset, first_asset_id);
        range->batch.textures.set_budget(options_.compose_texture_budget_bytes);
        if (options_.texture_prefetch) start_texture_prefetch(range->owner->context, range->batch);
        hardpoint_states_.resize(decisions_.size());
        MenuTimeline::mark("attract_load_population_classified");
        for (std::size_t decision_index = 0; decision_index < decisions_.size(); ++decision_index) {
            auto cursor = std::make_shared<Cursor>();
            enqueue([this, range, cursor, decision_index, enqueue] {
                auto& context = range->owner->context;
                auto& batch = range->batch;
                auto& cache = context.cache;
                auto& bind_poses = batch.bind_poses;
                auto& pieces = batch.pieces;
                if (options_.progress) options_.progress(decision_index, decisions_.size());
                // Hints for the decisions before this one that nobody asked for (stealth meshes, skipped surfaces) are dead.
                if (batch.prefetch) batch.prefetch->advance(decision_index);
                // The previous decision's steps (hull units, shell) are done: drop its pins (and, first time, the scene build's).
                if (options_.release_compose_pins) static_cast<void>(cache.release_raw_pins());
                scene::SpacePlacementDecision& decision = decisions_[decision_index];
                if (decision.role != scene::SpaceRole::drawn) return true;
                hardpoint_states_[decision_index].assign(decision.hardpoints.size(), scene::HardpointState::intact);
                const scene::Placement& placement = scene_->placements[decision.scene_ordinal];
                if (MenuTimeline::enabled()) MenuTimeline::detail("compose hull " + placement.model_path);
                cursor->colour = options_.team_colour ? options_.team_colour(placement) : TeamColour{};
                auto& live_ship = cursor->live_ship;
                for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
                    if (live_decisions_[ship] == decision_index) live_ship = ship;
                }
                ++team_colour_status_[placement.team_colour_status];
                cursor->model = cache.model(placement.model_path);
                const auto* model = cursor->model;
                if (model == nullptr) return true;  // drawn implies loaded; kept defensive
                if (live_ship) {
                    core::load_profile::Scope bind_scope(core::load_profile::Phase::compose_bind_pose);
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
                cursor->first_piece = pieces.size();
                cursor->cloak_ship = live_ship && options_.placed_ships[*live_ship].stealth_shell;
                cursor->drawn = true;
                const auto& colour = cursor->colour;
                const bool cloak_ship = cursor->cloak_ship;
                {
                    core::load_profile::Scope scope(core::load_profile::Phase::compose_hull);
                    const core::load_profile::DetailModelScope compose_timer(placement.model_path,
                        core::load_profile::ModelField::compose);
                    core::load_profile::detail_composed(placement.model_path);
                    compose_hull_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship,
                        &enqueue, range);
                }
                // Runs after the per-surface steps the call above inserted.
                enqueue([] { MenuTimeline::mark("attract_load_hull"); return true; });
                return true;
            });
            enqueue([this, range, cursor, decision_index] {
                if (!cursor->drawn) return true;
                auto& context = range->owner->context;
                auto& batch = range->batch;
                auto& decision = decisions_[decision_index];
                const auto& placement = scene_->placements[decision.scene_ordinal];
                const auto* model = cursor->model;
                const auto live_ship = cursor->live_ship;
                const bool cloak_ship = cursor->cloak_ship;
                {
                    core::load_profile::Scope scope(core::load_profile::Phase::compose_shell);
                    compose_shell_pieces(context, batch, decision, placement, model, live_ship, cloak_ship);
                }
                MenuTimeline::mark("attract_load_shell");
                return true;
            });
            enqueue([this, range, cursor, decision_index] {
                if (!cursor->drawn) return true;
                auto& context = range->owner->context;
                auto& batch = range->batch;
                auto& decision = decisions_[decision_index];
                const auto& placement = scene_->placements[decision.scene_ordinal];
                const auto* model = cursor->model;
                const auto live_ship = cursor->live_ship;
                const auto first_piece = cursor->first_piece;
                {
                    core::load_profile::Scope scope(core::load_profile::Phase::compose_animation);
                    compose_piece_animation(context, batch, placement, model, live_ship, first_piece);
                }
                MenuTimeline::mark("attract_load_animation");
                return true;
            });
            enqueue([this, range, cursor, decision_index] {
                if (!cursor->drawn) return true;
                auto& context = range->owner->context;
                auto& batch = range->batch;
                auto& decision = decisions_[decision_index];
                const auto& placement = scene_->placements[decision.scene_ordinal];
                const auto* model = cursor->model;
                const auto live_ship = cursor->live_ship;
                const bool cloak_ship = cursor->cloak_ship;
                const auto first_piece = cursor->first_piece;
                const auto& colour = cursor->colour;
                auto& pieces = batch.pieces;
                auto& debug_piece_range = batch.debug_piece_range;
                {
                    core::load_profile::Scope scope(core::load_profile::Phase::compose_hardpoints);
                    compose_hardpoint_pieces(context, batch, decision, decision_index, placement, model, live_ship, colour, cloak_ship);
                }
                // One line per distinct reason.
                std::sort(decision.missing.begin(), decision.missing.end());
                decision.missing.erase(std::unique(decision.missing.begin(), decision.missing.end()), decision.missing.end());
                if (options_.debug_ship && decision.scene_ordinal == scene_->placements.size() - 1)
                    debug_piece_range = std::pair{first_piece, pieces.size()};
                MenuTimeline::mark("attract_load_hardpoints");
                return true;
            });
        }
        enqueue([this, range] {
            if (options_.release_compose_pins) static_cast<void>(range->owner->context.cache.release_raw_pins());
            if (options_.progress) options_.progress(decisions_.size(), decisions_.size());
            if (auto& prefetch = range->batch.prefetch) {
                prefetch->finish();
                report_texture_prefetch(prefetch->stats());
                range->batch.textures.set_prefetch(nullptr);
                prefetch.reset();
            }
            MenuTimeline::mark("attract_load_draw_pieces");
            return true;
        });
        enqueue([this, range, first_entity_id] {
            auto& context = range->owner->context;
            auto& batch = range->batch;
            if (!batch.failed_placements.empty()) {
                failure_ = std::to_string(batch.failed_placements.size()) + " supported surface placement uploads failed";
                for (const auto& where : batch.failed_placements) failure_ += "; " + where;
                for (const auto& detail : upload_failures_) failure_ += "; " + detail;
                return false;
            }
            sim::EntityId next_entity = std::max(first_entity, first_entity_id);
            float debug_farthest = 0.0F;
            {
                core::load_profile::Scope scope(core::load_profile::Phase::compose_commit);
                debug_farthest = commit_composed_pieces(context, batch, next_entity);
            }
            core::load_profile::Scope scope(core::load_profile::Phase::compose_finish);
            report_compose_textures(batch.textures.stats());
            report_compose_models(context.cache.model_stats(), options_.release_compose_pins);
            const bool ready = finish_composed_range(context, false, batch.next_asset, next_entity,
                range->receiving_before, range->variant_failures_before, debug_farthest);
            if (ready) MenuTimeline::mark("attract_load_population_committed");
            return ready;
        });
        return true;
    });
    return true;
}

bool SpacePopulation::compose_range(ComposeContext& context, const std::size_t first_ship,
    const std::size_t first_decision, const sim::AssetId first_asset_id, const sim::EntityId first_entity_id) {
    auto& renderer = context.renderer;
    const auto& camera = context.camera;
    const bool appending = composed_;
    prepare_compose_slots(appending);
    {
        core::load_profile::Scope scope(core::load_profile::Phase::compose_classify);
        if (!classify_arrivals(context, first_ship, first_decision)) return false;
    }
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
    // Only the initial load is bounded: an arrival composes during the battle exactly as before.
    if (!appending) batch.textures.set_budget(options_.compose_texture_budget_bytes);
    // The initial compose gets the texture read-ahead as well (decode, and the mip chains of #5989, on reader threads);
    // an arrival never does, so no step during the battle waits for or grows by a chain.
    if (!appending && options_.texture_prefetch) start_texture_prefetch(context, batch);
    compose_draw_pieces(context, batch, first_decision, appending);
    if (auto& prefetch = batch.prefetch) {
        prefetch->finish();
        report_texture_prefetch(prefetch->stats());
        batch.textures.set_prefetch(nullptr);
        prefetch.reset();
    }
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
    float debug_farthest = 0.0F;
    {
        core::load_profile::Scope scope(core::load_profile::Phase::compose_commit);
        debug_farthest = commit_composed_pieces(context, batch, next_entity);
    }
    core::load_profile::Scope scope(core::load_profile::Phase::compose_finish);
    if (!appending) {
        report_compose_textures(batch.textures.stats());
        report_compose_models(context.cache.model_stats(), options_.release_compose_pins);
    }
    return finish_composed_range(context, appending, next_asset, next_entity, receiving_before,
        variant_failures_before, debug_farthest);
}
GodotRenderer::ProxyResult SpacePopulation::append_model_proxies(GodotRenderer& renderer,
    const std::size_t recipe_ship, const std::span<const Options::PlacedShip> ships) {
    using Outcome = GodotRenderer::ProxyOutcome;
    if (ships.empty()) return {};
    if (!composed_ || !scene_ || recipe_ship >= live_decisions_.size() || !live_decisions_[recipe_ship]
        || !(options_.placed_ships[recipe_ship].projectile_slot || options_.placed_ships[recipe_ship].launch_slot
            || options_.placed_ships[recipe_ship].death_clone))
        return {Outcome::invalid_recipe, 0, "model_recipe_unavailable"};
    for (const auto& ship : ships) {
        const auto& recipe = options_.placed_ships[recipe_ship];
        if (ship.object_id != recipe.object_id || ship.projectile_slot != recipe.projectile_slot
            || ship.launch_slot != recipe.launch_slot || ship.death_clone != recipe.death_clone)
            return {Outcome::invalid_recipe, 0, "model_recipe_mismatch"};
    }
    const auto first_ship = options_.placed_ships.size();
    const auto first_piece = pieces_.size();
    const auto first_clip = live_clip_instances_.size();
    const auto first_animated = animated_instances_.size();
    const auto first_decision = decisions_.size();
    const auto first_placement = scene_->placements.size();
    const auto recipe_decision = *live_decisions_[recipe_ship];
    const auto first_idle = idle_placements_.size();
    const auto first_player = clips_.size();
    std::vector<GodotRenderer::ProxyInstance> proxies;
    std::vector<GatedInstance> pieces;
    std::vector<LiveClipInstance> clips;
    std::vector<AnimatedInstance> animated;
    std::vector<std::map<std::size_t, std::size_t>> idle_maps;
    std::vector<Options::PlacedShip> placements;
    std::vector<scene::Placement> scenes;
    std::vector<scene::SpacePlacementDecision> decisions;
    auto next_entity = next_arrival_piece_;
    GodotRenderer::ProxyResult result;
    bool backend_reserved = false;
    const auto first_index_ships = piece_index_.ship_count();
    const auto first_mark_decisions = attachment_marks_.decisions();
    bool staged_state = false;
    const auto rollback = [&] {
        if (!staged_state) return;
        if (backend_reserved) for (const auto& proxy : proxies) renderer.forget_instance_pose(proxy.entity);
        pieces_.resize(first_piece);
        live_clip_instances_.resize(first_clip);
        animated_instances_.resize(first_animated);
        decisions_.resize(first_decision);
        hardpoint_states_.resize(first_decision);
        idle_placements_.resize(first_idle);
        clips_.resize(first_player);
        std::erase_if(idle_live_ships_, [&](const auto& entry) { return entry.first >= first_idle; });
        scene_->placements.resize(first_placement);
        options_.placed_ships.resize(first_ship);
        prepare_compose_slots(true);
        piece_index_.truncate(first_index_ships);
        attachment_marks_.truncate(first_mark_decisions);
    };
    try {
        staged_state = true;
        placements.assign(ships.begin(), ships.end());
        idle_maps.resize(ships.size());
        for (std::size_t offset = 0; offset < ships.size(); ++offset) {
            auto& idles = idle_maps[offset];
            const auto copy_idle = [&](const std::size_t source) {
                auto [found, inserted] = idles.emplace(source, idle_placements_.size());
                if (inserted) {
                    auto idle = idle_placements_[source];
                    if (!options_.placed_ships[recipe_ship].projectile_slot)
                        idle.start_frame = placement_start_frame(scenes.back(), clips_[idle.clip]->playable_frames());
                    idle_placements_.push_back(idle);
                    if (idle_live_ships_.contains(source)) idle_live_ships_.emplace(found->second, first_ship + offset);
                }
                return found->second;
            };
            scenes.push_back(scene_->placements[decisions_[recipe_decision].scene_ordinal]);
            scenes.back().scene_ordinal = first_placement + offset;
            scenes.back().entity_id = static_cast<sim::EntityId>(first_placement + offset + 1);
            if (!options_.placed_ships[recipe_ship].projectile_slot)
                scenes.back().record_ordinal = static_cast<std::uint32_t>(map_placements_ + first_ship + offset);
            decisions.push_back(decisions_[recipe_decision]);
            decisions.back().scene_ordinal = first_placement + offset;
            for (const auto index : piece_index_.pieces(recipe_ship)) {
                auto piece = pieces_[index];
                if (next_entity == std::numeric_limits<sim::EntityId>::max()) {
                    rollback();
                    return {Outcome::allocation_failed, 0, "proxy_identity_overflow"};
                }
                const auto source_entity = piece.instance.entity_id;
                piece.instance.entity_id = next_entity++;
                piece.live = first_ship + offset;
                if (piece.gate) piece.gate->decision = first_decision + offset;
                proxies.push_back({piece.instance.entity_id, source_entity, piece.instance.asset_id});
                for (const auto& entry : live_clip_instances_) if (entry.entity == source_entity)
                    clips.push_back({first_ship + offset, piece.instance.entity_id, entry.asset});
                for (const auto& entry : animated_instances_) if (entry.entity == source_entity)
                    animated.push_back({piece.instance.entity_id, entry.asset, copy_idle(entry.idle)});
                pieces.push_back(std::move(piece));
            }
            const auto map_binding_idle = [&](const auto& binding) {
                if (binding && binding->idle) static_cast<void>(copy_idle(*binding->idle));
            };
            map_binding_idle(turret_bindings_[recipe_ship]);
            map_binding_idle(parent_hardpoint_turret_bindings_[recipe_ship]);
            for (const auto& [index, binding] : hardpoint_turret_bindings_[recipe_ship]) {
                static_cast<void>(index);
                if (binding.idle) static_cast<void>(copy_idle(*binding.idle));
            }
        }
        // Everything mutable is staged; uploaded assets and immutable clips are shared.
        // Geometric: an exact-size reserve would reallocate and move every existing element on each growth.
        reserve_geometric(pieces_, first_piece + pieces.size());
        reserve_geometric(live_clip_instances_, first_clip + clips.size());
        reserve_geometric(animated_instances_, first_animated + animated.size());
        reserve_geometric(decisions_, first_decision + ships.size());
        reserve_geometric(scene_->placements, first_placement + ships.size());
        reserve_geometric(hardpoint_states_, first_decision + ships.size());
        reserve_geometric(options_.placed_ships, first_ship + ships.size());
        for (auto& placed : placements) options_.placed_ships.push_back(std::move(placed));
        prepare_compose_slots(true);
        for (std::size_t offset = 0; offset < ships.size(); ++offset) {
            const auto ship = first_ship + offset;
            live_decisions_[ship] = first_decision + offset;
            hull_surface_assets_[ship] = hull_surface_assets_[recipe_ship];
            live_models_[ship] = live_models_[recipe_ship];
            // Retirement clears a ship's clip indices. Give each copy its own
            // shared owner so retiring one never clears a sibling's animation.
            const auto copy_clip = [&](const auto index) -> std::optional<std::size_t> {
                if (!index) return std::nullopt;
                const auto player = clips_[*index];
                const auto copied = clips_.size();
                clips_.push_back(player);
                return copied;
            };
            live_clips_[ship] = copy_clip(live_clips_[recipe_ship]);
            live_alternate_clips_[ship] = copy_clip(live_alternate_clips_[recipe_ship]);
            live_bounds_[ship] = live_bounds_[recipe_ship];
            live_alternate_counts_[ship] = live_alternate_counts_[recipe_ship];
            live_death_clone_[ship] = options_.placed_ships[ship].death_clone;
            backdrop_ships_[ship] = backdrop_ships_[recipe_ship];
            contact_bones_[ship] = contact_bones_[recipe_ship];
            const auto copy_binding = [&](auto binding) {
                if (binding) {
                    binding->applied.reset();
                    if (binding->idle) binding->idle = idle_maps[offset].at(*binding->idle);
                }
                return binding;
            };
            turret_bindings_[ship] = copy_binding(turret_bindings_[recipe_ship]);
            parent_hardpoint_turret_bindings_[ship] = copy_binding(parent_hardpoint_turret_bindings_[recipe_ship]);
            hardpoint_turret_bindings_[ship] = hardpoint_turret_bindings_[recipe_ship];
            for (auto& [index, binding] : hardpoint_turret_bindings_[ship]) {
                static_cast<void>(index);
                binding.applied.reset();
                if (binding.idle) binding.idle = idle_maps[offset].at(*binding.idle);
            }
            decisions_.push_back(std::move(decisions[offset]));
            scene_->placements.push_back(std::move(scenes[offset]));
            hardpoint_states_.push_back(hardpoint_states_[recipe_decision]);
        }
        pieces_.insert(pieces_.end(), pieces.begin(), pieces.end());
        live_clip_instances_.insert(live_clip_instances_.end(), clips.begin(), clips.end());
        animated_instances_.insert(animated_instances_.end(), animated.begin(), animated.end());
        append_piece_index(first_piece, first_clip);
        attachment_marks_.extend(hardpoint_states_);
        result = renderer.reserve_proxy_instances(proxies);
        if (result.outcome != Outcome::reserved) { rollback(); return result; }
        backend_reserved = true;
        // Only non-allocating publication remains after successful backend admission.
        next_arrival_piece_ = next_entity;
        schedule_arrival_sample();
        result.first_slot = first_ship;
        return result;
    } catch (const std::bad_alloc&) {
        rollback();
        return {Outcome::allocation_failed, result.bytes, "proxy_population_allocation"};
    }
}

std::optional<std::uint64_t> SpacePopulation::live_copy_bytes(const GodotRenderer& renderer,
    const std::size_t ship) const {
    std::vector<sim::AssetId> assets;
    for (const auto index : piece_index_.pieces(ship)) assets.push_back(pieces_[index].instance.asset_id);
    return renderer.model_copy_bytes(assets);
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
    map.kind = domain() == sim::tactical::BattleDomain::land ? assets::MapKind::land : assets::MapKind::space;
    scene::VfsAssetCache cache(filesystem);
    admit_scene_shaders(cache, renderer);
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
