#pragma once

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"

#include <string>
#include <string_view>

namespace eawr::presentation::ui {

// Resolve a faction identity using the catalog and language database of the same active chain.
[[nodiscard]] inline std::string faction_display_name(const data::Catalog& catalog,
    data::ui::TextLookup* lookup, std::string_view faction) {
    const auto resolved = catalog.resolve(faction, data::Category::faction);
    if (!resolved) return std::string(faction);
    const auto* field = resolved.value().value("Text_ID");
    if (!field) return std::string(faction);
    std::string_view key = field->value.raw_text;
    const auto first = key.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::string(faction);
    key = key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);
    if (!lookup) return std::string(faction); // no text database: the identity, never the raw Text_ID key
    if (!lookup->contains(key)) return std::string(faction); // key absent from the text database: identity, not the key
    const std::string caption = data::ui::to_utf8(lookup->text(key));
    // Identity fallback for blank text is unverified against retail (#5456).
    return caption.find_first_not_of(" \t\r\n") == std::string::npos ? std::string(faction) : caption;
}

// The faction's caption as retail's lists show it: the localized Text_ID, or no caption at all when the faction
// authors none (an NA key or an empty entry). The galactic setup side list uses this (WGP-40a); the identity
// fallback above stays for the lists that never reach a faction without text.
[[nodiscard]] inline std::string faction_caption_or_blank(const data::Catalog& catalog,
    data::ui::TextLookup* lookup, std::string_view faction) {
    const auto resolved = catalog.resolve(faction, data::Category::faction);
    if (!resolved || !lookup) return {};
    const auto* field = resolved.value().value("Text_ID");
    if (!field) return {};
    std::string_view key = field->value.raw_text;
    const auto first = key.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    key = key.substr(first, key.find_last_not_of(" \t\r\n") - first + 1);
    if (!lookup->contains(key)) return {};
    const std::string caption = data::ui::to_utf8(lookup->text(key));
    return caption.find_first_not_of(" \t\r\n") == std::string::npos ? std::string{} : caption;
}

} // namespace eawr::presentation::ui
