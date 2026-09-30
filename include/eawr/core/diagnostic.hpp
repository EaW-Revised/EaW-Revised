#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::core {

enum class Severity : std::uint8_t {
    info,
    warning,
    error,
};

[[nodiscard]] constexpr std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    }
    return "error";
}

struct Diagnostic {
    std::string code;
    Severity severity{Severity::error};
    std::string message;
    std::optional<std::string> logical_path;
    std::optional<std::uint64_t> line;
    std::optional<std::uint64_t> column;
    std::optional<std::string> source_id;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_argument = "EAWR-CORE-0001";
inline constexpr std::string_view replay_unavailable = "EAWR-SIM-0001";
} // namespace diagnostic_codes

[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);

} // namespace eawr::core
