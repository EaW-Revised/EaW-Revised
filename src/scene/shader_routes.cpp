#include "eawr/scene/shader_routes.hpp"

#include "eawr/assets/environment_shaders.hpp"
#include "eawr/assets/model_listing.hpp"
#include "eawr/assets/model_shaders.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <tuple>
#include <utility>

namespace eawr::scene {
namespace {

[[nodiscard]] std::string folded(const std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

struct Key final {
    std::string layer;
    std::string shader; // folded
    friend bool operator<(const Key& a, const Key& b) { return std::tie(a.layer, a.shader) < std::tie(b.layer, b.shader); }
};

// Reads the scan through the asset layer's positional reader over the native file or archive entry. Only a view with no such
// reader falls back to reading the asset whole through the view; a failed header scan never does.
[[nodiscard]] assets::ModelShaderScan scan_record(const vfs::Vfs& filesystem, const vfs::AssetRecord& record) {
    if (auto reader = filesystem.open_range(record)) {
        const auto read = [&reader](const std::uint64_t offset, const std::span<std::byte> out) { return reader->read(offset, out); };
        return assets::scan_model_shaders(record.size, read); // an incomplete scan is reported as unreadable
    }
    const auto bytes = filesystem.open(record);
    if (!bytes) {
        assets::ModelShaderScan failed;
        failed.complete = false;
        return failed;
    }
    return assets::scan_model_shaders(bytes.value());
}

} // namespace

std::string_view to_string(const ShaderRoute route) noexcept {
    switch (route) {
    case ShaderRoute::hand_adapter: return "hand_adapter";
    case ShaderRoute::planet: return "planet";
    case ShaderRoute::missing_effect: return "missing_effect";
    case ShaderRoute::environment: return "environment";
    case ShaderRoute::intentional_skip: return "intentional_skip";
    case ShaderRoute::translated: return "translated";
    case ShaderRoute::unhandled: return "unhandled";
    }
    return "unknown";
}

bool is_environment_route_shader(const std::string_view shader) noexcept {
    // Skydome, nebula and the starfield gloss spelling: the one table the environment scene's route selection also reads.
    return assets::is_environment_only_shader(shader);
}

ShaderRoute shader_route(const std::string_view shader, const bool effect_missing, const bool translated) noexcept {
    if (effect_missing) return ShaderRoute::missing_effect;
    if (find_legacy_selector(shader) != nullptr) return ShaderRoute::hand_adapter;
    if (is_planet_effect(shader)) return ShaderRoute::planet;
    if (is_environment_route_shader(shader)) return ShaderRoute::environment;
    if (is_shadow_volume_shader(shader)) return ShaderRoute::intentional_skip;
    if (translated) return ShaderRoute::translated;
    return ShaderRoute::unhandled;
}

ShaderAudit audit_model_shaders(const vfs::Vfs& filesystem, bool (*translated)(std::string_view)) {
    ShaderAudit audit;
    const AssetAccess access{
        .exists = [&filesystem](const std::string_view path) { return static_cast<bool>(filesystem.stat(path)); },
        .model = {}, .sha256 = {}, .particle_system = {}};
    auto models = assets::list_models(filesystem);
    if (!models) return audit;
    // Disk order keeps a spinning drive from seeking back and forth between archives.
    std::vector<vfs::AssetRecord> records = std::move(models.value());
    std::vector<std::pair<std::string, std::uint64_t>> where(records.size());
    std::vector<std::size_t> order(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        order[i] = i;
        if (const auto location = filesystem.read_location(records[i])) where[i] = {location->source, location->offset};
    }
    std::stable_sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) { return where[a] < where[b]; });

    std::map<Key, ShaderAuditEntry> found;
    std::map<std::string, bool> missing_cache;
    for (const std::size_t index : order) {
        const vfs::AssetRecord& record = records[index];
        ++audit.models_scanned;
        const auto scan = scan_record(filesystem, record);
        if (!scan.complete) ++audit.models_unreadable;
        std::map<std::string, std::uint64_t> in_model; // folded shader -> visible meshes in this model
        for (const assets::MeshShaderName& mesh : scan.meshes) {
            if (!mesh.visible) continue;
            ++audit.visible_meshes;
            const std::string key = folded(mesh.shader);
            auto missing = missing_cache.find(key);
            if (missing == missing_cache.end()) missing = missing_cache.emplace(key, missing_material_effect(access, mesh.shader)).first;
            const bool admitted = translated && translated(mesh.shader);
            const ShaderRoute route = shader_route(mesh.shader, missing->second, admitted);
            ++audit.by_route[route];
            if (route != ShaderRoute::unhandled) continue;
            ++in_model[key];
            auto& entry = found[Key{record.layer_id, key}];
            if (entry.shader.empty()) {
                entry.shader = mesh.shader;
                entry.layer = record.layer_id;
                entry.example_model = record.original_path.empty() ? record.canonical_path : record.original_path;
            }
            ++entry.meshes;
        }
        for (const auto& [key, count] : in_model) {
            (void)count;
            ++found[Key{record.layer_id, key}].models;
        }
    }
    for (auto& [key, entry] : found) audit.unhandled.push_back(std::move(entry));
    std::stable_sort(audit.unhandled.begin(), audit.unhandled.end(), [](const ShaderAuditEntry& a, const ShaderAuditEntry& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.meshes != b.meshes) return a.meshes > b.meshes;
        return folded(a.shader) < folded(b.shader);
    });
    return audit;
}

DroppedLayers dropped_layers(const std::span<const Placement* const> drawn) {
    DroppedLayers result;
    std::map<std::string, DroppedLayer> groups; // folded shader
    std::map<std::string, std::set<std::uint64_t>> placement_sets;
    for (const Placement* placement : drawn) {
        if (placement == nullptr) continue;
        for (const Surface& surface : placement->surfaces) {
            if (surface.supported) continue;
            const std::string key = folded(surface.shader);
            auto [found, inserted] = groups.try_emplace(key);
            DroppedLayer& layer = found->second;
            if (inserted) {
                layer.shader = surface.shader;
                layer.route = shader_route(surface.shader, false, false);
                layer.object_type = placement->object_id;
                layer.model_path = placement->model_path;
            }
            ++layer.surfaces;
            ++result.surfaces;
            if (layer.route == ShaderRoute::unhandled) ++result.unhandled_surfaces;
            placement_sets[key].insert(placement->scene_ordinal);
        }
    }
    for (auto& [key, layer] : groups) {
        layer.placements = placement_sets[key].size();
        result.by_shader.push_back(std::move(layer));
    }
    std::stable_sort(result.by_shader.begin(), result.by_shader.end(), [](const DroppedLayer& a, const DroppedLayer& b) {
        if (a.surfaces != b.surfaces) return a.surfaces > b.surfaces;
        return folded(a.shader) < folded(b.shader);
    });
    return result;
}

std::string dropped_layers_json(const DroppedLayers& dropped) {
    std::string out = "{\"surfaces\": " + std::to_string(dropped.surfaces)
        + ", \"unhandled_surfaces\": " + std::to_string(dropped.unhandled_surfaces) + ", \"by_shader\": [";
    bool first = true;
    for (const DroppedLayer& layer : dropped.by_shader) {
        out += (first ? "" : ", ");
        out += "{\"shader\": " + core::json_quote(layer.shader) + ", \"route\": " + core::json_quote(to_string(layer.route))
            + ", \"surfaces\": " + std::to_string(layer.surfaces) + ", \"placements\": " + std::to_string(layer.placements)
            + ", \"object_type\": " + core::json_quote(layer.object_type)
            + ", \"model_path\": " + core::json_quote(layer.model_path) + "}";
        first = false;
    }
    return out + "]}";
}

std::string dropped_layers_summary(const DroppedLayers& dropped) {
    if (dropped.surfaces == 0) return "no dropped surfaces";
    std::string out = std::to_string(dropped.surfaces) + " dropped surfaces (" + std::to_string(dropped.unhandled_surfaces)
        + " unhandled):";
    for (const DroppedLayer& layer : dropped.by_shader)
        out += " " + layer.shader + " x" + std::to_string(layer.surfaces) + (layer.route == ShaderRoute::unhandled ? "" : " [" + std::string(to_string(layer.route)) + "]") + ";";
    return out;
}

std::vector<core::LoadNote> shader_audit_notes(const ShaderAudit& audit) {
    std::vector<core::LoadNote> notes;
    for (const ShaderAuditEntry& entry : audit.unhandled) {
        core::LoadNote note;
        note.code = std::string(shader_unrouted_code);
        note.classification = core::LoadCategory::unknown_degrade;
        note.layer.mod_id = entry.layer;
        note.file = entry.example_model;
        note.object = entry.shader;
        note.tag = "shader";
        note.authored_value = entry.shader;
        note.action = core::LoadAction::skipped_object;
        note.effective_value = "the layers using it are not drawn: " + std::to_string(entry.meshes) + " visible mesh layers in "
            + std::to_string(entry.models) + " models";
        note.phase = core::LoadPhase::load;
        note.message = "The shader " + entry.shader + " is not supported, so the model layers that use it will not be drawn "
            "in game. First model: " + entry.example_model;
        notes.push_back(std::move(note));
    }
    return notes;
}

} // namespace eawr::scene
