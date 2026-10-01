#include "perf_trace.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/string.hpp>

#include <iomanip>

using namespace godot;

namespace eawr::presentation::godot_backend {

std::optional<std::string> PerfTrace::parse(const PackedStringArray& arguments, std::string& error) {
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (String(arguments[index]) != String("--eawr-perf-trace")) continue;
        if (index + 1 >= arguments.size()) {
            error = "missing value for --eawr-perf-trace";
            return std::nullopt;
        }
        return std::string(String(arguments[index + 1]).utf8().get_data());
    }
    return std::nullopt;
}

bool PerfTrace::open(const std::string& path, std::string& error) {
    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_) {
        error = "--eawr-perf-trace: cannot write " + path;
        return false;
    }
    auto* tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
    if (tree != nullptr && tree->get_root() != nullptr) {
        viewport_ = tree->get_root()->get_viewport_rid();
        RenderingServer::get_singleton()->viewport_set_measure_render_time(viewport_, true);
    }
    output_ << "frame,frame_ms,process_ms,render_cpu_ms,render_gpu_ms,draw_calls,objects,primitives,tick,units,"
               "projectiles,effects,effect_particles,emitter_particles,particle_ms,ticks,tick_ms,submit_ms,pieces,sent,bookkeeping_ms,hud_ms,audio_ms,fog_ms\n";
    return true;
}

void PerfTrace::frame(const Frame& frame) {
    if (!output_) return;
    auto* server = RenderingServer::get_singleton();
    auto* performance = Performance::get_singleton();
    const double cpu = viewport_.is_valid() ? server->viewport_get_measured_render_time_cpu(viewport_) : 0.0;
    const double gpu = viewport_.is_valid() ? server->viewport_get_measured_render_time_gpu(viewport_) : 0.0;
    const double process = performance->get_monitor(Performance::TIME_PROCESS) * 1000.0;
    output_ << frames_++ << ',' << std::fixed << std::setprecision(3) << frame.frame_ms << ',' << process << ',' << cpu
            << ',' << gpu
            << ',' << static_cast<std::uint64_t>(performance->get_monitor(Performance::RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
            << ',' << static_cast<std::uint64_t>(performance->get_monitor(Performance::RENDER_TOTAL_OBJECTS_IN_FRAME))
            << ',' << static_cast<std::uint64_t>(performance->get_monitor(Performance::RENDER_TOTAL_PRIMITIVES_IN_FRAME))
            << ',' << frame.presented_tick << ',' << frame.units << ',' << frame.projectiles << ',' << frame.effects << ','
            << frame.effect_particles << ',' << frame.emitter_particles << ',' << frame.particle_ms << ',' << frame.ticks
            << ',' << frame.tick_ms << ',' << frame.submit_ms << ',' << frame.pieces << ',' << frame.sent
            << ',' << frame.bookkeeping_ms << ',' << frame.hud_ms << ',' << frame.audio_ms << ',' << frame.fog_ms << '\n';
}

} // namespace eawr::presentation::godot_backend
