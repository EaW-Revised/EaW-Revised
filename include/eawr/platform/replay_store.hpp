#pragma once

#include "eawr/platform/ui_files.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace eawr::presentation::ui {
struct LoadGameEntry;
enum class DeleteResult : std::uint8_t;
}

namespace eawr::platform {
// Shared name rule for the model display, directory scan and deletion boundary.
[[nodiscard]] std::string replay_name_key(std::string_view name);
[[nodiscard]] bool plain_replay_name(std::string_view name);
// MNU-41: the edit contains a display name, without the replay suffix.
[[nodiscard]] std::string replay_name_problem(std::string_view name);
enum class ReplayWriteResult : std::uint8_t { saved, exists, invalid_name, failed };
[[nodiscard]] ReplayWriteResult write_replay_store(const Path& store, std::string_view name,
    std::span<const std::uint8_t> bytes, bool overwrite, std::string& error);
// Multiplayer session companion (<name><replay suffix>.session) written after the recording is saved.
[[nodiscard]] bool write_replay_session_companion(const Path& store, std::string_view name,
    std::span<const std::uint8_t> bytes);

// Personal recordings stay on the native filesystem, separate from game-asset VFS mounts.
[[nodiscard]] std::vector<presentation::ui::LoadGameEntry> scan_replay_store(const Path& store);
[[nodiscard]] presentation::ui::DeleteResult delete_replay(const Path& store, std::string_view file_name);
} // namespace eawr::platform
