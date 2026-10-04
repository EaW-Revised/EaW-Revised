#pragma once

#include "eawr/core/load_profile.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>
#include <godot_cpp/classes/performance.hpp>

namespace eawr::presentation::godot_backend {

inline std::string_view direct_start_origin = "extension_load";

class StartupTrace final {
public:
    using Clock = core::load_profile::Clock;
    struct Run final {
        core::load_profile::Recording recording;
        Clock::time_point start{};
        double total_ms{}, return_ms{};
        double ready_ms{}, first_tick_ms{}, first_tick_lua_ms{}, first_tick_fog_ms{};
        std::array<double, 5> pipelines{};
        bool drawn{}, from_setup{};
    };
    void begin(bool setup, Clock::time_point start = Clock::now()) {
        runs_.emplace_back();
        runs_.back().start = start;
        runs_.back().from_setup = setup;
        runs_.back().pipelines = pipeline_counts();
        core::load_profile::active = &runs_.back().recording;
    }
    void drawn() {
        if (runs_.empty() || runs_.back().drawn) return;
        runs_.back().total_ms = milliseconds(runs_.back().start);
        runs_.back().drawn = true;
        const auto counts = pipeline_counts();
        for (std::size_t i = 0; i < counts.size(); ++i) runs_.back().pipelines[i] = counts[i] - runs_.back().pipelines[i];
        core::load_profile::active = nullptr;
        persist();
    }
    void returned(Clock::time_point start) {
        if (runs_.empty()) return;
        runs_.back().return_ms = milliseconds(start);
        persist();
    }
    void returning(Clock::time_point start) { return_start_ = start; }
    void setup_drawn() {
        if (return_start_) { returned(*return_start_); return_start_.reset(); }
    }
    void trace_path(const std::filesystem::path& path) { path_ = path; path_ += ".startup.csv"; }
    void report_path(const std::filesystem::path& path) { report_path_ = path; }
    void ready() { if (pending()) runs_.back().ready_ms = milliseconds(runs_.back().start); }
    void first_tick(double total, double lua, double fog) {
        if (pending()) {
            runs_.back().first_tick_ms = total;
            runs_.back().first_tick_lua_ms = lua;
            runs_.back().first_tick_fog_ms = fog;
        }
    }
    void write(std::ostream& output) const {
        output << "  \"startup\":{\"clock\":\"steady\",\"boundary\":\"frame_post_draw_after_first_battle_process\",\"runs\":[";
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            const auto& run = runs_[i];
            output << (i ? "," : "") << "{\"origin\":\"" << (run.from_setup ? "start_pressed" : direct_start_origin)
                   << "\",\"first_frame_drawn\":" << (run.drawn ? "true" : "false")
                   << ",\"total_ms\":" << run.total_ms << ",\"return_to_setup_ms\":" << run.return_ms
                   << ",\"ready_ms\":" << run.ready_ms
                   << ",\"first_draw_wait_ms\":" << (run.drawn ? run.total_ms - run.ready_ms : 0.0)
                   << ",\"first_tick_ms\":" << run.first_tick_ms
                   << ",\"first_tick_lua_ms\":" << run.first_tick_lua_ms
                   << ",\"first_tick_fog_ms\":" << run.first_tick_fog_ms
                   << ",\"pipeline_compilations\":[";
            for (std::size_t j = 0; j < run.pipelines.size(); ++j) output << (j ? "," : "") << run.pipelines[j];
            output << ']'
                   << ",\"phases\":";
            core::load_profile::write_costs(output, run.recording);
            output << '}';
        }
        output << "]},\n";
    }
    [[nodiscard]] bool pending() const { return !runs_.empty() && !runs_.back().drawn; }
private:
    static std::array<double, 5> pipeline_counts() {
        auto* performance = godot::Performance::get_singleton();
        return {performance->get_monitor(godot::Performance::PIPELINE_COMPILATIONS_CANVAS),
            performance->get_monitor(godot::Performance::PIPELINE_COMPILATIONS_MESH),
            performance->get_monitor(godot::Performance::PIPELINE_COMPILATIONS_SURFACE),
            performance->get_monitor(godot::Performance::PIPELINE_COMPILATIONS_DRAW),
            performance->get_monitor(godot::Performance::PIPELINE_COMPILATIONS_SPECIALIZATION)};
    }
    static double milliseconds(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    void persist() const {
        // A battle writes its report before its host is destroyed. Refresh just
        // our single-line field after returning so it includes teardown timing.
        if (!report_path_.empty()) {
            std::ifstream input(report_path_);
            std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            const auto first = content.find("  \"startup\":");
            if (first != std::string::npos) {
                const auto last = content.find('\n', first);
                if (last != std::string::npos) {
                    std::ostringstream field;
                    write(field);
                    content.replace(first, last + 1 - first, field.str());
                    std::ofstream report(report_path_, std::ios::trunc);
                    report << content;
                }
            }
        }
        if (path_.empty()) return;
        std::ofstream output(path_, std::ios::trunc);
        output << "run,origin,phase,exclusive_ms,inclusive_ms,calls,total_ms,return_to_setup_ms,ready_ms,"
                  "first_draw_wait_ms,first_tick_ms,first_tick_lua_ms,first_tick_fog_ms,"
                  "pipeline_canvas,pipeline_mesh,pipeline_surface,pipeline_draw,pipeline_specialization\n";
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            const auto& run = runs_[i];
            for (std::size_t j = 0; j < core::load_profile::names.size(); ++j) {
                const auto& cost = run.recording.costs[j];
                output << i << ',' << (run.from_setup ? "start_pressed" : direct_start_origin) << ','
                       << core::load_profile::names[j] << ',' << cost.exclusive_ms << ',' << cost.inclusive_ms
                       << ',' << cost.calls << ',' << run.total_ms << ',' << run.return_ms
                       << ',' << run.ready_ms << ',' << (run.drawn ? run.total_ms - run.ready_ms : 0.0)
                       << ',' << run.first_tick_ms << ',' << run.first_tick_lua_ms << ',' << run.first_tick_fog_ms;
                for (const auto count : run.pipelines) output << ',' << count;
                output << '\n';
            }
        }
    }
    std::vector<Run> runs_;
    std::filesystem::path path_;
    std::filesystem::path report_path_;
    std::optional<Clock::time_point> return_start_;
};
inline StartupTrace startup_trace;
// Captured at extension load, then anchored to OS process creation on Windows.
inline StartupTrace::Clock::time_point process_start = StartupTrace::Clock::now();
} // namespace eawr::presentation::godot_backend
