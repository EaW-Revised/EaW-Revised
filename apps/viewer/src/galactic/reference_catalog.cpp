#include "reference_catalog.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/data/retail_values.hpp"
#include <algorithm>
#include <sstream>

namespace eawr::viewer::galactic {
namespace page = eawr::viewer::galactic;
namespace {
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string type_key(std::string text) {
    for (auto& value : text) value = static_cast<char>(core::ascii_upper(static_cast<unsigned char>(value)));
    return text;
}
}
std::optional<ReferenceType> ReferenceCatalog::metadata(std::string_view name) const {
    page::ReferenceType type;
    auto resolved = catalog_.resolve(name, data::Category::game_object);
    if (!resolved) return std::nullopt;
    const auto field = [&](std::string_view tag) {
        const auto* value = resolved.value().value(tag);
        return value ? trim(value->value.raw_text) : std::string{};
    };
    const auto flag = [&](std::string_view tag) { return data::retail::boolean(field(tag)).value_or(false); };
    type.icon = {field("Icon_Name"), text_(field("Text_ID")),
        text_(field("Encyclopedia_Unit_Class")) + "\n" + text_(field("Encyclopedia_Text")), 0, 1};
    type.hero = resolved.value().type_name == "HeroUnit" || resolved.value().type_name == "HeroCompany"
        || resolved.value().type_name == "GenericHeroUnit" || flag("Is_Named_Hero");
    auto behaviours = field("Behavior"); std::replace(behaviours.begin(), behaviours.end(), ',', ' ');
    std::istringstream behaviour_list(behaviours); std::string behaviour;
    while (behaviour_list >> behaviour) if (behaviour == "FLEET" || behaviour == "TRANSPORT") type.container = true;
    // GREF-13: authored modifier source identity, not a type-name substring.
    if (const auto* abilities = resolved.value().value("Abilities")) for (const auto& ability : abilities->value.children)
        if (ability.name == "Planet_Income_Bonus_Ability") for (const auto& value : ability.children)
            if (value.name == "Specific_Mod_Source_Text" && trim(value.raw_text) == "TEXT_ECONOMY_MINING_FACILITIES")
                type.mining_facility = true;
    if (const auto value = data::retail::leading_number(field("Additional_Population_Capacity")))
        type.capacity = value->value.trunc_to_integer();
    return type;
}
std::vector<TreeSlot> ReferenceCatalog::tree_slots(const data::ui::Dialog* dialog, std::string_view faction,
    const std::map<std::uint64_t, std::string>& type_names) const {
    std::vector<page::TreeSlot> output;
    if (!dialog) return output;
    // GREF-27: registered types supply slot names, including hyphenated mod
    // names and types absent from the current production palette.
    std::map<std::string, const data::ui::DialogControl*> controls;
    for (const auto& control : dialog->controls)
        if (control.id_name.starts_with("IDC_TECHTREE_")
            && std::find(control.style.begin(), control.style.end(), "WS_DISABLED") == control.style.end())
            controls.emplace(type_key(control.id_name), &control);
    std::map<std::string, std::uint64_t> identities;
    for (const auto& [id, name] : type_names) identities.emplace(type_key(name), id);
    // GREF-27: exact ids claim controls before hyphen-normalized aliases.
    for (const bool normalized : {false, true}) for (const auto& definition : catalog_.definitions()) {
        if (definition.category != data::Category::game_object || !definition.active || !definition.namespace_winner) continue;
        const auto& name = definition.id;
        if ((name.find('-') != std::string::npos) != normalized) continue;
        auto gadget = type_key(name);
        std::replace(gadget.begin(), gadget.end(), '-', '_');
        const auto control = controls.find("IDC_TECHTREE_" + gadget);
        if (control == controls.end()) continue;
        const auto* matched_control = control->second;
        controls.erase(control); // Each authored control is claimed at most once.
        auto resolved = catalog_.resolve(name, data::Category::game_object);
        if (!resolved) continue;
        const auto type = metadata(name);
        if (!type) continue;
        page::TreeSlot slot; slot.control = matched_control->id_name; slot.type = type->icon;
        const auto* affiliation_field = resolved.value().value("Affiliation");
        if (affiliation_field) {
            auto names = affiliation_field->value.raw_text;
            std::replace(names.begin(), names.end(), ',', ' ');
            std::istringstream input(names); std::string token;
            while (input >> token) if (type_key(token) == type_key(std::string(faction))) slot.affiliated = true;
        }
        if (!slot.affiliated) continue;
        if (const auto* field = resolved.value().value("Tech_Level"))
            if (const auto value = data::retail::leading_number(field->value.raw_text)) slot.required_tech = value->value.trunc_to_integer();
        if (const auto* field = resolved.value().value("Build_Initially_Locked"))
            slot.initially_locked = data::retail::boolean(field->value.raw_text).value_or(false);
        if (const auto id = identities.find(type_key(resolved.value().object_id)); id != identities.end()) slot.type.type = id->second;
        output.push_back(std::move(slot));
    }
    return output;
}
}
