#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "eawr/core/perf_diag/phase.hpp"

namespace eawr::core::perf_diag {

// Tick-speed (simulation) against frame-time (rendering) verdict of design 2.7. Computed once per second over a 5 s
// window from already-measured numbers; a diagnostic hint, never a performance claim. Negative inputs mean "not
// measured" (no on-CPU reading, no GPU time, no PC context): a rule that needs them cannot match.
struct WindowStats final {
    bool idle{};                 // paused, loading, menu, no session
    float delivered_ratio{1.0F}; // D: ticks per second / requested ticks per second
    float interval_ms{33.3F};    // I: 1000 / requested ticks per second
    float tick_p95_ms{};         // p95 of the tick cost
    float sim_cpu{-1.0F};     // U_sim: share of tick wall time the sim thread ran
    float session_lock_ms{};     // mean per tick wait for the session mutex
    float mean_tick_ms{};        // for W_sim
    PhaseId top_phase{no_phase}; // phase with the largest share of the p95 ticks
    float top_phase_ms{};
    float frame_budget_ms{16.7F}; // B
    float frame_p95_ms{};         // F
    float main_ms{};              // M: viewer + engine work on the main thread (limiter sleep excluded)
    float main_viewer_ms{};
    float render_cpu_ms{-1.0F};   // RT
    float gpu_ms{-1.0F};          // G
    float ui_lock_ms{};           // mean per frame wait for the session mutex
    float mean_frame_ms{};        // for W_ui
    float stale_share{};          // share of frames whose presented tick was clamped at the newest tick
    float others_cpu{-1.0F};      // share of the machine used by other processes
    float cost_correlation{};     // correlation of per-second tick cost and frame time over the window
    float hitch_coincidence{};    // share of tick and frame hitches within one frame of each other
};

// Starting values, tuned with recorded data; one table, one test per rule.
struct VerdictThresholds final {
    float ok_delivered{0.95F};
    float sim_load{0.9F};      // L_sim at or above
    float sim_busy{0.8F};      // U_sim at or above: the sim's own work
    float sim_starved{0.6F};   // U_sim below: descheduled
    float wait_share{0.2F};    // W_sim / W_ui at or above: coupled by lock
    float external_cpu{0.25F}; // others_cpu at or above
    float render_share{0.85F}; // G or RT as a share of F
    float correlation{0.5F};
    float coincidence{0.5F};
    float stale_share{0.10F};
};

enum class VerdictKind : std::uint8_t {
    idle, ok, sim_bound, render_bound, coupled, external, both_independent, unknown
};

struct Verdict final {
    VerdictKind kind{VerdictKind::idle};
    int sim_rule{};   // 0 none, else 1..6 of the table
    int frame_rule{}; // 0 none, else 7..10
    bool sim_unknown{}; // the sim is slow but the on-CPU share was not measured: no sim cause is claimed
    bool picture_holds{}; // visible consequence of a sim verdict: the presented picture stayed on the newest tick
    // Inputs echoed for the text line.
    PhaseId phase{no_phase};
    float phase_ms{};
    float sim_value{};   // lock ms (rule 3), others share (rule 4)
    float frame_value{}; // GPU / render-thread / lock ms (rules 7-9), main viewer ms (rule 10)
    float engine_ms{};   // rule 10: main work outside the viewer
};

[[nodiscard]] inline Verdict evaluate_verdict(const WindowStats& s, const VerdictThresholds& t = {}) noexcept {
    Verdict v;
    if (s.idle) return v; // rule 0
    const bool sim_slow = s.delivered_ratio < t.ok_delivered;
    const bool frame_slow = s.frame_p95_ms > s.frame_budget_ms;
    if (!sim_slow && !frame_slow) {
        v.kind = VerdictKind::ok; // rule 1
        v.sim_rule = 1;
        return v;
    }
    const float load = s.interval_ms > 0.0F ? s.tick_p95_ms / s.interval_ms : 0.0F;
    const float w_sim = s.mean_tick_ms > 0.0F ? s.session_lock_ms / s.mean_tick_ms : 0.0F;
    const float w_ui = s.mean_frame_ms > 0.0F ? s.ui_lock_ms / s.mean_frame_ms : 0.0F;
    const bool u_known = s.sim_cpu >= 0.0F;
    if (sim_slow) {
        v.phase = s.top_phase;
        v.phase_ms = s.top_phase_ms;
        if (load >= t.sim_load && u_known && s.sim_cpu >= t.sim_busy) v.sim_rule = 2;
        else if (w_sim >= t.wait_share) { v.sim_rule = 3; v.sim_value = s.session_lock_ms; }
        else if (u_known && s.sim_cpu < t.sim_starved && s.others_cpu >= t.external_cpu) { v.sim_rule = 4; v.sim_value = s.others_cpu; }
        else if (u_known && s.sim_cpu < t.sim_starved) v.sim_rule = 5;
        else if (u_known) v.sim_rule = 6;
        else v.sim_unknown = true; // rules 2, 4 and 5 need the on-CPU share: say so instead of guessing a cause
        v.picture_holds = s.stale_share > t.stale_share;
    }
    if (frame_slow) {
        if (s.gpu_ms >= 0.0F && s.gpu_ms >= t.render_share * s.frame_p95_ms) { v.frame_rule = 7; v.frame_value = s.gpu_ms; }
        else if (s.render_cpu_ms >= 0.0F && s.render_cpu_ms >= t.render_share * s.frame_p95_ms) { v.frame_rule = 8; v.frame_value = s.render_cpu_ms; }
        else if (w_ui >= t.wait_share) { v.frame_rule = 9; v.frame_value = s.ui_lock_ms; }
        else {
            v.frame_rule = 10;
            v.frame_value = s.main_viewer_ms;
            v.engine_ms = s.main_ms > s.main_viewer_ms ? s.main_ms - s.main_viewer_ms : 0.0F;
        }
    }
    if (v.sim_unknown && v.frame_rule == 0) {
        v.kind = VerdictKind::unknown;
    } else if (v.sim_rule != 0 && v.frame_rule != 0) {
        const bool coupled = v.sim_rule == 3 || v.frame_rule == 9 || s.cost_correlation > t.correlation
            || s.hitch_coincidence > t.coincidence;
        v.kind = coupled ? VerdictKind::coupled : VerdictKind::both_independent;
    } else if (v.sim_rule != 0) {
        v.kind = v.sim_rule == 3 || v.sim_rule == 5 ? VerdictKind::coupled
               : v.sim_rule == 4 ? VerdictKind::external : VerdictKind::sim_bound;
    } else {
        v.kind = v.frame_rule == 9 ? VerdictKind::coupled : VerdictKind::render_bound;
    }
    return v;
}

// The one-line text, for example "render-bound: GPU 14.2 ms" or "sim-bound: command-source 31.0 ms".
template <class NameOf>
[[nodiscard]] std::string format_verdict(const Verdict& v, NameOf&& name_of) {
    char text[192];
    const auto sim = [&]() -> std::string {
        switch (v.sim_rule) {
        case 2: {
            const std::string_view name = v.phase == no_phase ? std::string_view{"tick"} : name_of(v.phase);
            std::snprintf(text, sizeof text, "sim-bound: %.*s %.1f ms", static_cast<int>(name.size()), name.data(),
                          static_cast<double>(v.phase_ms));
            return text;
        }
        case 3:
            std::snprintf(text, sizeof text, "coupled: sim waits on frame (session lock %.1f ms)", static_cast<double>(v.sim_value));
            return text;
        case 4:
            std::snprintf(text, sizeof text, "external: other processes %.0f %%", static_cast<double>(v.sim_value * 100.0F));
            return text;
        case 5: return "coupled: sim thread starved of CPU (own threads)";
        case 6: return "sim-bound (unattributed)";
        default: return {};
        }
    };
    const auto frame = [&]() -> std::string {
        switch (v.frame_rule) {
        case 7: std::snprintf(text, sizeof text, "render-bound: GPU %.1f ms", static_cast<double>(v.frame_value)); return text;
        case 8: std::snprintf(text, sizeof text, "render-bound: render thread %.1f ms", static_cast<double>(v.frame_value)); return text;
        case 9: std::snprintf(text, sizeof text, "coupled: frame waits on sim (lock %.1f ms)", static_cast<double>(v.frame_value)); return text;
        case 10:
            std::snprintf(text, sizeof text, "render-bound: main thread (viewer %.1f ms, engine %.1f ms)",
                          static_cast<double>(v.frame_value), static_cast<double>(v.engine_ms));
            return text;
        default: return {};
        }
    };
    std::string line;
    switch (v.kind) {
    case VerdictKind::idle: return "idle";
    case VerdictKind::ok: return "ok";
    case VerdictKind::both_independent: line = "sim-bound + render-bound (independent): " + sim() + "; " + frame(); break;
    case VerdictKind::coupled:
        if (v.sim_rule != 0 && v.frame_rule != 0) line = "coupled: " + sim() + "; " + frame();
        else line = v.sim_rule != 0 ? sim() : frame();
        break;
    case VerdictKind::unknown: line = "unknown: sim slow, on-CPU share not measured"; break;
    default: line = v.sim_rule != 0 ? sim() : frame(); break;
    }
    if (v.sim_unknown && v.kind != VerdictKind::unknown) line += "; sim slow, cause unknown (on-CPU share not measured)";
    if (v.picture_holds) line += " (picture holds)";
    return line;
}

} // namespace eawr::core::perf_diag
