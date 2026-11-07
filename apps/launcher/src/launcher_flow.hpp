#pragma once
// The launcher's steps, free of any window or process API so a fake UI and a fake process runner
// drive them in tests: command line -> game root (saved, Steam, GOG, picker, Browse) -> Godot and
// project -> hidden launch -> readable errors.
#include "install_discovery.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace eawr::launcher {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 2;
inline constexpr int kExitCancelled = 3;
// A Godot exit with a non-zero code inside this window gets an error dialog.
inline constexpr double kEarlyExitSeconds = 60.0;
inline constexpr const char* kGodotExecutableName = "Godot_v4.7.2-stable_win64.exe";
// Logged (never fatal) when the engine's SHA-256 is not the one runtime.json records; the viewer then gets
// --eawr-engine-unverified and shows the warning (or fails a test/gate run).
inline constexpr const char* kEngineUnpatchedCode = "EAWR-ENGINE-UNPATCHED";

struct Options {
    std::optional<fs::path> game_root;     // --game-root, else EAWR_EAW_GAME_ROOT
    std::optional<fs::path> godot;         // --godot, else EAWR_GODOT_EXECUTABLE
    std::optional<fs::path> project;       // --project, else <launcher folder>\project
    fs::path user_data;                    // --user-data: launcher state/logs only; else Godot's user folder
    std::optional<fs::path> viewer_user_data; // Injectable Godot user:// folder; independent of --user-data
    fs::path launcher_dir;                 // the folder holding the launcher exe
    bool allow_unknown_build = false;      // --allow-unknown-build: passed to the viewer as --eawr-allow-unknown-build
    std::optional<bool> safe_render_thread;       // Explicit CLI mode: --render-thread safe|separate; --safe-render-thread aliases Safe
    bool choose_game = false;              // --choose-game (or Shift held): ask even if a choice is saved
    bool no_dialogs = false;               // --no-dialogs: errors go to the log and stderr, nothing is asked (tests, scripts)
    bool mods = false;                     // --mods: open the bundled mod manager
    bool help = false;                     // --help
    std::optional<int> instance;           // --instance <n>: 1 = today's layout, 2.. = own state + APPDATA (#5619)
    bool instance_auto = false;            // --instance auto: win_main claims the lowest free id >= 2 and sets `instance`
    std::optional<std::string> profile;    // --profile <id>: start this mod profile directly, no manager
    std::optional<int> quit_after;         // --godot-quit-after N
    std::vector<std::wstring> extra;       // everything else, and anything after "--", goes to the viewer
    std::string error;                     // a malformed command line (readable)
};

// `args` excludes the program name. Environment overrides are applied by the caller.
[[nodiscard]] Options parse_command_line(const std::vector<std::wstring>& args);

// The Godot candidates that exist next to the launcher, in order: godot\<name>, then <name> in the launcher
// folder. (The parent folder and other Godot_v4.7.2-stable*.exe names are no longer searched.)
[[nodiscard]] std::vector<fs::path> godot_candidates(const fs::path& launcher_dir);
// The first candidate, or nullopt.
[[nodiscard]] std::optional<fs::path> find_godot(const fs::path& launcher_dir);

// Lower-case hex SHA-256 of a file (1 MiB chunks); nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> file_sha256(const fs::path& file);
// "godot_sha256" of <launcher_dir>\runtime.json (lower-case); nullopt when missing, unreadable or malformed.
[[nodiscard]] std::optional<std::string> expected_engine_sha256(const fs::path& launcher_dir);

struct EngineChoice {
    fs::path exe;
    bool verified = false;
    std::string expected;   // runtime.json hash ("" = none)
    std::string actual;     // hash of `exe` ("" = unreadable)
};
// `override_exe` (--godot / env) is hashed and used as is. Otherwise the first candidate whose hash matches
// wins; with none matching, the first existing candidate is returned unverified. nullopt only when there is
// nothing to launch. Never refuses a launch.
[[nodiscard]] std::optional<EngineChoice> choose_engine(const fs::path& launcher_dir, const std::optional<fs::path>& override_exe);

class Ui {
public:
    virtual ~Ui() = default;
    virtual void show_error(const std::string& title, const std::string& message_utf8) = 0;
    struct Pick {
        enum class Kind { install, browse, cancel } kind = Kind::cancel;
        std::size_t index = 0;
    };
    // `installs` may be empty (nothing found); the player may Browse or cancel. `heading` explains why.
    virtual Pick pick_install(const std::vector<Install>& installs, const std::string& heading_utf8) = 0;
    // A folder picker; nullopt when cancelled.
    virtual std::optional<fs::path> browse_for_folder(const std::string& prompt_utf8) = 0;
};

class Log {
public:
    Log() = default;
    explicit Log(fs::path file);   // appended to; trimmed first when it has grown past 1 MiB
    void line(const std::string& text);
    [[nodiscard]] const fs::path& file() const { return file_; }
private:
    fs::path file_;
};

struct ChildResult {
    bool started = false;
    std::string error;        // why it did not start (readable)
    unsigned exit_code = 0;
    double seconds = 0;       // time the child ran
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    // From now on every started child gets APPDATA=<folder> (all other variables unchanged); nullopt = inherit.
    // Instance n >= 2 sets it around the game start so the game's user:// is its own. Default: ignored.
    virtual void set_appdata(const std::optional<fs::path>&) {}
    // Starts exe with a hidden console (never a visible one) and waits for it to exit.
    virtual ChildResult run(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& working_directory) = 0;
    virtual ChildResult run_monitored(const fs::path& exe, const std::vector<std::wstring>& args,
                                      const fs::path& working_directory, const std::function<void()>& poll,
                                      const std::function<bool(unsigned)>& started) = 0;
};

// Returns the process exit code: kExitOk, kExitError, kExitCancelled or Godot's own code.
[[nodiscard]] int run_launcher(const Options& options, const RegistryReader& registry, Ui& ui, ProcessRunner& runner, Log& log);

} // namespace eawr::launcher
