// Unit tests of the launcher core: text formats, discovery (VDF + registry fixtures), validation,
// the saved choice, the Godot command line and the whole flow against a
// fake UI and a fake process runner. No game, no Godot, no retail bytes: fixtures are empty folders
// and a few made-up bytes.
#include "mods_start.hpp"
#include "install_discovery.hpp"
#include "instance.hpp"
#include "launch_plan.hpp"
#include "launcher_flow.hpp"
#include "launcher_state.hpp"
#include "text_formats.hpp"
#include "eawr/core/sha256.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <thread>

using namespace eawr::launcher;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            ++g_failures;                                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition "\n";    \
        }                                                                                      \
    } while (false)

#define CHECK_CONTAINS(text, needle)                                                           \
    do {                                                                                       \
        if ((text).find(needle) == std::string::npos) {                                        \
            ++g_failures;                                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected \"" << (needle)            \
                      << "\" in: " << (text) << "\n";                                          \
        }                                                                                      \
    } while (false)

// A scratch folder whose name has spaces and non-ASCII letters (the Unicode-path cases).
class Scratch {
public:
    Scratch() {
        static std::atomic<int> counter{0};
        root_ = fs::temp_directory_path() / (L"eawr launcher caf\u00e9 \u65e5\u672c\u8a9e " + std::to_wstring(GetCurrentProcessId()) + L"-"
                                             + std::to_wstring(counter++));
        fs::create_directories(root_);
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const fs::path& path() const { return root_; }
private:
    fs::path root_;
};

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

constexpr const char* kFakeExe = "not a real StarWarsG.exe: fixture bytes";

// GameData/Data, corruption/Data and corruption/StarWarsG.exe: the structure validation looks at.
fs::path make_install(const fs::path& root) {
    fs::create_directories(root / "GameData" / "Data");
    fs::create_directories(root / "corruption" / "Data");
    write_file(root / "corruption" / "StarWarsG.exe", kFakeExe);
    return root;
}

class FakeRegistry final : public RegistryReader {
public:
    void set(RegistryHive hive, RegistryView view, const std::string& key, const std::string& name, const std::string& value) {
        values_[id(hive, view, key) + "|" + name] = value;
        add_key(hive, view, key);
    }
    void add_key(RegistryHive hive, RegistryView view, const std::string& key) {
        const std::size_t slash = key.rfind('\\');
        if (slash == std::string::npos) return;
        auto& list = subkeys_[id(hive, view, key.substr(0, slash))];
        const std::string leaf = key.substr(slash + 1);
        for (const std::string& existing : list) {
            if (existing == leaf) return;
        }
        list.push_back(leaf);
    }
    std::optional<std::string> read_string(RegistryHive hive, RegistryView view, const std::string& key,
                                           const std::string& value) const override {
        const auto found = values_.find(id(hive, view, key) + "|" + value);
        if (found == values_.end()) return std::nullopt;
        return found->second;
    }
    std::vector<std::string> subkeys(RegistryHive hive, RegistryView view, const std::string& key) const override {
        const auto found = subkeys_.find(id(hive, view, key));
        return found == subkeys_.end() ? std::vector<std::string>() : found->second;
    }
private:
    static std::string id(RegistryHive hive, RegistryView view, const std::string& key) {
        return std::to_string(static_cast<int>(hive)) + "|" + std::to_string(static_cast<int>(view)) + "|" + key;
    }
    std::map<std::string, std::string> values_;
    std::map<std::string, std::vector<std::string>> subkeys_;
};

constexpr const char* kSteamKey = "Software\\Valve\\Steam";
constexpr const char* kGogGames = "SOFTWARE\\GOG.com\\Games";
constexpr const char* kUninstall = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

std::string with_forward_slashes(const fs::path& path) {
    std::string text = path_to_utf8(path);
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string vdf_escaped(const fs::path& path) {
    std::string out;
    for (const char c : path_to_utf8(path)) {
        out += c;
        if (c == '\\') out += '\\';
    }
    return out;
}

// The JSON, KeyValues and path cases moved with their code to tests/mods (src/mods/steam).

// --- validation ---------------------------------------------------------------------------------

void test_validation() {
    Scratch scratch;
    const fs::path game = make_install(scratch.path() / L"Star Wars Empire at War caf\u00e9");
    auto ok = validate_install(game);
    CHECK(ok.ok && same_location(ok.root, game));
    // Normalisation: the expansion folder, GameData, a trailing separator and surrounding quotes.
    CHECK(validate_install(game / "corruption").ok && same_location(validate_install(game / "corruption").root, game));
    CHECK(validate_install(game / "GameData").ok && same_location(validate_install(game / "GameData").root, game));
    CHECK(validate_install(path_from_utf8("\"" + path_to_utf8(game) + "\\\"")).ok);
    CHECK(validate_install(path_from_utf8(path_to_utf8(game / "CORRUPTION"))).ok);   // case

    // One level down (a GOG-style wrapper folder), only where allowed.
    const fs::path wrapper = scratch.path() / "GOG Games";
    make_install(wrapper / "Star Wars - Empire at War - Gold Pack");
    CHECK(validate_install(wrapper, true).ok);
    CHECK(!validate_install(wrapper, false).ok);

    // Failures say what is missing.
    const fs::path base_only = scratch.path() / "base";
    fs::create_directories(base_only / "GameData" / "Data");
    auto failure = validate_install(base_only);
    CHECK(!failure.ok);
    CHECK_CONTAINS(failure.error, "Forces of Corruption");
    CHECK_CONTAINS(failure.error, "corruption");

    const fs::path no_game_data = scratch.path() / "nogd";
    fs::create_directories(no_game_data / "corruption" / "Data");
    write_file(no_game_data / "corruption" / "StarWarsG.exe", kFakeExe);
    failure = validate_install(no_game_data);
    CHECK(!failure.ok);
    CHECK_CONTAINS(failure.error, "GameData");

    const fs::path no_data = scratch.path() / "nodata";
    fs::create_directories(no_data / "GameData" / "Data");
    fs::create_directories(no_data / "corruption");
    write_file(no_data / "corruption" / "StarWarsG.exe", kFakeExe);
    failure = validate_install(no_data);
    CHECK(!failure.ok);
    CHECK_CONTAINS(failure.error, path_to_utf8(no_data / "corruption" / "Data"));

    const fs::path no_exe = scratch.path() / "noexe";
    fs::create_directories(no_exe / "GameData" / "Data");
    fs::create_directories(no_exe / "corruption" / "Data");
    failure = validate_install(no_exe);
    CHECK(!failure.ok);
    CHECK_CONTAINS(failure.error, "StarWarsG.exe");
    CHECK_CONTAINS(failure.error, "Forces of Corruption only");

    failure = validate_install(scratch.path() / "does not exist");
    CHECK(!failure.ok);
    CHECK_CONTAINS(failure.error, "not a folder");
    CHECK(!validate_install(fs::path()).ok);
    // The Data folder is not an install root, and neither is a file.
    CHECK(!validate_install(game / "corruption" / "Data", false).ok);
    CHECK(!validate_install(game / "corruption" / "StarWarsG.exe").ok);
}

// --- discovery ------------------------------------------------------------------------------

std::string app_manifest(const std::string& installdir) {
    return "\"AppState\"\n{\n\t\"appid\"\t\t\"32470\"\n\t\"installdir\"\t\t\"" + installdir + "\"\n}\n";
}

void test_steam_modern_and_legacy() {
    Scratch scratch;
    const fs::path steam = scratch.path() / L"Steam caf\u00e9";
    const fs::path library2 = scratch.path() / L"Bibliothek \u65e5\u672c";
    const fs::path game = make_install(library2 / "steamapps" / "common" / "Star Wars Empire at War");
    write_file(library2 / "steamapps" / "appmanifest_32470.acf", app_manifest("Star Wars Empire at War"));
    fs::create_directories(steam / "steamapps" / "common");
    write_file(steam / "steamapps" / "libraryfolders.vdf",
               "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + vdf_escaped(steam) + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\""
                   + vdf_escaped(library2) + "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"32470\"\t\t\"1\"\n\t\t}\n\t}\n}\n");
    FakeRegistry registry;
    registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", with_forward_slashes(steam));

    const auto roots = steam_library_roots(registry);
    CHECK(roots.size() == 2);
    const auto installs = discover_installs(registry);
    CHECK(installs.size() == 1);
    if (installs.size() == 1) {
        CHECK(same_location(installs[0].root, game));
        CHECK(installs[0].source == "Steam");
    }

    // The pre-2021 format: "1" "D:\\Library".
    Scratch legacy_scratch;
    const fs::path legacy_steam = legacy_scratch.path() / "Steam";
    const fs::path legacy_library = legacy_scratch.path() / "Old Library";
    const fs::path legacy_game = make_install(legacy_library / "steamapps" / "common" / "Star Wars Empire at War");
    write_file(legacy_library / "steamapps" / "appmanifest_32470.acf", app_manifest("Star Wars Empire at War"));
    write_file(legacy_steam / "steamapps" / "libraryfolders.vdf",
               "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1700000000\"\n\t\"ContentStatsID\"\t\t\"-1\"\n\t\"1\"\t\t\""
                   + vdf_escaped(legacy_library) + "\"\n}\n");
    FakeRegistry legacy;
    legacy.set(RegistryHive::local_machine, RegistryView::view32, "SOFTWARE\\Valve\\Steam", "InstallPath", path_to_utf8(legacy_steam));
    const auto found = discover_installs(legacy);
    CHECK(found.size() == 1 && same_location(found[0].root, legacy_game));
}

void test_steam_manifest_name_must_be_one_folder() {
    Scratch scratch;
    const fs::path steam = scratch.path() / "Steam";
    make_install(scratch.path() / "evil");   // a valid install outside steamapps\common
    fs::create_directories(steam / "steamapps" / "common");
    write_file(steam / "steamapps" / "appmanifest_32470.acf", app_manifest("..\\..\\..\\evil"));
    FakeRegistry registry;
    registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", path_to_utf8(steam));
    CHECK(discover_installs(registry).empty());
    for (const Candidate& candidate : find_candidates(registry)) CHECK(!same_location(candidate.path, scratch.path() / "evil"));
}

void test_steam_without_manifest_scans_common() {
    Scratch scratch;
    const fs::path steam = scratch.path() / "Steam";
    const fs::path game = make_install(steam / "steamapps" / "common" / "SomeFolderName");
    fs::create_directories(steam / "steamapps" / "common" / "Other Game" / "Data");
    FakeRegistry registry;
    registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", path_to_utf8(steam));
    const auto installs = discover_installs(registry);
    CHECK(installs.size() == 1 && same_location(installs[0].root, game));
}

void test_gog_records() {
    Scratch scratch;
    const fs::path game_record = make_install(scratch.path() / "GOG Games" / "Star Wars - Empire at War - Gold Pack");
    const fs::path uninstall_game = make_install(scratch.path() / "Galaxy" / "Games" / L"Empire at War caf\u00e9");
    const fs::path derived_game = make_install(scratch.path() / "Derived" / "EaW");
    const fs::path nested_wrapper = scratch.path() / "Wrapper";
    const fs::path nested_game = make_install(nested_wrapper / "inner");
    FakeRegistry registry;
    // The game record only exists in the 32-bit view (GOG installers are 32-bit).
    registry.set(RegistryHive::local_machine, RegistryView::view32, std::string(kGogGames) + "\\1421404581", "path", path_to_utf8(game_record));
    registry.set(RegistryHive::local_machine, RegistryView::view32, std::string(kGogGames) + "\\1421404581", "gameName",
                 "STAR WARS Empire at War - Gold Pack");
    registry.set(RegistryHive::local_machine, RegistryView::view32, std::string(kGogGames) + "\\999", "path",
                 path_to_utf8(scratch.path() / "unrelated"));
    // Uninstall records: InstallLocation, then DisplayIcon / UninstallString fallbacks; both hives and views.
    const std::string galaxy = std::string(kUninstall) + "\\1421404581_is1";
    registry.set(RegistryHive::local_machine, RegistryView::view64, galaxy, "DisplayName", "STAR WARS Empire at War - Gold Pack");
    registry.set(RegistryHive::local_machine, RegistryView::view64, galaxy, "InstallLocation", path_to_utf8(game_record));   // duplicate
    const std::string classic = std::string(kUninstall) + "\\{AAAA-BBBB}_is1";
    registry.set(RegistryHive::current_user, RegistryView::view32, classic, "DisplayName", "Star Wars Empire At War: Forces of Corruption");
    registry.set(RegistryHive::current_user, RegistryView::view32, classic, "Inno Setup: App Path", "\"" + path_to_utf8(uninstall_game) + "\"");
    const std::string icon = std::string(kUninstall) + "\\SomethingElse";
    registry.set(RegistryHive::local_machine, RegistryView::view32, icon, "DisplayName", "Empire at War Gold");
    registry.set(RegistryHive::local_machine, RegistryView::view32, icon, "UninstallString",
                 "\"" + path_to_utf8(derived_game / "unins000.exe") + "\" /SILENT");
    const std::string wrapped = std::string(kUninstall) + "\\Wrapped";
    registry.set(RegistryHive::local_machine, RegistryView::view32, wrapped, "DisplayName", "Empire at War");
    registry.set(RegistryHive::local_machine, RegistryView::view32, wrapped, "DisplayIcon", path_to_utf8(nested_wrapper / "eaw.exe") + ",0");
    const std::string other = std::string(kUninstall) + "\\Other";
    registry.set(RegistryHive::local_machine, RegistryView::view64, other, "DisplayName", "Some Other Game");
    registry.set(RegistryHive::local_machine, RegistryView::view64, other, "InstallLocation", path_to_utf8(make_install(scratch.path() / "Other")));

    const auto installs = discover_installs(registry);
    CHECK(installs.size() == 4);   // game record and Galaxy record are one place; the other game is ignored
    bool seen_record = false;
    bool seen_uninstall = false;
    bool seen_derived = false;
    bool seen_nested = false;
    for (const Install& install : installs) {
        seen_record = seen_record || same_location(install.root, game_record);
        seen_uninstall = seen_uninstall || same_location(install.root, uninstall_game);
        seen_derived = seen_derived || same_location(install.root, derived_game);
        seen_nested = seen_nested || same_location(install.root, nested_game);
    }
    CHECK(seen_record && seen_uninstall && seen_derived && seen_nested);
    CHECK(installs.front().source == "GOG");
}

void test_nothing_found() {
    FakeRegistry registry;
    CHECK(discover_installs(registry).empty());
    CHECK(find_candidates(registry).empty());
}

void test_two_stores_two_installs() {
    Scratch scratch;
    const fs::path steam = scratch.path() / "Steam";
    const fs::path steam_game = make_install(steam / "steamapps" / "common" / "Star Wars Empire at War");
    write_file(steam / "steamapps" / "appmanifest_32470.acf", app_manifest("Star Wars Empire at War"));
    const fs::path gog_game = make_install(scratch.path() / "GOG" / "EaW");
    FakeRegistry registry;
    registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", with_forward_slashes(steam));
    registry.set(RegistryHive::local_machine, RegistryView::view32, std::string(kGogGames) + "\\1", "path", path_to_utf8(gog_game));
    const auto installs = discover_installs(registry);
    CHECK(installs.size() == 2);
    if (installs.size() == 2) {
        CHECK(same_location(installs[0].root, steam_game) && installs[0].source == "Steam");
        CHECK(same_location(installs[1].root, gog_game) && installs[1].source == "GOG");
    }
}

// --- saved choice -----------------------------------------------------------------------------

void test_state() {
    Scratch scratch;
    const fs::path data = scratch.path() / "user data";
    CHECK(!load_state(data).game_root);
    LauncherState state;
    state.game_root = path_from_utf8("D:\\Spiele\\caf\xC3\xA9 \"quoted\" \xE6\x97\xA5\xE6\x9C\xAC");
    std::string error;
    CHECK(save_state(data, state, error));
    CHECK(load_state(data).game_root == state.game_root);
    state.game_root = path_from_utf8("E:\\Other");
    CHECK(save_state(data, state, error));   // replaces the earlier file
    CHECK(load_state(data).game_root == state.game_root);
    fs::path leftover = state_file(data);
    leftover += ".tmp";
    CHECK(!fs::exists(leftover));
    write_file(state_file(data), "{ not json");
    CHECK(!load_state(data).game_root);
    write_file(state_file(data), "{\"game_root\": 5}");
    CHECK(!load_state(data).game_root);
    CHECK(kViewerProjectName == std::string("EAWR Viewer"));
    const fs::path user_dir = default_user_data_dir();
    CHECK(!user_dir.empty() && user_dir.filename() == "EAWR Viewer" && user_dir.parent_path().filename() == "app_userdata");
}

// --- Godot command line -----------------------------------------------------------------------

std::vector<std::wstring> parse_back(const std::wstring& line) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &count);
    std::vector<std::wstring> out;
    for (int index = 0; argv != nullptr && index < count; ++index) out.emplace_back(argv[index]);
    LocalFree(argv);
    return out;
}

void test_launch_plan() {
    LaunchInputs inputs;
    inputs.project = L"C:\\Spiele\\EAWR caf\u00e9\\project";
    inputs.game_root = L"D:\\Games\\Star Wars \u65e5\u672c\\";
    inputs.report = L"C:\\x\\report.json";
    auto args = godot_arguments(inputs);
    const std::vector<std::wstring> expected = {L"--path", inputs.project.wstring(), L"--", L"--eawr-game-root", inputs.game_root.wstring(),
                                                L"--eawr-front-end", L"--eawr-launcher-start", L"--eawr-report", inputs.report.wstring()};
    CHECK(args == expected);
    for (const auto* evidence : {L"--eawr-capture", L"--eawr-report", L"--eawr-benchmark",
                                 L"--eawr-headless-probe", L"--eawr-renderer-runtime-test",
                                 L"--eawr-camera-input-selftest", L"--eawr-window-resize-test", L"--eawr-perf-trace"}) {
        inputs.extra = {evidence, L"--eawr-mod-profile", L"profile.json"};
        args = godot_arguments(inputs);
        CHECK(std::find(args.begin(), args.end(), L"--eawr-launcher-start") == args.end());
    }
    inputs.extra.clear();
    inputs.safe_render_thread = true;   // the opt-out puts the engine flag right after --path
    args = godot_arguments(inputs);
    CHECK(args[2] == L"--render-thread" && args[3] == L"safe" && args[4] == L"--");
    inputs.render_thread = L"separate";
    args = godot_arguments(inputs);
    CHECK(args[2] == L"--render-thread" && args[3] == L"safe" && args[4] == L"--");
    inputs.render_thread.reset();
    inputs.safe_render_thread.reset();
    inputs.allow_unknown_build = true;
    inputs.quit_after = 30;
    inputs.extra = {L"--eawr-audio", L"off"};
    args = godot_arguments(inputs);
    CHECK(args[2] == L"--quit-after" && args[3] == L"30" && args[4] == L"--");
    CHECK(std::find(args.begin(), args.end(), std::wstring(L"--eawr-allow-unknown-build")) != args.end());
    CHECK(std::find(args.begin(), args.end(), std::wstring(L"--eawr-font-cache")) == args.end());   // the viewer extracts fonts itself
    CHECK(args[args.size() - 2] == L"--eawr-audio" && args.back() == L"off");

    // The command line parses back to exactly the same arguments.
    const std::wstring program = L"C:\\Program Files\\EAWR caf\u00e9\\godot\\Godot_v4.7.2-stable_win64.exe";
    const std::vector<std::wstring> tricky = {L"plain", L"with space", L"", L"quote\"inside", L"trailing\\", L"trailing space \\",
                                              L"two\\\\", L"C:\\dir with space\\", L"back\\\\\"quote", L"\u65e5\u672c \u8a9e", L"tab\there"};
    const auto parsed = parse_back(quote_command_line(program, tricky));
    CHECK(parsed.size() == tricky.size() + 1);
    if (parsed.size() == tricky.size() + 1) {
        CHECK(parsed[0] == program);
        for (std::size_t index = 0; index < tricky.size(); ++index) CHECK(parsed[index + 1] == tricky[index]);
    }
    const auto full = parse_back(quote_command_line(program, godot_arguments(inputs)));
    CHECK(full.size() == godot_arguments(inputs).size() + 1);
}

// --- the flow -----------------------------------------------------------------------------------

class FakeUi final : public Ui {
public:
    std::vector<std::string> errors;
    std::vector<std::pair<std::size_t, std::string>> picks;   // installs offered, heading
    std::vector<Pick> pick_answers;
    std::vector<std::optional<fs::path>> browse_answers;
    int browse_calls = 0;

    void show_error(const std::string&, const std::string& message) override { errors.push_back(message); }
    Pick pick_install(const std::vector<Install>& installs, const std::string& heading) override {
        picks.emplace_back(installs.size(), heading);
        if (pick_answers.empty()) return {};
        const Pick answer = pick_answers.front();
        pick_answers.erase(pick_answers.begin());
        return answer;
    }
    std::optional<fs::path> browse_for_folder(const std::string&) override {
        ++browse_calls;
        if (browse_answers.empty()) return std::nullopt;
        auto answer = browse_answers.front();
        browse_answers.erase(browse_answers.begin());
        return answer;
    }
};

class FakeRunner final : public ProcessRunner {
public:
    ChildResult result;
    int calls = 0;
    fs::path exe;
    std::vector<std::wstring> args;
    fs::path cwd;
    std::function<void()> on_run;
    std::function<void(const std::function<void()>&, const std::function<bool(unsigned)>&)> on_monitored;
    std::optional<fs::path> current_appdata;                    // what set_appdata last asked for
    std::vector<std::optional<fs::path>> appdata_at_start;      // the APPDATA redirect in force at each child start

    void set_appdata(const std::optional<fs::path>& folder) override { current_appdata = folder; }

    ChildResult run_monitored(const fs::path& program, const std::vector<std::wstring>& arguments,
                              const fs::path& working_directory, const std::function<void()>& poll,
                              const std::function<bool(unsigned)>& started) override {
        if (!on_monitored) return run(program, arguments, working_directory);
        ++calls;
        appdata_at_start.push_back(current_appdata);
        exe = program;
        args = arguments;
        cwd = working_directory;
        on_monitored(poll, started);
        ChildResult copy = result;
        copy.started = copy.error.empty();
        return copy;
    }

    ChildResult run(const fs::path& program, const std::vector<std::wstring>& arguments, const fs::path& working_directory) override {
        ++calls;
        appdata_at_start.push_back(current_appdata);
        exe = program;
        args = arguments;
        cwd = working_directory;
        if (on_run) on_run();
        ChildResult copy = result;
        copy.started = copy.error.empty();
        return copy;
    }
};

std::string sha_of(const std::string& bytes) {
    return eawr::core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

void write_runtime_json(const fs::path& package, const std::string& godot_sha256) {
    write_file(package / "runtime.json", "{\"schema\":\"eawr-player-runtime/1\",\"godot_sha256\":\"" + godot_sha256 + "\"}\n");
}

bool has_flag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    return std::find(args.begin(), args.end(), std::wstring(flag)) != args.end();
}

// A package folder (Godot stand-in + project), user data and a fixture game.
struct FlowFixture {
    Scratch scratch;
    fs::path package = scratch.path() / L"EAWR caf\u00e9";
    fs::path user_data = scratch.path() / "user data";
    fs::path viewer_data = scratch.path() / "Godot user data";
    fs::path game = make_install(scratch.path() / "Games" / L"Star Wars \u65e5\u672c");
    FakeRegistry registry;
    FakeUi ui;
    FakeRunner runner;
    Log log{user_data / "launcher" / "launcher.log"};

    FlowFixture() {
        write_file(package / "godot" / kGodotExecutableName, "godot stand-in");
        write_file(package / "project" / "project.godot", "config_version=5\n");
        write_runtime_json(package, sha_of("godot stand-in"));
    }
    Options options() const {
        Options options;
        options.launcher_dir = package;
        options.user_data = user_data;
        options.viewer_user_data = viewer_data;
        return options;
    }
    int run(const Options& options) { return run_launcher(options, registry, ui, runner, log); }
    std::string log_text() const { return read_file(log.file()); }
};

void test_flow_explicit_game_root() {
    FlowFixture f;
    Options options = f.options();
    options.game_root = f.game / "corruption";   // normalised to the install root
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 1);
    CHECK(f.ui.errors.empty());
    CHECK(f.runner.exe == f.package / "godot" / kGodotExecutableName);
    CHECK(f.runner.cwd == f.package);
    const auto& args = f.runner.args;
    CHECK(args.size() == 9 && args[0] == L"--path" && args[1] == (f.package / "project").wstring() && args[2] == L"--");
    CHECK(args[3] == L"--eawr-game-root" && same_location(fs::path(args[4]), f.game));
    CHECK(args[5] == L"--eawr-front-end" && args[6] == L"--eawr-launcher-start" && args[7] == L"--eawr-report");
    CHECK(fs::path(args[8]).parent_path() == f.user_data / "launcher");
    for (const std::wstring& argument : args) CHECK(argument.find(L"console") == std::wstring::npos && argument.find(L"font") == std::wstring::npos);
    CHECK(!load_state(f.user_data).game_root);   // an explicit folder is a one-off: not remembered
    CHECK(f.ui.picks.empty());
}

void test_render_thread_precedence() {
    FlowFixture f;
    Options options = f.options(); options.game_root = f.game;
    const auto mode = [&]() -> std::wstring {
        CHECK(f.run(options) == kExitOk);
        const auto it = std::find(f.runner.args.begin(), f.runner.args.end(), L"--render-thread");
        return it == f.runner.args.end() ? L"project" : *std::next(it);
    };
    CHECK(mode() == L"project");
    write_file(f.user_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=1\n");
    CHECK(mode() == L"project"); // stale launcher-side file cannot supply a viewer preference
    CHECK(f.user_data != f.viewer_data);
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=1\n");
    write_file(f.user_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=0\n");
    CHECK(mode() == L"safe");
    options.safe_render_thread = false;
    CHECK(mode() == L"separate"); // explicit CLI beats saved Safe
    options.safe_render_thread.reset();
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=0\n");
    write_file(f.user_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=1\n");
    CHECK(mode() == L"separate");
    options.safe_render_thread = true;
    CHECK(mode() == L"safe"); // explicit Safe beats saved Separate
    options.safe_render_thread.reset();
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 999\nvideo.render_thread=1\n");
    CHECK(mode() == L"project");
    const auto alternate = f.scratch.path() / L"custom settings.cfg";
    write_file(alternate, "eawr-settings 1\nvideo.render_thread=1\n");
    options.extra = {L"--eawr-settings-file", alternate.wstring()};
    CHECK(mode() == L"safe");
    CHECK(fs::current_path() != f.package);
    const fs::path relative = L"relative settings.cfg";
    write_file(f.package / relative, "eawr-settings 1\nvideo.render_thread=1\n");
    options.extra = {L"--eawr-settings-file", relative.wstring()};
    CHECK(mode() == L"safe"); // same working directory as the Godot child
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=0\n");
    options.extra.insert(options.extra.end(), {L"--eawr-settings-file", L""});
    CHECK(mode() == L"separate"); // an empty final override restores Godot's default user:// path
    CHECK(parse_command_line({L"--render-thread", L"separate"}).safe_render_thread == false);
    CHECK(!parse_command_line({L"--render-thread", L"invalid"}).error.empty());
    CHECK(!parse_command_line({L"--render-thread"}).error.empty());
}

void test_depth_prepass_precedence() {
    FlowFixture f;
    Options options = f.options(); options.game_root = f.game;
    const auto flags = [&]() {
        CHECK(f.run(options) == kExitOk);
        std::vector<std::wstring> values;
        for (std::size_t i = 0; i < f.runner.args.size(); ++i) {
            if (f.runner.args[i] == L"--eawr-depth-prepass" && i + 1 < f.runner.args.size())
                values.push_back(f.runner.args[i + 1]);
            else if (f.runner.args[i].starts_with(L"--eawr-depth-prepass=")) values.push_back(f.runner.args[i]);
        }
        return values;
    };
    CHECK(flags().empty()); // Missing file: the project decides.
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.depth_prepass=1\n");
    CHECK(flags().empty()); // On remains the project default, even after saving.
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.depth_prepass=0\n");
    CHECK(flags() == std::vector<std::wstring>{L"off"});
    options.safe_render_thread = true;
    CHECK(flags() == std::vector<std::wstring>{L"off"});
    for (const wchar_t* flag : {L"--eawr-capture", L"--eawr-report", L"--eawr-benchmark", L"--eawr-perf-trace"}) {
        options.extra = {flag, L"fixture"};
        CHECK(flags().empty()); // Evidence runs do not inherit the player preference.
    }
    options.extra = {L"--eawr-depth-prepass", L"on"};
    CHECK(flags() == std::vector<std::wstring>{L"on"});
    options.extra = {L"--eawr-depth-prepass=bad"};
    CHECK(flags() == std::vector<std::wstring>{L"--eawr-depth-prepass=bad"});
    const auto alternate = f.scratch.path() / L"depth settings.cfg";
    write_file(alternate, "eawr-settings 1\nvideo.depth_prepass=0\n");
    options.extra = {L"--eawr-settings-file", alternate.wstring()};
    CHECK(flags() == std::vector<std::wstring>{L"off"});
    write_file(alternate, "eawr-settings 999\nvideo.depth_prepass=0\n");
    CHECK(flags().empty());
}

void test_flow_mods() {
    FlowFixture f;
    Options options = f.options();
    options.game_root = f.game;
    options.mods = true;
    CHECK(parse_command_line({L"--mods"}).mods);
    CHECK(!parse_command_line({L"--", L"--mods"}).mods);
    CHECK(f.run(options) == kExitError);
    CHECK(f.runner.calls == 0);
    write_file(f.package / "launcher" / "project.godot", "config_version=5\n");
    write_file(f.package / "eawr_mods_service.exe", "service stand-in");
    CHECK(f.run(options) == kExitOk);
    const std::vector<std::wstring> expected = {L"--path", (f.package / "launcher").wstring(), L"--",
        L"--eawr-mods-service", (f.package / "eawr_mods_service.exe").wstring(), L"--eawr-godot",
        (f.package / "godot" / kGodotExecutableName).wstring(), L"--eawr-game-project",
        (f.package / "project").wstring(), L"--eawr-game-root", f.game.wstring(),
        L"--eawr-launch-loop", (f.user_data / "launcher" / "next-action.json").wstring(),
        L"--eawr-launch-loop-stay-open", (f.user_data / "launcher" / "game-running.json").wstring()};
    CHECK(f.runner.args == expected);
    CHECK(f.runner.cwd == f.package);
    CHECK_CONTAINS(f.log_text(), "the mod manager exited with code");
    f.runner.result.error = "denied";
    CHECK(f.run(options) == kExitError);
    CHECK_CONTAINS(f.ui.errors.back(), "could not start the mod manager");
}

// Every transition runs through the same process seam as the real hidden Windows child.
void test_flow_launch_loop() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.mods = true;
    write_file(f.package / "launcher/project.godot", "manager");
    write_file(f.package / "eawr_mods_service.exe", "service");
    const auto request = f.user_data / "launcher/next-action.json";
    const auto profile = f.user_data / "mods" / "profiles" / "sample.json";
    const auto profile_json = [](const std::string& render) {
        return "{\"format\":\"eawr-mod-profile\",\"version\":1,\"id\":\"sample\","
               "\"launch\":{\"render_thread\":\"" + render + "\"}}";
    };
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.render_thread=1\n"); // profile Separate wins over saved Safe
    write_file(profile, profile_json("safe") + std::string(65536, ' '));
    LauncherState state;
    state.game_root = f.game;
    std::string error;
    CHECK(save_state(f.user_data, state, error));
    write_file(request, R"({"action":"launch-profile","profile":"stale"})");
    f.runner.on_run = [&] {
        CHECK(!fs::exists(request)); // stale and consumed requests never reach a child
        const auto& args = f.runner.args;
        if (f.runner.calls == 1) {
            CHECK(std::find(args.begin(), args.end(), L"--eawr-launch-loop") != args.end());
            write_file(request, R"({"action":"launch-profile","profile":"sample"})");
        } else if (f.runner.calls == 2 || f.runner.calls == 4 || f.runner.calls == 5) {
            auto flag = std::find(args.begin(), args.end(), L"--eawr-mod-profile");
            CHECK(flag != args.end() && flag + 1 != args.end() && *(flag + 1) == profile.wstring());
            auto render = std::find(args.begin(), args.end(), L"--render-thread");
            const std::wstring expected = f.runner.calls == 2 ? L"safe" : L"separate";
            CHECK(render != args.end() && render + 1 != args.end() && *(render + 1) == expected);
            CHECK(render < std::find(args.begin(), args.end(), L"--"));
            CHECK(load_state(f.user_data).last_profile == std::optional<std::string>("sample"));
            CHECK(load_state(f.user_data).game_root && same_location(*load_state(f.user_data).game_root, f.game));
            const auto depth = std::find(args.begin(), args.end(), L"--eawr-depth-prepass");
            if (f.runner.calls == 4) {
                CHECK(depth != args.end() && depth + 1 != args.end() && *(depth + 1) == L"off");
                write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.depth_prepass=1\n");
                write_file(request, R"({"action":"launch-profile","profile":"sample"})");
            } else {
                CHECK(depth == args.end());
                if (f.runner.calls == 2) {
                    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.depth_prepass=0\n");
                    write_file(request, R"({"action":"open-manager"})");
                }
            }
        } else if (f.runner.calls == 3) {
            CHECK(args[1] == (f.package / "launcher").wstring());
            write_file(profile, profile_json("separate"));
            write_file(request, R"({"action":"launch-profile","profile":"sample"})");
        } else {
            CHECK(false);
        }
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 5);
    CHECK(!fs::exists(request));
    // Remembering a profile never changes a plain EAWR start.
    options.mods = false;
    f.runner.on_run = [&] {
        CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), L"--eawr-mod-profile") == f.runner.args.end());
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 6);
}

void test_stale_running_state_at_start() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    const auto running = f.user_data / "launcher/game-running.json";
    const auto run = [&] { CHECK(f.run(options) == kExitOk); };

    write_file(running, R"({"profile":"old","pid":4294967295})");
    run();
    CHECK(!fs::exists(running));

    const auto live = std::to_string(GetCurrentProcessId());
    write_file(running, "{\"profile\":\"live\",\"pid\":" + live + "}");
    run();
    CHECK(fs::exists(running));

    write_file(running, "{\"profile\":\"old\",\"pid\":" + live + ",\"process_start_time\":\"0\"}");
    run();
    CHECK(!fs::exists(running));

    write_file(running, "{malformed");
    run();
    CHECK(read_file(running) == "{malformed");

    write_file(running, R"({"profile":"old","pid":4294967295})");
    HANDLE locked = CreateFileW(running.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    run();
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    CHECK(fs::exists(running));
}

void test_flow_stay_open() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.mods = true;
    write_file(f.package / "launcher/project.godot", "manager");
    write_file(f.package / "eawr_mods_service.exe", "service");
    const auto request = f.user_data / "launcher/next-action.json";
    const auto running = f.user_data / "launcher/game-running.json";
    const auto profile = f.user_data / "mods/profiles/sample.json";
    write_file(profile, R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":"safe"}})");
    write_file(running, R"({"profile":"stale","pid":1})");
    f.runner.on_monitored = [&](const auto& poll, const auto& started) {
        if (poll) {
            write_file(request, R"({"action":"launch-profile","profile":"sample","stay_open":true})");
            poll();
            CHECK(!fs::exists(running));
            CHECK(!fs::exists(request));
        } else {
            CHECK(started && started(1234));
            std::ifstream stream(running);
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            CHECK_CONTAINS(text, R"("profile":"sample")");
            CHECK_CONTAINS(text, R"("pid":1234)");
            CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), L"--eawr-mod-profile") != f.runner.args.end());
        }
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 2);
    CHECK(!fs::exists(running));
    std::ifstream result(f.user_data / "launcher/launch-result.json");
    const std::string text((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    CHECK_CONTAINS(text, R"("status":"finished")");
}

void test_flow_stay_open_failures() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.mods = true;
        write_file(f.package / "launcher/project.godot", "manager");
        write_file(f.package / "eawr_mods_service.exe", "service");
        const auto request = f.user_data / "launcher/next-action.json";
        const auto running = f.user_data / "launcher/game-running.json";
        const auto result = f.user_data / "launcher/launch-result.json";
        write_file(f.user_data / "mods/profiles/sample.json",
                   R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":"safe"}})");
        f.runner.on_monitored = [&](const auto& poll, const auto& started) {
            if (poll) {
                write_file(request, R"({"action":"launch-profile","profile":"sample","stay_open":true})");
                HANDLE locked = INVALID_HANDLE_VALUE;
                if (scenario == 0) {
                    locked = CreateFileW(request.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    CHECK(locked != INVALID_HANDLE_VALUE);
                }
                if (scenario == 1) write_file(running / "block", "x");
                poll();
                if (scenario == 0) {
                    const auto count = f.ui.errors.size();
                    poll();
                    CHECK(f.ui.errors.size() == count);
                    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
                }
                f.runner.result = {};
            } else if (scenario == 1) {
                CHECK(!started(1234));
                f.runner.result.error = "publish failed";
            } else {
                CHECK(started(1234));
                f.runner.result.exit_code = 9;
            }
        };
        CHECK(f.run(options) == (scenario == 0 ? kExitError : kExitOk));
        std::ifstream stream(result);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        CHECK_CONTAINS(text, R"("status":"failed")");
        CHECK(!f.ui.errors.empty());
    }
}

void test_flow_launch_loop_invalid() {
    const std::string oversized_request = std::string(R"({"action":"open-manager","pad":")")
        + std::string(65536, ' ') + R"("})";
    CHECK(oversized_request.size() > 64 * 1024);
    CHECK(parse_json(oversized_request).has_value());
    const std::vector<std::string> requests = {
        "", "{", "[]", "null", "{}", R"({"action":7})", R"({"action":"unknown"})",
        R"({"action":"launch-profile"})", R"({"action":"launch-profile","profile":2})",
        R"({"action":"launch-profile","profile":"../escape"})",
        R"({"action":"launch-profile","profile":"CON"})",
        R"({"action":"launch-profile","profile":"missing"})", oversized_request
    };
    for (const auto& text : requests) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.mods = true;
        write_file(f.package / "launcher/project.godot", "manager");
        write_file(f.package / "eawr_mods_service.exe", "service");
        const auto request = f.user_data / "launcher/next-action.json";
        f.runner.on_run = [&] { if (!text.empty()) write_file(request, text); };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 1);
        CHECK(!fs::exists(request));
    }
    for (const std::string text : {"{", "{}",
            R"({"format":"eawr-mod-profile","version":1,"id":"other","launch":{"render_thread":"safe"}})",
            R"({"format":"eawr-mod-profile","version":2,"id":"sample","launch":{"render_thread":"safe"}})",
            R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":"invalid"}})"}) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        const auto request = f.user_data / "launcher/next-action.json";
        write_file(f.user_data / "mods/profiles/sample.json", text);
        f.runner.on_run = [&] { write_file(request, R"({"action":"launch-profile","profile":"sample"})"); };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 1);
        CHECK(!fs::exists(request));
        CHECK(!load_state(f.user_data).last_profile);
    }
}

void test_flow_launch_loop_profile_traversal() {
    for (bool absolute : {false, true}) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.mods = true;
        write_file(f.package / "launcher/project.godot", "manager");
        write_file(f.package / "eawr_mods_service.exe", "service");
        const auto request = f.user_data / "launcher" / "next-action.json";
        const std::string id = absolute ? path_to_utf8(f.scratch.path() / "absolute") : "../escape";
        const auto target = absolute ? f.scratch.path() / "absolute.json" : f.user_data / "mods" / "escape.json";
        // Everything the loop reads is valid except the requested id: removing the
        // portable-id guard would read this file and start the game a second time.
        write_file(target, "{\"format\":\"eawr-mod-profile\",\"version\":1,\"id\":" + json_quote(id)
            + R"(,"name":"Synthetic","order_source":"player","layers":[],"launch":{"mode":"front-end","render_thread":"safe"}})");
        fs::create_directories(f.user_data / "mods" / "profiles");
        const auto unguarded_path = f.user_data / "mods" / "profiles" / path_from_utf8(id + ".json");
        CHECK(fs::is_regular_file(target));
        CHECK(read_file(unguarded_path) == read_file(target));
        CHECK(parse_json(read_file(unguarded_path)).has_value());
        f.runner.on_run = [&] {
            if (f.runner.calls == 1) {
                write_file(request, "{\"action\":\"launch-profile\",\"profile\":" + json_quote(id) + "}");
            }
        };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 1);
        CHECK(!fs::exists(request));
        CHECK(!load_state(f.user_data).last_profile);
    }
}

void test_flow_launch_loop_explicit_safe_render_thread() {
    for (bool safe : {true, false}) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.mods = true;
        options.safe_render_thread = safe ? parse_command_line({L"--safe-render-thread"}).safe_render_thread
                                          : parse_command_line({L"--render-thread", L"separate"}).safe_render_thread;
        write_file(f.package / "launcher/project.godot", "manager");
        write_file(f.package / "eawr_mods_service.exe", "service");
        write_file(f.viewer_data / "eawr-settings.cfg", safe ? "eawr-settings 1\nvideo.render_thread=0\n"
                                                           : "eawr-settings 1\nvideo.render_thread=1\n");
        write_file(f.user_data / "mods/profiles/sample.json",
            std::string(R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":")")
            + (safe ? "separate" : "safe") + R"("}})");
        f.runner.on_run = [&] {
            if (f.runner.calls == 1) {
                write_file(f.user_data / "launcher/next-action.json", R"({"action":"launch-profile","profile":"sample"})");
            } else {
                const auto& args = f.runner.args;
                const auto render = std::find(args.begin(), args.end(), L"--render-thread");
                CHECK(render != args.end() && render + 1 != args.end() && *(render + 1) == (safe ? L"safe" : L"separate"));
                CHECK(std::count(args.begin(), args.end(), L"--render-thread") == 1);
                CHECK(render < std::find(args.begin(), args.end(), L"--"));
            }
        };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 2);
    }
}

void test_flow_launch_loop_profile_beats_saved_render_thread() {
    for (bool profile_safe : {true, false}) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.mods = true;
        write_file(f.package / "launcher/project.godot", "manager");
        write_file(f.package / "eawr_mods_service.exe", "service");
        write_file(f.viewer_data / "eawr-settings.cfg", profile_safe ? "eawr-settings 1\nvideo.render_thread=0\n"
                                                                   : "eawr-settings 1\nvideo.render_thread=1\n");
        write_file(f.user_data / "mods/profiles/sample.json",
            std::string(R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":")")
            + (profile_safe ? "safe" : "separate") + R"("}})");
        f.runner.on_run = [&] {
            if (f.runner.calls == 1) {
                write_file(f.user_data / "launcher/next-action.json", R"({"action":"launch-profile","profile":"sample"})");
            } else {
                const auto& args = f.runner.args;
                const auto render = std::find(args.begin(), args.end(), L"--render-thread");
                CHECK(render != args.end() && render + 1 != args.end() && *(render + 1) == (profile_safe ? L"safe" : L"separate"));
                CHECK(std::count(args.begin(), args.end(), L"--render-thread") == 1);
                CHECK_CONTAINS(f.log_text(), " (mod profile)");
            }
        };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 2);
    }
}

void test_flow_launch_loop_guard_and_failure() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.mods = true;
    write_file(f.package / "launcher/project.godot", "manager");
    write_file(f.package / "eawr_mods_service.exe", "service");
    const auto request = f.user_data / "launcher/next-action.json";
    f.runner.on_run = [&] {
        CHECK(!fs::exists(request));
        write_file(request, R"({"action":"open-manager"})");
    };
    CHECK(f.run(options) == kExitError);
    CHECK(f.runner.calls == 17);
    CHECK(!fs::exists(request));
    CHECK_CONTAINS(f.ui.errors.back(), "repeated launch requests");
    f.runner.result.exit_code = 9;
    CHECK(f.run(options) == 9); // a failed manager cannot request a restart
    CHECK(f.runner.calls == 18);
    CHECK(!fs::exists(request));
    options.mods = false;
    CHECK(f.run(options) == 9); // neither can a failed game
    CHECK(f.runner.calls == 19);
    CHECK(!fs::exists(request));
}

void test_flow_launch_loop_long_sessions_and_unclearable_request() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    write_file(f.package / "launcher/project.godot", "manager");
    write_file(f.package / "eawr_mods_service.exe", "service");
    const auto request = f.user_data / "launcher/next-action.json";
    write_file(f.user_data / "mods/profiles/sample.json",
        R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":"safe"}})");
    f.runner.result.seconds = kEarlyExitSeconds;
    f.runner.on_run = [&] {
        CHECK(!fs::exists(request));
        if (f.runner.calls < 36) {
            write_file(request, f.runner.calls % 2 == 1 ? R"({"action":"open-manager"})"
                : R"({"action":"launch-profile","profile":"sample"})");
        }
    };
    CHECK(f.run(options) == kExitOk); // real game sessions do not exhaust the restart guard
    CHECK(f.runner.calls == 36);
    write_file(request / "occupied", "cannot remove a nonempty directory");
    CHECK(f.run(options) == kExitError);
    CHECK(f.runner.calls == 36); // never start with an uncleared request
}

void test_flow_explicit_game_root_invalid() {
    FlowFixture f;
    Options options = f.options();
    options.game_root = f.scratch.path() / "nothing here";
    CHECK(f.run(options) == kExitError);
    CHECK(f.runner.calls == 0);
    CHECK(f.ui.errors.size() == 1);
    if (!f.ui.errors.empty()) {
        CHECK_CONTAINS(f.ui.errors[0], "not a folder");
        CHECK_CONTAINS(f.ui.errors[0], path_to_utf8(f.log.file()));
    }
    CHECK(f.ui.picks.empty());   // an explicit choice is never silently replaced by a search
    CHECK(!load_state(f.user_data).game_root);
}

void test_flow_saved_choice_revalidated() {
    FlowFixture f;
    LauncherState state;
    state.game_root = f.game;
    std::string error;
    CHECK(save_state(f.user_data, state, error));
    CHECK(f.run(f.options()) == kExitOk);
    CHECK(f.ui.picks.empty() && f.runner.calls == 1);

    // The saved folder disappears: the flow falls back to discovery and finds nothing -> picker.
    fs::remove_all(f.game);
    f.runner.calls = 0;
    f.ui.pick_answers = {Ui::Pick{}};   // cancel
    CHECK(f.run(f.options()) == kExitCancelled);
    CHECK(f.ui.picks.size() == 1 && f.ui.picks[0].first == 0 && f.runner.calls == 0);
    CHECK_CONTAINS(f.log_text(), "no longer valid");
}

void test_flow_one_discovered_install_is_used_silently() {
    FlowFixture f;
    const fs::path steam = f.scratch.path() / "Steam";
    const fs::path game = make_install(steam / "steamapps" / "common" / "Star Wars Empire at War");
    write_file(steam / "steamapps" / "appmanifest_32470.acf", app_manifest("Star Wars Empire at War"));
    f.registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", with_forward_slashes(steam));
    CHECK(f.run(f.options()) == kExitOk);
    CHECK(f.ui.picks.empty());
    CHECK(f.runner.calls == 1);
    CHECK(same_location(*load_state(f.user_data).game_root, game));
}

void test_flow_several_installs_ask() {
    FlowFixture f;
    const fs::path steam = f.scratch.path() / "Steam";
    make_install(steam / "steamapps" / "common" / "Star Wars Empire at War");
    write_file(steam / "steamapps" / "appmanifest_32470.acf", app_manifest("Star Wars Empire at War"));
    f.registry.set(RegistryHive::current_user, RegistryView::view64, kSteamKey, "SteamPath", with_forward_slashes(steam));
    f.registry.set(RegistryHive::local_machine, RegistryView::view32, std::string(kGogGames) + "\\1", "path", path_to_utf8(f.game));
    Ui::Pick second;
    second.kind = Ui::Pick::Kind::install;
    second.index = 1;
    f.ui.pick_answers = {second};
    CHECK(f.run(f.options()) == kExitOk);
    CHECK(f.ui.picks.size() == 1 && f.ui.picks[0].first == 2);
    CHECK(f.runner.calls == 1);
    CHECK(same_location(*load_state(f.user_data).game_root, f.game));   // the GOG one the player picked

    // --choose-game asks again even though a choice is saved, listing the saved one too.
    f.runner.calls = 0;
    f.ui.picks.clear();
    f.ui.pick_answers = {second};
    Options options = f.options();
    options.choose_game = true;
    CHECK(f.run(options) == kExitOk);
    CHECK(f.ui.picks.size() == 1 && f.ui.picks[0].first == 2);   // saved == GOG install: deduplicated
}

void test_flow_browse_after_nothing_found() {
    FlowFixture f;
    Ui::Pick browse;
    browse.kind = Ui::Pick::Kind::browse;
    f.ui.pick_answers = {browse, browse};
    f.ui.browse_answers = {std::optional<fs::path>(f.scratch.path() / "wrong"), std::optional<fs::path>(f.game / "corruption")};
    CHECK(f.run(f.options()) == kExitOk);
    CHECK(f.ui.browse_calls == 2);
    CHECK(f.ui.errors.size() == 1);   // the rejected folder was explained
    if (!f.ui.errors.empty()) CHECK_CONTAINS(f.ui.errors[0], "not a folder");
    CHECK(f.runner.calls == 1);
    CHECK_CONTAINS(f.ui.picks[0].second, "could not find");

    FlowFixture cancelled;
    cancelled.ui.pick_answers = {browse};
    CHECK(cancelled.run(cancelled.options()) == kExitCancelled);   // browse dialog cancelled, nothing else to offer
    CHECK(cancelled.runner.calls == 0);
}

void test_flow_missing_runtime_or_project() {
    {
        FlowFixture f;
        fs::remove(f.package / "godot" / kGodotExecutableName);
        Options options = f.options();
        options.game_root = f.game;
        CHECK(f.run(options) == kExitError);
        CHECK(f.runner.calls == 0 && f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors[0], "Godot runtime");
    }
    {
        FlowFixture f;
        fs::remove(f.package / "project" / "project.godot");
        Options options = f.options();
        options.game_root = f.game;
        CHECK(f.run(options) == kExitError);
        CHECK(f.runner.calls == 0 && f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors[0], "project folder");
    }
    {
        FlowFixture f;
        Options options = f.options();
        options.game_root = f.game;
        options.godot = f.scratch.path() / "no such godot.exe";
        CHECK(f.run(options) == kExitError);
        CHECK_CONTAINS(f.ui.errors.at(0), "no such godot.exe");
    }
}

void test_flow_godot_failures() {
    {
        FlowFixture f;
        Options options = f.options();
        options.game_root = f.game;
        f.runner.result.exit_code = 7;
        f.runner.result.seconds = 1.5;
        f.runner.on_run = [&f] {
            // The viewer's own explanation, as it writes it to --eawr-report.
            write_file(f.user_data / "launcher" / "front-end-report.json",
                       "{\"status\":\"failed\",\"failure\":\"Cannot read Config.meg. Repair the FoC installation.\"}");
        };
        CHECK(f.run(options) == 7);
        CHECK(f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors.at(0), "Cannot read Config.meg");
        CHECK_CONTAINS(f.ui.errors.at(0), path_to_utf8(f.user_data / "logs" / "godot.log"));
    }
    {
        FlowFixture f;
        Options options = f.options();
        options.game_root = f.game;
        write_file(f.user_data / "launcher" / "front-end-report.json", "{\"status\":\"failed\",\"failure\":\"stale\"}");
        f.runner.result.exit_code = 3;
        f.runner.result.seconds = 0.2;
        CHECK(f.run(options) == 3);
        CHECK(f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors.at(0), "exit code 3");
        CHECK(f.ui.errors.at(0).find("stale") == std::string::npos);   // a previous run's report is removed
    }
    {
        FlowFixture f;   // the viewer's report names a failure however late the game exits (slow first-run provisioning)
        Options options = f.options();
        options.game_root = f.game;
        f.runner.result.exit_code = 4;
        f.runner.result.seconds = 3600;
        f.runner.on_run = [&f] {
            write_file(f.user_data / "launcher" / "front-end-report.json", "{\"status\":\"failed\",\"failure\":\"Font provisioning failed.\"}");
        };
        CHECK(f.run(options) == 4);
        CHECK(f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors.at(0), "Font provisioning failed");
        CHECK_CONTAINS(f.ui.errors.at(0), path_to_utf8(f.user_data / "logs" / "godot.log"));
    }
    {
        FlowFixture f;   // a late non-zero exit without a report (the player's session) is not an error dialog
        Options options = f.options();
        options.game_root = f.game;
        f.runner.result.exit_code = 1;
        f.runner.result.seconds = 3600;
        CHECK(f.run(options) == 1);
        CHECK(f.ui.errors.empty());
    }
    {
        FlowFixture f;
        Options options = f.options();
        options.game_root = f.game;
        f.runner.result.error = "Access is denied. (error 5)";
        CHECK(f.run(options) == kExitError);
        CHECK(f.ui.errors.size() == 1);
        CHECK_CONTAINS(f.ui.errors.at(0), "Access is denied");
    }
}

void test_flow_unknown_build_flag_and_the_viewers_explanation() {
    FlowFixture f;
    Options options = f.options();
    options.game_root = f.game;
    options.allow_unknown_build = true;
    CHECK(f.run(options) == kExitOk);
    CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), std::wstring(L"--eawr-allow-unknown-build")) != f.runner.args.end());
    // Without the flag the viewer refuses an unverified build and says so in its report; the dialog shows that text.
    FlowFixture g;
    options = g.options();
    options.game_root = g.game;
    g.runner.result.exit_code = 3;
    g.runner.result.seconds = 0.8;
    g.runner.on_run = [&g] {
        write_file(g.user_data / "launcher" / "front-end-report.json",
                   "{\"status\":\"failed\",\"failure\":\"This StarWarsG.exe is not a build EAWR has verified.\"}");
    };
    CHECK(g.run(options) == 3);
    CHECK(std::find(g.runner.args.begin(), g.runner.args.end(), std::wstring(L"--eawr-allow-unknown-build")) == g.runner.args.end());
    CHECK_CONTAINS(g.ui.errors.at(0), "not a build EAWR has verified");
}

void test_flow_unwritable_user_data_and_bad_command_line() {
    FlowFixture f;
    Options options = f.options();
    options.game_root = f.game;
    write_file(f.scratch.path() / "blocker", "a file where a folder is needed");
    options.user_data = f.scratch.path() / "blocker";
    CHECK(f.run(options) == kExitError);
    CHECK_CONTAINS(f.ui.errors.at(0), "user data folder");

    FlowFixture g;
    const Options bad = parse_command_line({L"--game-root"});
    CHECK(!bad.error.empty());
    Options with_error = g.options();
    with_error.error = bad.error;
    CHECK(g.run(with_error) == kExitError);
    CHECK_CONTAINS(g.ui.errors.at(0), "needs a value");
}

void test_command_line_parsing() {
    const Options options = parse_command_line({L"--game-root", L"D:\\Games\\EaW", L"--godot-quit-after", L"30", L"--allow-unknown-build",
                                                L"--choose-game", L"--no-dialogs", L"--safe-render-thread", L"--user-data", L"U:\\data", L"--godot", L"G.exe",
                                                L"--project", L"P", L"--eawr-audio", L"off", L"--", L"--eawr-hud", L"off"});
    CHECK(options.error.empty());
    CHECK(options.game_root == fs::path(L"D:\\Games\\EaW"));
    CHECK(options.quit_after == 30);
    CHECK(options.allow_unknown_build && options.choose_game && options.no_dialogs);
    CHECK(options.safe_render_thread && !parse_command_line({L"--game-root", L"G"}).safe_render_thread);
    CHECK(options.user_data == fs::path(L"U:\\data") && options.godot == fs::path(L"G.exe") && options.project == fs::path(L"P"));
    CHECK((options.extra == std::vector<std::wstring>{L"--eawr-audio", L"off", L"--eawr-hud", L"off"}));
    CHECK(!parse_command_line({L"--godot-quit-after", L"soon"}).error.empty());
    CHECK(!parse_command_line({L"--godot-quit-after", L"-4"}).error.empty());
    CHECK(parse_command_line({L"--help"}).help);
}

void test_find_godot() {
    Scratch scratch;
    const fs::path dir = scratch.path() / "EAWR";
    CHECK(!find_godot(dir));
    write_file(dir / "Godot_v4.7.2-stable_win64_console.exe", "console runtime");
    CHECK(!find_godot(dir));   // never the console runtime
    write_file(dir / "Godot_v4.7.2-stable_mono_win64.exe", "other flavour");
    CHECK(!find_godot(dir));   // other file names are not searched any more
    write_file(scratch.path() / kGodotExecutableName, "parent folder");
    CHECK(!find_godot(dir));   // nor is the parent folder
    write_file(dir / kGodotExecutableName, "launcher folder");
    CHECK(find_godot(dir) == dir / kGodotExecutableName);
    write_file(dir / "godot" / kGodotExecutableName, "bundled");
    CHECK(find_godot(dir) == dir / "godot" / kGodotExecutableName);
    CHECK(godot_candidates(dir).size() == 2);
}

void test_file_sha256_streams() {
    Scratch scratch;
    std::string big((3u << 20) + 12345u, 'x');   // crosses the 1 MiB chunk edges
    for (std::size_t i = 0; i < big.size(); i += 4099) big[i] = static_cast<char>('a' + (i % 23));
    write_file(scratch.path() / "big.bin", big);
    CHECK(file_sha256(scratch.path() / "big.bin") == sha_of(big));
    write_file(scratch.path() / "empty.bin", "");
    CHECK(file_sha256(scratch.path() / "empty.bin") == sha_of(""));
    CHECK(!file_sha256(scratch.path() / "nothing.bin"));
}

std::string engine_file_text(const char* tag) { return std::string("engine ") + tag; }

void test_engine_match_has_no_flag() {
    FlowFixture f;
    Options options = f.options(); options.game_root = f.game;
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 1 && f.ui.errors.empty());
    CHECK(!has_flag(f.runner.args, L"--eawr-engine-unverified"));
    CHECK(f.log_text().find(kEngineUnpatchedCode) == std::string::npos);
}

void test_engine_mismatch_flags_and_still_launches() {
    FlowFixture f;
    write_file(f.package / "godot" / kGodotExecutableName, "an official build");
    Options options = f.options(); options.game_root = f.game;
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 1 && f.ui.errors.empty());   // never blocked, never a dialog
    CHECK(f.runner.exe == f.package / "godot" / kGodotExecutableName);
    CHECK(has_flag(f.runner.args, L"--eawr-engine-unverified"));
    const std::string log = f.log_text();
    CHECK_CONTAINS(log, kEngineUnpatchedCode);
    CHECK_CONTAINS(log, sha_of("an official build"));
    CHECK_CONTAINS(log, sha_of("godot stand-in"));
    CHECK_CONTAINS(log, path_to_utf8(f.package / "godot" / kGodotExecutableName));
}

void test_engine_missing_hash_flags() {
    {
        FlowFixture f;
        fs::remove(f.package / "runtime.json");
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitOk && f.runner.calls == 1 && f.ui.errors.empty());
        CHECK(has_flag(f.runner.args, L"--eawr-engine-unverified"));
        CHECK_CONTAINS(f.log_text(), kEngineUnpatchedCode);
        CHECK_CONTAINS(f.log_text(), "none");
    }
    for (const std::string& text : {std::string("not json"), std::string("{\"schema\":1}\n"),
                                   std::string("{\"godot_sha256\":\"abc\"}\n"), std::string("{\"godot_sha256\":7}\n"),
                                   std::string("[]")}) {
        FlowFixture f;
        write_file(f.package / "runtime.json", text);
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitOk && f.runner.calls == 1);
        CHECK(has_flag(f.runner.args, L"--eawr-engine-unverified"));
    }
    {   // an upper-case hash from another tool still matches
        FlowFixture f;
        std::string upper = sha_of("godot stand-in");
        for (char& c : upper) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        write_runtime_json(f.package, upper);
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitOk);
        CHECK(!has_flag(f.runner.args, L"--eawr-engine-unverified"));
    }
}

void test_engine_override_is_hashed_not_blocked() {
    {   // a matching override: no flag
        FlowFixture f;
        const fs::path other = f.scratch.path() / "tools" / "my godot.exe";
        write_file(other, "godot stand-in");
        Options options = f.options(); options.game_root = f.game; options.godot = other;
        CHECK(f.run(options) == kExitOk && f.runner.exe == other);
        CHECK(!has_flag(f.runner.args, L"--eawr-engine-unverified"));
    }
    {   // a different override: still used, flagged
        FlowFixture f;
        const fs::path other = f.scratch.path() / "tools" / "official.exe";
        write_file(other, engine_file_text("official"));
        Options options = f.options(); options.game_root = f.game; options.godot = other;
        CHECK(f.run(options) == kExitOk && f.runner.calls == 1 && f.runner.exe == other);
        CHECK(has_flag(f.runner.args, L"--eawr-engine-unverified"));
        CHECK_CONTAINS(f.log_text(), kEngineUnpatchedCode);
        CHECK_CONTAINS(f.log_text(), sha_of(engine_file_text("official")));
    }
    {   // an override beats a verified bundled engine, and a missing one is still the old error
        FlowFixture f;
        Options options = f.options(); options.game_root = f.game; options.godot = f.scratch.path() / "gone.exe";
        CHECK(f.run(options) == kExitError && f.runner.calls == 0);
    }
}

void test_engine_fallback_order() {
    {   // godot\ is wrong, the launcher folder holds the packaged one: the verified one wins
        FlowFixture f;
        write_file(f.package / "godot" / kGodotExecutableName, "official");
        write_file(f.package / kGodotExecutableName, "godot stand-in");
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.exe == f.package / kGodotExecutableName);
        CHECK(!has_flag(f.runner.args, L"--eawr-engine-unverified"));
    }
    {   // none matches: the first existing candidate (godot\) starts, flagged
        FlowFixture f;
        write_file(f.package / "godot" / kGodotExecutableName, "official one");
        write_file(f.package / kGodotExecutableName, "official two");
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitOk && f.runner.calls == 1);
        CHECK(f.runner.exe == f.package / "godot" / kGodotExecutableName);
        CHECK(has_flag(f.runner.args, L"--eawr-engine-unverified"));
    }
    {   // the parent folder and other Godot names are not candidates any more
        FlowFixture f;
        fs::remove(f.package / "godot" / kGodotExecutableName);
        write_file(f.scratch.path() / kGodotExecutableName, "godot stand-in");
        write_file(f.package / "Godot_v4.7.2-stable_mono_win64.exe", "godot stand-in");
        Options options = f.options(); options.game_root = f.game;
        CHECK(f.run(options) == kExitError && f.runner.calls == 0);
    }
}

void test_project_name_matches_godot_project(const fs::path& project_godot) {
    const std::string text = read_file(project_godot);
    CHECK(!text.empty());
    CHECK_CONTAINS(text, std::string("config/name=\"") + kViewerProjectName + "\"");
    CHECK(text.find("use_custom_user_dir") == std::string::npos);   // else user:// is not under app_userdata\<name>
}

// --- several instances (#5619, P1) --------------------------------------------------------------

std::vector<std::wstring> split_block(const std::wstring& block) {
    std::vector<std::wstring> entries;
    for (std::size_t i = 0; i < block.size() && block[i] != L'\0';) {
        entries.emplace_back(block.c_str() + i);
        i += entries.back().size() + 1;
    }
    return entries;
}

void test_instance_ids_paths_ports() {
    CHECK(parse_instance_id(L"1") == 1);
    CHECK(parse_instance_id(L"2") == 2);
    CHECK(parse_instance_id(L"8") == 8);
    for (const wchar_t* bad : {L"0", L"-3", L"abc", L"99", L"9", L"", L"2x", L" 2", L"+2", L"002x", L"1000"})
        CHECK(!parse_instance_id(bad));
    CHECK(instance_host_port(1) == 47624 && instance_host_port(2) == 47626 && instance_host_port(3) == 47628
          && instance_host_port(8) == 47638);
    const fs::path root = L"U:\\user data";
    CHECK(instance_root(root, 1) == root);
    CHECK(instance_launcher_dir(root, 1) == root / "launcher");   // instance 1 keeps today's place
    CHECK(instance_root(root, 3) == root / "instances" / "3");
    CHECK(instance_launcher_dir(root, 3) == root / "instances" / "3" / "launcher");
    CHECK(instance_appdata_dir(root, 3) == root / "instances" / "3" / "appdata");
    CHECK(instance_viewer_dir(root, 3) == root / "instances" / "3" / "appdata" / "Godot" / "app_userdata" / "EAWR Viewer");

    const Options two = parse_command_line({L"--instance", L"2", L"--profile", L"my-profile"});
    CHECK(two.error.empty() && two.instance == 2 && !two.instance_auto && two.profile == std::optional<std::string>("my-profile"));
    CHECK(parse_command_line({L"--instance", L"auto"}).instance_auto);
    for (const wchar_t* bad : {L"0", L"-3", L"abc", L"99"})
        CHECK(!parse_command_line({L"--instance", bad}).error.empty());
    CHECK(!parse_command_line({L"--instance"}).error.empty());
    CHECK(!parse_command_line({L"--profile"}).error.empty());
}

void test_environment_block() {
    const std::vector<std::wstring> env = {L"=C:=C:\\work", L"Path=C:\\Windows", L"appdata=C:\\old", L"NAME=\u65e5\u672c", L"APPDATAX=keep"};
    const std::wstring block = build_environment_block(env, L"APPDATA", L"D:\\inst 2\\appdata \u00e9");
    CHECK(block.size() >= 2 && block[block.size() - 1] == L'\0' && block[block.size() - 2] == L'\0');
    const auto entries = split_block(block);
    CHECK((entries == std::vector<std::wstring>{L"=C:=C:\\work", L"Path=C:\\Windows", L"APPDATA=D:\\inst 2\\appdata \u00e9",
                                                L"NAME=\u65e5\u672c", L"APPDATAX=keep"}));
    // Not present: appended. Present twice: one entry remains.
    const auto appended = split_block(build_environment_block({L"A=1"}, L"APPDATA", L"x"));
    CHECK((appended == std::vector<std::wstring>{L"A=1", L"APPDATA=x"}));
    const auto deduped = split_block(build_environment_block({L"AppData=1", L"APPDATA=2", L"B=3"}, L"APPDATA", L"x"));
    CHECK((deduped == std::vector<std::wstring>{L"APPDATA=x", L"B=3"}));
    CHECK(split_block(build_environment_block({}, L"APPDATA", L"x")).size() == 1);
}

void test_seeding() {
    // Pure text steps.
    CHECK(seed_settings_text("eawr-settings 1\nvideo.windowed=0\naudio.master=40\n", true) == "eawr-settings 1\nvideo.windowed=1\naudio.master=40\n");
    CHECK(seed_settings_text("eawr-settings 1\r\naudio.master=40\r\n", true) == "eawr-settings 1\naudio.master=40\nvideo.windowed=1\n");
    CHECK(seed_settings_text("", false) == "eawr-settings 1\nvideo.windowed=1\n");
    CHECK(seed_settings_text("garbage\nvideo.windowed=0\n", true) == "eawr-settings 1\nvideo.windowed=1\n");
    CHECK(seed_profile_text("eawr-profile 1\nplayer.name=Ann\nplayer.network_name=Annie\nplayer.lobby_colour=3\n", true, 2)
          == "eawr-profile 1\nplayer.name=Ann\nplayer.network_name=Annie #2\nplayer.lobby_colour=3\n");
    CHECK(seed_profile_text("eawr-profile 1\nplayer.name=Ann\n", true, 3) == "eawr-profile 1\nplayer.name=Ann\nplayer.network_name=Ann #3\n");
    CHECK(seed_profile_text("", false, 4) == "eawr-profile 1\nplayer.network_name=Player #4\n");
    CHECK(seed_profile_text("eawr-profile 1\nplayer.network_name=\n", true, 2) == "eawr-profile 1\nplayer.network_name=Player #2\n");
    const std::string long_name(40, 'x');
    const std::string seeded = seed_profile_text("eawr-profile 1\nplayer.network_name=" + long_name + "\n", true, 2);
    CHECK_CONTAINS(seeded, "player.network_name=" + std::string(29, 'x') + " #2\n");   // 32 characters in all
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC";   // two 3-byte characters: never cut inside one
    CHECK_CONTAINS(seed_profile_text("eawr-profile 1\nplayer.network_name=" + japanese + std::string(40, 'y') + "\n", true, 5),
                   "player.network_name=" + japanese + std::string(27, 'y') + " #5\n");

    // Files: only the two cfg files, never the other contents; existing instance files are never overwritten.
    Scratch scratch;
    const fs::path primary = scratch.path() / "primary user";
    const fs::path inst = scratch.path() / "inst";
    write_file(primary / "eawr-settings.cfg", "eawr-settings 1\nvideo.windowed=0\n");
    write_file(primary / "eawr-profile.cfg", "eawr-profile 1\nplayer.name=Bob\n");
    write_file(primary / "saves" / "slot.sav", "save");
    write_file(primary / "logs" / "godot.log", "log");
    std::string error;
    CHECK(seed_instance_files(primary, inst, 2, error) && error.empty());
    CHECK(read_file(inst / "eawr-settings.cfg") == "eawr-settings 1\nvideo.windowed=1\n");
    CHECK(read_file(inst / "eawr-profile.cfg") == "eawr-profile 1\nplayer.name=Bob\nplayer.network_name=Bob #2\n");
    CHECK(!fs::exists(inst / "saves") && !fs::exists(inst / "logs"));
    CHECK(!fs::exists(inst / "eawr-settings.cfg.tmp"));
    write_file(inst / "eawr-settings.cfg", "eawr-settings 1\nvideo.windowed=0\n");   // the player's own later choice
    CHECK(seed_instance_files(primary, inst, 2, error));
    CHECK(read_file(inst / "eawr-settings.cfg") == "eawr-settings 1\nvideo.windowed=0\n");
    // No primary files at all: minimal ones are made, so the instance is still windowed with its own name.
    CHECK(seed_instance_files(scratch.path() / "none", scratch.path() / "inst3", 3, error));
    CHECK(read_file(scratch.path() / "inst3" / "eawr-profile.cfg") == "eawr-profile 1\nplayer.network_name=Player #3\n");
}

bool all_live(unsigned, const std::optional<std::string>&) { return false; }
bool all_dead(unsigned, const std::optional<std::string>&) { return true; }
bool only_pid_100_is_dead(unsigned pid, const std::optional<std::string>&) { return pid == 100; }

void test_claims() {
    Scratch scratch;
    const fs::path data = scratch.path() / "user data";
    const ProcessIdentity a{1001, "11"}, b{1002, "22"}, c{1003, "33"};
    const fs::path claim2 = data / "instances" / "2" / ".claim";

    CHECK(claim_instance(data, 2, a, all_live).status == ClaimStatus::claimed);
    CHECK(read_file(claim2) == "{\"pid\":1001,\"process_start_time\":\"11\"}\n");
    CHECK(claim_instance(data, 2, a, all_live).status == ClaimStatus::mine);
    const ClaimResult held = claim_instance(data, 2, b, all_live);
    CHECK(held.status == ClaimStatus::held && held.owner_pid == 1001 && !held.ok());
    CHECK(read_file(claim2).find("1001") != std::string::npos);   // the loser did not touch it

    release_claim(data, 2, b);   // not the owner: no effect
    CHECK(fs::exists(claim2));
    release_claim(data, 2, a);
    CHECK(!fs::exists(claim2));
    CHECK(!fs::exists(fs::path(claim2) += ".1001.tmp"));

    // Stale: dead owner, unreadable, empty or incomplete files are reused, never a crash.
    write_file(claim2, "{\"pid\":100}\n");
    CHECK(claim_instance(data, 2, b, only_pid_100_is_dead).status == ClaimStatus::claimed_after_stale);
    CHECK(read_file(claim2).find("1002") != std::string::npos);
    for (const char* bad : {"", "not json", "{\"pid\":\"7\"}", "{\"pid\":0}", "{\"pid\":-4}", "[1]", "{\"pid\":7,\"process_start_time\":5}", "{\"pid\":1.5}"}) {
        write_file(claim2, bad);
        CHECK(claim_instance(data, 2, c, all_live).status == ClaimStatus::claimed_after_stale);
        CHECK(read_file(claim2).find("1003") != std::string::npos);
    }
    // A live owner is not displaced.
    CHECK(claim_instance(data, 2, a, all_live).status == ClaimStatus::held);

    // PID reuse: the real check compares the creation time (this process with a wrong start time is "another process").
    const ProcessIdentity self = current_process_identity();
    CHECK(self.pid != 0 && !self.start_time.empty());
    CHECK(!process_gone(self.pid, std::nullopt));
    CHECK(!process_gone(self.pid, self.start_time));
    CHECK(process_gone(self.pid, std::string("1")));
    write_file(claim2, "{\"pid\":" + std::to_string(self.pid) + ",\"process_start_time\":\"1\"}\n");
    CHECK(claim_instance(data, 2, ProcessIdentity{1004, "44"}).status == ClaimStatus::claimed_after_stale);
    CHECK(process_gone(0x3FFFFFF0u, std::nullopt));   // no such process

    // Lowest free id, gaps reused, a cap, nobody gets a held slot.
    Scratch gaps;
    const fs::path d = gaps.path();
    CHECK(claim_instance(d, 2, a, all_live).ok() && claim_instance(d, 4, a, all_live).ok());
    ClaimResult how;
    CHECK(claim_next_free(d, b, kMaxInstanceId, &how, all_live) == 3 && how.status == ClaimStatus::claimed);
    CHECK(claim_next_free(d, c, kMaxInstanceId, nullptr, all_live) == 5);
    CHECK(claim_next_free(d, ProcessIdentity{1005, "5"}, 4, nullptr, all_live) == std::nullopt);
    CHECK(claim_next_free(d, ProcessIdentity{1006, "6"}, kMaxInstanceId, nullptr, all_dead) == 2);   // everything stale
}

void test_claim_concurrency() {
    Scratch scratch;
    const fs::path data = scratch.path();
    constexpr int kThreads = 6;
    std::vector<std::optional<int>> got(kThreads);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!go) std::this_thread::yield();
            got[static_cast<std::size_t>(i)] = claim_next_free(data, ProcessIdentity{2000u + static_cast<unsigned>(i), "1"}, kMaxInstanceId, nullptr, all_live);
        });
    }
    go = true;
    for (std::thread& thread : threads) thread.join();
    std::set<int> ids;
    for (const auto& id : got) {
        CHECK(id.has_value());
        if (id) ids.insert(*id);
    }
    CHECK(ids.size() == kThreads);   // exclusive create: no two instances share an id
    for (const int id : ids) CHECK(id >= 2 && id < 2 + kThreads);   // no gaps either
}

void test_launch_plan_instance_flags() {
    LaunchInputs inputs;
    inputs.project = L"P";
    inputs.game_root = L"G";
    inputs.report = L"R";
    const auto plain = godot_arguments(inputs);
    for (const wchar_t* flag : {L"--eawr-save-dir", L"--eawr-font-cache", L"--eawr-lan-game-port"})
        CHECK(std::find(plain.begin(), plain.end(), std::wstring(flag)) == plain.end());   // instance 1 is unchanged
    inputs.mod_profile = fs::path(L"M\\p.json");
    inputs.save_dir = fs::path(L"S\\saves");
    inputs.font_cache = fs::path(L"S\\fonts");
    inputs.lan_game_port = 47626;
    inputs.extra = {L"--x"};
    const auto args = godot_arguments(inputs);
    const std::vector<std::wstring> tail = {L"--eawr-report", L"R", L"--eawr-mod-profile", L"M\\p.json", L"--eawr-save-dir", L"S\\saves",
                                            L"--eawr-font-cache", L"S\\fonts", L"--eawr-lan-game-port", L"47626", L"--x"};
    CHECK(args.size() > tail.size() && std::equal(tail.begin(), tail.end(), args.end() - static_cast<std::ptrdiff_t>(tail.size())));
}

const char* kSampleProfile = R"({"format":"eawr-mod-profile","version":1,"id":"sample","launch":{"render_thread":"separate"}})";

void test_flow_instance_two() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.instance = 2;
    options.profile = "sample";
    write_file(f.user_data / "mods/profiles/sample.json", kSampleProfile);
    write_file(f.viewer_data / "eawr-settings.cfg", "eawr-settings 1\nvideo.windowed=0\n");
    write_file(f.viewer_data / "eawr-profile.cfg", "eawr-profile 1\nplayer.name=Cy\n");
    // Files of instance 1 that instance 2 must never touch or delete.
    const fs::path primary_launcher = f.user_data / "launcher";
    write_file(primary_launcher / "next-action.json", "{\"action\":\"open-manager\"}");
    write_file(primary_launcher / "front-end-report.json", "{\"status\":\"ok\"}");
    write_file(primary_launcher / "game-running.json", "{\"profile\":\"one\",\"pid\":4}");   // PID 4: alive
    write_file(primary_launcher / "launch-result.json", "{\"status\":\"finished\"}");
    const fs::path inst = f.user_data / "instances" / "2";
    const fs::path state = inst / "launcher";

    f.runner.on_monitored = [&](const auto& poll, const auto& started) {
        CHECK(!poll);
        CHECK(started && started(4321));
        const std::string running = read_file(state / "game-running.json");
        CHECK_CONTAINS(running, "\"profile\":\"sample\"");
        CHECK_CONTAINS(running, "\"pid\":4321");
        CHECK_CONTAINS(running, "\"instance\":2");
        CHECK_CONTAINS(running, "\"host_port\":47626");
        CHECK(fs::exists(inst / ".claim"));
        CHECK(f.runner.current_appdata == inst / "appdata");
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.ui.errors.empty());
    CHECK(f.runner.calls == 1);
    CHECK(!f.runner.current_appdata);   // the redirect does not outlive the game start
    CHECK(f.runner.appdata_at_start == std::vector<std::optional<fs::path>>{inst / "appdata"});

    // The golden flag set for instance 2 (after the fixed front-end flags).
    const auto& args = f.runner.args;
    const auto flag_value = [&](const wchar_t* flag) -> std::wstring {
        const auto it = std::find(args.begin(), args.end(), std::wstring(flag));
        return it == args.end() || it + 1 == args.end() ? std::wstring(L"<missing>") : *(it + 1);
    };
    CHECK(flag_value(L"--eawr-save-dir") == (f.viewer_data / "saves").wstring());
    CHECK(flag_value(L"--eawr-font-cache") == (f.viewer_data / "fonts").wstring());
    CHECK(flag_value(L"--eawr-lan-game-port") == L"47626");
    CHECK(fs::path(flag_value(L"--eawr-mod-profile")) == f.user_data / "mods/profiles/sample.json");
    CHECK(flag_value(L"--eawr-report") == (state / "front-end-report.json").wstring());
    CHECK(std::count(args.begin(), args.end(), std::wstring(L"--eawr-save-dir")) == 1);

    // Own files: state published and cleared, result written, claim released, files seeded.
    CHECK(!fs::exists(state / "game-running.json") && !fs::exists(inst / ".claim"));
    const std::string result = read_file(state / "launch-result.json");
    CHECK_CONTAINS(result, "\"status\":\"finished\"");
    CHECK_CONTAINS(result, "\"instance\":2");
    const fs::path viewer2 = inst / "appdata/Godot/app_userdata/EAWR Viewer";
    CHECK(read_file(viewer2 / "eawr-settings.cfg") == "eawr-settings 1\nvideo.windowed=1\n");
    CHECK_CONTAINS(read_file(viewer2 / "eawr-profile.cfg"), "player.network_name=Cy #2\n");
    CHECK(read_file(f.viewer_data / "eawr-settings.cfg") == "eawr-settings 1\nvideo.windowed=0\n");   // the primary is untouched

    // Instance 1's files are exactly as before.
    CHECK(read_file(primary_launcher / "next-action.json") == "{\"action\":\"open-manager\"}");
    CHECK(read_file(primary_launcher / "front-end-report.json") == "{\"status\":\"ok\"}");
    CHECK(read_file(primary_launcher / "game-running.json") == "{\"profile\":\"one\",\"pid\":4}");
    CHECK(read_file(primary_launcher / "launch-result.json") == "{\"status\":\"finished\"}");
    CHECK(load_state(f.user_data).last_profile == std::optional<std::string>("sample"));
}

void test_flow_instance_vanilla_and_crash() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.instance = 3;
    const fs::path inst = f.user_data / "instances" / "3";
    f.runner.result.exit_code = 7;
    f.runner.result.seconds = 500;   // a crash after a long session: nothing to explain
    f.runner.on_monitored = [&](const auto&, const auto& started) {
        CHECK(started && started(77));
        CHECK_CONTAINS(read_file(inst / "launcher/game-running.json"), "\"profile\":\"\"");
    };
    CHECK(f.run(options) == 7);   // the game's exit code
    CHECK(!fs::exists(inst / "launcher/game-running.json"));
    CHECK(!fs::exists(inst / ".claim"));
    CHECK_CONTAINS(read_file(inst / "launcher/launch-result.json"), "\"status\":\"failed\"");
    CHECK_CONTAINS(read_file(inst / "launcher/launch-result.json"), "\"exit_code\":7");
    CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), std::wstring(L"--eawr-mod-profile")) == f.runner.args.end());
    CHECK(f.runner.appdata_at_start.size() == 1 && f.runner.appdata_at_start[0] == inst / "appdata");
    CHECK(!fs::exists(f.user_data / "launcher/game-running.json"));   // nothing written into instance 1's folder
    CHECK(!fs::exists(f.user_data / "launcher/launch-result.json"));
}

void test_flow_instance_one_is_unchanged() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.instance = 1;
    f.runner.on_run = [&] {
        CHECK(!f.runner.current_appdata);
        for (const wchar_t* flag : {L"--eawr-save-dir", L"--eawr-font-cache", L"--eawr-lan-game-port"})
            CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), std::wstring(flag)) == f.runner.args.end());
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 1);
    CHECK(!fs::exists(f.user_data / "instances"));
    CHECK(f.runner.appdata_at_start.empty() || !f.runner.appdata_at_start[0]);
}

void test_flow_instance_claim_conflicts() {
    {   // A live owner (PID 4) holds slot 2: refuse readably, touch nothing of the owner's.
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.instance = 2;
        write_file(f.user_data / "instances/2/.claim", "{\"pid\":4}\n");
        write_file(f.user_data / "instances/2/launcher/game-running.json", "{\"profile\":\"x\",\"pid\":4}");
        CHECK(f.run(options) == kExitError);
        CHECK(f.runner.calls == 0);
        CHECK(f.ui.errors.size() == 1);
        if (!f.ui.errors.empty()) CHECK_CONTAINS(f.ui.errors[0], "Instance 2 is already running");
        CHECK(read_file(f.user_data / "instances/2/.claim") == "{\"pid\":4}\n");
        CHECK(fs::exists(f.user_data / "instances/2/launcher/game-running.json"));
    }
    {   // A dead owner's claim and running state are reused; the log says so once.
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.instance = 2;
        const ProcessIdentity self = current_process_identity();
        write_file(f.user_data / "instances/2/.claim", "{\"pid\":" + std::to_string(self.pid) + ",\"process_start_time\":\"1\"}\n");
        write_file(f.user_data / "instances/2/launcher/game-running.json", "{\"profile\":\"x\",\"pid\":1}");
        f.runner.on_monitored = [&](const auto&, const auto& started) {
            CHECK(!fs::exists(f.user_data / "instances/2/launcher/game-running.json"));
            CHECK(started && started(5));
        };
        CHECK(f.run(options) == kExitOk);
        CHECK(f.runner.calls == 1);
        const std::string log = f.log_text();
        CHECK_CONTAINS(log, "reused a stale slot claim");
        CHECK_CONTAINS(log, "removed stale game-running state");
        CHECK(!fs::exists(f.user_data / "instances/2/.claim"));
    }
    {   // A missing or invalid --profile stops before any start; --profile and --mods do not mix.
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.instance = 2;
        options.profile = "nope";
        CHECK(f.run(options) == kExitError);
        CHECK(f.runner.calls == 0 && !f.ui.errors.empty());
        CHECK(!fs::exists(f.user_data / "instances/2/.claim"));
        options.profile = "../evil";
        CHECK(f.run(options) == kExitError);
        options.profile = "sample";
        options.mods = true;
        CHECK(f.run(options) == kExitError);
        CHECK(f.runner.calls == 0);
    }
}

fs::path g_race_claim;

// While process B judges the claim stale (this runs inside its liveness check), process A takes the slot first
// and publishes a live claim: the interleaving a check-then-remove takeover gets wrong.
bool gone_while_a_competitor_takes_the_slot(unsigned pid, const std::optional<std::string>&) {
    if (pid != 100) return false;
    std::error_code error;
    fs::remove(g_race_claim, error);
    write_file(g_race_claim, "{\"pid\":1001,\"process_start_time\":\"11\"}\n");
    return true;
}

void test_stale_takeover_never_removes_a_live_claim() {
    Scratch scratch;
    g_race_claim = scratch.path() / "instances" / "2" / ".claim";
    write_file(g_race_claim, "{\"pid\":100}\n");
    const ClaimResult result = claim_instance(scratch.path(), 2, ProcessIdentity{1002, "22"}, gone_while_a_competitor_takes_the_slot);
    CHECK(result.status == ClaimStatus::held && result.owner_pid == 1001);
    CHECK(read_file(g_race_claim) == "{\"pid\":1001,\"process_start_time\":\"11\"}\n");   // A still owns it
    CHECK(!fs::exists(fs::path(g_race_claim) += ".takeover") && !fs::exists(fs::path(g_race_claim) += ".1002.tmp"));
    // A plain stale takeover leaves nothing behind either.
    write_file(g_race_claim, "{\"pid\":100}\n");
    CHECK(claim_instance(scratch.path(), 2, ProcessIdentity{1003, "33"}, only_pid_100_is_dead).status == ClaimStatus::claimed_after_stale);
    CHECK(!fs::exists(fs::path(g_race_claim) += ".takeover"));
}

// The takeover lock is an OS handle: another launcher holding it (here the test, with the same share mode 0 open) makes
// a taker wait and finally give up, without touching or hiding the claim; a leftover lock FILE that nobody holds (a
// crashed launcher) blocks nothing, so there is no stale lock to clear by name.
struct HeldTakeoverLock {
    HANDLE handle;
    explicit HeldTakeoverLock(const fs::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr)) {}
    ~HeldTakeoverLock() { release(); }
    void release() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    HeldTakeoverLock(const HeldTakeoverLock&) = delete;
    HeldTakeoverLock& operator=(const HeldTakeoverLock&) = delete;
};

void test_takeover_lock() {
    Scratch scratch;
    const fs::path claim = scratch.path() / "instances" / "2" / ".claim";
    const fs::path lock = fs::path(claim) += ".takeover";
    write_file(claim, "{\"pid\":100}\n");
    {
        HeldTakeoverLock held(lock);
        CHECK(held.handle != INVALID_HANDLE_VALUE);
        const ClaimResult busy = claim_instance(scratch.path(), 2, ProcessIdentity{1004, "44"}, only_pid_100_is_dead);
        CHECK(busy.status == ClaimStatus::held && busy.owner_pid == 0);   // gave up waiting; not "process 0 owns it"
        CHECK(read_file(claim) == "{\"pid\":100}\n");                     // untouched, still in place
        CHECK(fs::exists(lock));                                          // and the holder's lock was not deleted by name
    }
    CHECK(!fs::exists(lock));   // delete-on-close: nothing is left when the holder is gone
    CHECK(claim_instance(scratch.path(), 2, ProcessIdentity{1005, "55"}, only_pid_100_is_dead).status == ClaimStatus::claimed_after_stale);
    CHECK(read_file(claim).find("1005") != std::string::npos);
    // A lock file nobody holds (left by a crash) blocks nothing.
    write_file(claim, "{\"pid\":100}\n");
    write_file(lock, "{\"pid\":100}\n");
    CHECK(claim_instance(scratch.path(), 2, ProcessIdentity{1006, "66"}, only_pid_100_is_dead).status == ClaimStatus::claimed_after_stale);
    write_file(claim, "{\"pid\":100}\n");
    write_file(lock, "");
    CHECK(claim_instance(scratch.path(), 2, ProcessIdentity{1007, "77"}, only_pid_100_is_dead).status == ClaimStatus::claimed_after_stale);
}

// Many launchers take over the same stale claim at once: exactly one gets the slot.
// One round of eight launchers taking over the same stale claim at once: exactly one gets the slot and the others
// name it. `under_load`: CPU spinners at normal priority plus claim threads at below-normal priority, to widen the
// interleavings. Returns whether every check of the round held; a failing round prints what it saw.
bool takeover_round(bool under_load) {
    const int failures_before = g_failures;
    Scratch scratch;
    const fs::path claim = scratch.path() / "instances" / "2" / ".claim";
    write_file(claim, "{\"pid\":100}\n");
    constexpr int kThreads = 8;
    std::atomic<bool> go{false};
    std::atomic<bool> stop_load{false};
    std::atomic<int> winners{0};
    std::vector<unsigned> owners(kThreads);
    std::vector<int> statuses(kThreads);
    std::vector<std::thread> spinners;
    if (under_load) {
        for (unsigned i = 0; i < std::max(2u, std::thread::hardware_concurrency()); ++i)
            spinners.emplace_back([&] { while (!stop_load) {} });
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            if (under_load) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!go) std::this_thread::yield();
            // A launcher that gives up waiting for the takeover lock (owner 0) simply starts again, as a user would;
            // the check below is about who owns the slot, not about how long one attempt may wait under load.
            ClaimResult result;
            for (int attempt = 0; attempt < 30; ++attempt) {
                result = claim_instance(scratch.path(), 2, ProcessIdentity{3000u + static_cast<unsigned>(i), "1"}, only_pid_100_is_dead);
                if (result.ok() || result.owner_pid != 0) break;
            }
            if (result.ok()) ++winners;
            owners[static_cast<std::size_t>(i)] = result.owner_pid;
            statuses[static_cast<std::size_t>(i)] = static_cast<int>(result.status);
        });
    }
    go = true;
    for (std::thread& thread : threads) thread.join();
    stop_load = true;
    for (std::thread& spinner : spinners) spinner.join();
    CHECK(winners == 1);
    const std::string text = read_file(claim);
    unsigned winner = 0;
    for (int i = 0; i < kThreads; ++i)
        if (text.find("\"pid\":" + std::to_string(3000 + i) + ",") != std::string::npos) winner = 3000u + static_cast<unsigned>(i);
    CHECK(winner != 0);
    for (int i = 0; i < kThreads; ++i)
        if (3000u + static_cast<unsigned>(i) != winner) CHECK(owners[static_cast<std::size_t>(i)] == winner);   // the losers all name the winner
    CHECK(!fs::exists(fs::path(claim) += ".takeover"));
    const bool held = g_failures == failures_before;
    if (!held) {
        std::cerr << "takeover round failed (under_load=" << under_load << "): winners=" << winners << " claim=" << text << "\n";
        for (int i = 0; i < kThreads; ++i)
            std::cerr << "  launcher " << 3000 + i << ": status " << statuses[static_cast<std::size_t>(i)] << " (0 claimed, 1 after stale, 2 mine, 3 held), owner "
                      << owners[static_cast<std::size_t>(i)] << "\n";
    }
    return held;
}

void test_concurrent_takeovers_have_one_winner() {
    for (int round = 0; round < 5; ++round) takeover_round(round % 2 == 1);
}

// `launcher_tests --soak <n>`: n rounds, every other one under CPU load; prints the pass count.
int takeover_soak(int rounds) {
    int passed = 0;
    for (int round = 0; round < rounds; ++round)
        if (takeover_round(round % 2 == 1)) ++passed;
    std::cout << "takeover soak: " << passed << "/" << rounds << " rounds passed (" << (rounds + 1) / 2 << " without load, "
              << rounds / 2 << " under CPU load with below-normal claim threads)\n";
    return passed == rounds ? 0 : 1;
}

void test_flow_claim_busy_message() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.instance = 2;
    write_file(f.user_data / "instances/2/.claim", "{\"pid\":4000000000}\n");   // stale (no such process) ...
    HeldTakeoverLock held(f.user_data / "instances/2/.claim.takeover");        // ... but another launcher is taking it over
    CHECK(f.run(options) == kExitError);
    CHECK(f.runner.calls == 0 && f.ui.errors.size() == 1);
    if (!f.ui.errors.empty()) {
        CHECK_CONTAINS(f.ui.errors[0], "being started by another launcher");
        CHECK(f.ui.errors[0].find("process 0") == std::string::npos);
    }
}

void test_flow_early_failures_release_the_claim() {
    // `--instance auto` claims the slot in wWinMain before run_launcher; every way out must free it.
    const auto expect_released = [](const char* label, const auto& configure) {
        FlowFixture f;
        auto options = f.options();
        options.game_root = f.game;
        options.instance = 2;
        configure(options);
        CHECK(claim_instance(f.user_data, 2, current_process_identity()).ok());
        const fs::path claim = f.user_data / "instances/2/.claim";
        CHECK(fs::exists(claim));
        CHECK(f.run(options) == kExitError);
        if (fs::exists(claim)) std::cerr << "claim still held after: " << label << "\n";
        CHECK(!fs::exists(claim));
        CHECK(f.runner.calls == 0);
    };
    expect_released("missing profile", [](Options& o) { o.profile = "nope"; });
    expect_released("profile with --mods", [](Options& o) { o.profile = "x"; o.mods = true; });
    expect_released("missing Godot runtime", [](Options& o) { o.godot = fs::path(L"no such godot.exe"); });
}

void test_flow_instance_failure_dialog_names_its_own_log() {
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.instance = 2;
    f.runner.result.exit_code = 5;
    f.runner.result.seconds = 3;   // right after starting: the dialog is shown
    f.runner.on_monitored = [&](const auto&, const auto& started) { CHECK(started && started(8)); };
    CHECK(f.run(options) == 5);
    CHECK(f.ui.errors.size() == 1);
    const std::string own = path_to_utf8(instance_viewer_dir(f.user_data, 2) / "logs" / "godot.log");
    if (!f.ui.errors.empty()) {
        CHECK_CONTAINS(f.ui.errors[0], own);
        CHECK(f.ui.errors[0].find(path_to_utf8(f.user_data / "logs" / "godot.log")) == std::string::npos);
    }
}

void test_flow_instance_with_manager() {
    // A second launcher window: its manager runs in the normal environment, only its games get the redirect.
    FlowFixture f;
    auto options = f.options();
    options.game_root = f.game;
    options.mods = true;
    options.instance = 2;
    write_file(f.package / "launcher/project.godot", "manager");
    write_file(f.package / "eawr_mods_service.exe", "service");
    write_file(f.user_data / "mods/profiles/sample.json", kSampleProfile);
    const fs::path inst = f.user_data / "instances" / "2";
    write_file(f.user_data / "launcher/next-action.json", "{\"action\":\"open-manager\"}");   // the first window's
    f.runner.on_monitored = [&](const auto& poll, const auto& started) {
        if (poll) {
            CHECK(!f.runner.current_appdata);   // the manager child
            // The manager of window 1 and window 2 use different request files.
            CHECK(std::any_of(f.runner.args.begin(), f.runner.args.end(),
                              [&](const std::wstring& arg) { return fs::path(arg) == inst / "launcher/next-action.json"; }));
            write_file(inst / "launcher/next-action.json", R"({"action":"launch-profile","profile":"sample","stay_open":true})");
            poll();
            CHECK(!f.runner.current_appdata);
            CHECK(!fs::exists(inst / "launcher/game-running.json"));
        } else {
            CHECK(started && started(99));
            CHECK(f.runner.current_appdata == inst / "appdata");
            CHECK_CONTAINS(read_file(inst / "launcher/game-running.json"), "\"instance\":2");
            CHECK(std::find(f.runner.args.begin(), f.runner.args.end(), std::wstring(L"--eawr-lan-game-port")) != f.runner.args.end());
        }
    };
    CHECK(f.run(options) == kExitOk);
    CHECK(f.runner.calls == 2);
    CHECK(read_file(f.user_data / "launcher/next-action.json") == "{\"action\":\"open-manager\"}");
    CHECK_CONTAINS(read_file(inst / "launcher/launch-result.json"), "\"status\":\"finished\"");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--soak") return takeover_soak(std::atoi(argv[2]));
    CHECK(mods_start_mode({}) == ModsStartMode::launch);
    const std::array<std::wstring_view, 1> shortcut{L"--create-shortcut"};
    const std::array<std::wstring_view, 1> bad{L"--mods"};
    const std::array<std::wstring_view, 2> extra{L"--create-shortcut", L"extra"};
    CHECK(mods_start_mode(shortcut) == ModsStartMode::create_shortcut);
    CHECK(mods_start_mode(bad) == ModsStartMode::invalid);
    CHECK(mods_start_mode(extra) == ModsStartMode::invalid);
    CHECK(mods_launcher_path(L"X:/portable kit/EAWR Mods.exe") == fs::path(L"X:/portable kit/EAWR.exe"));
    CHECK(mods_launcher_path(L"Y:/moved kit/EAWR Mods.exe") == fs::path(L"Y:/moved kit/EAWR.exe"));

    test_validation();
    test_steam_modern_and_legacy();
    test_steam_manifest_name_must_be_one_folder();
    test_steam_without_manifest_scans_common();
    test_gog_records();
    test_nothing_found();
    test_two_stores_two_installs();
    test_state();
    test_launch_plan();
    test_flow_explicit_game_root();
    test_render_thread_precedence();
    test_depth_prepass_precedence();
    test_flow_mods();
    test_flow_launch_loop();
    test_stale_running_state_at_start();
    test_flow_stay_open();
    test_flow_stay_open_failures();
    test_flow_launch_loop_invalid();
    test_flow_launch_loop_profile_traversal();
    test_flow_launch_loop_explicit_safe_render_thread();
    test_flow_launch_loop_profile_beats_saved_render_thread();
    test_flow_launch_loop_guard_and_failure();
    test_flow_launch_loop_long_sessions_and_unclearable_request();
    test_flow_explicit_game_root_invalid();
    test_flow_saved_choice_revalidated();
    test_flow_one_discovered_install_is_used_silently();
    test_flow_several_installs_ask();
    test_flow_browse_after_nothing_found();
    test_flow_missing_runtime_or_project();
    test_flow_godot_failures();
    test_flow_unknown_build_flag_and_the_viewers_explanation();
    test_flow_unwritable_user_data_and_bad_command_line();
    test_command_line_parsing();
    test_instance_ids_paths_ports();
    test_environment_block();
    test_seeding();
    test_claims();
    test_claim_concurrency();
    test_launch_plan_instance_flags();
    test_flow_instance_two();
    test_flow_instance_vanilla_and_crash();
    test_flow_instance_one_is_unchanged();
    test_flow_instance_claim_conflicts();
    test_flow_instance_with_manager();
    test_stale_takeover_never_removes_a_live_claim();
    test_flow_early_failures_release_the_claim();
    test_takeover_lock();
    test_concurrent_takeovers_have_one_winner();
    test_flow_claim_busy_message();
    test_flow_instance_failure_dialog_names_its_own_log();
    test_find_godot();
    test_file_sha256_streams();
    test_engine_match_has_no_flag();
    test_engine_mismatch_flags_and_still_launches();
    test_engine_missing_hash_flags();
    test_engine_override_is_hashed_not_blocked();
    test_engine_fallback_order();
    if (argc > 1) test_project_name_matches_godot_project(argv[1]);
    if (g_failures != 0) {
        std::cerr << g_failures << " launcher check(s) failed\n";
        return 1;
    }
    std::cout << "launcher_tests: all checks passed\n";
    return 0;
}
