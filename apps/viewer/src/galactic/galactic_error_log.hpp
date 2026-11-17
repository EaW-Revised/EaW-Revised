#pragma once

#include "eawr/core/degradation_ledger.hpp"
#include "eawr/platform/live_game_session.hpp"
#include <godot_cpp/variant/utility_functions.hpp>
#include <utility>

namespace eawr::presentation::godot_backend {
inline void configure_galactic_error_logging(platform::LiveGameSession::Options& options) {
    // Logging only, after the deterministic commit. stderr is not the Godot
    // log sink; both new and restored campaigns install this observer.
    auto previous = std::move(options.tick_diagnostics);
    options.tick_diagnostics = [previous = std::move(previous)](std::span<const core::Diagnostic> rows) {
        if (previous) previous(rows);
        for (const auto& row : rows) {
            if (!core::degradation::is_script_fault_code(row.code) && !row.code.starts_with("EAWR-GCAI-")) continue;
            const auto text = godot::String::utf8(core::format_diagnostic(row).c_str());
            if (row.severity == core::Severity::error) godot::UtilityFunctions::push_error(text);
            else godot::UtilityFunctions::push_warning(text);
        }
    };
}
} // namespace eawr::presentation::godot_backend
