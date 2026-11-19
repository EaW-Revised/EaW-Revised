#pragma once

#include "eawr/core/text.hpp"
#include "eawr/data/xml.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace eawr::presentation::ui {

namespace world_encyclopedia_detail {

// The object's own SELECTABLE or DUMMY_TOOLTIP behaviour token (Behavior or SpaceBehavior).
[[nodiscard]] inline bool own_admission(const data::EffectiveObject& object) {
    const auto matches = [](const std::string_view token, const std::string_view wanted) {
        return core::iequals(token, wanted);
    };
    for (const auto tag : {std::string_view{"Behavior"}, std::string_view{"SpaceBehavior"}}) {
        const auto* field = object.value(tag);
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

} // namespace world_encyclopedia_detail

// CARD-20: physical mouse contact alone does not admit a world encyclopedia.
// Selectable and dummy-tooltip behaviours admit it independently of target validity.
// A team-creating type (a squadron purchase object: authored Squadron_Units or Create_Team_Type)
// is the unit a craft pick and a squadron icon report, but it is only a dummy; the world object
// that carries the behaviour is the team container it creates (Create_Team_Type, else the generic
// team), so that container's admission applies to it (CARD-21).
[[nodiscard]] inline bool world_encyclopedia_admitted(const std::string_view type, const data::Catalog* objects) {
    if (objects == nullptr) return false;
    const auto resolved = objects->resolve(type, data::Category::game_object);
    if (!resolved) return false;
    if (world_encyclopedia_detail::own_admission(resolved.value())) return true;
    const std::string team = core::trim(resolved.value().single_text("Create_Team_Type"), core::xml_whitespace);
    if (team.empty() && resolved.value().value("Squadron_Units") == nullptr) return false;
    if (!team.empty() && core::iequals(team, resolved.value().object_id)) return false;
    const auto container = objects->resolve(team.empty() ? std::string_view{"Team"} : std::string_view{team},
        data::Category::game_object);
    return container && world_encyclopedia_detail::own_admission(container.value());
}

} // namespace eawr::presentation::ui
