#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace eawr::testing {

enum class EmptyEnvironment { missing, present };

inline std::optional<std::string> environment(const char* name,
    EmptyEnvironment empty = EmptyEnvironment::missing) {
#if defined(_MSC_VER)
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) || !value) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const auto* value = std::getenv(name);
    if (!value) return std::nullopt;
    std::string result(value);
#endif
    if (empty == EmptyEnvironment::missing && result.empty()) return std::nullopt;
    return result;
}

inline std::optional<std::string> game_root(EmptyEnvironment empty = EmptyEnvironment::missing) {
    return environment("EAWR_EAW_GAME_ROOT", empty);
}

// The caller supplies the complete existing diagnostic, including its newline,
// and keeps the return code or CTest skip-regex policy of its test case.
// The returned code serves int-returning callers. Void callers may discard it
// before taking their original return path; both forms write the same message.
inline int skip_installed(std::string_view message, int exit_code, std::ostream& output) {
    output << message;
    return exit_code;
}

} // namespace eawr::testing
