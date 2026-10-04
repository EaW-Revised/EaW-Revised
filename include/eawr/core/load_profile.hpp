#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace eawr::core::load_profile {

// Presentation diagnostics only. The caller owns the recording on its thread;
// clocks never enter simulation input or replay state. Nested timings are exclusive.
using Clock = std::chrono::steady_clock;
enum class Phase : std::size_t {
    vfs, catalog, unit_tables, map, session, lua, fog, model, texture,
    material, audio, hud, particles, scene, texture_upload,
    shader_compile, shader_reflection, material_binding,
    mesh_upload, scene_build, scene_population, scene_environment, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Phase::count)> names{
    "vfs_mount_index", "xml_catalog", "unit_tables", "map_decode", "session_setup_first_frame",
    "lua_start", "fog_visibility", "model_decode", "texture_decode", "material_shader_creation",
    "audio_banks", "hud_fonts", "particles", "scene_composition", "texture_gpu_upload",
    "shader_compile", "shader_reflection", "material_binding",
    "mesh_gpu_upload", "scene_build", "scene_population", "scene_environment"};
struct Cost final { double exclusive_ms{}, inclusive_ms{}; std::uint64_t calls{}; };
struct Recording final {
    std::array<Cost, names.size()> costs{};
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
