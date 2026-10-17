#pragma once
// The Godot command line the launcher starts: the front end (main menu) with the player's game root.
// The viewer extracts the fonts itself at startup (no --eawr-font-cache) and keeps its settings in
// user data (no --eawr-settings-file). Layout contract with the package: EAWR.exe at the root,
// godot\Godot_v4.7.2-stable_win64.exe and project\ beside it.
#include "paths.hpp"

#include <optional>
#include <string>
#include <vector>

namespace eawr::launcher {

struct LaunchInputs {
    fs::path project;              // the Godot project folder (contains project.godot)
    fs::path game_root;
    fs::path report;               // --eawr-report: the viewer writes {"status":"failed","failure":...} here
    bool allow_unknown_build = false; // --eawr-allow-unknown-build: accept a game build the viewer has not verified
    std::optional<int> quit_after; // smoke checks: Godot --quit-after N frames
    std::vector<std::wstring> extra; // appended after the viewer arguments (after "--")
};

// argv[1..] for the Godot executable.
[[nodiscard]] std::vector<std::wstring> godot_arguments(const LaunchInputs& inputs);

// One CreateProcess command line that CommandLineToArgvW / the MSVC CRT parse back into
// program + args exactly (spaces, quotes and trailing backslashes included).
[[nodiscard]] std::wstring quote_command_line(const std::wstring& program, const std::vector<std::wstring>& args);

} // namespace eawr::launcher
