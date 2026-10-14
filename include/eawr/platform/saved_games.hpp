#pragma once

// A session-free view of a save directory (#2942): the same repository the live session lists, deletes and
// loads through (src/platform/galactic/save_repository.cpp), for a front end that has no running game.
// Presentation uses it to list and delete saved games before a campaign exists; restoring one is still
// LiveGameSession::prepare_load.
//
// Each call takes the directory the live session was given (LiveSaveOptions::directory), as a path: the
// repository's string-typed directory is read through the ANSI code page on Windows, so a profile path
// outside it would not round-trip. The list is ordered by stored name, reads every file's header and reports
// an unreadable file as a slot with its diagnostic, exactly as LiveGameSession::save_slots does.

#include "eawr/platform/live_game_session.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::platform {

// A missing directory lists no saves; a directory that cannot be read, or more than 1024 saves, fails.
[[nodiscard]] core::Result<std::vector<LiveSaveSlot>> list_saved_games(const std::filesystem::path& directory);
// Removes one save by its stored name. Refuses an invalid name and a symbolic link.
[[nodiscard]] core::Result<void> delete_saved_game(const std::filesystem::path& directory, std::string_view name);
// The save name rule: 1 to 64 of letters, digits, space, '-' and '_', with no space at either end.
[[nodiscard]] bool valid_saved_game_name(std::string_view name) noexcept;

} // namespace eawr::platform
