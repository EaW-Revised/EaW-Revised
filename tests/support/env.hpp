#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

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

// The Remake content a test mounts as the game the owner plays it. EAWR_REMAKE_MOD_CHAIN (';'-separated absolute item
// folders, leaf first: the --mod-root order, set by the offload hosts that hold the whole chain) wins; otherwise the single
// item EAWR_REMAKE_MOD_ROOT names. `chain` says which one it is, so a skip can name the layers that were mounted.
struct RemakeMount {
    std::string mod_root;
    bool chain{};
};

inline std::optional<RemakeMount> remake_mount() {
    if (auto chain = environment("EAWR_REMAKE_MOD_CHAIN")) return RemakeMount{std::move(*chain), true};
    if (auto root = environment("EAWR_REMAKE_MOD_ROOT")) return RemakeMount{std::move(*root), false};
    return std::nullopt;
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
