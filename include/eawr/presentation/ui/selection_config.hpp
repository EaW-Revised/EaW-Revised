#pragma once
// SH-03: normal VFS XML configuration, independent of voice installation paths.
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "eawr/core/text.hpp"
#include <algorithm>
#include <span>

namespace eawr::presentation::ui {
[[nodiscard]] inline SelectionConfig load_selection_config(const vfs::Vfs& filesystem) {
    SelectionConfig result;
    auto candidates = filesystem.candidates("data/xml/selectioncontrols.xml");
    if (!candidates) return result;
    const auto trim = [](const std::string& value) {
        const auto first = value.find_first_not_of(core::xml_whitespace);
        if (first == std::string::npos) return std::string{};
        return value.substr(first, value.find_last_not_of(core::xml_whitespace) - first + 1);
    };
    const auto lower = [](std::string value) {
        for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
        return value;
    };
    const auto layers = filesystem.layer_ids();
    const auto revised_layer = std::find(layers.begin(), layers.end(), vfs::revised_layer_id);
    // Apply low precedence first so a partial upper layer overrides only its own rows.
    for (auto candidate = candidates.value().rbegin(); candidate != candidates.value().rend(); ++candidate) {
        const auto candidate_layer = std::find(layers.begin(), layers.end(), candidate->layer_id);
        const bool allow_faction_override = revised_layer == layers.end() || candidate_layer < revised_layer;
        auto document = data::load_document(filesystem, *candidate);
        if (!document) continue;
        for (const auto& node : document.value().root.children) {
            if (node.name == "Selection_Advisor_Enabled") {
                const auto value = lower(trim(node.raw_text));
                result.enabled = value == "yes" || value == "true" || value == "1";
            }
            if (node.name != "Selection_Action") continue;
            std::string name;
            for (const auto& attribute : node.attributes) if (attribute.name == "Name") name = trim(attribute.value);
            const auto found = std::find(selection_commands.begin(), selection_commands.end(), name);
            if (found == selection_commands.end()) continue;
            auto& action = result.actions[static_cast<std::size_t>(found - selection_commands.begin())];
            for (const auto& field : node.children) {
                const auto value = trim(field.raw_text);
                if (field.name == "Default_Key"
                    && std::find(selection_key_choices.begin(), selection_key_choices.end(), value) != selection_key_choices.end())
                    action.default_key = value;
                if (field.name == "Include_Heroes") {
                    const auto flag = lower(value);
                    if (flag == "yes" || flag == "true" || flag == "1") action.include_heroes = true;
                    else if (flag == "no" || flag == "false" || flag == "0") action.include_heroes = false;
                }
                if (field.name == "Faction_SFXEvent" && !allow_faction_override) continue;
                if (field.name == "Faction_SFXEvent" || field.name == "Default_Faction_SFXEvent"
                    || field.name == "Advisor_SFXEvent") {
                    const auto comma = value.find(',');
                    if (comma == std::string::npos) continue;
                    const auto faction = trim(value.substr(0, comma));
                    const auto event = trim(value.substr(comma + 1));
                    if (!faction.empty() && !event.empty()) {
                        auto& mappings = field.name == "Faction_SFXEvent" ? action.faction_events
                            : field.name == "Default_Faction_SFXEvent" ? action.default_faction_events : action.advisor_events;
                        mappings[lower(faction)] = event;
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> selection_faction_advisors(const data::EffectiveObject& faction) {
    constexpr std::array advisor_tags{"Reinforcements_Selection_SFXEvent",
        "SFXEvent_HUD_Landing_Zone_Captured", "SFXEvent_HUD_Won_Land_Battle",
        "SFXEvent_Land_Invasion_Commencing", "SFXEvent_Tactical_Unit_Cap_Reached"};
    std::vector<std::string> result;
    for (const auto* key : advisor_tags) {
        if (const auto* field = faction.value(key)) {
            data::tag_trace::used(field->value);
            const auto& value = field->value.raw_text;
            const auto first = value.find_first_not_of(core::xml_whitespace);
            if (first != std::string::npos)
                result.push_back(value.substr(first, value.find_last_not_of(core::xml_whitespace) - first + 1));
        }
    }
    return result;
}

// An explicit faction override wins, followed by the first available advisor and the authored default.
[[nodiscard]] inline std::string selection_advisor_event(const SelectionActionConfig& action,
    std::string_view faction, std::span<const std::string> advisors) {
    const auto lookup = [](const auto& map, std::string_view key) -> std::string {
        const auto found = map.find(core::to_lower(std::string(key)));
        return found == map.end() ? std::string{} : found->second;
    };
    if (auto value = lookup(action.faction_events, faction); !value.empty()) return value;
    if (!advisors.empty())
        if (auto value = lookup(action.advisor_events, advisors.front()); !value.empty()) return value;
    return lookup(action.default_faction_events, faction);
}
inline void apply_selection_config(PlayerSettings& settings, const vfs::Vfs& filesystem) {
    settings.selection_config = load_selection_config(filesystem);
    if (!settings.selection_hotkeys_authored) settings.selection_hotkeys = settings.selection_config.enabled;
}
} // namespace eawr::presentation::ui
