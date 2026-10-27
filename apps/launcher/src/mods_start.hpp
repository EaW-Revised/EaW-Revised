#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace eawr::launcher {

enum class ModsStartMode { launch, create_shortcut, invalid };

inline ModsStartMode mods_start_mode(std::span<const std::wstring_view> arguments) {
    if (arguments.empty()) return ModsStartMode::launch;
    if (arguments.size() == 1 && arguments.front() == L"--create-shortcut") return ModsStartMode::create_shortcut;
    return ModsStartMode::invalid;
}

inline std::filesystem::path mods_launcher_path(const std::filesystem::path& own_executable) {
    return own_executable.parent_path() / L"EAWR.exe";
}

} // namespace eawr::launcher
