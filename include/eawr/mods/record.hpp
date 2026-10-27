#pragma once
// The mod record (docs/design/mod-manager.md, "Data model") and the catalog cache file that holds a list of
// them. Records are built by discovery (discovery.hpp); this header is the type the service, profiles and the
// launcher's rows share.
#include "eawr/mods/steam/text_formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::mods {

// Stable warning codes. Codes are never renumbered or reused; a retired code stays reserved.
namespace codes {
inline constexpr std::string_view folder_not_listed = "EAWR-MODS-0001";      // Workshop folder Steam does not list
inline constexpr std::string_view workshop_manifest_unreadable = "EAWR-MODS-0002"; // appworkshop_32470.acf malformed
inline constexpr std::string_view parent_not_installed = "EAWR-MODS-0003";   // reserved for chain resolution (MM-2)
inline constexpr std::string_view tree_walk_capped = "EAWR-MODS-0004";       // fingerprint walk stopped at the cap
inline constexpr std::string_view duplicate_workshop_item = "EAWR-MODS-0005"; // same id in two Steam libraries
inline constexpr std::string_view folder_missing = "EAWR-MODS-0006";          // a given folder does not exist
inline constexpr std::string_view mods_folder_cut = "EAWR-MODS-0007";         // the Mods folder list cut at 2048
inline constexpr std::string_view tree_walk_incomplete = "EAWR-MODS-0008";    // an entry could not be read
inline constexpr std::string_view needs_update = "EAWR-MODS-0009";            // Steam has a newer version listed
inline constexpr std::string_view downloading = "EAWR-MODS-0010";             // listed, but Steam has not finished
} // namespace codes

enum class ModKind { workshop, mods_folder, manual };
enum class ModRole { playable, parent_only, unknown };
enum class ModState { installed, needs_update, downloading, missing };

[[nodiscard]] std::string_view to_string(ModKind kind) noexcept;   // workshop | mods-folder | manual
[[nodiscard]] std::string_view to_string(ModRole role) noexcept;   // playable | parent-only | unknown
[[nodiscard]] std::string_view to_string(ModState state) noexcept; // installed | needs-update | downloading | missing
[[nodiscard]] std::optional<ModKind> parse_mod_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<ModRole> parse_mod_role(std::string_view text) noexcept;
[[nodiscard]] std::optional<ModState> parse_mod_state(std::string_view text) noexcept;

struct Warning {
    std::string code;     // codes::*
    std::string message;  // readable English, UTF-8; may name ids and folder names, never file contents

    friend bool operator==(const Warning&, const Warning&) = default;
};

// What changes when the mod changes. The .acf fields are Steam's (Workshop items only, else empty / 0); `tree`
// is the folder walk (fingerprint.hpp). Two equal fingerprints mean "unchanged since last time", unless
// `tree_capped` is set (then the folder's state is unknown).
struct Fingerprint {
    std::string acf_manifest;        // WorkshopItemsInstalled/<id>/manifest
    std::int64_t time_updated = 0;   // WorkshopItemsInstalled/<id>/timeupdated (Unix seconds)
    std::uint64_t acf_size = 0;      // WorkshopItemsInstalled/<id>/size
    std::string tree;                // lowercase sha256 hex; empty when the folder was not walked
    std::uint64_t tree_files = 0;    // files hashed into `tree`
    bool tree_capped = false;        // the walk stopped at its cap (codes::tree_walk_capped): `tree` covers part of
                                     // the folder only, so an equal `tree` does not mean unchanged

    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

struct ModRecord {
    std::string key;                 // workshop:<id> | mods:<folder> | path:<sha256 of the normalised absolute path>
    ModKind kind = ModKind::manual;
    std::string name;                // display name (modinfo, known chains or a fallback; see discovery.hpp)
    std::optional<std::string> alias; // the player's own name (manager state, never written to the mod)
    std::string root;                // absolute folder, UTF-8; local only
    bool has_data = false;           // a Data folder exists (ASCII case-insensitive)
    steam::JsonValue metadata;       // object; filled by later readers (modinfo.json, MM-2); empty object here
    ModRole role = ModRole::unknown;
    ModState state = ModState::installed;
    Fingerprint fingerprint;
    std::vector<Warning> warnings;

    [[nodiscard]] bool has_warning(std::string_view code) const;
};

inline constexpr std::string_view kCatalogFormat = "eawr-mod-catalog";
inline constexpr int kCatalogVersion = 1;

struct Catalog {
    std::vector<ModRecord> records;
    std::vector<Warning> warnings;   // discovery-wide (e.g. codes::mods_folder_cut)
};

// One record as a JSON object (UTF-8, no trailing newline). Member order is fixed, so equal records give equal text.
[[nodiscard]] std::string record_to_json(const ModRecord& record);
// nullopt when a required member is missing or has the wrong type, or an enum value is unknown.
[[nodiscard]] std::optional<ModRecord> record_from_json(const steam::JsonValue& value);

// The catalog cache file: {"format": "eawr-mod-catalog", "version": 1, "records": [...], "warnings": [...]}.
[[nodiscard]] std::string catalog_to_json(const Catalog& catalog);
// nullopt on malformed text, another format or another version (before 1.0 an old cache is rebuilt, not read).
[[nodiscard]] std::optional<Catalog> catalog_from_json(std::string_view text);

} // namespace eawr::mods
