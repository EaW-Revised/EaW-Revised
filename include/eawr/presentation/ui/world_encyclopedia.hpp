#pragma once

#include "eawr/core/text.hpp"
#include "eawr/data/xml.hpp"

#include <initializer_list>
#include <string_view>

namespace eawr::presentation::ui {

// CARD-20: physical mouse contact alone does not admit a world encyclopedia.
// Selectable and dummy-tooltip behaviours admit it independently of target validity.
[[nodiscard]] inline bool world_encyclopedia_admitted(const std::string_view type, const data::Catalog* objects) {
    if (objects == nullptr) return false;
    const auto resolved = objects->resolve(type, data::Category::game_object);
    if (!resolved) return false;
    const auto matches = [](const std::string_view token, const std::string_view wanted) {
        return core::iequals(token, wanted);
    };
    for (const auto tag : {std::string_view{"Behavior"}, std::string_view{"SpaceBehavior"}}) {
        const auto* field = resolved.value().value(tag);
        if (field == nullptr) continue;
        const std::string_view raw = field->value.raw_text;
        auto begin = raw.find_first_not_of(" ,\t\r\n");
        while (begin != std::string_view::npos) {
            const auto end = raw.find_first_of(" ,\t\r\n", begin);
            const auto token = raw.substr(begin, end == std::string_view::npos ? end : end - begin);
            if (matches(token, "SELECTABLE") || matches(token, "DUMMY_TOOLTIP")) return true;
            begin = end == std::string_view::npos ? end : raw.find_first_not_of(" ,\t\r\n", end);
        }
    }
    return false;
}

} // namespace eawr::presentation::ui
