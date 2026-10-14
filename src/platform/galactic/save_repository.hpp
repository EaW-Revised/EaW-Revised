#pragma once

#include "eawr/platform/live_game_session.hpp"
#include <filesystem>
#include <span>

namespace eawr::platform::save_detail {
[[nodiscard]] core::Result<void> write(const std::string& directory, std::string_view name,
    std::span<const std::uint8_t> bytes, bool overwrite);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> read(const std::string& directory, std::string_view name);
[[nodiscard]] core::Result<std::vector<LiveSaveSlot>> list(const std::string& directory);
[[nodiscard]] core::Result<void> erase(const std::string& directory, std::string_view name);
[[nodiscard]] bool valid_name(std::string_view name) noexcept;
// One slot's repository facts; read and parse failures land in its diagnostic.
[[nodiscard]] LiveSaveSlot describe(const std::string& directory, std::string_view name);
[[nodiscard]] std::filesystem::path slot_file(const std::string& directory, std::string_view name);
} // namespace eawr::platform::save_detail
