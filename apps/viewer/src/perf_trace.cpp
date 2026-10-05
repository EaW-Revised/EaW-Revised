#include "perf_trace.hpp"
#include "startup_trace.hpp"

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
    startup_trace.trace_path(path);
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
               "projectiles,effects,effect_particles,emitter_particles,particle_ms,ticks,tick_ms,submit_ms,pieces,sent,bookkeeping_ms,hud_ms,audio_ms,fog_ms,pose_ms,compose_ms,refresh_ms,opacity_ms,"
               "ai_ms,ai_worst_ms,lua_ms,lua_worst_ms,particle_streams,particle_uploads,particle_replacements,particle_conversion_ms,particle_submission_ms,emitter_prepare_ms,projectile_prepare_ms,clone_prepare_ms,wall_frame_ms,viewer_main_ms,outside_viewer_ms,render_setup_cpu_ms,tick_wait_ms,live_frame_ms,session_tail_ms,clip_pose_ms,emitter_frame_ms,effects_frame_ms,debris_frame_ms,idle_frame_ms,idle_sample_ms,idle_upload_ms,particle_groups,particle_visible_groups,particle_prepared_groups,particle_stepped_groups\n";
    return true;
}

void PerfTrace::begin_frame() {
    frame_entry_ = Clock::now();
    const auto ms = [](const Clock::duration elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
    wall_frame_ms_ = previous_entry_ ? ms(frame_entry_ - *previous_entry_) : 0.0;
    outside_viewer_ms_ = previous_report_end_ ? ms(frame_entry_ - *previous_report_end_) : 0.0;
    previous_entry_ = frame_entry_;
}

void PerfTrace::frame(const Frame& frame) {
    if (!output_) return;
    const double viewer_main_ms = std::chrono::duration<double, std::milli>(Clock::now() - frame_entry_).count();
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
            << ',' << frame.bookkeeping_ms << ',' << frame.hud_ms << ',' << frame.audio_ms << ',' << frame.fog_ms << ',' << frame.pose_ms << ',' << frame.compose_ms
            << ',' << frame.refresh_ms << ',' << frame.opacity_ms
            << ',' << frame.ai_ms << ',' << frame.ai_worst_ms << ',' << frame.lua_ms << ',' << frame.lua_worst_ms
            << ',' << frame.particle_work.streams << ',' << frame.particle_work.uploads << ',' << frame.particle_work.replacements
            << ',' << frame.particle_work.conversion_ms << ',' << frame.particle_work.submission_ms
            << ',' << frame.emitter_prepare_ms << ',' << frame.projectile_prepare_ms << ',' << frame.clone_prepare_ms
            << ',' << wall_frame_ms_ << ',' << viewer_main_ms << ',' << outside_viewer_ms_
            << ',' << server->get_frame_setup_time_cpu() << ',' << frame.tick_wait_ms << ',' << frame.live_frame_ms
            << ',' << frame.session_tail_ms << ',' << frame.clip_pose_ms << ',' << frame.emitter_frame_ms
            << ',' << frame.effects_frame_ms << ',' << frame.debris_frame_ms
            << ',' << frame.idle_frame_ms << ',' << frame.idle_sample_ms << ',' << frame.idle_upload_ms
            << ',' << frame.particle_work.groups << ',' << frame.particle_work.visible_groups
            << ',' << frame.particle_work.prepared_groups << ',' << frame.particle_work.stepped_groups << '\n';
    previous_report_end_ = Clock::now();
}

} // namespace eawr::presentation::godot_backend
