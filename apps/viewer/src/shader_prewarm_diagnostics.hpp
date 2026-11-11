#pragma once
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/json_text.hpp"
#include <string>

namespace eawr::presentation::godot_backend {
// Only renderer-owned bounded sentences enter this loading-time report. Never
// shader text, backend compiler output or an unbounded per-effect log.
inline constexpr std::size_t prewarm_diagnostic_bytes = 16384U;
inline void append_prewarm_refusal(std::string& output, const core::Diagnostic& error) {
    const auto line = "prewarm_refusal code=" + core::json_quote(error.code.substr(0U, 96U))
        + " reason=" + core::json_quote(error.message.substr(0U, 768U)) + "\n";
    if (output.size() <= prewarm_diagnostic_bytes
        && line.size() <= prewarm_diagnostic_bytes - output.size()) output += line;
}
} // namespace eawr::presentation::godot_backend
