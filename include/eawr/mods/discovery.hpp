#pragma once
// Mod discovery (docs/design/mod-manager.md, "Discovery"): Workshop items in every Steam library, the game's
// Mods folder and folders the player added. Read-only: directory listings and Steam's .vdf/.acf text, nothing
// else is opened and nothing is written, moved or deleted.
//
// Each source is its own function that appends records to a Catalog; discover() runs them in a fixed order
// (Workshop, Mods folder, manual). A later source (for example data-overwrite detection in the game folder) is
// one more such function.
#include "eawr/mods/fingerprint.hpp"
#include "eawr/mods/record.hpp"
#include "eawr/mods/steam/paths.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace eawr::mods {

// Retail's mod list (MNU-18): folders only, at most 2048, in ASCII case-insensitive order.
inline constexpr std::size_t kModsFolderLimit = 2048;

struct DiscoveryInput {
    // Steam install folders; each one's libraryfolders.vdf adds its libraries (the caller finds Steam).
    std::vector<steam::fs::path> steam_roots;
    // Library folders given directly (tests, or a library the caller already knows), after those of steam_roots.
    std::vector<steam::fs::path> steam_libraries;
    // The game's Mods folder (corruption/Mods of the install); none when unset.
    std::optional<steam::fs::path> mods_folder;
    // Folders the player added with Browse, in the player's order.
    std::vector<steam::fs::path> manual_folders;
    // Fingerprint walk per mod folder (fingerprint.hpp); false leaves `tree` empty.
    bool walk_trees = true;
    std::uint64_t tree_entry_cap = kDefaultTreeEntryCap;
};

// All sources, in order. Records keep their source's order; a key seen twice keeps the first record, except that
// a Workshop item's first copy without a folder gives way to a later library's copy with one.
[[nodiscard]] Catalog discover(const DiscoveryInput& input);

struct WorkshopSnapshot {
    std::string revision;  // opaque metadata digest; empty when incomplete
    bool complete = true;
};
// Steam library map, ACF size/mtime and immediate numeric item directories only. No asset bytes or tree walks.
// Missing Workshop content is normal; an inaccessible/offline library makes the snapshot incomplete.
[[nodiscard]] WorkshopSnapshot workshop_snapshot(const DiscoveryInput& input);

// Workshop items of every library (libraries in order, ids in numeric order within a library).
//  - listed in WorkshopItemsInstalled with a folder: installed, or needs-update when WorkshopItemDetails names a
//    newer manifest or time (downloading instead while Steam's NeedsDownload is set);
//  - listed (installed or subscribed) without a folder, or a folder only WorkshopItemDetails lists: downloading;
//  - a folder Steam does not list at all: installed with codes::folder_not_listed.
void discover_workshop(const DiscoveryInput& input, Catalog& catalog);
// The game's Mods folder: one record per sub-folder (retail's rule, kModsFolderLimit).
void discover_mods_folder(const DiscoveryInput& input, Catalog& catalog);
// The player's folders; one that is gone gives a `missing` record with codes::folder_missing.
void discover_manual(const DiscoveryInput& input, Catalog& catalog);

// The Mods folder's sub-folder names, sorted ASCII case-insensitively (ties by bytes) and cut at `limit`;
// `cut` is set when there were more.
[[nodiscard]] std::vector<std::string> list_mods_folder(const steam::fs::path& mods_folder, bool* cut = nullptr,
                                                        std::size_t limit = kModsFolderLimit);

// path:<sha256> for a manual folder: the absolute, lexically normal path with '/' separators and no trailing
// separator, ASCII-lowercased on Windows (case-insensitive file names), as UTF-8. Non-ASCII letters keep their
// case, so the same folder typed with other non-ASCII case gets another key.
[[nodiscard]] std::string manual_key(const steam::fs::path& folder);

} // namespace eawr::mods
