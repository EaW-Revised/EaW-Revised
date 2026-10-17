#pragma once
// Path helpers shared by the launcher: UTF-8 text <-> std::filesystem::path, case-insensitive
// child lookup (the Python launcher's _child) and "same place" comparison.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::launcher {

namespace fs = std::filesystem;

// Invalid UTF-8 gives an empty path rather than an exception.
[[nodiscard]] fs::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const fs::path& path);
[[nodiscard]] std::string lower_ascii(std::string_view text);
[[nodiscard]] std::wstring widen_ascii(std::string_view text);

// directory/name, matched ignoring ASCII case; nullopt when nothing matches.
[[nodiscard]] std::optional<fs::path> find_child_ci(const fs::path& directory, std::string_view name);

// True when both paths name the same place (existing paths by identity, others by normalised text).
[[nodiscard]] bool same_location(const fs::path& left, const fs::path& right);

} // namespace eawr::launcher
