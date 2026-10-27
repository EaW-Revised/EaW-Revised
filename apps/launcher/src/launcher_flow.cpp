#include "launcher_flow.hpp"

#include "launch_plan.hpp"
#include "launcher_state.hpp"
#include "text_formats.hpp"
#include "../../../include/eawr/presentation/ui/render_thread_setting.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace eawr::launcher {

namespace {

constexpr const char* kTitle = "Empire at War Revised";

std::string timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer;
}

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

struct Context {
    const Options& options;
    Ui& ui;
    Log& log;

    int fail(const std::string& message, const std::string& extra_log = {}) const {
        log.line("error: " + message);
        std::string text = message;
        if (!log.file().empty()) text += "\n\nDetails are in " + path_to_utf8(log.file());
        if (!extra_log.empty()) text += "\nGame log: " + extra_log;
        ui.show_error(kTitle, text);
        return kExitError;
    }
};

struct Resolution {
    std::optional<fs::path> root;
    int exit_code = kExitOk;
    bool remember = false;
};

Resolution resolve_game_root(const Context& context, const RegistryReader& registry) {
    const Options& options = context.options;
    Resolution resolution;

    if (options.game_root) {
        const Validation validation = validate_install(*options.game_root);
        if (!validation.ok) {
            resolution.exit_code = context.fail(validation.error);
            return resolution;
        }
        resolution.root = validation.root;   // a one-off override: never replaces the player's saved choice
        return resolution;
    }

    std::vector<Install> installs;
    const LauncherState state = load_state(options.user_data);
    bool saved_valid = false;
    if (state.game_root) {
        const Validation validation = validate_install(*state.game_root);
        if (validation.ok) {
            saved_valid = true;
            if (!options.choose_game) {
                context.log.line("using the saved game folder " + path_to_utf8(validation.root));
                resolution.root = validation.root;
                return resolution;
            }
            add_unique(installs, {validation.root, "saved choice"});
        } else {
            context.log.line("the saved game folder is no longer valid: " + validation.error);
        }
    }
    for (Install& install : discover_installs(registry)) add_unique(installs, std::move(install));
    context.log.line("found " + std::to_string(installs.size()) + " installation(s)" + (saved_valid ? " (including the saved one)" : ""));
    for (const Install& install : installs) context.log.line("  " + install.source + ": " + path_to_utf8(install.root));

    std::string heading;
    if (installs.size() == 1 && !options.choose_game) {
        resolution.root = installs.front().root;
        resolution.remember = true;
        return resolution;
    }
    if (installs.empty()) {
        heading = "EAWR could not find Star Wars: Empire at War \xE2\x80\x93 Forces of Corruption on this PC. Browse to the folder that "
                  "contains GameData and corruption.";
    } else if (installs.size() == 1) {
        heading = "Which Forces of Corruption folder should EAWR use?";
    } else {
        heading = "More than one Forces of Corruption installation was found. Which one should EAWR use?";
    }

    for (;;) {
        const Ui::Pick pick = context.ui.pick_install(installs, heading);
        if (pick.kind == Ui::Pick::Kind::cancel) {
            context.log.line("cancelled while choosing the game folder");
            resolution.exit_code = kExitCancelled;
            return resolution;
        }
        if (pick.kind == Ui::Pick::Kind::install) {
            if (pick.index >= installs.size()) continue;
            resolution.root = installs[pick.index].root;
            resolution.remember = true;
            return resolution;
        }
        const std::optional<fs::path> browsed = context.ui.browse_for_folder(
            "Select your Forces of Corruption folder (the one containing GameData and corruption)");
        if (!browsed) {
            if (installs.empty()) {
                context.log.line("cancelled while browsing for the game folder");
                resolution.exit_code = kExitCancelled;
                return resolution;
            }
            continue;
        }
        const Validation validation = validate_install(*browsed);
        if (validation.ok) {
            resolution.root = validation.root;
            resolution.remember = true;
            return resolution;
        }
        context.log.line("rejected " + path_to_utf8(*browsed) + ": " + validation.error);
        context.ui.show_error(kTitle, validation.error);
    }
}

// IPC files are deliberately small; bound the read before allocating/parsing.
std::optional<JsonValue> read_loop_json(const fs::path& path, std::size_t limit = 64 * 1024) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string text(limit + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto size = stream.gcount();
    if (static_cast<std::size_t>(size) > limit || stream.bad()) return {};
    text.resize(static_cast<std::size_t>(size));
    auto json = parse_json(text);
    if (!json || json->kind != JsonValue::Kind::object) return {};
    return json;
}

bool portable_profile_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    const auto lower = lower_ascii(id);
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") return false;
    return !(lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt"))
             && lower[3] >= '0' && lower[3] <= '9');
}

std::string failure_from_report(const fs::path& report) {
    std::ifstream stream(report, std::ios::binary);
    if (!stream) return {};
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::optional<JsonValue> json = parse_json(text);
    if (!json) return {};
    const std::string* status = json->string_member("status");
    const std::string* failure = json->string_member("failure");
    return status != nullptr && *status == "failed" && failure != nullptr ? *failure : std::string();
}

} // namespace

Options parse_command_line(const std::vector<std::wstring>& args) {
    Options options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::wstring& argument = args[index];
        const auto value = [&](const char* name) -> const std::wstring* {
            if (index + 1 >= args.size()) {
                options.error = std::string("Option ") + name + " needs a value.";
                return nullptr;
            }
            return &args[++index];
        };
        if (argument == L"--") {
            options.extra.insert(options.extra.end(), args.begin() + static_cast<std::ptrdiff_t>(index) + 1, args.end());
            break;
        }
        if (argument == L"--game-root") {
            if (const std::wstring* text = value("--game-root")) options.game_root = fs::path(*text);
        } else if (argument == L"--godot") {
            if (const std::wstring* text = value("--godot")) options.godot = fs::path(*text);
        } else if (argument == L"--project") {
            if (const std::wstring* text = value("--project")) options.project = fs::path(*text);
        } else if (argument == L"--user-data") {
            if (const std::wstring* text = value("--user-data")) options.user_data = fs::path(*text);
        } else if (argument == L"--godot-quit-after") {
            if (const std::wstring* text = value("--godot-quit-after")) {
                try {
                    std::size_t used = 0;
                    const int frames = std::stoi(*text, &used);
                    if (used != text->size() || frames < 0) throw std::invalid_argument("frames");
                    options.quit_after = frames;
                } catch (const std::exception&) {
                    options.error = "--godot-quit-after needs a whole number of frames.";
                }
            }
        } else if (argument == L"--allow-unknown-build") {
            options.allow_unknown_build = true;
        } else if (argument == L"--render-thread") {
            if (index + 1 >= args.size() || (args[index + 1] != L"safe" && args[index + 1] != L"separate")) {
                options.error = "--render-thread needs safe or separate.";
            } else {
                options.safe_render_thread = args[++index] == L"safe";
            }
        } else if (argument == L"--safe-render-thread") {
            options.safe_render_thread = true;
        } else if (argument == L"--mods") {
            options.mods = true;
        } else if (argument == L"--choose-game") {
            options.choose_game = true;
        } else if (argument == L"--no-dialogs") {
            options.no_dialogs = true;
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            options.help = true;
        } else {
            options.extra.push_back(argument);
        }
        if (!options.error.empty()) break;
    }
    return options;
}

std::optional<fs::path> find_godot(const fs::path& launcher_dir) {
    const fs::path name = path_from_utf8(kGodotExecutableName);
    for (const fs::path& folder : {launcher_dir / "godot", launcher_dir, launcher_dir.parent_path()}) {
        if (folder.empty()) continue;
        if (is_file(folder / name)) return folder / name;
    }
    for (const fs::path& folder : {launcher_dir / "godot", launcher_dir}) {
        std::error_code error;
        std::vector<fs::path> found;
        for (fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const std::string file = lower_ascii(path_to_utf8(iterator->path().filename()));
            if (file.rfind("godot_v4.7.2-stable", 0) == 0 && file.ends_with(".exe") && file.find("console") == std::string::npos
                && is_file(iterator->path())) {
                found.push_back(iterator->path());
            }
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.front();
    }
    return std::nullopt;
}

Log::Log(fs::path file) : file_(std::move(file)) {
    std::error_code error;
    fs::create_directories(file_.parent_path(), error);
    const std::uintmax_t size = fs::file_size(file_, error);
    if (!error && size > (1u << 20)) fs::remove(file_, error);
}

void Log::line(const std::string& text) {
    if (file_.empty()) return;
    std::ofstream stream(file_, std::ios::binary | std::ios::app);
    if (stream) stream << timestamp() << ' ' << text << '\n';
}

int run_launcher(const Options& options, const RegistryReader& registry, Ui& ui, ProcessRunner& runner, Log& log) {
    const Context context{options, ui, log};
    log.line("launcher started; user data " + path_to_utf8(options.user_data));
    if (!options.error.empty()) return context.fail(options.error);
    if (options.user_data.empty()) {
        return context.fail("EAWR could not find the user data folder (%APPDATA%). Pass --user-data <folder>.");
    }
    std::error_code error;
    fs::create_directories(options.user_data / "launcher", error);
    if (error) {
        return context.fail("EAWR cannot write to its user data folder " + path_to_utf8(options.user_data) + ": " + error.message());
    }

    const Resolution resolution = resolve_game_root(context, registry);
    if (!resolution.root) return resolution.exit_code;
    const fs::path game_root = *resolution.root;
    log.line("game folder " + path_to_utf8(game_root));
    if (resolution.remember) {
        LauncherState state = load_state(options.user_data);
        state.game_root = game_root;
        std::string save_error;
        if (!save_state(options.user_data, state, save_error)) log.line("could not remember the game folder: " + save_error);
    }

    fs::path godot;
    if (options.godot) {
        godot = *options.godot;
        if (!is_file(godot)) {
            return context.fail("The Godot runtime " + path_to_utf8(godot) + " does not exist.");
        }
    } else if (const std::optional<fs::path> found = find_godot(options.launcher_dir)) {
        godot = *found;
    } else {
        return context.fail(std::string("The Godot runtime (") + kGodotExecutableName + ") is missing next to the launcher in "
                            + path_to_utf8(options.launcher_dir) + ". Extract the whole EAWR download into one folder and try again.");
    }
    const fs::path project = options.project ? *options.project : options.launcher_dir / "project";
    if (!is_file(project / "project.godot")) {
        return context.fail("The game project folder " + path_to_utf8(project) + " is missing or incomplete. Extract the whole EAWR "
                            "download into one folder and try again.");
    }

    LaunchInputs inputs;
    inputs.project = project;
    inputs.game_root = game_root;
    inputs.allow_unknown_build = options.allow_unknown_build;
    inputs.report = options.user_data / "launcher" / "front-end-report.json";
    inputs.safe_render_thread = options.safe_render_thread;
    if (!inputs.safe_render_thread.has_value()) {
        const fs::path viewer_data = options.viewer_user_data.value_or(default_user_data_dir());
        const fs::path default_settings = viewer_data.empty() ? fs::path{} : viewer_data / "eawr-settings.cfg";
        fs::path settings_file = default_settings;
        for (std::size_t i = 0; i + 1 < options.extra.size(); ++i)
            if (options.extra[i] == L"--eawr-settings-file") {
                const fs::path forwarded = options.extra[++i];
                settings_file = forwarded.empty() ? default_settings : forwarded;
            }
        // Godot starts in the launcher folder; relative overrides must name the
        // same file here even when EAWR.exe was called from another directory.
        if (!settings_file.empty() && settings_file.is_relative()) settings_file = options.launcher_dir / settings_file;
        std::ifstream stream(settings_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (const auto saved = presentation::ui::stored_render_thread(text))
            inputs.render_thread = *saved == presentation::ui::RenderThreadSetting::safe ? L"safe" : L"separate";
    }
    inputs.quit_after = options.quit_after;
    inputs.extra = options.extra;
    const fs::path next_action = options.user_data / "launcher" / "next-action.json";
    bool manager = options.mods;
    // Bound immediate restart requests, but reset after a real game session.
    unsigned rapid_restarts = 0;
    for (;;) {
        if (manager && (!is_file(options.launcher_dir / "launcher" / "project.godot")
                       || !is_file(options.launcher_dir / "eawr_mods_service.exe"))) {
            return context.fail("EAWR Mods is missing or incomplete. Extract the whole EAWR download and try again.");
        }
        error.clear();
        fs::remove(next_action, error);
        if (error) return context.fail("EAWR cannot clear the launch request: " + error.message());
        fs::remove(inputs.report, error);   // never blame a new failure on an old report
        if (!manager) {
            if (inputs.safe_render_thread.has_value()) {
                log.line(std::string("render thread: ") + (*inputs.safe_render_thread ? "safe" : "separate") + " (command line)");
            } else if (inputs.render_thread) {
                log.line(std::string("render thread: ") + (*inputs.render_thread == L"safe" ? "safe" : "separate")
                         + (inputs.mod_profile ? " (mod profile)" : " (saved setting)"));
            } else {
                log.line("render thread: project default");
            }
        }
        const std::vector<std::wstring> args = manager
            ? mods_arguments(options.launcher_dir, godot, project, game_root, next_action) : godot_arguments(inputs);
        log.line("starting " + path_to_utf8(godot));

        const std::string godot_log = path_to_utf8(options.user_data / "logs" / "godot.log");
        const ChildResult child = runner.run(godot, args, options.launcher_dir);
        if (!child.started) {
            if (manager) return context.fail("EAWR could not start the mod manager: " + child.error);
            return context.fail("EAWR could not start the Godot runtime " + path_to_utf8(godot) + ": " + child.error
                                + ". Security software or Windows application control may have blocked it.", godot_log);
        }
        // Consume before interpreting the exit status, including requests left by a failed child.
        const auto action = read_loop_json(next_action);
        error.clear();
        fs::remove(next_action, error);
        if (error) return context.fail("EAWR cannot consume the launch request: " + error.message());
        log.line(std::string(manager ? "the mod manager" : "the game") + " exited with code "
                 + std::to_string(child.exit_code) + " after " + std::to_string(child.seconds) + " s");
        if (manager && child.exit_code != 0) {
            if (child.seconds < kEarlyExitSeconds) {
                context.fail("The mod manager stopped right after starting (exit code " + std::to_string(child.exit_code) + ").");
            }
            return static_cast<int>(child.exit_code);
        }
        if (child.exit_code != 0) {
            // The viewer's report names a startup failure at any time (first-run provisioning can take minutes). Without one, only an
            // exit right after starting is shown; a crash after a long session has nothing to explain and stays in the log.
            std::string reason = failure_from_report(inputs.report);
            if (reason.empty() && child.seconds < kEarlyExitSeconds) {
                reason = "The game stopped right after starting (exit code " + std::to_string(child.exit_code) + ").";
            }
            if (!reason.empty()) context.fail(reason, godot_log);
        }
        if (child.exit_code != 0 || !action) return static_cast<int>(child.exit_code);
        const auto* kind = action->string_member("action");
        if (!kind) return kExitOk;
        if (!manager && child.seconds >= kEarlyExitSeconds) rapid_restarts = 0;
        if (*kind == "open-manager") {
            manager = true;
        } else if (*kind == "launch-profile") {
            const auto* id = action->string_member("profile");
            if (!id || !portable_profile_id(*id)) return kExitOk;
            const fs::path profile = options.user_data / "mods" / "profiles" / path_from_utf8(*id + ".json");
            // Match the mod core's 4 MiB profile-file bound.
            const auto json = read_loop_json(profile, 4u * 1024u * 1024u);
            if (!json) return kExitOk;
            const auto* format = json->string_member("format");
            const auto* version = json->member("version");
            const auto* profile_id = json->string_member("id");
            const auto* launch = json->member("launch");
            if (!format || *format != "eawr-mod-profile" || !version || version->kind != JsonValue::Kind::number
                || version->number != 1 || !profile_id || *profile_id != *id
                || !launch || launch->kind != JsonValue::Kind::object) return kExitOk;
            const auto* render = launch->string_member("render_thread");
            if (!render || (*render != "safe" && *render != "separate")) return kExitOk;
            inputs.mod_profile = profile;
            inputs.render_thread = widen_ascii(*render);
            LauncherState state = load_state(options.user_data);
            state.last_profile = *id;
            std::string save_error;
            if (!save_state(options.user_data, state, save_error)) log.line("could not remember the profile: " + save_error);
            manager = false;
        } else {
            return kExitOk;
        }
        if (++rapid_restarts > 16) return context.fail("EAWR stopped repeated launch requests. Start EAWR Mods again to continue.");
    }
}

} // namespace eawr::launcher
