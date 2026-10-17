#include "install_discovery.hpp"

#include "text_formats.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace eawr::launcher {

namespace {

constexpr std::uintmax_t kMaxTextFile = 4u * 1024u * 1024u;

std::optional<std::string> read_text_file(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > kMaxTextFile) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool directory_exists(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

bool all_digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string trim_quotes(std::string text) {
    while (!text.empty() && (text.front() == '"' || text.front() == ' ')) text.erase(text.begin());
    while (!text.empty() && (text.back() == '"' || text.back() == ' ')) text.pop_back();
    return text;
}

// "C:\\GOG Games\\EaW\\unins000.exe" /SILENT -> C:\GOG Games\EaW
std::optional<fs::path> directory_of_command(const std::string& command) {
    std::string program;
    const std::string trimmed = trim_quotes(command);
    if (!command.empty() && command.front() == '"') {
        const std::size_t close = command.find('"', 1);
        program = command.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        const std::size_t exe = lower_ascii(trimmed).find(".exe");
        program = exe == std::string::npos ? trimmed : trimmed.substr(0, exe + 4);
    }
    const std::size_t comma = program.rfind(',');   // DisplayIcon "path\\app.exe,0"
    if (comma != std::string::npos && all_digits(program.substr(comma + 1))) program.resize(comma);
    fs::path path = path_from_utf8(program);
    if (path.empty() || !path.has_parent_path()) return std::nullopt;
    return path.parent_path();
}

void add_candidate(std::vector<Candidate>& list, fs::path path, const std::string& source, bool nested_ok) {
    if (path.empty()) return;
    for (const Candidate& existing : list) {
        if (lower_ascii(path_to_utf8(existing.path.lexically_normal())) == lower_ascii(path_to_utf8(path.lexically_normal()))) return;
    }
    list.push_back({std::move(path), source, nested_ok});
}

void add_library(std::vector<fs::path>& libraries, fs::path path) {
    if (path.empty()) return;
    for (const fs::path& existing : libraries) {
        if (lower_ascii(path_to_utf8(existing.lexically_normal())) == lower_ascii(path_to_utf8(path.lexically_normal()))) return;
    }
    libraries.push_back(std::move(path));
}

void read_library_folders(const fs::path& file, std::vector<fs::path>& libraries) {
    const std::optional<std::string> text = read_text_file(file);
    if (!text) return;
    const std::optional<VdfNode> root = parse_vdf(*text);
    if (!root) return;
    const VdfNode* folders = root->child("libraryfolders");
    if (folders == nullptr || !folders->is_object) return;
    for (const VdfNode& entry : folders->children) {
        if (entry.is_object) {
            // Modern format: "0" { "path" "D:\\Games\\Library" ... }
            const VdfNode* path = entry.child("path");
            if (path != nullptr && !path->is_object) add_library(libraries, path_from_utf8(path->value));
        } else if (all_digits(entry.key)) {
            // Legacy format: "1" "D:\\Games\\Library"
            add_library(libraries, path_from_utf8(entry.value));
        }
    }
}

bool is_single_component(const fs::path& name) {
    return !name.empty() && name.has_filename() && !name.has_parent_path() && !name.has_root_name() && name != "." && name != "..";
}

struct RootCheck {
    bool ok = false;
    fs::path root;
    std::string error;
};

RootCheck check_root(const fs::path& root) {
    RootCheck result;
    result.root = root;
    const std::string shown = path_to_utf8(root);
    if (!directory_exists(root)) {
        result.error = shown + " is not a folder.";
        return result;
    }
    const std::optional<fs::path> corruption = find_child_ci(root, "corruption");
    const std::optional<fs::path> game_data = find_child_ci(root, "GameData");
    if (!corruption || !directory_exists(*corruption)) {
        result.error = shown + " has no \"corruption\" folder. Empire at War: Forces of Corruption is required; "
                               "the base game alone is not enough.";
        return result;
    }
    if (!game_data || !directory_exists(*game_data)) {
        result.error = shown + " has no \"GameData\" folder. Pick the folder that contains both GameData and corruption.";
        return result;
    }
    for (const fs::path& base : {*game_data, *corruption}) {
        const std::optional<fs::path> data = find_child_ci(base, "Data");
        if (!data || !directory_exists(*data)) {
            result.error = "The Forces of Corruption installation is missing " + path_to_utf8(base / "Data")
                           + ". Verify or repair the game in your game library.";
            return result;
        }
    }
    const std::optional<fs::path> executable = find_child_ci(*corruption, "StarWarsG.exe");
    std::error_code error;
    if (!executable || !fs::is_regular_file(*executable, error)) {
        result.error = "There is no corruption\\StarWarsG.exe under " + shown + ". EAWR supports Forces of Corruption only.";
        return result;
    }
    std::error_code canonical_error;
    fs::path canonical = fs::weakly_canonical(root, canonical_error);
    result.root = canonical_error ? root.lexically_normal() : canonical;
    result.ok = true;
    return result;
}

} // namespace

std::vector<fs::path> steam_library_roots(const RegistryReader& registry) {
    std::vector<fs::path> steam;
    if (const auto path = registry.read_string(RegistryHive::current_user, RegistryView::view64, "Software\\Valve\\Steam", "SteamPath")) {
        add_library(steam, path_from_utf8(*path));
    }
    for (const RegistryView view : {RegistryView::view64, RegistryView::view32}) {
        if (const auto path = registry.read_string(RegistryHive::local_machine, view, "SOFTWARE\\Valve\\Steam", "InstallPath")) {
            add_library(steam, path_from_utf8(*path));
        }
    }
    std::vector<fs::path> libraries;
    for (const fs::path& root : steam) {
        add_library(libraries, root);
        read_library_folders(root / "steamapps" / "libraryfolders.vdf", libraries);
        read_library_folders(root / "config" / "libraryfolders.vdf", libraries);
    }
    return libraries;
}

std::vector<Candidate> find_candidates(const RegistryReader& registry) {
    std::vector<Candidate> candidates;

    for (const fs::path& library : steam_library_roots(registry)) {
        const fs::path steamapps = library / "steamapps";
        for (const unsigned id : kSteamAppIds) {
            const std::optional<std::string> text = read_text_file(steamapps / ("appmanifest_" + std::to_string(id) + ".acf"));
            if (!text) continue;
            const std::optional<VdfNode> manifest = parse_vdf(*text);
            const VdfNode* state = manifest ? manifest->child("AppState") : nullptr;
            const VdfNode* installdir = state != nullptr ? state->child("installdir") : nullptr;
            if (installdir == nullptr || installdir->is_object) continue;
            const fs::path name = path_from_utf8(installdir->value);
            if (is_single_component(name)) add_candidate(candidates, steamapps / "common" / name, "Steam", false);
        }
    }
    // Any game folder in every library is a candidate too (covers a missing or unreadable manifest); validation decides.
    for (const fs::path& library : steam_library_roots(registry)) {
        std::error_code error;
        const fs::path common = library / "steamapps" / "common";
        for (fs::directory_iterator iterator(common, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error)) {
            std::error_code type_error;
            if (iterator->is_directory(type_error)) add_candidate(candidates, iterator->path(), "Steam", false);
        }
    }

    // GOG: the game records, then uninstall records (Galaxy and the classic installer), both views.
    for (const RegistryView view : {RegistryView::view64, RegistryView::view32}) {
        const std::string games = "SOFTWARE\\GOG.com\\Games";
        for (const std::string& id : registry.subkeys(RegistryHive::local_machine, view, games)) {
            if (const auto path = registry.read_string(RegistryHive::local_machine, view, games + "\\" + id, "path")) {
                add_candidate(candidates, path_from_utf8(*path), "GOG", true);
            }
        }
    }
    const std::string uninstall = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (const RegistryHive hive : {RegistryHive::local_machine, RegistryHive::current_user}) {
        for (const RegistryView view : {RegistryView::view64, RegistryView::view32}) {
            for (const std::string& id : registry.subkeys(hive, view, uninstall)) {
                const std::string key = uninstall + "\\" + id;
                const auto name = registry.read_string(hive, view, key, "DisplayName");
                if (!name || lower_ascii(*name).find("empire at war") == std::string::npos) continue;
                std::optional<fs::path> location;
                for (const char* value : {"InstallLocation", "Inno Setup: App Path", "InstallPath"}) {
                    if (const auto text = registry.read_string(hive, view, key, value); text && !trim_quotes(*text).empty()) {
                        location = path_from_utf8(trim_quotes(*text));
                        break;
                    }
                }
                if (!location) {
                    for (const char* value : {"DisplayIcon", "UninstallString"}) {
                        if (const auto text = registry.read_string(hive, view, key, value)) {
                            location = directory_of_command(*text);
                            if (location) break;
                        }
                    }
                }
                if (!location) continue;
                const auto publisher = registry.read_string(hive, view, key, "Publisher");
                const bool gog = id.size() > 3 && lower_ascii(id).ends_with("_is1");
                const bool gog_publisher = publisher && lower_ascii(*publisher).find("gog") != std::string::npos;
                const bool steam = lower_ascii(id).rfind("steam app ", 0) == 0;
                add_candidate(candidates, *location, steam ? "Steam" : (gog || gog_publisher) ? "GOG" : "Windows uninstall entry", !steam);
            }
        }
    }
    return candidates;
}

Validation validate_install(const fs::path& selected, bool nested_ok) {
    Validation validation;
    std::string text = trim_quotes(path_to_utf8(selected));
    if (text.empty()) {
        validation.error = "No folder was given.";
        return validation;
    }
    fs::path base = path_from_utf8(text);
    if (base.empty()) {
        validation.error = "The folder name is not valid text.";
        return validation;
    }
    while (base.has_relative_path() && !base.has_filename()) base = base.parent_path();
    const std::string leaf = lower_ascii(path_to_utf8(base.filename()));

    std::vector<fs::path> roots;
    if ((leaf == "corruption" || leaf == "gamedata") && base.has_parent_path()) roots.push_back(base.parent_path());
    roots.push_back(base);
    if (nested_ok && directory_exists(base)) {
        std::error_code error;
        for (fs::directory_iterator iterator(base, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error)) {
            std::error_code type_error;
            if (iterator->is_directory(type_error)) roots.push_back(iterator->path());
        }
    }
    RootCheck first;
    for (std::size_t index = 0; index < roots.size(); ++index) {
        RootCheck check = check_root(roots[index]);
        if (check.ok) {
            validation.ok = true;
            validation.root = check.root;
            return validation;
        }
        if (index == 0) first = std::move(check);
    }
    validation.error = first.error;
    return validation;
}

void add_unique(std::vector<Install>& installs, Install install) {
    for (const Install& existing : installs) {
        if (same_location(existing.root, install.root)) return;
    }
    installs.push_back(std::move(install));
}

std::vector<Install> discover_installs(const RegistryReader& registry) {
    std::vector<Install> installs;
    for (const Candidate& candidate : find_candidates(registry)) {
        const Validation validation = validate_install(candidate.path, candidate.nested_ok);
        if (validation.ok) add_unique(installs, {validation.root, candidate.source});
    }
    return installs;
}

} // namespace eawr::launcher
