#include "perf_trace.hpp"
#include "startup_trace.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/string.hpp>

#include <iomanip>

using namespace godot;

namespace eawr::presentation::godot_backend {

bool PerfTrace::fine_particle_timings_{};

std::optional<std::string> PerfTrace::parse(const PackedStringArray& arguments, std::string& error) {
    fine_particle_timings_=false;
    particles::EffectRegistry::set_stream_bucketing_default(true);
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (String(arguments[index]) == String("--eawr-particle-fine-timings")) fine_particle_timings_=true;
        if (String(arguments[index]) == String("--eawr-particle-stream-reference"))
            particles::EffectRegistry::set_stream_bucketing_default(false);
    }
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
               "ai_ms,ai_worst_ms,lua_ms,lua_worst_ms,particle_streams,particle_uploads,particle_replacements,particle_conversion_ms,particle_submission_ms,emitter_prepare_ms,projectile_prepare_ms,clone_prepare_ms,wall_frame_ms,viewer_main_ms,outside_viewer_ms,render_setup_cpu_ms,tick_wait_ms,live_frame_ms,session_tail_ms,clip_pose_ms,emitter_frame_ms,effects_frame_ms,debris_frame_ms,idle_frame_ms,idle_sample_ms,idle_upload_ms,particle_groups,particle_visible_groups,particle_prepared_groups,particle_stepped_groups,emitter_spawn_cpu_ms,emitter_spawn_backend_ms,emitter_scalar_step_ms,emitter_scalar_build_ms,emitter_batch_step_cpu_ms,emitter_batch_build_cpu_ms,emitter_executor_ms,emitter_executor_wait_dispatch_ms,emitter_commit_ms,effects_spawn_cpu_ms,effects_spawn_backend_ms,effects_scalar_step_ms,effects_scalar_build_ms,effects_batch_step_cpu_ms,effects_batch_build_cpu_ms,effects_executor_ms,effects_executor_wait_dispatch_ms,effects_commit_ms,debris_spawn_cpu_ms,debris_spawn_backend_ms,debris_scalar_step_ms,debris_scalar_build_ms,debris_batch_step_cpu_ms,debris_batch_build_cpu_ms,debris_executor_ms,debris_executor_wait_dispatch_ms,debris_commit_ms,world_ms,worker_ms,serial_ms,sim_fog_ms,worker_phases,vsync_mode,engine_max_fps,sim_lock_ms,sim_publish_ms,particle_main_emission_ms,particle_main_update_ms,particle_main_sort_ms,particle_main_stream_ms,particle_main_packing_ms,particle_worker_emission_ms,particle_worker_update_ms,particle_worker_sort_ms,particle_worker_stream_ms,particle_worker_packing_ms\n";
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
            << ',' << frame.particle_work.prepared_groups << ',' << frame.particle_work.stepped_groups;
    for (const auto& timing : {frame.emitter_timing,frame.effects_timing,frame.debris_timing}) {
        output_ << ',' << timing.spawn_cpu_ms
            << ',' << timing.spawn_backend_ms
            << ',' << timing.scalar_step_ms
            << ',' << timing.scalar_build_ms
            << ',' << timing.batch_step_cpu_ms
            << ',' << timing.batch_build_cpu_ms
            << ',' << timing.executor_ms
            << ',' << timing.executor_wait_dispatch_ms
            << ',' << timing.commit_ms;
    }
    output_ << ',' << frame.world_ms << ',' << frame.worker_ms << ',' << frame.serial_ms << ',' << frame.sim_fog_ms
            << ",\"";
    for (const auto& [name, elapsed] : frame.worker_phases) output_ << name << '=' << elapsed << ';';
    output_ << "\"," << static_cast<int>(DisplayServer::get_singleton()->window_get_vsync_mode())
            << ',' << Engine::get_singleton()->get_max_fps()
            << ',' << frame.sim_lock_ms << ',' << frame.sim_publish_ms;
    for (const bool caller : {true, false}) {
        std::array<double, 5> stages{};
        for (const auto& timing : {frame.emitter_timing, frame.effects_timing, frame.debris_timing}) {
            const auto& measured = caller ? timing.main_stages_ms : timing.worker_stages_ms;
            for (std::size_t phase = 0; phase < stages.size(); ++phase) stages[phase] += measured[phase];
        }
        for (const double elapsed : stages) output_ << ',' << elapsed;
    }
    output_ << '\n';
    previous_report_end_ = Clock::now();
}

} // namespace eawr::presentation::godot_backend
