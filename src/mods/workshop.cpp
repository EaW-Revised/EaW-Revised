#include "eawr/mods/workshop.hpp"

#include "eawr/mods/steam/text_formats.hpp"

#include <charconv>
#include <system_error>

namespace eawr::mods {

namespace {

using steam::VdfNode;

template <typename Integer>
Integer number_of(const VdfNode& parent, std::string_view key) {
    const VdfNode* node = parent.child(key);
    if (node == nullptr || node->is_object) return 0;
    Integer value = 0;
    const char* begin = node->value.data();
    const char* end = begin + node->value.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc() && result.ptr == end ? value : 0;
}

std::string text_of(const VdfNode& parent, std::string_view key) {
    const VdfNode* node = parent.child(key);
    return node == nullptr || node->is_object ? std::string() : node->value;
}

} // namespace

bool is_workshop_id(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::optional<WorkshopManifest> parse_workshop_manifest(std::string_view text) {
    const std::optional<VdfNode> root = steam::parse_vdf(text);
    const VdfNode* app = root ? root->child("AppWorkshop") : nullptr;
    if (app == nullptr || !app->is_object) return std::nullopt;
    WorkshopManifest manifest;
    manifest.needs_update = number_of<std::int64_t>(*app, "NeedsUpdate") != 0;
    manifest.needs_download = number_of<std::int64_t>(*app, "NeedsDownload") != 0;
    if (const VdfNode* installed = app->child("WorkshopItemsInstalled"); installed != nullptr && installed->is_object) {
        for (const VdfNode& item : installed->children) {
            if (!item.is_object || !is_workshop_id(item.key)) continue;
            manifest.installed[item.key] = {number_of<std::uint64_t>(item, "size"), number_of<std::int64_t>(item, "timeupdated"),
                                            text_of(item, "manifest")};
        }
    }
    if (const VdfNode* details = app->child("WorkshopItemDetails"); details != nullptr && details->is_object) {
        for (const VdfNode& item : details->children) {
            if (!item.is_object || !is_workshop_id(item.key)) continue;
            manifest.details[item.key] = {text_of(item, "manifest"), number_of<std::int64_t>(item, "timeupdated"),
                                          text_of(item, "latest_manifest"), number_of<std::int64_t>(item, "latest_timeupdated")};
        }
    }
    return manifest;
}

} // namespace eawr::mods
