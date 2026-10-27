#include "eawr/mods/discovery.hpp"
#include "eawr/mods/workshop.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include "eawr/mods/steam/text_formats.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <span>
#include <system_error>

namespace eawr::mods {
namespace {
namespace fs = steam::fs;
// Framing keeps paths/names containing separators or newlines unambiguous.
void field(std::string& out, const std::string& value) {
    out += std::to_string(value.size()) + ':' + value;
}
bool absent(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory;
}
bool metadata(const fs::path& path, std::string& out, bool directory) {
    field(out, steam::path_to_utf8(path.lexically_normal()));
    std::error_code error;
    const auto status = fs::status(path, error);
    if (absent(error) || (!error && !fs::exists(status))) { field(out, "absent"); return true; }
    if (error || (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) return false;
    const fs::directory_entry entry(path, error);
    if (error) return false;
    const auto time = entry.last_write_time(error);
    if (error) return false;
    field(out, std::to_string(time.time_since_epoch().count()));
    if (!directory) {
        const auto size = fs::file_size(path, error);
        if (error) return false;
        field(out, std::to_string(size));
    }
    return true;
}
bool library_snapshot(const fs::path& library, std::string& out) {
    std::error_code error;
    if (!fs::is_directory(library, error) || error) return false;
    field(out, steam::path_to_utf8(library.lexically_normal()));
    const auto workshop = library / "steamapps" / "workshop";
    const auto acf = workshop / ("appworkshop_" + std::string(kWorkshopAppId) + ".acf");
    const auto content = workshop / "content" / std::string(kWorkshopAppId);
    std::string before;
    if (!metadata(acf, before, false) || !metadata(content, before, true)) return false;
    std::vector<std::string> entries;
    fs::directory_iterator iterator(content, error), end;
    if (error && !absent(error)) return false;
    if (absent(error)) error.clear();
    while (iterator != end && !error) {
        const auto name = steam::path_to_utf8(iterator->path().filename());
        if (is_workshop_id(name)) {
            std::error_code type_error;
            const bool directory = iterator->is_directory(type_error);
            if (type_error) return false;
            if (directory) {
                std::string entry;
                if (!metadata(iterator->path(), entry, true)) return false;
                entries.push_back(std::move(entry));
            }
        }
        // Bound memory for a malformed or enormous immediate listing. Never publish a partial revision.
        if (entries.size() > 100000) return false;
        iterator.increment(error);
    }
    if (error) return false;
    std::string after;
    if (!metadata(acf, after, false) || !metadata(content, after, true) || before != after) return false;
    field(out, before);
    std::sort(entries.begin(), entries.end());
    for (const auto& entry : entries) field(out, entry);
    field(out, "end-library");
    return true;
}
} // namespace

WorkshopSnapshot workshop_snapshot(const DiscoveryInput& input) {
    std::vector<fs::path> libraries;
    std::string signature = "workshop-metadata-1";
    for (const auto& root : input.steam_roots) {
        steam::add_library(libraries, root);
        for (const auto& file : {root / "steamapps" / "libraryfolders.vdf", root / "config" / "libraryfolders.vdf"}) {
            if (!metadata(file, signature, false)) return {{}, false};
            std::error_code error;
            if (!fs::exists(file, error)) { if (error && !absent(error)) return {{}, false}; continue; }
            const auto text = steam::read_text_file(file);
            const auto parsed = text ? steam::parse_vdf(*text) : std::nullopt;
            const auto* folders = parsed ? parsed->child("libraryfolders") : nullptr;
            if (!folders || !folders->is_object) return {{}, false};
            for (auto& library : steam::library_folders_from_vdf(*text)) steam::add_library(libraries, std::move(library));
        }
    }
    for (const auto& library : input.steam_libraries) steam::add_library(libraries, library);
    for (const auto& library : libraries) if (!library_snapshot(library, signature)) return {{}, false};
    return {core::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(signature.data()), signature.size())), true};
}
} // namespace eawr::mods
