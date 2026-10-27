#pragma once
// Steam's library list (libraryfolders.vdf) and the bounded text-file read the launcher and the mod core use
// for Steam's .vdf/.acf files. Standard library only (see paths.hpp). Finding Steam itself (the registry) stays
// with the caller: EAWR.exe reads the registry, the mod core takes Steam roots as input.
#include "eawr/mods/steam/paths.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::mods::steam {

// Steam's own .vdf/.acf files are a few KiB; anything larger is not one of them.
inline constexpr std::uintmax_t kMaxSteamTextFile = 4u * 1024u * 1024u;

// The whole file, opened read-only; nullopt when it is not a regular file, larger than `limit` or unreadable.
[[nodiscard]] std::optional<std::string> read_text_file(const fs::path& path, std::uintmax_t limit = kMaxSteamTextFile);

// Library folders named by a libraryfolders.vdf text, in file order: the modern format
// ("0" { "path" "D:\\Library" ... }) and the pre-2021 one ("1" "D:\\Library"). Malformed text gives none.
[[nodiscard]] std::vector<fs::path> library_folders_from_vdf(std::string_view text);

// Appends `path` unless the list already holds it (ASCII case-insensitive, lexically normalised).
void add_library(std::vector<fs::path>& libraries, fs::path path);

// `steam_root` itself, then the libraries of its steamapps/libraryfolders.vdf and config/libraryfolders.vdf,
// appended to `libraries` without duplicates.
void add_steam_libraries(const fs::path& steam_root, std::vector<fs::path>& libraries);

} // namespace eawr::mods::steam
