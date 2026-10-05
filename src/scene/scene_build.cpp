#include "eawr/scene/scene.hpp"

#include "scene_internal.hpp"

#include "eawr/sim/replay.hpp"
#include "eawr/core/load_profile.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::scene {
namespace {

using sim::math::Fixed;

constexpr std::array<std::string_view, 5> team_colour_status_order{
    "faction_colour", "owner_absent", "owner_unmapped", "colour_undeclared", "colour_invalid",
};

struct DeclaredValue final {
    std::string text;
    const data::XmlNode* node{};
    Provenance provenance;
};

[[nodiscard]] std::optional<DeclaredValue> declared(
    const data::EffectiveObject& object, const std::string_view tag) {
    const data::EffectiveValue* value = object.value(tag);
    if (value == nullptr || value->value.raw_text.empty()) return std::nullopt;
    DeclaredValue result;
    result.text = value->value.raw_text;
    result.node = &value->value;
    result.provenance.tag = std::string(tag);
    result.provenance.source_object_id = value->source_object_id;
    result.provenance.logical_path = value->value.source.logical_path;
    result.provenance.line = value->value.source.line;
    return result;
}

[[nodiscard]] bool has_behavior(const std::string_view declared_behaviors,
                                const std::string_view wanted) noexcept {
    std::size_t first = 0;
    while (first < declared_behaviors.size()) {
        while (first < declared_behaviors.size()
               && (declared_behaviors[first] == ',' || declared_behaviors[first] == ' '
                   || declared_behaviors[first] == '\t' || declared_behaviors[first] == '\r'
                   || declared_behaviors[first] == '\n')) ++first;
        std::size_t last = first;
        while (last < declared_behaviors.size() && declared_behaviors[last] != ','
               && declared_behaviors[last] != ' ' && declared_behaviors[last] != '\t'
               && declared_behaviors[last] != '\r' && declared_behaviors[last] != '\n') ++last;
        if (ieq(declared_behaviors.substr(first, last - first), wanted)) return true;
        first = last == first ? last + 1 : last;
    }
    return false;
}

// One faction as a player index names it, with its team colour.
struct FactionColour final {
    std::string id;
    std::optional<std::array<std::uint8_t, 3>> rgb;
    std::string_view status;
    Provenance provenance;
};

// Three or four integers in 0..255 separated by commas and/or whitespace.
[[nodiscard]] std::optional<std::array<std::uint8_t, 3>> parse_colour(const std::string_view text) {
    std::vector<int> values;
    std::size_t at = 0;
    const auto separator = [](const char c) { return c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (at < text.size()) {
        while (at < text.size() && separator(text[at])) ++at;
        if (at == text.size()) break;
        int value = 0;
        std::size_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 4) {
            value = value * 10 + (text[at] - '0');
            ++at;
            ++digits;
        }
        if (digits == 0 || value > 255 || (at < text.size() && !separator(text[at]))) return std::nullopt;
        values.push_back(value);
    }
    if (values.size() != 3 && values.size() != 4) return std::nullopt;
    return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(values[0]),
        static_cast<std::uint8_t>(values[1]), static_cast<std::uint8_t>(values[2])};
}

// The faction table player indices resolve against. The colour is read from
// the faction's own last active definition, not through Catalog::resolve,
// whose winner lookup spans every category.
[[nodiscard]] std::vector<FactionColour> faction_colours(const data::Catalog& catalog) {
    std::vector<FactionColour> result;
    for (const std::string& id : faction_order(catalog)) {
        const data::Definition* source = nullptr;
        for (const data::Definition* definition : catalog.find_all(id)) {
            if (definition->category != data::Category::faction || !definition->active) continue;
            if (source == nullptr || definition->registry_order > source->registry_order
                || (definition->registry_order == source->registry_order
                    && definition->definition_order > source->definition_order)) {
                source = definition;
            }
        }
        FactionColour faction;
        faction.id = id;
        faction.status = team_colour_status_order[3];
        const data::XmlNode* colour = nullptr;
        if (source != nullptr) {
            for (const data::XmlNode& child : source->root.children) {
                if (ieq(child.name, "Color")) colour = &child;
            }
        }
        if (colour != nullptr) {
            faction.provenance = {"Color", id, colour->source.logical_path, colour->source.line};
            faction.rgb = parse_colour(colour->raw_text);
            faction.status = faction.rgb ? team_colour_status_order[0] : team_colour_status_order[4];
        }
        result.push_back(std::move(faction));
    }
    return result;
}

// Mini 2 of the placement payload, last valid occurrence, like the loader's
// reading of the other placement minis.
[[nodiscard]] std::optional<std::int32_t> decode_owner(const assets::Placement& source) {
    std::optional<std::int32_t> owner;
    for (const assets::RawField& field : source.fields) {
        if (field.id != 2 || field.bytes.size() != 4) continue;
        std::uint32_t value{};
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.bytes[index])) << (8U * index);
        }
        owner = static_cast<std::int32_t>(value);
    }
    return owner;
}

void resolve_team_colour(Placement& placement, const assets::Placement& source,
                         const std::vector<FactionColour>& factions) {
    placement.owner_player = decode_owner(source);
    if (!placement.owner_player) {
        placement.team_colour_status = std::string(team_colour_status_order[1]);
        return;
    }
    const std::int32_t index = *placement.owner_player;
    if (index < 0 || static_cast<std::size_t>(index) >= factions.size()) {
        placement.team_colour_status = std::string(team_colour_status_order[2]);
        return;
    }
    const FactionColour& faction = factions[static_cast<std::size_t>(index)];
    placement.owner_faction = faction.id;
    placement.team_colour = faction.rgb;
    placement.team_colour_status = std::string(faction.status);
    placement.team_colour_provenance = faction.provenance;
}

void add_issue(Placement& placement, const Cause cause, std::string detail = {}) {
    placement.issues.push_back({cause, std::move(detail)});
}

[[nodiscard]] Cause conversion_cause(const core::Diagnostic& error) {
    return error.code == diagnostic_codes::nonfinite ? Cause::transform_nonfinite
                                                     : Cause::transform_overflow;
}

void resolve_transform(Placement& placement, const assets::Placement& source) {
    if (!source.position) {
        add_issue(placement, Cause::position_absent);
        return;
    }
    // A missing or nonfinite orientation is a cause, never an identity or a
    // yaw-only rotation.
    if (!source.orientation_degrees
        || source.orientation_status == assets::OrientationStatus::absent) {
        add_issue(placement, Cause::orientation_absent);
        return;
    }
    if (source.orientation_status == assets::OrientationStatus::nonfinite) {
        add_issue(placement, Cause::transform_nonfinite);
        return;
    }
    // R-ROT-01..03: T + Rz(yaw) Ry(pitch) Rx(roll) Rz(+90) v, once at scene build.
    const auto& degrees = *source.orientation_degrees;
    const std::array<float, 6> inputs{source.position->x, source.position->y, source.position->z,
        degrees.z, degrees.y, degrees.x};
    std::array<Fixed, 6> converted{};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        auto value = fixed_from_binary32(inputs[index]);
        if (!value) {
            add_issue(placement, conversion_cause(value.error()));
            return;
        }
        converted[index] = value.value();
    }
    auto height = sim::math::add(converted[2], Fixed::from_raw(placement.layer_z_adjust_raw));
    if (!height) {
        add_issue(placement, Cause::transform_overflow);
        return;
    }
    converted[2] = height.value();
    auto matrix = placement_transform(converted[0], converted[1], converted[2], converted[3], converted[4], converted[5],
                                Fixed::from_raw(placement.scale_raw));
    if (!matrix) {
        add_issue(placement, Cause::transform_overflow);
        return;
    }
    Transform transform;
    transform.position_raw = {converted[0].raw(), converted[1].raw(), converted[2].raw()};
    transform.yaw_degrees_raw = converted[3].raw();
    transform.scale_raw = placement.scale_raw;
    transform.matrix = matrix.value();
    placement.transform = transform;
}

void resolve_object(Placement& placement, const assets::Placement& source,
                    const std::optional<assets::MapKind> kind, const data::Catalog& catalog,
                    const AssetAccess& access, std::map<std::string, ModelFacts>& models) {
    if (!source.type_crc) {
        add_issue(placement, Cause::crc_absent);
        return;
    }
    if (source.type_resolution == assets::TypeResolution::missing) {
        add_issue(placement, Cause::crc_missing, hex32(*source.type_crc));
        return;
    }
    if (source.type_resolution == assets::TypeResolution::collision) {
        add_issue(placement, Cause::crc_collision, hex32(*source.type_crc));
        return;
    }
    const assets::ObjectTypeRef& type = source.type_candidates.front();
    placement.object_id = type.logical_name;
    placement.object_provenance.tag = "type_crc";
    placement.object_provenance.source_object_id = type.logical_name;
    placement.object_provenance.logical_path = type.source.logical_path;
    placement.object_provenance.line = type.source.line;
    auto effective = catalog.resolve(type.logical_name, data::Category::game_object);
    if (!effective) {
        add_issue(placement, Cause::object_unresolvable, type.logical_name);
        return;
    }
    const data::EffectiveObject& object = effective.value();
    const auto behavior = declared(object, "Behavior");
    const auto land_behavior = declared(object, "LandBehavior");
    placement.capture_point = (behavior && has_behavior(behavior->text, "CAPTURE_POINT"))
        || (land_behavior && has_behavior(land_behavior->text, "CAPTURE_POINT"));

    if (const auto scale = declared(object, "Scale_Factor")) {
        placement.scale_declared = true;
        placement.scale_provenance = scale->provenance;
        auto value = data::fixed_value(*scale->node);
        if (!value || value.value().raw() <= 0) {
            add_issue(placement, Cause::scale_invalid, scale->text);
        } else {
            placement.scale_raw = value.value().raw();
        }
    }

    // LZ-01 includes map SpaceProps; only the presentation transform is raised.
    if (kind == assets::MapKind::space && ieq(object.type_name, "SpaceProp")) {
        if (const auto height = declared(object, "Layer_Z_Adjust")) {
            auto value = data::fixed_value(*height->node);
            if (value) placement.layer_z_adjust_raw = value.value().raw();
            else add_issue(placement, Cause::transform_nonfinite, "Layer_Z_Adjust");
        }
    }

    // Model selection mirrors the loader's documented rule: the kind-specific
    // tag wins where declared, otherwise Model_Name.
    std::optional<DeclaredValue> model;
    if (kind == assets::MapKind::land) model = declared(object, "Land_Model_Name");
    if (kind == assets::MapKind::space) model = declared(object, "Space_Model_Name");
    if (!model) model = declared(object, "Model_Name");
    if (!model) {
        add_issue(placement, Cause::model_undeclared, type.logical_name);
        return;
    }
    placement.model_declared = trimmed(model->text);
    placement.model_provenance = model->provenance;
    const std::string path = probe(access, "data/art/models/", placement.model_declared, model_suffixes);
    if (path.empty()) {
        add_issue(placement, Cause::model_not_in_vfs, placement.model_declared);
        return;
    }
    placement.model_path = path;
    auto found = models.find(path);
    if (found == models.end()) found = models.emplace(path, model_facts(access, path)).first;
    const ModelFacts& facts = found->second;
    if (!facts.loaded) {
        add_issue(placement, facts.particle_system ? Cause::model_particle_system
                                                   : Cause::model_failed_to_load, path);
        return;
    }
    if (facts.surfaces.empty()) add_issue(placement, Cause::model_has_no_surface, path);
    placement.surfaces = facts.surfaces;
    placement.effects = facts.effects;
    for (const Surface& surface : facts.surfaces) {
        if (!surface.supported) add_issue(placement, Cause::shader_unsupported, surface.shader);
        for (const TextureBinding& texture : surface.textures) {
            if (texture.resolved.empty()) add_issue(placement, Cause::texture_unresolved, texture.declared);
        }
    }
    for (const AttachedEffect& effect : facts.effects) {
        if (effect.resolved.empty()) add_issue(placement, Cause::effect_unresolved, effect.proxy_name);
    }

    // Idle animation. The catalog names no idle clip; it can only override
    // the animation base name. The clip is looked up by the corpus's
    // `<base>_idle_00.ala` naming, reported as that observation rather than as
    // an XML value, and it is recorded, not played: the static scene has no
    // movement.
    std::string base = path.substr(0, path.size() - 4);
    placement.idle_animation_provenance.tag = "model_stem";
    placement.idle_animation_provenance.logical_path = path;
    if (const auto override_name = declared(object, "Land_Model_Anim_Override_Name");
        override_name && kind == assets::MapKind::land) {
        std::string name = canonical(trimmed(override_name->text));
        if (name.ends_with(".alo")) name.resize(name.size() - 4);
        base = "data/art/models/" + name;
        placement.idle_animation_provenance = override_name->provenance;
    }
    const std::string idle = base + "_idle_00.ala";
    if (access.exists && access.exists(idle)) {
        placement.idle_animation = idle;
        placement.idle_animation_status = "corpus_naming_observed";
    } else {
        placement.idle_animation_status = "none_found";
    }
}

} // namespace

std::span<const std::string_view> team_colour_statuses() noexcept { return team_colour_status_order; }

namespace {

struct PreparedScene final {
    Scene scene;
    std::vector<const assets::Placement*> ordered;
    std::map<std::string, ModelFacts> models;
};

PreparedScene prepare(const BuildInput& input) {
    PreparedScene prepared;
    Scene& scene = prepared.scene;
    auto& ordered = prepared.ordered;
    if (input.map == nullptr || input.catalog == nullptr) return prepared;
    const assets::Map& map = *input.map;
    scene.map_logical_path = map.source.logical_path;
    scene.map_sha256 = input.map_sha256;
    scene.map_kind = !map.kind ? "unknown" : (*map.kind == assets::MapKind::land ? "land" : "space");

    // Scene order is the map Source identity plus the authored record
    // ordinal. The loader assigns ordinals in document order, and a stable
    // sort keeps that order even if a caller hands records over permuted.
    ordered.reserve(map.placements.size());
    for (const assets::Placement& placement : map.placements) ordered.push_back(&placement);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const assets::Placement* left, const assets::Placement* right) {
                         if (left->key.map.logical_path != right->key.map.logical_path) {
                             return left->key.map.logical_path < right->key.map.logical_path;
                         }
                         return left->key.record_ordinal < right->key.record_ordinal;
                     });

    const std::vector<FactionColour> factions = faction_colours(*input.catalog);
    scene.placements.resize(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const assets::Placement& source = *ordered[index];
        Placement& placement = scene.placements[index];
        placement.scene_ordinal = index;
        placement.entity_id = static_cast<sim::EntityId>(index + 1U);
        placement.map_logical_path = source.key.map.logical_path;
        placement.record_ordinal = source.key.record_ordinal;
        placement.serialized_object_id = source.serialized_object_id;
        placement.type_crc = source.type_crc;
        resolve_team_colour(placement, source, factions);
        resolve_object(placement, source, map.kind, *input.catalog, input.access, prepared.models);
    }
    return prepared;
}

void finalize_placement(Placement& placement, const assets::Placement& source) {
    // MD-06: a mesh-free model's resolved proxies still need its authored pose.
    const bool effect_model = !placement.effects.empty()
        && std::all_of(placement.effects.begin(), placement.effects.end(),
                       [](const AttachedEffect& effect) { return !effect.resolved.empty(); });
    const bool blocked = std::any_of(placement.issues.begin(), placement.issues.end(),
        [&](const Issue& issue) {
            return blocks_drawing(issue.cause)
                && !(effect_model && issue.cause == Cause::model_has_no_surface);
        });
    // A placement whose object did not resolve still has a position, but a
    // transform is only meaningful with the object's scale, so it is
    // converted only once the object chain is usable.
    if (!blocked) resolve_transform(placement, source);
    std::sort(placement.issues.begin(), placement.issues.end(),
              [](const Issue& left, const Issue& right) {
                  if (left.cause != right.cause) return left.cause < right.cause;
                  return left.detail < right.detail;
              });
    placement.issues.erase(std::unique(placement.issues.begin(), placement.issues.end()),
                           placement.issues.end());
}

Scene finish(PreparedScene prepared) {
    Scene& scene = prepared.scene;
    const auto& models = prepared.models;
    // Stable asset IDs: the distinct loaded model paths in byte order, so an
    // ID depends only on which models the scene uses, never on the order in
    // which placements, probes or loads happened.
    std::set<std::string> paths;
    for (const Placement& placement : scene.placements) {
        if (placement.model_path.empty()) continue;
        const auto found = models.find(placement.model_path);
        if (found != models.end() && found->second.loaded) paths.insert(placement.model_path);
    }
    std::map<std::string, sim::AssetId> ids;
    for (const std::string& path : paths) {
        const sim::AssetId id = static_cast<sim::AssetId>(scene.assets.size() + 1U);
        ids.emplace(path, id);
        scene.assets.push_back({id, path, models.at(path).sha256});
    }
    for (Placement& placement : scene.placements) {
        const auto found = ids.find(placement.model_path);
        if (found != ids.end()) placement.asset_id = found->second;
    }

    const std::string text = canonical_text(scene);
    scene.scene_sha256 = sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return std::move(scene);
}

core::Diagnostic invalid_worker_diagnostic() {
    return diagnostic(diagnostic_codes::invalid_workers, "scene builder requires 1, 2, or 4 workers");
}

} // namespace

Scene build(const BuildInput& input) {
    core::load_profile::Scope scope(core::load_profile::Phase::scene_build);
    if (input.map == nullptr || input.catalog == nullptr) return {};
    auto prepared = prepare(input);
    for (std::size_t index = 0; index < prepared.ordered.size(); ++index) {
        finalize_placement(prepared.scene.placements[index], *prepared.ordered[index]);
    }
    return finish(std::move(prepared));
}

core::Result<BuildResult> build(const BuildInput& input, const sim::PartitionExecutor& executor) {
    using Result = core::Result<BuildResult>;
    const std::size_t workers = executor.worker_count();
    if (workers != 1 && workers != 2 && workers != 4) {
        return Result::failure(invalid_worker_diagnostic());
    }
    auto prepared = prepare(input);
    ExecutionStats stats;
    stats.workers_requested = workers;
    stats.placement_count = prepared.ordered.size();
    stats.partition_placement_counts.resize(workers);
    std::vector<std::optional<std::thread::id>> threads(workers);
    std::vector<int> claimed(workers);
    std::vector<std::uint8_t> completed(workers);
    std::atomic<bool> invalid_partition{false};
    auto executed = executor.execute(workers, [&](const std::size_t partition) {
        if (partition >= workers) { invalid_partition = true; return; }
        if (std::atomic_ref<int>(claimed[partition]).exchange(1) != 0) {
            invalid_partition = true;
            return;
        }
        threads[partition] = std::this_thread::get_id();
        for (std::size_t index = partition; index < prepared.ordered.size(); index += workers) {
            finalize_placement(prepared.scene.placements[index], *prepared.ordered[index]);
            ++stats.partition_placement_counts[partition];
        }
        completed[partition] = true;
    });
    if (!executed) return Result::failure(executed.error());
    if (invalid_partition || !std::all_of(completed.begin(), completed.end(),
                                          [](std::uint8_t value) { return value != 0; })) {
        return Result::failure(invalid_worker_diagnostic());
    }
    stats.partitions_completed = workers;
    std::set<std::thread::id> unique_threads;
    for (const auto& id : threads) unique_threads.insert(*id);
    stats.observed_worker_threads = unique_threads.size();
    return Result::success({finish(std::move(prepared)), std::move(stats)});
}

} // namespace eawr::scene
