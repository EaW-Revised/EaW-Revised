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

} // namespace eawr::presentation::ui
