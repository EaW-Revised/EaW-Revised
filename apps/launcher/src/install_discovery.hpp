#pragma once
// Finding and validating the player's Forces of Corruption installation (Steam, GOG, saved choice,
// Browse). The registry is reached through RegistryReader so the logic runs against fixtures.
#include "paths.hpp"

#include <optional>
#include <string>
#include <vector>

namespace eawr::launcher {

enum class RegistryHive { current_user, local_machine };
// The 32-bit (WOW6432Node) and the 64-bit registry view.
enum class RegistryView { view32, view64 };

class RegistryReader {
public:
    virtual ~RegistryReader() = default;
    // Keys use backslashes and no hive prefix, e.g. "SOFTWARE\\GOG.com\\Games". UTF-8 results.
    [[nodiscard]] virtual std::optional<std::string> read_string(RegistryHive hive, RegistryView view, const std::string& key,
                                                                 const std::string& value) const = 0;
    [[nodiscard]] virtual std::vector<std::string> subkeys(RegistryHive hive, RegistryView view, const std::string& key) const = 0;
};

// Steam app ids whose appmanifest names the installation. 32470 is "STAR WARS Empire at War: Gold Pack"
// (installdir "Star Wars Empire at War", with Forces of Corruption in its corruption subfolder), verified on a real
// install. Other ids are not assumed; the steamapps\common scan finds the rest and validation decides.
inline constexpr unsigned kSteamAppIds[] = {32470};

struct Candidate {
    fs::path path;
    std::string source;   // "Steam", "GOG", "Windows uninstall entry"
    bool nested_ok = false; // a GOG-style folder may hold the game one level down
};

struct Install {
    fs::path root;        // the folder holding GameData and corruption
    std::string source;
};

struct Validation {
    bool ok = false;
    fs::path root;
    std::string error;    // readable, UTF-8; empty when ok
};

// Steam's own folder plus every library folder (libraryfolders.vdf, modern and legacy formats).
[[nodiscard]] std::vector<fs::path> steam_library_roots(const RegistryReader& registry);
// Unvalidated places to look, best evidence first, without duplicates.
[[nodiscard]] std::vector<Candidate> find_candidates(const RegistryReader& registry);

// `selected` may be the install root, its corruption or GameData folder, or (nested_ok) a folder
// holding the install one level down. The root needs GameData/Data, corruption/Data and
// corruption/StarWarsG.exe (names matched ignoring case).
[[nodiscard]] Validation validate_install(const fs::path& selected, bool nested_ok = true);

// All valid installs from Steam and GOG, deduplicated by location.
[[nodiscard]] std::vector<Install> discover_installs(const RegistryReader& registry);

// Appends `install` unless the same place is already listed.
void add_unique(std::vector<Install>& installs, Install install);

} // namespace eawr::launcher
