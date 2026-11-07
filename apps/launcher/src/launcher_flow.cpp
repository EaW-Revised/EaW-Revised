#include "launcher_flow.hpp"

#include "launch_plan.hpp"
#include "instance.hpp"
#include "launcher_state.hpp"
#include "text_formats.hpp"
#include "../../../include/eawr/core/sha256.hpp"
#include "../../../include/eawr/presentation/ui/render_thread_setting.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <limits>
#include <span>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

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

void clear_dead_running_state(const fs::path& path, Log& log) {
    const auto json = read_loop_json(path);
    if (!json) return;
    if (!json->string_member("profile")) return;
    const auto* start_value = json->member("process_start_time");
    if (start_value && (start_value->kind != JsonValue::Kind::string || start_value->string.empty()
        || !std::all_of(start_value->string.begin(), start_value->string.end(),
                        [](char c) { return c >= '0' && c <= '9'; }))) return;
    const auto* pid_value = json->member("pid");
    if (!pid_value || pid_value->kind != JsonValue::Kind::number || !std::isfinite(pid_value->number)
        || pid_value->number < 1 || pid_value->number > (std::numeric_limits<unsigned>::max)()
        || std::floor(pid_value->number) != pid_value->number) return;
    const auto pid = static_cast<unsigned>(pid_value->number);
    const auto* saved_start = json->string_member("process_start_time");
    if (process_gone(pid, saved_start ? std::optional<std::string>(*saved_start) : std::nullopt)) {
        std::error_code error;
        if (fs::remove(path, error) && !error) log.line("removed stale game-running state");
    }
}

// The manager may read these files concurrently, so publish a complete replacement.
bool write_loop_file(const fs::path& path, const std::string& contents) {
    fs::path temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << contents;
    stream.close();
    if (!stream) return false;
#ifdef _WIN32
    const bool replaced = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    const bool replaced = !error;
#endif
    if (!replaced) {
        std::error_code error;
        fs::remove(temporary, error);
    }
    return replaced;
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
        } else if (argument == L"--instance") {
            if (const std::wstring* text = value("--instance")) {
                if (*text == L"auto") {
                    options.instance_auto = true;
                } else if (const auto id = parse_instance_id(*text)) {
                    options.instance = *id;
                } else {
                    options.error = "--instance needs a whole number from 1 to " + std::to_string(kMaxInstanceId) + ", or auto.";
                }
            }
        } else if (argument == L"--profile") {
            if (const std::wstring* text = value("--profile")) options.profile = path_to_utf8(fs::path(*text));
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

std::vector<fs::path> godot_candidates(const fs::path& launcher_dir) {
    std::vector<fs::path> found;
    const fs::path name = path_from_utf8(kGodotExecutableName);
    for (const fs::path& folder : {launcher_dir / "godot", launcher_dir}) {
        if (!folder.empty() && is_file(folder / name)) found.push_back(folder / name);
    }
    return found;
}

std::optional<fs::path> find_godot(const fs::path& launcher_dir) {
    const auto found = godot_candidates(launcher_dir);
    if (found.empty()) return std::nullopt;
    return found.front();
}

std::optional<std::string> file_sha256(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    eawr::core::Sha256Hasher hasher;
    std::vector<std::uint8_t> chunk(1u << 20);
    while (stream) {
        stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(stream.gcount());
        if (got > 0) hasher.update(std::span<const std::uint8_t>(chunk.data(), got));
    }
    if (stream.bad()) return std::nullopt;
    return hasher.hex_digest();
}

std::optional<std::string> expected_engine_sha256(const fs::path& launcher_dir) {
    const auto json = read_loop_json(launcher_dir / "runtime.json", 1u << 20);
    if (!json) return std::nullopt;
    const std::string* hash = json->string_member("godot_sha256");
    if (!hash) return std::nullopt;
    std::string lower = lower_ascii(*hash);
    if (lower.size() != 64 || !std::all_of(lower.begin(), lower.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return std::nullopt;
    return lower;
}

std::optional<EngineChoice> choose_engine(const fs::path& launcher_dir, const std::optional<fs::path>& override_exe) {
    const std::string expected = expected_engine_sha256(launcher_dir).value_or(std::string());
    const auto check = [&](const fs::path& exe) {
        EngineChoice choice;
        choice.exe = exe;
        choice.expected = expected;
        choice.actual = file_sha256(exe).value_or(std::string());
        choice.verified = !expected.empty() && choice.actual == expected;
        return choice;
    };
    if (override_exe) return check(*override_exe);
    std::optional<EngineChoice> first;
    for (const fs::path& candidate : godot_candidates(launcher_dir)) {
        EngineChoice choice = check(candidate);
        if (choice.verified) return choice;
        if (!first) first = std::move(choice);
    }
    return first;
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
    const int instance = options.instance.value_or(1);
    const bool multi = instance >= 2;
    // Releases this process's claim on every way out (also one `--instance auto` took before we started); it only
    // ever removes a claim that names this very process.
    const ProcessIdentity self = current_process_identity();
    struct ClaimGuard {
        const fs::path& user_data;
        int instance;
        const ProcessIdentity& self;
        bool active;
        ~ClaimGuard() { if (active) release_claim(user_data, instance, self); }
    } claim_guard{options.user_data, instance, self, multi};
    // Every request/state/report file of this run lives in this instance's own folder: a second EAWR.exe
    // never clears another instance's files (instance 1 keeps <user-data>/launcher as before).
    const fs::path state_dir = instance_launcher_dir(options.user_data, instance);
    std::error_code error;
    fs::create_directories(state_dir, error);
    if (error) {
        return context.fail("EAWR cannot write to its user data folder " + path_to_utf8(options.user_data) + ": " + error.message());
    }
    if (options.profile && options.mods) return context.fail("--profile cannot be combined with --mods.");

    const fs::path primary_viewer = options.viewer_user_data.value_or(default_user_data_dir());
    if (multi) {
        const ClaimResult claim = claim_instance(options.user_data, instance, self);
        if (!claim.ok()) {
            if (claim.owner_pid == 0) {
                return context.fail("Instance " + std::to_string(instance) + " is being started by another launcher right now. Try again in a moment.");
            }
            return context.fail("Instance " + std::to_string(instance) + " is already running (process " + std::to_string(claim.owner_pid)
                                + "). Start another instance with --instance auto.");
        }
        if (claim.status == ClaimStatus::claimed_after_stale) log.line("instance " + std::to_string(instance) + ": reused a stale slot claim");
        log.line("instance " + std::to_string(instance) + " of the game; its files are in " + path_to_utf8(instance_root(options.user_data, instance)));
        std::string seed_error;
        if (primary_viewer.empty()) {
            return context.fail("EAWR could not find the first instance's user folder to copy the settings from.");
        }
        if (!seed_instance_files(primary_viewer, instance_viewer_dir(options.user_data, instance), instance, seed_error)) {
            return context.fail("EAWR cannot prepare instance " + std::to_string(instance) + ": " + seed_error);
        }
    }

    clear_dead_running_state(state_dir / "game-running.json", log);

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

    if (options.godot && !is_file(*options.godot)) {
        return context.fail("The Godot runtime " + path_to_utf8(*options.godot) + " does not exist.");
    }
    const auto engine_start = std::chrono::steady_clock::now();
    const auto chosen = choose_engine(options.launcher_dir, options.godot);
    if (!chosen) {
        return context.fail(std::string("The Godot runtime (") + kGodotExecutableName + ") is missing next to the launcher in "
                            + path_to_utf8(options.launcher_dir) + ". Extract the whole EAWR download into one folder and try again.");
    }
    const fs::path godot = chosen->exe;
    log.line(std::string("engine check: ") + (chosen->verified ? "SHA-256 matches runtime.json" : "not verified") + ", took "
             + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - engine_start).count())
             + " ms");
    // The engine must be our patched build. A mismatch is flagged to the viewer and logged, never a reason to refuse play.
    const bool engine_unverified = !chosen->verified;
    if (engine_unverified) {
        log.line(std::string(kEngineUnpatchedCode) + ": " + path_to_utf8(godot) + " has SHA-256 "
                 + (chosen->actual.empty() ? "(unreadable)" : chosen->actual) + ", runtime.json records "
                 + (chosen->expected.empty() ? "(none: missing or unreadable)" : chosen->expected) + "; starting anyway, flagged");
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
    inputs.engine_unverified = engine_unverified;
    inputs.report = state_dir / "front-end-report.json";
    inputs.safe_render_thread = options.safe_render_thread;
    fs::path settings_file;
    {
        const fs::path viewer_data = multi ? instance_viewer_dir(options.user_data, instance) : primary_viewer;
        const fs::path default_settings = viewer_data.empty() ? fs::path{} : viewer_data / "eawr-settings.cfg";
        settings_file = default_settings;
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
        if (!inputs.safe_render_thread.has_value()) {
            if (const auto saved = presentation::ui::stored_render_thread(text))
                inputs.render_thread = *saved == presentation::ui::RenderThreadSetting::safe ? L"safe" : L"separate";
        }
    }
    const auto refresh_depth_prepass = [&] {
        const bool explicit_depth = std::any_of(options.extra.begin(), options.extra.end(), [](const std::wstring& arg) {
            return arg == L"--eawr-depth-prepass" || arg.starts_with(L"--eawr-depth-prepass=");
        });
        std::ifstream stream(settings_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        inputs.depth_prepass_off = !explicit_depth && !evidence_requested(options.extra)
            && presentation::ui::stored_depth_prepass_off(text);
    };
    inputs.quit_after = options.quit_after;
    inputs.extra = options.extra;
    if (multi) {
        // Saves and the font cache stay shared with the first instance; the host port is this instance's own.
        inputs.save_dir = primary_viewer / "saves";
        inputs.font_cache = primary_viewer / "fonts";
        inputs.lan_game_port = instance_host_port(instance);
    }
    const fs::path next_action = state_dir / "next-action.json";
    const fs::path running_state = state_dir / "game-running.json";
    const fs::path launch_result = state_dir / "launch-result.json";
    const auto select_profile_id = [&](const std::string* id) -> bool {
        if (!id || !portable_profile_id(*id)) return false;
        const fs::path profile = options.user_data / "mods" / "profiles" / path_from_utf8(*id + ".json");
        // Match the mod core's 4 MiB profile-file bound.
        const auto json = read_loop_json(profile, 4u * 1024u * 1024u);
        if (!json) return false;
        const auto* format = json->string_member("format");
        const auto* version = json->member("version");
        const auto* profile_id = json->string_member("id");
        const auto* launch = json->member("launch");
        if (!format || *format != "eawr-mod-profile" || !version || version->kind != JsonValue::Kind::number
            || version->number != 1 || !profile_id || *profile_id != *id
            || !launch || launch->kind != JsonValue::Kind::object) return false;
        const auto* render = launch->string_member("render_thread");
        if (!render || (*render != "safe" && *render != "separate")) return false;
        inputs.mod_profile = profile;
        inputs.render_thread = widen_ascii(*render);
        LauncherState state = load_state(options.user_data);
        state.last_profile = *id;
        std::string save_error;
        if (!save_state(options.user_data, state, save_error)) log.line("could not remember the profile: " + save_error);
        return true;
    };
    const auto select_profile = [&](const JsonValue& action) { return select_profile_id(action.string_member("profile")); };
    // Instance n >= 2: the game's user:// follows APPDATA, set only for the game child (never the manager).
    const auto game_environment = [&](bool on) {
        runner.set_appdata(on && multi ? std::optional<fs::path>(instance_appdata_dir(options.user_data, instance)) : std::nullopt);
    };
    const auto running_json = [&](const std::string& id, unsigned pid) {
        std::string state = "{\"profile\":" + json_quote(id) + ",\"pid\":" + std::to_string(pid);
        if (const auto started = process_start_time(pid)) state += ",\"process_start_time\":\"" + *started + "\"";
        if (multi) state += ",\"instance\":" + std::to_string(instance) + ",\"host_port\":" + std::to_string(instance_host_port(instance));
        return state + "}\n";
    };
    bool manager = options.mods;
    if (options.profile) {
        if (!select_profile_id(&*options.profile)) {
            return context.fail("The mod profile \"" + *options.profile + "\" is missing or not valid in "
                                + path_to_utf8(options.user_data / "mods" / "profiles") + ".");
        }
    }
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
        if (!manager) refresh_depth_prepass();
        const std::vector<std::wstring> args = manager
            ? mods_arguments(options.launcher_dir, godot, project, game_root, next_action) : godot_arguments(inputs);
        log.line("starting " + path_to_utf8(godot));

        // The log of a GAME child (the manager is never named here): instance 1 uses the first user:// folder as before,
        // an extra instance has its own because only its games run with the redirected APPDATA.
        const std::string godot_log = path_to_utf8((multi ? instance_viewer_dir(options.user_data, instance) : options.user_data) / "logs" / "godot.log");
        int live_error = 0;
        const auto poll_manager = [&] {
            if (live_error) return;
            const auto request = read_loop_json(next_action);
            if (!request) return;
            const auto* stay_open = request->member("stay_open");
            const auto* kind = request->string_member("action");
            if (!stay_open || stay_open->kind != JsonValue::Kind::boolean || !stay_open->boolean
                || !kind || *kind != "launch-profile") return;
            std::error_code io_error;
            const auto* requested_id = request->string_member("profile");
            const std::string id = requested_id && portable_profile_id(*requested_id) ? *requested_id : std::string();
            const auto result = [&](const char* status, unsigned exit_code) {
                const std::string json = "{\"profile\":\"" + id + "\",\"status\":\"" + status
                    + "\",\"exit_code\":" + std::to_string(exit_code) + "}\n";
                if (!write_loop_file(launch_result, json) && !live_error)
                    live_error = context.fail("EAWR cannot publish the launch result.");
            };
            fs::remove(next_action, io_error);
            if (io_error) {
                result("failed", 0);
                if (!live_error) live_error = context.fail("EAWR cannot consume the launch request: " + io_error.message());
                return;
            }
            if (!select_profile(*request)) { result("failed", 0); return; }
            fs::remove(inputs.report, io_error);
            bool published = false;
            const auto publish = [&](unsigned pid) {
                published = write_loop_file(running_state, running_json(id, pid));
                return published;
            };
            game_environment(true);
            refresh_depth_prepass();
            const ChildResult game = runner.run_monitored(godot, godot_arguments(inputs), options.launcher_dir, {}, publish);
            game_environment(false);
            if (published) fs::remove(running_state, io_error);
            if (published && io_error) live_error = context.fail("EAWR cannot clear the running game state: " + io_error.message());
            if (!game.started) context.fail("EAWR could not start the game: " + game.error, godot_log);
            else if (game.exit_code != 0) {
                std::string reason = failure_from_report(inputs.report);
                if (reason.empty() && game.seconds < kEarlyExitSeconds)
                    reason = "The game stopped right after starting (exit code " + std::to_string(game.exit_code) + ").";
                if (!reason.empty()) context.fail(reason, godot_log);
            }
            // A request from the game must not relaunch the already-open manager.
            fs::remove(next_action, io_error);
            result(game.started && game.exit_code == 0 ? "finished" : "failed", game.exit_code);
        };
        // An extra instance runs its one game directly: it publishes its own running state and result.
        const bool direct_instance = multi && !manager;
        ChildResult child;
        if (direct_instance) {
            const std::string id = options.profile.value_or(std::string());
            bool published = false;
            const auto publish = [&](unsigned pid) {
                published = write_loop_file(running_state, running_json(id, pid));
                return published;
            };
            game_environment(true);
            child = runner.run_monitored(godot, args, options.launcher_dir, {}, publish);
            game_environment(false);
            std::error_code io_error;
            if (published) fs::remove(running_state, io_error);
            const bool ok = child.started && child.exit_code == 0;
            const std::string result = "{\"profile\":" + json_quote(id) + ",\"status\":\"" + (ok ? "finished" : "failed")
                + "\",\"exit_code\":" + std::to_string(child.exit_code) + ",\"instance\":" + std::to_string(instance) + "}\n";
            if (!write_loop_file(launch_result, result)) log.line("could not publish the launch result");
        } else {
            child = manager ? runner.run_monitored(godot, args, options.launcher_dir, poll_manager, {})
                            : runner.run(godot, args, options.launcher_dir);
        }
        if (live_error) return live_error;
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
        if (direct_instance) return static_cast<int>(child.exit_code);   // one game per extra instance
        if (child.exit_code != 0 || !action) return static_cast<int>(child.exit_code);
        const auto* kind = action->string_member("action");
        if (!kind) return kExitOk;
        if (!manager && child.seconds >= kEarlyExitSeconds) rapid_restarts = 0;
        if (*kind == "open-manager") {
            manager = true;
        } else if (*kind == "launch-profile") {
            if (!select_profile(*action)) return kExitOk;
            manager = false;
        } else {
            return kExitOk;
        }
        if (++rapid_restarts > 16) return context.fail("EAWR stopped repeated launch requests. Start EAWR Mods again to continue.");
    }
}

} // namespace eawr::launcher
