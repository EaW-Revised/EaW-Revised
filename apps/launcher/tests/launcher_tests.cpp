// Unit tests of the launcher core: text formats, discovery (VDF + registry fixtures), validation,
// the saved choice, the Godot command line and the whole flow against a
// fake UI and a fake process runner. No game, no Godot, no retail bytes: fixtures are empty folders
// and a few made-up bytes.
#include "install_discovery.hpp"
#include "launch_plan.hpp"
#include "launcher_flow.hpp"
#include "launcher_state.hpp"
#include "text_formats.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>

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

// --- text formats ---------------------------------------------------------------------------

void test_json() {
    const auto json = parse_json("\xEF\xBB\xBF{\"a\": \"caf\\u00e9 \\ud83d\\ude00\", \"n\": -12.5e1, \"b\": true, \"z\": null,"
                                 " \"l\": [1, {\"k\": \"v\"}], \"s\": \"x\\\\y\\\"q\\n\"}");
    CHECK(json.has_value());
    if (!json) return;
    CHECK(*json->string_member("a") == "caf\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(json->member("n")->number == -125.0);
    CHECK(json->member("b")->boolean);
    CHECK(json->member("z")->kind == JsonValue::Kind::null);
    CHECK(json->member("l")->array.size() == 2);
    CHECK(*json->member("l")->array[1].string_member("k") == "v");
    CHECK(*json->string_member("s") == "x\\y\"q\n");
    CHECK(json->member("missing") == nullptr);
    CHECK(!parse_json("{\"a\": }"));
    CHECK(!parse_json("{\"a\": 1} trailing"));
    CHECK(!parse_json("{\"a\": \"\\ud83d\"}"));   // lone surrogate
    CHECK(!parse_json(""));
    CHECK(json_quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    const auto round = parse_json(json_quote("caf\xC3\xA9 \"x\"\\ \t"));
    CHECK(round && round->string == "caf\xC3\xA9 \"x\"\\ \t");
}

void test_vdf() {
    const auto modern = parse_vdf("// comment\n\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                                  "\t\t\"apps\" { \"32470\" \"1234\" }\n\t}\n\t\"1\" { \"path\" \"D:\\\\Spiele \xC3\xBC\" }\n}\n");
    CHECK(modern.has_value());
    if (!modern) return;
    const VdfNode* folders = modern->child("LibraryFolders");
    CHECK(folders != nullptr && folders->children.size() == 2);
    CHECK(folders->children[0].child("path")->value == "C:\\Program Files (x86)\\Steam");
    CHECK(folders->children[0].child("apps")->child("32470")->value == "1234");
    CHECK(folders->children[1].child("path")->value == "D:\\Spiele \xC3\xBC");
    const auto unknown_escape = parse_vdf("\"a\" \"C:\\Umlaut\\x\"");
    CHECK(unknown_escape && unknown_escape->child("a")->value == "C:\\Umlaut\\x");
    const auto bare = parse_vdf("key value\nother \"two words\"");
    CHECK(bare && bare->child("key")->value == "value" && bare->child("other")->value == "two words");
    CHECK(!parse_vdf("\"a\" {"));
    CHECK(!parse_vdf("\"a\" \"b\" }"));
    CHECK(!parse_vdf("\"unterminated"));
}

void test_paths() {
    const fs::path path = path_from_utf8("C:\\Spiele\\caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(path.wstring() == L"C:\\Spiele\\caf\u00e9 \u65e5\u672c");
    CHECK(path_to_utf8(path) == "C:\\Spiele\\caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(path_from_utf8("bad \xC3\x28 utf8").empty());
    CHECK(path_from_utf8("surrogate \xED\xA0\x80").empty());
    CHECK(lower_ascii("GameDATA") == "gamedata");
    Scratch scratch;
    fs::create_directories(scratch.path() / "Corruption");
    const auto child = find_child_ci(scratch.path(), "corruption");
    CHECK(child && fs::is_directory(*child));   // Windows resolves the case itself; elsewhere the scan does
    CHECK(!find_child_ci(scratch.path(), "gamedata"));
    CHECK(same_location(scratch.path() / "corruption", scratch.path() / "CORRUPTION" / ".." / "Corruption"));
    CHECK(!same_location(scratch.path() / "a", scratch.path() / "b"));
}

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
                                                L"--eawr-front-end", L"--eawr-report", inputs.report.wstring()};
    CHECK(args == expected);
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

    ChildResult run(const fs::path& program, const std::vector<std::wstring>& arguments, const fs::path& working_directory) override {
        ++calls;
        exe = program;
        args = arguments;
        cwd = working_directory;
        if (on_run) on_run();
        ChildResult copy = result;
        copy.started = copy.error.empty();
        return copy;
    }
};

// A package folder (Godot stand-in + project), user data and a fixture game.
struct FlowFixture {
    Scratch scratch;
    fs::path package = scratch.path() / L"EAWR caf\u00e9";
    fs::path user_data = scratch.path() / "user data";
    fs::path game = make_install(scratch.path() / "Games" / L"Star Wars \u65e5\u672c");
    FakeRegistry registry;
    FakeUi ui;
    FakeRunner runner;
    Log log{user_data / "launcher" / "launcher.log"};

    FlowFixture() {
        write_file(package / "godot" / kGodotExecutableName, "godot stand-in");
        write_file(package / "project" / "project.godot", "config_version=5\n");
    }
    Options options() const {
        Options options;
        options.launcher_dir = package;
        options.user_data = user_data;
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
    CHECK(args.size() == 8 && args[0] == L"--path" && args[1] == (f.package / "project").wstring() && args[2] == L"--");
    CHECK(args[3] == L"--eawr-game-root" && same_location(fs::path(args[4]), f.game));
    CHECK(args[5] == L"--eawr-front-end" && args[6] == L"--eawr-report");
    CHECK(fs::path(args[7]).parent_path() == f.user_data / "launcher");
    for (const std::wstring& argument : args) CHECK(argument.find(L"console") == std::wstring::npos && argument.find(L"font") == std::wstring::npos);
    CHECK(!load_state(f.user_data).game_root);   // an explicit folder is a one-off: not remembered
    CHECK(f.ui.picks.empty());
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
                                                L"--choose-game", L"--no-dialogs", L"--user-data", L"U:\\data", L"--godot", L"G.exe",
                                                L"--project", L"P", L"--eawr-audio", L"off", L"--", L"--eawr-hud", L"off"});
    CHECK(options.error.empty());
    CHECK(options.game_root == fs::path(L"D:\\Games\\EaW"));
    CHECK(options.quit_after == 30);
    CHECK(options.allow_unknown_build && options.choose_game && options.no_dialogs);
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
    CHECK(find_godot(dir) == dir / "Godot_v4.7.2-stable_mono_win64.exe");
    write_file(dir / "godot" / kGodotExecutableName, "bundled");
    CHECK(find_godot(dir) == dir / "godot" / kGodotExecutableName);
}

void test_project_name_matches_godot_project(const fs::path& project_godot) {
    const std::string text = read_file(project_godot);
    CHECK(!text.empty());
    CHECK_CONTAINS(text, std::string("config/name=\"") + kViewerProjectName + "\"");
    CHECK(text.find("use_custom_user_dir") == std::string::npos);   // else user:// is not under app_userdata\<name>
}

} // namespace

int main(int argc, char** argv) {
    test_json();
    test_vdf();
    test_paths();
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
    test_find_godot();
    if (argc > 1) test_project_name_matches_godot_project(argv[1]);
    if (g_failures != 0) {
        std::cerr << g_failures << " launcher check(s) failed\n";
        return 1;
    }
    std::cout << "launcher_tests: all checks passed\n";
    return 0;
}
