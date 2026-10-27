#include "galactic_internal.hpp"

#include "../xml_internal.hpp"

#include <algorithm>
#include <utility>

namespace eawr::data::galactic::detail {
namespace {

// WST-86: Story_Name occurrences append to one token stream, then adjacent tokens form pairs.
[[nodiscard]] std::vector<FactionName> story_names(const Record& record) {
    std::vector<std::string> names;
    for (const auto* at : record.records("Story_Name")) {
        const auto text = trimmed(at->raw_text);
        if (text.size() >= 65536) {
            record.issue(IssueKind::bad_value, at, "Story_Name", {}, "story association text exceeds the WST-86 parser limit");
            continue;
        }
        for (auto begin = text.find_first_not_of(" ,\t\n\r"); begin != std::string::npos;) {
            const auto end = text.find_first_of(" ,\t\n\r", begin);
            names.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
            begin = text.find_first_not_of(" ,\t\n\r", end);
        }
    }
    std::vector<FactionName> result;
    for (std::size_t i = 0; i + 1 < names.size(); i += 2) result.push_back({names[i], names[i + 1]});
    if (names.size() % 2 != 0)
        record.issue(IssueKind::bad_value, nullptr, "Story_Name", names.back(), "expected a plot path after the final faction (WST-86)");
    return result;
}

[[nodiscard]] std::vector<LocationLevel> location_levels(const Record& record, const std::string_view tag) {
    std::vector<LocationLevel> result;
    for (const auto& value : record.faction_numbers(tag)) result.push_back({value.faction, value.value});
    return result;
}

// WGS-11 reads the first record of a faction; WGS-U01 leaves open how FoC stores a repeated one.
void note_duplicates(const Record& record, const std::vector<FactionNumber>& values, const std::string_view tag) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (!ascii_iequals(values[earlier].faction, values[index].faction)) continue;
            record.issue(IssueKind::duplicate_record, nullptr, std::string(tag),
                values[index].faction + ", " + std::to_string(values[index].value),
                "repeated faction record; both kept in authored order, the first match is read (WGS-11, WGS-U01)");
            break;
        }
    }
}

} // namespace

void load_campaigns(const Catalog& catalog, GalacticData& data) {
    std::vector<const Definition*> definitions;
    for (const auto& definition : catalog.definitions()) {
        if (definition.category == Category::campaign && definition.active && definition.namespace_winner) {
            definitions.push_back(&definition);
        }
    }
    // Registry order, then definition order: the authored order of campaignfiles.xml (WGS-01).
    std::stable_sort(definitions.begin(), definitions.end(), [](const Definition* left, const Definition* right) {
        if (left->registry_order != right->registry_order) return left->registry_order < right->registry_order;
        return left->definition_order < right->definition_order;
    });
    for (const auto* definition : definitions) {
        const Record record(definition->id, layers_of(catalog, *definition, data.issues), data.issues);
        record.unknown_tags();
        catalog_input(catalog, data, definition->root.source);
        Campaign campaign;
        campaign.name = definition->id;
        campaign.file = definition->root.source.logical_path;
        campaign.line = definition->root.source.line;
        campaign.campaign_set = record.text("Campaign_Set");
        campaign.sort_order = record.whole("Sort_Order");
        campaign.text_id = record.text("Text_ID");
        campaign.description_text = record.text("Description_Text");
        campaign.is_listed = record.boolean("Is_Listed");
        campaign.is_multiplayer = record.boolean("Is_Multiplayer");
        campaign.is_quickmatch_allowed = record.boolean("Is_Quickmatch_Allowed");
        campaign.supports_custom_settings = record.boolean("Supports_Custom_Settings");
        campaign.minimum_custom_tech = record.whole("Min_Custom_Tech_Level");
        campaign.maximum_custom_tech = record.whole("Max_Custom_Tech_Level");
        campaign.show_completed_tab = record.boolean("Show_Completed_Tab");
        campaign.tutorial = record.boolean("Tutorial");
        campaign.camera_shift_x = record.number("Camera_Shift_X");
        campaign.camera_shift_y = record.number("Camera_Shift_Y");
        campaign.camera_distance = record.number("Camera_Distance");
        campaign.locations = record.names("Locations");
        campaign.trade_routes = record.names("Trade_Routes");
        campaign.home_locations = record.faction_names("Home_Location");
        campaign.starting_active_player = record.text("Starting_Active_Player");
        campaign.starting_credits = record.faction_numbers("Starting_Credits");
        campaign.starting_tech_levels = record.faction_numbers("Starting_Tech_Level");
        campaign.max_tech_levels = record.faction_numbers("Max_Tech_Level");
        campaign.starting_forces = record.placed_types("Starting_Forces");
        campaign.ai_player_control = record.faction_names("AI_Player_Control");
        campaign.markup_filenames = record.faction_names("Markup_Filename");
        campaign.rebel_story_name = record.text("Rebel_Story_Name");
        campaign.empire_story_name = record.text("Empire_Story_Name");
        campaign.underworld_story_name = record.text("Underworld_Story_Name");
        campaign.story_names = story_names(record);
        campaign.is_story_campaign = record.boolean("Is_Story_Campaign");
        campaign.planet_auto_reveal = record.boolean("Planet_Auto_Reveal");
        campaign.is_autoresolve_allowed = record.boolean("Is_Autoresolve_Allowed");
        campaign.autoresolve_exclusion_locations = record.names("Autoresolve_Exclusion_Locations");
        campaign.corruption_level_overrides = location_levels(record, "Corruption_Level_Override");
        campaign.human_victory_conditions = record.names("Human_Victory_Conditions");
        campaign.ai_victory_conditions = record.names("AI_Victory_Conditions");
        campaign.human_victory_planet_names = record.names("Human_Victory_Planet_Names");
        campaign.ai_victory_cycle_limit = record.whole("AI_Victory_Cycle_Limit");
        // WGS-78: parse the complete human/AI parameter families; installer owns defaults.
        campaign.human_victory_cycle_limit = record.whole("Human_Victory_Cycle_Limit");
        campaign.human_victory_credit_target = record.whole("Human_Victory_Credit_Target");
        campaign.ai_victory_credit_target = record.whole("AI_Victory_Credit_Target");
        campaign.human_victory_galactic_control = record.number("Human_Victory_Galactic_Control");
        campaign.ai_victory_galactic_control = record.number("AI_Victory_Galactic_Control");
        campaign.ai_victory_planet_names = record.names("AI_Victory_Planet_Names");
        campaign.human_victory_faction_names = record.names("Human_Victory_Faction_Names");
        campaign.ai_victory_faction_names = record.names("AI_Victory_Faction_Names");
        campaign.human_victory_leader_names = record.names("Human_Victory_Leader_Names");
        campaign.ai_victory_leader_names = record.names("AI_Victory_Leader_Names");
        campaign.special_case_production = record.placed_types("Special_Case_Production");
        note_duplicates(record, campaign.starting_credits, "Starting_Credits");
        note_duplicates(record, campaign.starting_tech_levels, "Starting_Tech_Level");
        note_duplicates(record, campaign.max_tech_levels, "Max_Tech_Level");
        if (campaign.locations.empty()) {
            record.issue(IssueKind::missing_required, nullptr, "Locations", {}, "a campaign without locations has no galaxy");
        }
        data.campaigns.push_back(std::move(campaign));
    }
}

} // namespace eawr::data::galactic::detail
