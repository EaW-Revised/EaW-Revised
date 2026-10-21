#include "galactic_internal.hpp"

#include "eawr/data/tag_trace.hpp"
#include "../xml_internal.hpp"

#include <algorithm>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace eawr::data::galactic::detail {
namespace {

constexpr std::string_view trade_route_registry = "data/xml/traderoutefiles.xml";
constexpr std::string_view constants_path = "data/xml/gameconstants.xml";

// The tags of a TradeRoute record (plain document, so no schema classification).
constexpr std::string_view trade_route_tags[] = {
    "Point_A", "Point_B", "HS_Speed_Factor", "Political_Control_Gain", "Credit_Gain_Factor", "Visible_Line_Name"};

[[nodiscard]] std::vector<const Definition*> sorted_definitions(const Catalog& catalog, const Category category,
    const std::string_view type_name) {
    std::vector<const Definition*> result;
    for (const auto& definition : catalog.definitions()) {
        if (definition.category != category || !definition.active || !definition.namespace_winner) continue;
        if (!type_name.empty() && !ascii_iequals(definition.type_name, type_name)) continue;
        result.push_back(&definition);
    }
    std::stable_sort(result.begin(), result.end(), [](const Definition* left, const Definition* right) {
        if (left->registry_order != right->registry_order) return left->registry_order < right->registry_order;
        return left->definition_order < right->definition_order;
    });
    return result;
}

void add_issue(GalacticData& data, const IssueKind kind, std::string file, const std::uint64_t line, std::string owner,
    std::string tag, std::string value, std::string reason) {
    data.issues.push_back({kind, std::move(file), line, std::move(owner), std::move(tag), std::move(value),
        std::move(reason)});
}

[[nodiscard]] std::optional<std::string> attribute(const XmlNode& node, const std::string_view name) {
    for (const auto& item : node.attributes) {
        if (!ascii_iequals(item.name, name)) continue;
        tag_trace::used_attribute(node, item.name);
        return trim(item.value);
    }
    return std::nullopt;
}

using NameSet = std::unordered_set<std::string>;

[[nodiscard]] bool contains(const NameSet& names, const std::string_view name) { return names.contains(lower(name)); }

} // namespace

void load_planets(const Catalog& catalog, GalacticData& data) {
    for (const auto* definition : sorted_definitions(catalog, Category::game_object, "Planet")) {
        const Record record(definition->id, layers_of(catalog, *definition, data.issues), data.issues);
        record.unknown_tags();
        catalog_input(catalog, data, definition->root.source);
        Planet planet;
        planet.name = definition->id;
        planet.file = definition->root.source.logical_path;
        planet.galactic_position = record.position("Galactic_Position");
        planet.galactic_influence_range = record.number("Galactic_Influence_Range");
        planet.system_spies = system_spy_definitions(catalog, *definition, data.issues);
        planet.political_control = record.number("Political_Control");
        planet.force_alignment = record.text("The_Force");
        planet.force_strength = record.number("Force_Strength");
        planet.political_faction = record.text("Political_Faction");
        planet.planet_retains_residual_influence = record.boolean("Planet_Retains_Residual_Influence");
        planet.planet_credit_value = record.number("Planet_Credit_Value");
        planet.planet_destroyed_credit_value = record.number("Planet_Destroyed_Credit_Value");
        planet.max_space_base = record.whole("Max_Space_Base");
        planet.special_structures_land = record.whole("Special_Structures_Land");
        planet.special_structures_space = record.whole("Special_Structures_Space");
        planet.additional_population_capacity = record.whole("Additional_Population_Capacity");
        planet.population_value = record.whole("Population_Value");
        planet.planet_capture_bonus_reward = record.whole("Planet_Capture_Bonus_Reward");
        planet.planet_surface_accessible = record.boolean("Planet_Surface_Accessible");
        planet.planet_starbase_stealth = record.boolean("Planet_Starbase_Stealth");
        planet.planet_groundbase_stealth = record.boolean("Planet_Groundbase_Stealth");
        planet.galactic_model_name = record.text("Galactic_Model_Name");
        planet.planet_ability_name = record.text("Planet_Ability_Name");
        planet.planet_ability_description = record.text("Planet_Ability_Description");
        planet.planet_ability_icon = record.text("Planet_Ability_Icon");
        planet.describe_advantage = record.text("Describe_Advantage");
        planet.encyclopedia_text = record.names("Encyclopedia_Text");
        planet.scale_factor = record.number("Scale_Factor");
        planet.destroyed_galactic_model_name = record.text("Destroyed_Galactic_Model_Name");
        planet.space_tactical_map = record.text("Space_Tactical_Map");
        planet.destroyed_space_tactical_map = record.text("Destroyed_Space_Tactical_Map");
        planet.land_tactical_map = record.text("Land_Tactical_Map");
        planet.zoomed_terrain_index = record.whole("Zoomed_Terrain_Index");
        planet.behaviors = record.names("Behavior");
        planet.galactic_behaviors = record.names("GalacticBehavior");
        planet.in_background = record.boolean("In_Background");
        if (!planet.galactic_position) {
            record.issue(IssueKind::missing_required, nullptr, "Galactic_Position", {},
                "a planet without a galactic position cannot be placed (WGS-09)");
        }
        // WGS-01a: the authored XML class does not imply runtime planetary behavior.
        // In_Background is presentation metadata and cannot remove a real planet.
        if (planet.has_planetary_behavior()) data.planets.push_back(std::move(planet));
        else data.galaxy_art_objects.push_back(std::move(planet));
    }
}

void load_factions(const Catalog& catalog, GalacticData& data) {
    for (const auto* definition : sorted_definitions(catalog, Category::faction, {})) {
        const Record record(definition->id, layers_of(catalog, *definition, data.issues), data.issues);
        record.unknown_tags();
        catalog_input(catalog, data, definition->root.source);
        Faction faction;
        faction.name = definition->id;
        faction.file = definition->root.source.logical_path;
        faction.force_alignment = record.text("Force_Alignment");
        faction.is_playable = record.boolean("Is_Playable");
        faction.is_neutral = record.boolean("Is_Neutral");
        faction.can_control_planets = record.boolean("Can_Control_Planets");
        faction.primary_enemy = record.text("Primary_Enemy");
        faction.basic_ai = record.text("Basic_AI");
        faction.allies = record.names("Allies");
        faction.enemies = record.names("Enemies");
        faction.home_planet = record.text("Home_Planet");
        faction.faction_leader = record.text("Faction_Leader");
        faction.faction_leader_company = record.text("Faction_Leader_Company");
        faction.multiplayer_campaign_heroes = record.names("Multiplayer_Campaign_Heroes");
        faction.displayed_tech_level_adjustment = record.whole("Displayed_Tech_Level_Adjustment");
        faction.hyperspace_speed_factor = record.number("Hyperspace_Speed_Factor");
        faction.maintenance_cost = record.number("Maintenance_Cost");
        faction.easily_bribed = record.boolean("Easily_Bribed");
        faction.benefits_from_corruption = record.boolean("Benefits_From_Corruption");
        faction.minimum_visible_base_level = record.whole("Minimum_Visible_Base_Level");
        faction.base_population_capacity = record.whole("Base_Population_Capacity");
        faction.ai_uses_galactic_pop_cap = record.boolean("AI_Uses_Galactic_Pop_Cap");
        data.factions.push_back(std::move(faction));
    }
}

core::Result<bool> load_trade_routes(const vfs::Vfs& filesystem, GalacticData& data) {
    auto registry = load_document(filesystem, trade_route_registry);
    if (!registry) return core::Result<bool>::failure(registry.error());
    record_input(data, registry.value().source, registry.value().input_sha256);
    tag_trace::document(registry.value().root);
    for (const auto& entry : registry.value().root.children) {
        if (!ascii_iequals(entry.name, "File")) continue;
        tag_trace::used(entry);
        const auto path = registry_include_path(trade_route_registry, trim(entry.raw_text));
        auto document = load_document(filesystem, path);
        if (!document) {
            add_issue(data, IssueKind::dangling_reference, std::string(trade_route_registry), entry.source.line,
                "TradeRouteFiles", "File", trim(entry.raw_text), document.error().message);
            continue;
        }
        record_input(data, document.value().source, document.value().input_sha256);
        tag_trace::document(document.value().root);
        for (const auto& node : document.value().root.children) {
            if (!ascii_iequals(node.name, "TradeRoute")) continue;
            const auto name = attribute(node, "Name");
            if (!name || name->empty()) {
                add_issue(data, IssueKind::missing_required, node.source.logical_path, node.source.line, "TradeRoute",
                    "@Name", {}, "a trade route without a name cannot be referenced");
                continue;
            }
            tag_trace::object(node, *name);
            const Record record(*name, node, node.source.logical_path, data.issues);
            for (const auto& child : node.children) {
                const bool known = std::any_of(std::begin(trade_route_tags), std::end(trade_route_tags),
                    [&](const std::string_view tag) { return ascii_iequals(child.name, tag); });
                if (!known) {
                    record.issue(IssueKind::unknown_tag, &child, child.name, trim(child.raw_text),
                        "the tag is not known for this class");
                }
            }
            TradeRoute route;
            route.name = *name;
            route.file = node.source.logical_path;
            route.point_a = record.text("Point_A");
            route.point_b = record.text("Point_B");
            route.hs_speed_factor = record.number("HS_Speed_Factor");
            route.political_control_gain = record.number("Political_Control_Gain");
            route.credit_gain_factor = record.number("Credit_Gain_Factor");
            route.visible_line_name = record.text("Visible_Line_Name");
            if (route.point_a.empty() || route.point_b.empty()) {
                record.issue(IssueKind::missing_required, &node, route.point_a.empty() ? "Point_A" : "Point_B", {},
                    "a trade route needs both end points");
            }
            const auto existing = std::find_if(data.trade_routes.begin(), data.trade_routes.end(),
                [&](const TradeRoute& other) { return ascii_iequals(other.name, route.name); });
            if (existing != data.trade_routes.end()) {
                // The catalog's rule for a repeated name: the later definition wins (unverified for routes).
                record.issue(IssueKind::duplicate_record, &node, "@Name", route.name,
                    "repeated trade route; the later definition replaces " + existing->file + " (unverified)");
                *existing = std::move(route);
            } else {
                data.trade_routes.push_back(std::move(route));
            }
        }
    }
    return core::Result<bool>::success(true);
}

} // namespace eawr::data::galactic::detail

namespace eawr::data::galactic {
std::vector<SystemSpyDefinition> system_spy_definitions(const Catalog& catalog,
    const Definition& definition, std::vector<Issue>& issues) {
    const auto layers = detail::layers_of(catalog, definition, issues);
    const detail::Record record(definition.id, layers, issues);
    std::vector<const XmlNode*> blocks;
    // WAR-84: a derived authored ability list replaces the inherited list.
    for (auto n = layers.size(); n > 0; --n) for (const auto& child : layers[n - 1]->root.children) {
        if (!ascii_iequals(child.name, "Abilities")) continue;
        if (n != layers.size()) blocks.clear();
        blocks.push_back(&child);
    }
    constexpr std::pair<std::string_view, std::uint32_t> flags[] = {
        {"See_Base_Levels", 6}, {"See_Credit_Income", 8}, {"See_Num_Fleets", 48},
        {"See_Fleet_Contents", 64}, {"See_Num_Ground_Companies", 128},
        {"See_Ground_Company_Contents", 256}, {"See_Special_Structures", 512},
        {"See_Political_Control", 1024}, {"See_Political_Control_Breakdown", 2048},
        {"See_Current_Production", 4096}, {"See_Minor_Stealth_Heroes", 8192},
        {"See_Major_Stealth_Heroes", 16384}, {"See_Credit_Income_Breakdown", 32768},
        {"See_Most_Powerful_Ship", 65536}, {"See_Force_Sensitive_Heroes", 131072}, {"See_Super_Weapons", 262144}}; // WGUI-98, implications at runtime WGUI-20.
    std::vector<SystemSpyDefinition> result;
    for (const auto* block : blocks) for (const auto& ability : block->children) {
        if (!ascii_iequals(ability.name, "System_Spy_Ability")) continue;
        tag_trace::used(*block); tag_trace::used(ability);
        SystemSpyDefinition spy;
        spy.influence_range = record.number("Galactic_Influence_Range").value_or(Fixed{}); // WGUI-102.
        for (const auto& attr : ability.attributes) if (ascii_iequals(attr.name, "Name")) {
            tag_trace::used_attribute(ability, attr.name); spy.name = trim(attr.value);
        }
        bool valid = true;
        for (const auto& child : ability.children) {
            if (retail::is_placeholder(child.raw_text)) continue;
            const auto flag = std::find_if(std::begin(flags), std::end(flags),
                [&](const auto& row) { return ascii_iequals(row.first, child.name); });
            if (flag != std::end(flags) || ascii_iequals(child.name, "Initially_Enabled")
                || ascii_iequals(child.name, "Causes_Despawn")) {
                tag_trace::used(child); const auto value = retail::boolean(child.raw_text);
                if (!value) { valid = false; record.issue(IssueKind::bad_value, &child, "Abilities/System_Spy_Ability/" + child.name, child.raw_text, "expected spy flag"); }
                else if (flag != std::end(flags)) { if (*value) spy.mask |= flag->second; else spy.mask &= ~flag->second; }
                else if (ascii_iequals(child.name, "Initially_Enabled")) spy.initially_enabled = *value;
                else spy.causes_despawn = *value;
            } else if (ascii_iequals(child.name, "Activation_Style")) {
                tag_trace::used(child); spy.activation_style = trim(child.raw_text);
            } else if (ascii_iequals(child.name, "Duration_In_Secs")) {
                tag_trace::used(child); const auto value = retail::leading_number(child.raw_text);
                if (value) spy.duration = value->value;
                else { valid = false; record.issue(IssueKind::bad_value, &child, "Abilities/System_Spy_Ability/" + child.name, child.raw_text, "expected spy duration"); }
            }
        }
        if (!valid) spy.initially_enabled = false;
        result.push_back(std::move(spy));
    }
    return result;
}
} // namespace eawr::data::galactic

namespace eawr::data::galactic {

const std::vector<std::string_view>& galactic_constant_tags() {
    // docs/galactic-data.md names the walk rule and the consuming ticket of each.
    static const std::vector<std::string_view> tags{
        "Object_Max_Speed_Multiplier_Galactic", // WGM-97
        "Corruption_Hyperspace_Bonus",          // WGM-27
        "Fiscal_Cycle_Time_In_Secs",            // WGS-05, WGEP-02
        "Good_Side_Name",                       // WGS-07
        "Evil_Side_Name",                       // WGS-07
        "Corrupt_Side_Name",                    // WGS-07
        "Credit_Cap_Per_Planet",
        "MaxCreditIncomeAlignmentBonus",
        "MaxCreditIncomeAlignmentPenalty",
        "Political_Income_Curve",
        "MaximumPoliticalControl",
        "Political_Control_Change_Time_Seconds",
        "Max_Ground_Forces_On_Planet",          // WGM-39
        "Raid_Force_Required_Faction",          // WGM-37
        "Raid_Force_Free_Object_Category_Mask",
        "Raid_Force_Limited_Object_Category_Mask",
        "Raid_Force_Max_Heros",
        "Raid_Force_Max_Limited_Objects",
        "TradeRouteMovementFactor",
        "MaximumFleetMovementDistance",
        "AIUsesFogOfWarGalactic",              // WGA-08/266
        "Planet_Reveal_Delay_Time",             // WGUI-02, GFC-21
        "Strategic_Queue_Tactical_Battles",
        "Battle_Pending_Timeout_Seconds",
        "AutoResolveDisplayTime",              // WAR-28
        "AutoResolveWinnerAttrition",          // WAR-29/39
        "AutoResolveLoserAttrition",
        "RetreatAutoResolveWinnerAttrition",
        "RetreatAutoResolveLoserAttrition",
        "AutoResolveAttritionAllowanceFactor", // WAR-44
        "AutoResolveTransportLosses",          // WAR-38
        "Auto_Resolve_Tactical_Multiplier",     // WAR-40 (mid-tactical seam)
    };
    return tags;
}

} // namespace eawr::data::galactic

namespace eawr::data::galactic::detail {

void load_constants(const vfs::Vfs& filesystem, GalacticData& data) {
    auto document = load_document(filesystem, constants_path);
    if (!document) {
        add_issue(data, IssueKind::missing_required, std::string(constants_path), 0, "GameConstants", "file", {},
            document.error().message);
        return;
    }
    record_input(data, document.value().source, document.value().input_sha256);
    tag_trace::document(document.value().root);
    const Record record("GameConstants", document.value().root, document.value().source.logical_path, data.issues);
    for (const auto tag : galactic_constant_tags()) {
        const auto* node = record.node(tag);
        if (node == nullptr) {
            record.issue(IssueKind::missing_required, nullptr, std::string(tag), {},
                "absent; its consumer must not invent a default");
            continue;
        }
        Constant constant{std::string(tag), trim(node->raw_text), std::nullopt};
        if (tag != "Good_Side_Name" && tag != "Evil_Side_Name" && tag != "Corrupt_Side_Name"
            && tag != "Strategic_Queue_Tactical_Battles" && tag != "Political_Income_Curve"
            && tag != "AIUsesFogOfWarGalactic" && tag != "Raid_Force_Required_Faction"
            && tag != "Raid_Force_Free_Object_Category_Mask" && tag != "Raid_Force_Limited_Object_Category_Mask") {
            constant.number = record.number(tag);
        }
        data.constants.push_back(std::move(constant));
    }
}

void check_references(const Catalog& catalog, GalacticData& data) {
    NameSet planets;
    for (const auto& planet : data.planets) planets.insert(lower(planet.name));
    NameSet factions;
    for (const auto& faction : data.factions) factions.insert(lower(faction.name));
    NameSet routes;
    for (const auto& route : data.trade_routes) routes.insert(lower(route.name));
    const auto object = [&](const std::string_view name) {
        return catalog.find(name, Category::game_object) != nullptr;
    };

    for (const auto& route : data.trade_routes) {
        for (const auto* point : {&route.point_a, &route.point_b}) {
            if (!point->empty() && !contains(planets, *point)) {
                add_issue(data, IssueKind::dangling_reference, route.file, 0, route.name, "Point", *point,
                    "the end point is not a planet");
            }
        }
    }
    for (const auto& faction : data.factions) {
        for (const auto& [tag, names] : {std::pair{"Allies", &faction.allies}, std::pair{"Enemies", &faction.enemies}})
            for (const auto& name : *names) if (!contains(factions, name))
                add_issue(data, IssueKind::dangling_reference, faction.file, 0, faction.name, tag, name, "not a faction");
        for (const auto& [tag, name, is_faction] : {std::tuple{"Primary_Enemy", &faction.primary_enemy, true},
                 std::tuple{"Home_Planet", &faction.home_planet, false},
                 std::tuple{"Faction_Leader", &faction.faction_leader, false},
                 std::tuple{"Faction_Leader_Company", &faction.faction_leader_company, false}}) {
            if (name->empty()) continue;
            const bool found = is_faction ? contains(factions, *name) : object(*name);
            if (!found) {
                add_issue(data, IssueKind::dangling_reference, faction.file, 0, faction.name, tag, *name,
                    is_faction ? "not a faction" : "not a game object");
            }
        }
        for (const auto& hero : faction.multiplayer_campaign_heroes) {
            if (!object(hero)) {
                add_issue(data, IssueKind::dangling_reference, faction.file, 0, faction.name,
                    "Multiplayer_Campaign_Heroes", hero, "not a game object");
            }
        }
    }

    for (auto& campaign : data.campaigns) {
        const auto issue = [&](const IssueKind kind, std::string tag, std::string value, std::string reason) {
            add_issue(data, kind, campaign.file, campaign.line, campaign.name, std::move(tag), std::move(value),
                std::move(reason));
        };
        NameSet locations;
        for (const auto& location : campaign.locations) {
            if (!locations.insert(lower(location)).second) {
                issue(IssueKind::duplicate_record, "Locations", location, "the location is listed twice");
                continue;
            }
            if (contains(planets, location)) {
                campaign.planets.push_back(location);
            } else if (!object(location)) {
                issue(IssueKind::dangling_reference, "Locations", location, "not a game object");
            }
        }
        for (const auto& name : campaign.trade_routes) {
            const auto* route = data.trade_route(name);
            if (route == nullptr) {
                issue(IssueKind::dangling_reference, "Trade_Routes", name, "not a trade route");
                continue;
            }
            for (const auto* point : {&route->point_a, &route->point_b}) {
                if (!point->empty() && !contains(locations, *point)) {
                    issue(IssueKind::dangling_reference, "Trade_Routes", name,
                        "end point " + *point + " is not a location of this campaign");
                }
            }
        }
        const auto faction = [&](const std::string_view tag, const std::string& name) {
            if (!name.empty() && !contains(factions, name)) {
                issue(IssueKind::dangling_reference, std::string(tag), name, "not a faction");
            }
        };
        const auto location = [&](const std::string_view tag, const std::string& name) {
            if (!contains(locations, name)) {
                issue(IssueKind::dangling_reference, std::string(tag), name, "not a location of this campaign");
            }
        };
        const auto placement_location = [&](const std::string_view tag, const std::string& name) {
            // WGS-01a: authored scene art remains in Locations, but cannot host a home or force.
            if (data.galaxy_art_object(name)) {
                issue(IssueKind::dangling_reference, std::string(tag), name, "scene art is not a placement location");
            } else {
                location(tag, name);
            }
        };
        faction("Starting_Active_Player", campaign.starting_active_player);
        for (const auto& value : campaign.home_locations) {
            faction("Home_Location", value.faction);
            placement_location("Home_Location", value.name);
        }
        for (const auto* values : {&campaign.starting_credits, &campaign.starting_tech_levels, &campaign.max_tech_levels}) {
            for (const auto& value : *values) faction("Faction record", value.faction);
        }
        for (const auto& value : campaign.ai_player_control) faction("AI_Player_Control", value.faction);
        for (const auto& value : campaign.markup_filenames) faction("Markup_Filename", value.faction);
        for (const auto& force : campaign.starting_forces) {
            faction("Starting_Forces", force.faction);
            placement_location("Starting_Forces", force.location);
            if (!object(force.type)) {
                issue(IssueKind::dangling_reference, "Starting_Forces", force.type, "not a game object");
            }
        }
        for (const auto& entry : campaign.special_case_production) {
            faction("Special_Case_Production", entry.faction);
            if (!contains(planets, entry.location)) {
                issue(IssueKind::dangling_reference, "Special_Case_Production", entry.location, "not a planet");
            }
            if (!object(entry.type)) {
                issue(IssueKind::dangling_reference, "Special_Case_Production", entry.type, "not a game object");
            }
        }
        for (const auto& name : campaign.human_victory_planet_names) {
            if (!contains(planets, name)) issue(IssueKind::dangling_reference, "Human_Victory_Planet_Names", name, "not a planet");
        }
        for (const auto& name : campaign.autoresolve_exclusion_locations) location("Autoresolve_Exclusion_Locations", name);
        for (const auto& value : campaign.corruption_level_overrides) location("Corruption_Level_Override", value.location);
    }
}

} // namespace eawr::data::galactic::detail
