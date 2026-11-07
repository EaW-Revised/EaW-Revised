#pragma once

#include "eawr/core/load_profile.hpp"
#include "startup_trace.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace eawr::presentation::godot_backend {

// Opt-in (`--eawr-menu-timeline`, user arguments) timeline of the start to the interactive main menu (#5197): wall
// clock since the process was created, process memory including the peaks, the load-phase and per-layer costs of the
// data load, and the main-menu frame times (the attract battle starts on the menu's first frames). Presentation
// diagnostics only: without the flag every call returns before it reads a clock, the process or the engine.
class MenuTimeline final {
public:
    [[nodiscard]] static bool enabled() {
        static const bool value = [] {
            auto* os = godot::OS::get_singleton();
            return os != nullptr && os->get_cmdline_user_args().has("--eawr-menu-timeline");
        }();
        return value;
    }

    // CPU time (user + kernel, ms) of the calling thread; 0 where the platform has no counter. Pure Win32, no engine
    // object: the attract load worker calls it.
    [[nodiscard]] static double thread_cpu_ms() {
#if defined(_WIN32)
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
        const auto ticks = [](const FILETIME& time) {
            return static_cast<double>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime) / 1.0e4;
        };
        return ticks(kernel) + ticks(user);
#else
        return 0.0;
#endif
    }

    // Records the data load and the attract battle's load on the calling (main) thread until end().
    static void begin() {
        if (!enabled() || state().active) return;
        state().active = true;
        core::load_profile::active = &state().recording;
        mark("timeline_begin");
    }

    static void end(const char* label) {
        if (!enabled() || !state().active) return;
        report(label);
        state().active = false;
        core::load_profile::active = nullptr;
    }

    [[nodiscard]] static bool active() { return enabled() && state().active; }

    static void mark(const char* label) {
        if (!enabled()) return;
        state().last_mark = label;
        godot::UtilityFunctions::print(String_(line("menu_mark", label)));
    }

    // A free-form diagnostic line (`kind` becomes the line's name), with the same memory and CPU fields as the marks.
    static void note(const char* kind, const std::string& detail) {
        if (!enabled()) return;
        godot::UtilityFunctions::print(String_(line(kind, detail.c_str())));
    }

    // One finished load callback of the attract battle (LoadJob step observer): its ordinal, duration and the last
    // mark reached, which the callback itself emits when it has one (`attract_load_*`). Callbacks under 1 ms are skipped.
    static void step(const std::size_t ordinal, const double ms) {
        if (!enabled()) return;
        const std::string step_detail = std::move(state().detail);
        state().detail.clear();
        if (ms < 1.0) return;
        std::ostringstream out;
        out << "n=" << ordinal << " ms=" << ms << " last_mark=" << (state().last_mark.empty() ? "-" : state().last_mark);
        // What the callback was working on (set by the callback itself), so a heavy step can be named.
        if (ms >= 100.0 && !step_detail.empty()) out << " detail=" << step_detail;
        godot::UtilityFunctions::print(String_(line("menu_step", out.str().c_str())));
    }

    // Names what the running load callback works on (a path, a phase); printed with its step when it takes 100 ms or
    // more and cleared when the step ends. Call it only when enabled() if building the text costs anything.
    static void detail(std::string text) {
        if (!enabled()) return;
        state().detail = std::move(text);
    }

    // Adds to the running callback's detail (a callback with several parts names each).
    static void detail_add(const std::string& text) {
        if (!enabled()) return;
        auto& step_detail = state().detail;
        step_detail += step_detail.empty() ? text : " | " + text;
    }

    // One call per presented front-end frame: the first frames, every hitch, and a summary of the first 600.
    static void frame(const bool attract_running) {
        if (!enabled()) return;
        auto& s = state();
        const auto now = Clock::now();
        const double dt = s.frames == 0 ? 0.0 : std::chrono::duration<double, std::milli>(now - s.last_frame).count();
        s.last_frame = now;
        ++s.frames;
        if (s.frames <= 5 || s.frames == 10) {
            std::ostringstream out;
            out << "n=" << s.frames << " dt_ms=" << dt << " attract=" << attract_running;
            godot::UtilityFunctions::print(String_(line("menu_frame", out.str().c_str())));
        }
        if (s.frames > 1) {
            if (dt > 50.0) {
                ++s.over_50ms;
                s.responsive_run = 0;
                if (dt > 100.0) {
                    std::ostringstream out;
                    out << "n=" << s.frames << " dt_ms=" << dt << " attract=" << attract_running;
                    godot::UtilityFunctions::print(String_(line("menu_hitch", out.str().c_str())));
                }
            } else if (++s.responsive_run == 30 && !s.responsive_frame) {
                s.responsive_frame = s.frames;
                std::ostringstream out;
                out << "n=" << s.frames << " attract=" << attract_running;
                godot::UtilityFunctions::print(String_(line("menu_responsive", out.str().c_str())));
            }
            s.max_ms = dt > s.max_ms ? dt : s.max_ms;
            s.sum_ms += dt;
        }
        if (s.frames == 600) {
            std::ostringstream out;
            out << "frames=600 over_50ms=" << s.over_50ms << " max_ms=" << s.max_ms << " mean_ms=" << s.sum_ms / 599.0
                << " responsive_frame=" << s.responsive_frame << " attract=" << attract_running;
            godot::UtilityFunctions::print(String_(line("menu_frames_summary", out.str().c_str())));
        }
    }

    // The accumulated phase and per-layer costs; the recording keeps running so a later report is cumulative.
    static void report(const char* label) {
        if (!enabled()) return;
        const auto& recording = state().recording;
        std::ostringstream out;
        out << "{\"label\":\"" << label << "\",\"phases\":{";
        bool first = true;
        for (std::size_t i = 0; i < core::load_profile::names.size(); ++i) {
            const auto& cost = recording.costs[i];
            if (cost.calls == 0) continue;
            out << (first ? "" : ",") << '"' << core::load_profile::names[i] << "\":{\"exclusive_ms\":" << cost.exclusive_ms
                << ",\"inclusive_ms\":" << cost.inclusive_ms << ",\"calls\":" << cost.calls << '}';
            first = false;
        }
        out << "},\"layers\":";
        core::load_profile::write_layer_costs(out, recording);
        out << '}';
        godot::UtilityFunctions::print(godot::String("EAWR menu_costs ") + godot::String::utf8(out.str().c_str()));
    }

private:
    using Clock = StartupTrace::Clock;
    struct State final {
        bool active{};
        core::load_profile::Recording recording;
        Clock::time_point last_frame{};
        std::uint64_t frames{}, over_50ms{}, responsive_frame{}, responsive_run{};
        double max_ms{}, sum_ms{};
        std::string last_mark;
        std::string detail;
    };
    static State& state() {
        static State value;
        return value;
    }
    static godot::String String_(const std::string& text) { return godot::String::utf8(text.c_str()); }

    static std::string line(const char* kind, const char* text) {
        const double since_start = std::chrono::duration<double, std::milli>(Clock::now() - process_start).count();
        double private_mib = 0.0, peak_commit_mib = 0.0, working_mib = 0.0, peak_working_mib = 0.0;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            constexpr double mib = 1024.0 * 1024.0;
            private_mib = static_cast<double>(counters.PrivateUsage) / mib;
            peak_commit_mib = static_cast<double>(counters.PeakPagefileUsage) / mib;
            working_mib = static_cast<double>(counters.WorkingSetSize) / mib;
            peak_working_mib = static_cast<double>(counters.PeakWorkingSetSize) / mib;
        }
#endif
        // CPU time of this process (user + kernel, all threads) and the busy time of the whole machine, in ms since boot or
        // process start: the difference of two lines over their t_ms gives CPU % of the logical processors (logical=).
        double cpu_ms = 0.0, sys_cpu_ms = 0.0;
        unsigned logical = 1;
#if defined(_WIN32)
        const auto ticks = [](const FILETIME& time) {
            return static_cast<double>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime) / 1.0e4;
        };
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) cpu_ms = ticks(kernel) + ticks(user);
        FILETIME idle{}, system_kernel{}, system_user{};
        if (GetSystemTimes(&idle, &system_kernel, &system_user)) // the kernel time includes the idle time
            sys_cpu_ms = ticks(system_kernel) + ticks(system_user) - ticks(idle);
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        logical = info.dwNumberOfProcessors;
#endif
        const auto unix_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream out;
        out << "EAWR " << kind << ' ' << text << " t_ms=" << since_start << " private_mib=" << private_mib
            << " peak_commit_mib=" << peak_commit_mib << " ws_mib=" << working_mib << " peak_ws_mib=" << peak_working_mib
            << " cpu_ms=" << cpu_ms << " sys_cpu_ms=" << sys_cpu_ms << " logical=" << logical << " unix_us=" << unix_us;
        return out.str();
    }
};

} // namespace eawr::presentation::godot_backend
