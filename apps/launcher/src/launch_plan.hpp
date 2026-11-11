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
    bool depth_prepass_off = false; // Saved Off only; otherwise project default.
    bool engine_unverified = false;   // --eawr-engine-unverified: the engine SHA-256 is not the packaged one
    bool allow_unknown_build = false; // --eawr-allow-unknown-build: accept a game build the viewer has not verified
    std::optional<bool> safe_render_thread;  // Explicit CLI override: true Safe, false Separate
    std::optional<fs::path> mod_profile; // selected by the manager loop
    std::optional<fs::path> save_dir;    // instance n >= 2: --eawr-save-dir (the primary's saves, shared)
    std::optional<fs::path> digest_cache; // instance n >= 2: shared content digest cache
    std::optional<fs::path> font_cache;  // instance n >= 2: --eawr-font-cache (the primary's fonts, shared)
    std::optional<unsigned> lan_game_port; // instance n >= 2: --eawr-lan-game-port (47624 + 2*(n-1))
    std::optional<std::wstring> render_thread; // profile mode or saved fallback, before Godot user arguments
    std::optional<int> quit_after; // smoke checks: Godot --quit-after N frames
    std::vector<std::wstring> extra; // appended after the viewer arguments (after "--")
};

[[nodiscard]] bool evidence_requested(const std::vector<std::wstring>& extra);

// argv[1..] for the Godot executable.
[[nodiscard]] std::vector<std::wstring> godot_arguments(const LaunchInputs& inputs);

[[nodiscard]] std::vector<std::wstring> mods_arguments(const fs::path& kit, const fs::path& godot,
                                                        const fs::path& project, const fs::path& game_root,
                                                        const fs::path& next_action);

// One CreateProcess command line that CommandLineToArgvW / the MSVC CRT parse back into
// program + args exactly (spaces, quotes and trailing backslashes included).
[[nodiscard]] std::wstring quote_command_line(const std::wstring& program, const std::vector<std::wstring>& args);

} // namespace eawr::launcher
