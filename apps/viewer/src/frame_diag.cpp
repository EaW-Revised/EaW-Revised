#include "frame_diag.hpp"
#include "perf_context.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {

using namespace godot;
namespace pd = core::perf_diag;

namespace {

double ms_between(const std::chrono::steady_clock::time_point from, const std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace

FrameDiag::FrameDiag() = default;

FrameDiag& FrameDiag::instance() {
    // Never destroyed: a callback queued for the render thread may still run while the process shuts down.
    static FrameDiag* const value = new FrameDiag();
    return *value;
}

void FrameDiag::parse(const PackedStringArray& arguments) {
    bool on = false;
    calibrate_ = false;
    csv_path_.clear();
    // A telemetry feed (settings key or --eawr-telemetry) turns the frame side on with it.
    if (pd::FeedWriter::active()) on = true;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const String argument = arguments[index];
        if (argument == String("--eawr-frame-diag")) on = true;
        if (argument == String("--eawr-frame-diag-calibrate")) { on = true; calibrate_ = true; }
        if (argument == String("--eawr-frame-diag-csv") && index + 1 < arguments.size()) {
            on = true;
            csv_path_ = String(arguments[index + 1]).utf8().get_data();
        }
    }
    set_enabled(on);
}

void FrameDiag::set_enabled(const bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    feed_ = on && pd::FeedWriter::active();
    pd::MarkerSink::set_enabled(on);
    if (on) {
        if (feed_ && !main_label_) main_label_.emplace("main");
        pd::MarkerSink::bind_thread(&markers_);
        auto* tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
        if (tree != nullptr && tree->get_root() != nullptr) {
            viewport_ = tree->get_root()->get_viewport_rid();
            // The measurement request is a plain command (no return value): no render thread synchronization.
            RenderingServer::get_singleton()->viewport_set_measure_render_time(viewport_, true);
        }
    } else {
        pd::MarkerSink::bind_thread(nullptr);
        main_label_.reset();
    }
    have_previous_ = false;
    started_ = false;
}

void FrameDiag::render_thread_sample(const std::int64_t tag) {
    // Runs on the render thread (Separate render thread): the server answers these from its own thread, no sync.
    FrameDiag& self = instance();
    thread_local const pd::ThreadLabel render_label("render"); // a no-op without a telemetry feed
    static_cast<void>(render_label);
    auto* server = RenderingServer::get_singleton();
    pd::RenderSample sample{};
    sample.tag = static_cast<std::uint64_t>(tag);
    sample.cpu_ms = static_cast<float>(server->viewport_get_measured_render_time_cpu(self.viewport_));
    sample.gpu_ms = static_cast<float>(server->viewport_get_measured_render_time_gpu(self.viewport_));
    sample.setup_ms = static_cast<float>(server->get_frame_setup_time_cpu());
    // The renderer own counts of the preceding frame (the viewport answers on this thread without a sync).
    // The totals the engine monitors report (not the viewport's per-camera counts, which read far lower).
    sample.draw_calls = static_cast<std::uint32_t>(std::max<int64_t>(0,
        static_cast<int64_t>(server->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME))));
    sample.objects = static_cast<std::uint32_t>(std::max<int64_t>(0,
        static_cast<int64_t>(server->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME))));
    self.rings_.render.push(sample);
}

void FrameDiag::begin_frame() {
    if (!enabled_) return;
    entry_ = Clock::now();
    frame_ = Engine::get_singleton()->get_frames_drawn();
    if (viewport_.is_valid()) {
        RenderingServer::get_singleton()->call_on_render_thread(
            callable_mp_static(&FrameDiag::render_thread_sample).bind(static_cast<int64_t>(frame_)));
    }
    begin_cost_ms_ = ms_between(entry_, Clock::now());
}

void FrameDiag::read_pipeline_monitors(const std::uint64_t frame) {
    auto* performance = Performance::get_singleton();
    constexpr Performance::Monitor monitors[] = {
        Performance::PIPELINE_COMPILATIONS_CANVAS, Performance::PIPELINE_COMPILATIONS_MESH,
        Performance::PIPELINE_COMPILATIONS_SURFACE, Performance::PIPELINE_COMPILATIONS_DRAW,
        Performance::PIPELINE_COMPILATIONS_SPECIALIZATION};
    std::int64_t events = 0;
    for (std::size_t i = 0; i < pipeline_previous_.size(); ++i) {
        const double value = performance->get_monitor(monitors[i]);
        // Cumulative counters; a decrease means the engine restarted the count: take the value as the new events.
        events += static_cast<std::int64_t>(value >= pipeline_previous_[i] ? value - pipeline_previous_[i] : value);
        pipeline_previous_[i] = value;
    }
    if (events > 0 && started_) {
        summary_.pipeline_compiles += static_cast<std::uint64_t>(events);
        pd::MarkerSink::emit(pd::MarkerKind::pipeline_compile, frame, static_cast<std::int32_t>(events), true);
    }
}

void FrameDiag::on_matched(const pd::FrameSample& sample) {
    ++summary_.frames;
    summary_.frame_ms_sum += sample.main.frame_ms;
    summary_.viewer_main_ms_sum += sample.main.viewer_main_ms;
    summary_.outside_ms_sum += sample.main.outside_ms;
    summary_.lock_wait_ms_sum += sample.main.lock_wait_ms;
    summary_.lock_wait_ms_max = std::max<double>(summary_.lock_wait_ms_max, sample.main.lock_wait_ms);
    pd::count_stale_frame(summary_.stale_frames, sample.main.flags);
    if (sample.have_cpu) {
        ++summary_.matched;
        summary_.render_cpu_ms_sum += sample.render_cpu_ms;
        summary_.render_cpu_ms_max = std::max<double>(summary_.render_cpu_ms_max, sample.render_cpu_ms);
    }
    if (!csv_path_.empty() && history_.size() < history_cap) history_.push_back(sample);
    if (feed_) push_feed_frame(sample);
}

void FrameDiag::push_feed_frame(const pd::FrameSample& sample) {
    if (sample.main.frame_ms <= 0.0F) return; // the first frame has no predecessor
    pd::FrameRecord record{};
    record.frame = sample.main.frame;
    record.wall_us = static_cast<std::uint64_t>(sample.main.entry_us);
    record.frame_ms = sample.main.frame_ms;
    record.main_ms = sample.main.viewer_main_ms;
    record.render_cpu_ms = sample.have_cpu ? sample.render_cpu_ms : -1.0F;
    record.gpu_ms = latest_gpu_ms_;
    record.ui_lock_ms = sample.main.lock_wait_ms;
    record.flags = (sample.main.flags & pd::frame_flag_stale) != 0U ? pd::FrameFlags::stale : std::uint16_t{0};
    const Extra& extra = extras_[sample.main.frame & (extras_.size() - 1)];
    if (extra.frame == sample.main.frame && extra.fight_valid) {
        record.flags = static_cast<std::uint16_t>(record.flags | pd::FrameFlags::fight);
        record.units = extra.fight.units;
        record.enemy_units = extra.fight.enemy_units;
        record.attacking = extra.fight.attacking;
        record.projectiles = extra.fight.projectiles;
        record.effects = extra.fight.effects;
        record.particles = extra.fight.particles;
        record.draw_calls = sample.render_draw_calls;
        record.objects = sample.render_objects;
    }
    pd::FeedWriter::push_frame_active(record);
}

void FrameDiag::note_phase(const std::string_view name, const double ms) {
    if (!feed_) return;
    const auto found = phase_ids_.find(name);
    pd::PhaseId id = pd::no_phase;
    if (found != phase_ids_.end()) {
        id = found->second;
    } else {
        id = pd::FeedWriter::intern_frame_phase_active(name);
        phase_ids_.emplace(std::string(name), id);
    }
    if (id == pd::no_phase) return;
    for (auto& entry : frame_phase_) {
        if (entry.first == id) {
            entry.second += static_cast<float>(ms);
            return;
        }
    }
    frame_phase_.emplace_back(id, static_cast<float>(ms));
}

void FrameDiag::push_feed_phases(const core::tick_profile::Recording& ui_recording) {
    // The named scopes of this frame (the ui.lock.* waits included), summed per name, beside the phases the frame
    // timers noted. The largest stay when a frame has more than a record holds.
    for (const auto& cost : ui_recording.costs) note_phase(cost.name, cost.ms);
    if (frame_phase_.empty()) return;
    pd::FramePhases phases{};
    phases.frame = frame_;
    std::sort(frame_phase_.begin(), frame_phase_.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const std::size_t count = std::min<std::size_t>(frame_phase_.size(), pd::FramePhases::max_entries);
    for (std::size_t i = 0; i < count; ++i) {
        phases.entries[i].id = frame_phase_[i].first;
        phases.entries[i].ms = frame_phase_[i].second;
    }
    phases.count = static_cast<std::uint32_t>(count);
    pd::FeedWriter::push_frame_phases_active(phases);
    frame_phase_.clear();
}

void FrameDiag::drain_rings() {
    pd::RenderSample sample{};
    const auto sink = [this](const pd::FrameSample& matched) { on_matched(matched); };
    while (rings_.render.pop(sample)) {
        ++summary_.gpu_samples;
        if (have_previous_gpu_ && sample.gpu_ms == previous_gpu_ms_) ++summary_.gpu_repeated;
        previous_gpu_ms_ = sample.gpu_ms;
        latest_gpu_ms_ = sample.gpu_ms;
        have_previous_gpu_ = true;
        summary_.render_gpu_ms_sum += sample.gpu_ms;
        summary_.render_gpu_ms_max = std::max<double>(summary_.render_gpu_ms_max, sample.gpu_ms);
        if (!csv_path_.empty() && raw_.size() < history_cap) raw_.push_back(sample);
        matcher_.add_render(sample, sink);
    }
    // Markers are consumed by the later feed writer; here the ring is only emptied so it never reports drops.
    pd::Marker marker{};
    while (markers_.pop(marker)) {}
}

void FrameDiag::end_frame(const bool stale, const bool running, const core::tick_profile::Recording& ui_recording,
                          const FightCounts* fight) {
    if (!enabled_) return;
    const auto exit_probe = Clock::now();
    pd::MainFrame record{};
    record.frame = frame_;
    record.entry_us = std::chrono::duration_cast<std::chrono::microseconds>(entry_.time_since_epoch()).count();
    record.viewer_main_ms = static_cast<float>(ms_between(entry_, exit_probe));
    record.frame_ms = have_previous_ ? static_cast<float>(ms_between(previous_entry_, entry_)) : 0.0F;
    record.outside_ms = have_previous_ ? static_cast<float>(ms_between(previous_exit_, entry_)) : 0.0F;
    double lock = 0.0;
    for (const auto& cost : ui_recording.costs)
        if (cost.name.starts_with("ui.lock.")) lock += cost.ms;
    record.lock_wait_ms = static_cast<float>(lock);
    record.flags = (stale ? pd::frame_flag_stale : 0U) | (running ? pd::frame_flag_running : 0U);
    if (calibrate_ && viewport_.is_valid()) {
        auto* server = RenderingServer::get_singleton();
        old_.push_back({frame_, server->viewport_get_measured_render_time_cpu(viewport_),
                        server->viewport_get_measured_render_time_gpu(viewport_)});
    }
    if (feed_) {
        Extra& extra = extras_[frame_ & (extras_.size() - 1)];
        extra.frame = frame_;
        extra.fight_valid = fight != nullptr;
        if (fight != nullptr) extra.fight = *fight;
        // The context record follows the settings and the window: re-read every two seconds (an unchanged text is ignored).
        if (frame_ % 120 == 0) pd::FeedWriter::set_context_active(perf_context_fields());
    }
    drain_rings();
    read_pipeline_monitors(frame_);
    matcher_.add_main(record, [this](const pd::FrameSample& matched) { on_matched(matched); });
    summary_.dropped = rings_.render.dropped();
    summary_.incomplete = matcher_.incomplete();
    summary_.late = matcher_.late();
    previous_entry_ = entry_;
    have_previous_ = true;
    started_ = true;
    previous_exit_ = Clock::now();
    summary_.own_cost_ms_sum += begin_cost_ms_ + ms_between(exit_probe, previous_exit_);
}

std::string FrameDiag::report_json() const {
    if (!enabled_ && summary_.frames == 0) return "null";
    const Summary& s = summary_;
    const auto mean = [&](const double sum, const std::uint64_t count) { return count > 0 ? sum / static_cast<double>(count) : 0.0; };
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\"frames\": " << s.frames << ", \"matched\": " << s.matched << ", \"incomplete\": " << s.incomplete
        << ", \"late\": " << s.late << ", \"dropped\": " << s.dropped << ", \"stale_frames\": " << s.stale_frames
        << ", \"pipeline_compiles\": " << s.pipeline_compiles
        << ", \"frame_ms_mean\": " << mean(s.frame_ms_sum, s.frames)
        << ", \"viewer_main_ms_mean\": " << mean(s.viewer_main_ms_sum, s.frames)
        << ", \"outside_ms_mean\": " << mean(s.outside_ms_sum, s.frames)
        << ", \"lock_wait_ms_mean\": " << mean(s.lock_wait_ms_sum, s.frames) << ", \"lock_wait_ms_max\": " << s.lock_wait_ms_max
        << ", \"render_cpu_ms_mean\": " << mean(s.render_cpu_ms_sum, s.matched) << ", \"render_cpu_ms_max\": " << s.render_cpu_ms_max
        << ", \"render_gpu_ms_mean\": " << mean(s.render_gpu_ms_sum, s.gpu_samples) << ", \"render_gpu_ms_max\": " << s.render_gpu_ms_max
        << ", \"own_cost_ms_mean\": " << mean(s.own_cost_ms_sum, s.frames)
        << ", \"lag_cpu\": " << matcher_.lags().cpu << ", \"gpu_join_verified\": false, \"gpu_samples\": " << s.gpu_samples
        << ", \"gpu_repeated\": " << s.gpu_repeated << "}";
    return out.str();
}

void FrameDiag::write_files() {
    if (csv_path_.empty()) return;
    std::ofstream frames(csv_path_, std::ios::binary | std::ios::trunc);
    if (frames) {
        frames << "frame,frame_ms,viewer_main_ms,outside_ms,lock_wait_ms,flags,render_cpu_ms,render_gpu_ms,render_setup_ms,have_cpu,have_gpu\n";
        frames << std::fixed << std::setprecision(4);
        for (const pd::FrameSample& s : history_)
            frames << s.main.frame << ',' << s.main.frame_ms << ',' << s.main.viewer_main_ms << ',' << s.main.outside_ms << ','
                   << s.main.lock_wait_ms << ',' << s.main.flags << ',' << s.render_cpu_ms << ',' << s.render_gpu_ms << ','
                   << s.render_setup_ms << ',' << (s.have_cpu ? 1 : 0) << ',' << (s.have_gpu ? 1 : 0) << '\n';
    }
    std::ofstream raw(csv_path_ + ".render.csv", std::ios::binary | std::ios::trunc);
    if (raw) {
        raw << "tag,cpu_ms,gpu_ms,setup_ms\n" << std::fixed << std::setprecision(4);
        for (const pd::RenderSample& s : raw_) raw << s.tag << ',' << s.cpu_ms << ',' << s.gpu_ms << ',' << s.setup_ms << '\n';
    }
    if (calibrate_) {
        std::ofstream old(csv_path_ + ".old.csv", std::ios::binary | std::ios::trunc);
        old << "frame,old_cpu_ms,old_gpu_ms\n" << std::fixed << std::setprecision(4);
        for (const Old& s : old_) old << s.frame << ',' << s.cpu << ',' << s.gpu << '\n';
    }
}

} // namespace eawr::presentation::godot_backend
