#include "eawr/mods/steam/library_folders.hpp"

#include "eawr/mods/steam/text_formats.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace eawr::mods::steam {

namespace {

bool all_digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

// The one file read of src/mods (tests/vfs/test_asset_io_boundary.py): Steam's .vdf/.acf text, read-only.
std::optional<std::string> read_text_file(const fs::path& path, std::uintmax_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > limit) return std::nullopt;
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) return std::nullopt;
    // Bound the read itself as well: another process may grow the file after file_size().
    std::string text;
    char byte = 0;
    while (text.size() < limit && stream.get(byte)) text.push_back(byte);
    if (stream.get(byte) || !stream.eof()) return std::nullopt;
    return text;
}

std::vector<fs::path> library_folders_from_vdf(std::string_view text) {
    std::vector<fs::path> libraries;
    const std::optional<VdfNode> root = parse_vdf(text);
    if (!root) return libraries;
    const VdfNode* folders = root->child("libraryfolders");
    if (folders == nullptr || !folders->is_object) return libraries;
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
    return libraries;
}

void add_library(std::vector<fs::path>& libraries, fs::path path) {
    if (path.empty()) return;
    for (const fs::path& existing : libraries) {
        if (lower_ascii(path_to_utf8(existing.lexically_normal())) == lower_ascii(path_to_utf8(path.lexically_normal()))) return;
    }
    libraries.push_back(std::move(path));
}

void add_steam_libraries(const fs::path& steam_root, std::vector<fs::path>& libraries) {
    add_library(libraries, steam_root);
    for (const fs::path& file : {steam_root / "steamapps" / "libraryfolders.vdf", steam_root / "config" / "libraryfolders.vdf"}) {
        const std::optional<std::string> text = read_text_file(file);
        if (!text) continue;
        for (fs::path& library : library_folders_from_vdf(*text)) add_library(libraries, std::move(library));
    }
}

} // namespace eawr::mods::steam
