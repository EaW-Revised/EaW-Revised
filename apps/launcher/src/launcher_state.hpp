#pragma once
// What the launcher remembers between runs, in the launcher subfolder of the Godot user-data folder the
// viewer also uses.
#include "paths.hpp"

#include <optional>
#include <string>

namespace eawr::launcher {

// config/name of apps/viewer/project/project.godot; Godot's per-project user folder is named after it.
inline constexpr const char* kViewerProjectName = "EAWR Viewer";

// %APPDATA%\Godot\app_userdata\EAWR Viewer; on Linux, $XDG_DATA_HOME/godot/app_userdata/EAWR Viewer
// (else ~/.local/share/godot/app_userdata/EAWR Viewer). Empty if no data folder can be found.
// Godot 4 uses this folder for user:// unless a project sets a custom user dir; ours does not.
[[nodiscard]] fs::path default_user_data_dir();

struct LauncherState {
    std::optional<fs::path> game_root;
    std::optional<std::string> last_profile;
};

[[nodiscard]] fs::path state_file(const fs::path& user_data);
// Never throws: a missing, unreadable or malformed file is an empty state.
[[nodiscard]] LauncherState load_state(const fs::path& user_data);
// Writes through a temporary file; false (with `error`) when the folder is not writable.
[[nodiscard]] bool save_state(const fs::path& user_data, const LauncherState& state, std::string& error);

} // namespace eawr::launcher
