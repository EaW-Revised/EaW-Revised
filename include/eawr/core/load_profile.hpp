#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::core::load_profile {

// Presentation diagnostics only. The caller owns the recording on its thread;
// clocks never enter simulation input or replay state. Nested timings are exclusive.
using Clock = std::chrono::steady_clock;
enum class Phase : std::size_t {
    vfs, catalog, unit_tables, map, session, lua, fog, model, texture,
    material, audio, hud, particles, scene, texture_upload,
    shader_compile, shader_reflection, material_binding,
    mesh_upload, scene_build, scene_population, scene_environment,
    // Inside scene_population (compose_range): where the compose stage's own time goes.
    compose_classify, compose_bind_pose, compose_hull, compose_shell, compose_animation, compose_hardpoints,
    compose_commit, compose_finish,
    // Inside one surface upload (upload_surface): texture lookup/copy, material description, the renderer call,
    // and the bind-pose bounds loop.
    surface_texture_prepare, surface_material_build, surface_renderer_upload, surface_pose_bounds,
    // Inside xml_catalog (load_catalog): the registry files with their definitions, the parse of every other
    // effective XML for the physical inventory, and the winner index. Inside vfs_mount_index: the loose-tree walk
    // and the MEG header parse (#5197).
    catalog_registry, catalog_inventory, catalog_index, vfs_loose_walk, vfs_meg_parse, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Phase::count)> names{
    "vfs_mount_index", "xml_catalog", "unit_tables", "map_decode", "session_setup_first_frame",
    "lua_start", "fog_visibility", "model_decode", "texture_decode", "material_shader_creation",
    "audio_banks", "hud_fonts", "particles", "scene_composition", "texture_gpu_upload",
    "shader_compile", "shader_reflection", "material_binding",
    "mesh_gpu_upload", "scene_build", "scene_population", "scene_environment",
    "compose_classify", "compose_bind_pose", "compose_hull", "compose_shell", "compose_animation", "compose_hardpoints",
    "compose_commit", "compose_finish",
    "surface_texture_prepare", "surface_material_build", "surface_renderer_upload", "surface_pose_bounds",
    "catalog_registry", "catalog_inventory", "catalog_index", "vfs_loose_walk", "vfs_meg_parse"};
struct Cost final { double exclusive_ms{}, inclusive_ms{}; std::uint64_t calls{}; };
// One mounted layer's share of the load: the loose-tree walk and MEG header parse of the mount, and the XML parsed
// from it (registry files with their definitions, then the physical inventory pass).
struct LayerCost final {
    std::string layer;
    double loose_ms{}, meg_ms{}, xml_registry_ms{}, xml_inventory_ms{};
    std::uint64_t loose_files{}, meg_entries{}, xml_registry_files{}, xml_inventory_files{}, xml_bytes{};
};
struct Recording final {
    std::array<Cost, names.size()> costs{};
    std::vector<LayerCost> layers;
    [[nodiscard]] LayerCost& layer(const std::string_view id) {
        for (auto& entry : layers) if (entry.layer == id) return entry;
        layers.push_back(LayerCost{std::string(id)});
        return layers.back();
    }
};
inline thread_local Recording* active{};
class Scope;
inline thread_local Scope* parent{};

class Scope final {
public:
    explicit Scope(Phase phase) : recording_(active), phase_(phase) {
        if (recording_) { parent_ = parent; parent = this; start_ = Clock::now(); }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!recording_) return;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        auto& cost = recording_->costs[static_cast<std::size_t>(phase_)];
        cost.exclusive_ms += elapsed - children_ms_;
        cost.inclusive_ms += elapsed;
        ++cost.calls;
        if (parent_) parent_->children_ms_ += elapsed;
        parent = parent_;
    }
private:
    Recording* recording_{};
    Phase phase_;
    Scope* parent_{};
    Clock::time_point start_{};
    double children_ms_{};
};

// Adds a layer's cost to the calling thread's recording; a no-op without one.
template <class Update>
void layer_cost(const std::string_view id, Update&& update) {
    if (active) update(active->layer(id));
}

inline double since_ms(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Which XML pass of a layer a LayerScope adds to. The clock and the millisecond fields stay in this header: the
// simulation boundary check reads src/data/xml.cpp, which only names the scope.
enum class LayerKind { registry, inventory };

// Adds the time from construction to the end of the scope, one file and its bytes to the layer's XML cost (#5197).
class LayerScope final {
public:
    LayerScope(const std::string_view layer, const LayerKind kind, const std::uint64_t bytes = 0)
        : layer_(layer), kind_(kind), bytes_(bytes), live_(active != nullptr) {
        if (live_) start_ = Clock::now();
    }
    LayerScope(const LayerScope&) = delete;
    LayerScope& operator=(const LayerScope&) = delete;
    ~LayerScope() {
        if (!live_) return;
        layer_cost(layer_, [&](LayerCost& cost) {
            const double elapsed = since_ms(start_);
            if (kind_ == LayerKind::registry) { cost.xml_registry_ms += elapsed; ++cost.xml_registry_files; }
            else { cost.xml_inventory_ms += elapsed; ++cost.xml_inventory_files; }
            cost.xml_bytes += bytes_;
        });
    }
    void add_bytes(const std::uint64_t bytes) noexcept { bytes_ += bytes; }
    // The file was not read: nothing is added.
    void discard() noexcept { live_ = false; }
private:
    std::string_view layer_;
    LayerKind kind_;
    std::uint64_t bytes_;
    bool live_;
    Clock::time_point start_{};
};

inline void write_layer_costs(std::ostream& output, const Recording& recording) {
    output << '[';
    for (std::size_t i = 0; i < recording.layers.size(); ++i) {
        const auto& layer = recording.layers[i];
        output << (i ? "," : "") << "{\"layer\":\"" << layer.layer << "\",\"loose_ms\":" << layer.loose_ms
               << ",\"loose_files\":" << layer.loose_files << ",\"meg_ms\":" << layer.meg_ms
               << ",\"meg_entries\":" << layer.meg_entries << ",\"xml_registry_ms\":" << layer.xml_registry_ms
               << ",\"xml_registry_files\":" << layer.xml_registry_files
               << ",\"xml_inventory_ms\":" << layer.xml_inventory_ms
               << ",\"xml_inventory_files\":" << layer.xml_inventory_files << ",\"xml_bytes\":" << layer.xml_bytes << '}';
    }
    output << ']';
}

inline void write_costs(std::ostream& output, const Recording& recording) {
    output << '{';
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& cost = recording.costs[i];
        output << (i ? "," : "") << '"' << names[i] << "\":{\"exclusive_ms\":" << cost.exclusive_ms
               << ",\"inclusive_ms\":" << cost.inclusive_ms << ",\"calls\":" << cost.calls << '}';
    }
    output << '}';
}
} // namespace eawr::core::load_profile
