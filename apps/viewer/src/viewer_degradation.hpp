#pragma once

// The viewer's side of the degradation ledger (docs/degradation-ledger.md): the summary file beside godot.log and the one
// log line per session. Observation only: nothing here feeds the session, and a failed write never fails it.
#include "eawr/core/degradation_ledger.hpp"
#include "eawr/platform/degradation_summary.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// Beside godot.log: Godot's file log setting, user://logs/godot.log by default.
// Empty when Godot's settings singleton is already gone (the host teardown can run late).
[[nodiscard]] inline std::filesystem::path viewer_degradation_summary_path() {
    auto* settings = godot::ProjectSettings::get_singleton();
    if (settings == nullptr) return {};
    const godot::String log = settings->get_setting("debug/file_logging/log_path", godot::String("user://logs/godot.log"));
    const godot::CharString global = settings->globalize_path(log).utf8();
    return ViewerPath{std::string(global.get_data(), static_cast<std::size_t>(global.length()))}.native().parent_path()
        / std::string(core::degradation::summary_file_name);
}

inline void describe_viewer_degradation_session() {
    core::degradation::SessionInfo info;
    info.mode = "viewer";
    core::degradation::describe_session(info);
}

// Writes degradation-summary.json (atomic, overwrites this session's earlier write) and, when `print_line`, the one log
// line. `ended` replaces the session's end state when given; a failure write passes failed, the host teardown keeps it.
inline void publish_viewer_degradation_summary(const std::optional<core::degradation::Ended> ended = std::nullopt,
                                               const bool print_line = true) {
    if (ended) core::degradation::set_ended(*ended);
    const auto snapshot = core::degradation::snapshot();
    const auto target = viewer_degradation_summary_path();
    if (target.empty()) {
        // No settings singleton, so no folder to write to: say so as a warning instead of staying silent.
        if (print_line)
            godot::UtilityFunctions::printerr(godot::String::utf8(
                (core::degradation::summary_line(snapshot) + " (summary file not written: no log folder known)").c_str()));
        return;
    }
    const auto saved = platform::write_degradation_summary(target, snapshot);
    if (saved.failure) {
        godot::UtilityFunctions::printerr(godot::String::utf8(("degradation summary: " + saved.failure->message).c_str()));
        return;
    }
    if (print_line) godot::UtilityFunctions::print(godot::String::utf8(core::degradation::summary_line(snapshot).c_str()));
}

} // namespace eawr::presentation::godot_backend
