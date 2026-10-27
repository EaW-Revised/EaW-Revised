#include "eawr/mods/discovery.hpp"
#include "eawr/mods/chain.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include "eawr/mods/workshop.hpp"

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace eawr::mods {

namespace fs = steam::fs;

namespace {

using steam::lower_ascii;
using steam::path_to_utf8;

bool folder_exists(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

bool has_data_folder(const fs::path& root) {
    const std::optional<fs::path> data = steam::find_child_ci(root, "Data");
    return data && folder_exists(*data);
}

void add_warning(std::vector<Warning>& warnings, std::string_view code, std::string message) {
    warnings.push_back({std::string(code), std::move(message)});
}

// The record of a folder that exists; fingerprints its tree when asked.
ModRecord folder_record(const DiscoveryInput& input, std::string key, ModKind kind, std::string name, const fs::path& root) {
    ModRecord record;
    record.key = std::move(key);
    record.kind = kind;
    record.name = std::move(name);
    record.root = path_to_utf8(root);
    record.metadata.kind = steam::JsonValue::Kind::object;
    record.has_data = has_data_folder(root);
    if (input.walk_trees) {
        const TreeFingerprint tree = tree_fingerprint(root, input.tree_entry_cap);
        record.fingerprint.tree = tree.tree;
        record.fingerprint.tree_files = tree.files;
        record.fingerprint.tree_capped = tree.capped;
        if (tree.capped) {
            add_warning(record.warnings, codes::tree_walk_capped,
                        "the folder holds more than " + std::to_string(input.tree_entry_cap)
                            + " entries; its change check covers only the first ones");
        }
        if (tree.incomplete) {
            add_warning(record.warnings, codes::tree_walk_incomplete, "some entries of the folder could not be read");
        }
    }
    return record;
}

fs::path absolute_folder(const fs::path& folder) {
    std::error_code error;
    fs::path absolute = fs::absolute(folder, error);
    if (error) absolute = folder;
    return absolute.lexically_normal();
}

void append_unique(Catalog& catalog, ModRecord record) {
    for (ModRecord& existing : catalog.records) {
        if (existing.key == record.key) return;
    }
    catalog.records.push_back(std::move(record));
}

// Decimal ids in numeric order.
bool id_less(const std::string& left, const std::string& right) {
    return left.size() != right.size() ? left.size() < right.size() : left < right;
}

std::vector<fs::path> libraries_of(const DiscoveryInput& input) {
    std::vector<fs::path> libraries;
    for (const fs::path& root : input.steam_roots) steam::add_steam_libraries(root, libraries);
    for (const fs::path& library : input.steam_libraries) steam::add_library(libraries, library);
    return libraries;
}

void discover_library(const DiscoveryInput& input, const fs::path& library, Catalog& catalog) {
    const fs::path workshop = library / "steamapps" / "workshop";
    const fs::path content = workshop / "content" / std::string(kWorkshopAppId);
    const fs::path acf = workshop / ("appworkshop_" + std::string(kWorkshopAppId) + ".acf");

    std::optional<WorkshopManifest> manifest;
    bool unreadable = false;
    if (const std::optional<std::string> text = steam::read_text_file(acf)) {
        manifest = parse_workshop_manifest(*text);
        unreadable = !manifest;
    } else {
        std::error_code error;
        unreadable = fs::exists(acf, error);   // present but too large or not a file
    }
    if (!manifest) manifest.emplace();

    std::set<std::string> folders;
    std::error_code error;
    for (fs::directory_iterator iterator(content, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code type_error;
        const std::string name = path_to_utf8(iterator->path().filename());
        if (is_workshop_id(name) && iterator->is_directory(type_error)) folders.insert(name);
    }

    std::vector<std::string> ids(folders.begin(), folders.end());
    for (const auto& entry : manifest->installed) ids.push_back(entry.first);
    for (const auto& entry : manifest->details) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end(), id_less);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    for (const std::string& id : ids) {
        const std::string key = "workshop:" + id;
        const auto installed = manifest->installed.find(id);
        const auto details = manifest->details.find(id);
        const bool listed = installed != manifest->installed.end();
        const bool subscribed = details != manifest->details.end();
        const bool has_folder = folders.count(id) != 0;
        const fs::path root = content / id;

        ModRecord record = has_folder ? folder_record(input, key, ModKind::workshop, "Workshop item " + id, root) : ModRecord{};
        if (!has_folder) {
            record.key = key;
            record.kind = ModKind::workshop;
            record.name = "Workshop item " + id;
            record.root = path_to_utf8(root);
            record.metadata.kind = steam::JsonValue::Kind::object;
        }
        if (listed) {
            record.fingerprint.acf_manifest = installed->second.manifest;
            record.fingerprint.time_updated = installed->second.time_updated;
            record.fingerprint.acf_size = installed->second.size;
        }
        if (has_folder && listed) {
            const bool newer = subscribed
                               && ((!details->second.latest_manifest.empty() && details->second.latest_manifest != installed->second.manifest)
                                   || details->second.latest_time_updated > installed->second.time_updated);
            if (newer && manifest->needs_download) {
                record.state = ModState::downloading;
                add_warning(record.warnings, codes::downloading, "Steam is downloading an update of Workshop item " + id);
            } else if (newer) {
                record.state = ModState::needs_update;
                add_warning(record.warnings, codes::needs_update, "Steam lists a newer version of Workshop item " + id);
            }
        } else if (listed || subscribed) {
            record.state = ModState::downloading;
            add_warning(record.warnings, codes::downloading, "Steam has not finished downloading Workshop item " + id);
        } else {
            add_warning(record.warnings, codes::folder_not_listed,
                        "Workshop item " + id + " has a folder that Steam's Workshop list does not name");
        }
        if (unreadable) {
            add_warning(record.warnings, codes::workshop_manifest_unreadable, "Steam's Workshop list of this library could not be read");
        }
        // The same item in two libraries: the first copy with a folder is used.
        const auto existing = std::find_if(catalog.records.begin(), catalog.records.end(), [&](const ModRecord& r) { return r.key == key; });
        if (existing == catalog.records.end()) {
            catalog.records.push_back(std::move(record));
            continue;
        }
        const bool existing_has_folder = folder_exists(steam::path_from_utf8(existing->root));
        if (!existing_has_folder && has_folder) {
            *existing = std::move(record);
        } else if (existing_has_folder && has_folder && !existing->has_warning(codes::duplicate_workshop_item)) {
            add_warning(existing->warnings, codes::duplicate_workshop_item,
                        "Workshop item " + id + " is also in another Steam library; that copy is not used");
        }
    }
    if (unreadable) {
        add_warning(catalog.warnings, codes::workshop_manifest_unreadable,
                    "a Steam library's Workshop list (appworkshop_" + std::string(kWorkshopAppId) + ".acf) could not be read");
    }
}

} // namespace

void discover_workshop(const DiscoveryInput& input, Catalog& catalog) {
    for (const fs::path& library : libraries_of(input)) discover_library(input, library, catalog);
}

std::vector<std::string> list_mods_folder(const fs::path& mods_folder, bool* cut, std::size_t limit) {
    std::vector<std::pair<std::string, std::string>> names;   // (lowercase, as listed)
    std::error_code error;
    for (fs::directory_iterator iterator(mods_folder, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code type_error;
        if (!iterator->is_directory(type_error)) continue;
        std::string name = path_to_utf8(iterator->path().filename());
        if (name.empty()) continue;
        names.emplace_back(lower_ascii(name), std::move(name));
    }
    std::sort(names.begin(), names.end());
    if (cut != nullptr) *cut = names.size() > limit;
    if (names.size() > limit) names.resize(limit);
    std::vector<std::string> result;
    result.reserve(names.size());
    for (auto& entry : names) result.push_back(std::move(entry.second));
    return result;
}

void discover_mods_folder(const DiscoveryInput& input, Catalog& catalog) {
    if (!input.mods_folder) return;
    bool cut = false;
    const fs::path mods_folder = absolute_folder(*input.mods_folder);   // roots absolute, as manual folders are
    for (const std::string& name : list_mods_folder(mods_folder, &cut)) {
        append_unique(catalog, folder_record(input, "mods:" + name, ModKind::mods_folder, name, mods_folder / steam::path_from_utf8(name)));
    }
    if (cut) {
        add_warning(catalog.warnings, codes::mods_folder_cut,
                    "the Mods folder holds more than " + std::to_string(kModsFolderLimit) + " folders; only the first "
                        + std::to_string(kModsFolderLimit) + " in name order are listed");
    }
}

std::string manual_key(const fs::path& folder) {
    std::string text = path_to_utf8(absolute_folder(folder));
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) text.pop_back();
#ifdef _WIN32
    text = lower_ascii(text);
#endif
    return "path:" + core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void discover_manual(const DiscoveryInput& input, Catalog& catalog) {
    for (const fs::path& given : input.manual_folders) {
        const fs::path folder = absolute_folder(given);
        std::string key = manual_key(folder);
        std::string name = path_to_utf8(folder.filename());
        if (name.empty()) name = path_to_utf8(folder.parent_path().filename());
        if (folder_exists(folder)) {
            append_unique(catalog, folder_record(input, std::move(key), ModKind::manual, std::move(name), folder));
            continue;
        }
        ModRecord record;
        record.key = std::move(key);
        record.kind = ModKind::manual;
        record.name = std::move(name);
        record.root = path_to_utf8(folder);
        record.metadata.kind = steam::JsonValue::Kind::object;
        record.state = ModState::missing;
        add_warning(record.warnings, codes::folder_missing, "the folder " + record.root + " does not exist or is not a folder");
        append_unique(catalog, std::move(record));
    }
}

Catalog discover(const DiscoveryInput& input) {
    Catalog catalog;
    discover_workshop(input, catalog);
    discover_mods_folder(input, catalog);
    discover_manual(input, catalog);
    read_mod_metadata(catalog);
    return catalog;
}

} // namespace eawr::mods
