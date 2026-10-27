#pragma once
// Steam's per-library Workshop manifest for the game (steamapps/workshop/appworkshop_32470.acf). Read-only:
// the manager never subscribes, downloads, moves or repairs an item; Steam is the only downloader.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::mods {

// Empire at War's Steam app id; Forces of Corruption mods are Workshop items of this app.
inline constexpr std::string_view kWorkshopAppId = "32470";

struct WorkshopInstalledItem {   // WorkshopItemsInstalled/<id>
    std::uint64_t size = 0;
    std::int64_t time_updated = 0;
    std::string manifest;
};

struct WorkshopItemDetails {     // WorkshopItemDetails/<id>: what the account is subscribed to and Steam's latest
    std::string manifest;
    std::int64_t time_updated = 0;
    std::string latest_manifest;     // empty when Steam did not record one
    std::int64_t latest_time_updated = 0;
};

struct WorkshopManifest {
    bool needs_update = false;       // App-wide hint, not evidence that every installed item is stale
    bool needs_download = false;     // AppWorkshop/NeedsDownload (Steam is fetching items of this app)
    std::map<std::string, WorkshopInstalledItem> installed;   // keyed by the decimal item id
    std::map<std::string, WorkshopItemDetails> details;
};

// nullopt when the text is not KeyValues or has no AppWorkshop block. Entries whose key is not a decimal id are
// skipped; numbers that do not parse read as 0.
[[nodiscard]] std::optional<WorkshopManifest> parse_workshop_manifest(std::string_view text);

// True for a non-empty run of ASCII digits (a Workshop item id, also its folder name).
[[nodiscard]] bool is_workshop_id(std::string_view text) noexcept;

} // namespace eawr::mods
