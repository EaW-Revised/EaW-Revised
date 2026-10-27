#include "eawr/campaign/planet_economy.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
using sim::math::Fixed;
std::string key(std::string_view text) {
    std::string result(text); for (auto& c : result) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); return result;
}
std::vector<std::string> words(std::string text) {
    for (auto& c : text) if (c == ',') c = ' ';
    std::istringstream in(text); std::vector<std::string> result;
    for (std::string word; in >> word;) result.push_back(std::move(word));
    return result;
}
std::vector<const data::Definition*> layers(const data::Catalog& catalog, std::string_view name, data::Category category) {
    std::vector<const data::Definition*> result; auto resolved = catalog.resolve(name, category);
    if (resolved) for (const auto& original : resolved.value().chain)
        if (const auto* row = catalog.find(original, category)) result.push_back(row);
    return result;
}
core::Result<void> invalid(const char* message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.message = message; return core::Result<void>::failure(d);
}
bool number(const data::XmlNode* node, Fixed& value) {
    if (!node || data::retail::is_placeholder(node->raw_text)) { value = {}; return true; } // WGCH-172
    data::tag_trace::used(node); const auto parsed = data::retail::leading_number(node->raw_text);
    if (!parsed) return false;
    value = parsed->value;
    return true;
}
bool count(const data::XmlNode* node, std::uint32_t& value) {
    Fixed parsed; if (!number(node, parsed) || parsed < Fixed{} || parsed.raw() % Fixed::scale
        || parsed.trunc_to_integer() > std::numeric_limits<std::uint32_t>::max()) return false;
    value = static_cast<std::uint32_t>(parsed.trunc_to_integer()); return true;
}
std::optional<gc::corruption_fixture::Type> corruption_type(const data::XmlNode* node) {
    using T = gc::corruption_fixture::Type;
    if (!node || data::retail::is_placeholder(node->raw_text)) return T::none; // WGCH-172
    data::tag_trace::used(node); const auto values = words(node->raw_text); if (values.size() != 1) return {};
    const std::array<std::string_view, 9> names{"CORRUPTION_NONE", "CORRUPTION_INTIMIDATION", "CORRUPTION_PIRACY", "CORRUPTION_KIDNAPPING",
        "CORRUPTION_RACKETEERING", "CORRUPTION_CORRUPT_MILITIA", "CORRUPTION_BRIBERY", "CORRUPTION_BLACK_MARKET", "CORRUPTION_BONDED_CITIZENS"};
    const auto at = std::find(names.begin(), names.end(), key(values.front()));
    return at == names.end() ? std::nullopt : std::optional(static_cast<T>(at - names.begin()));
}
std::string text(const data::XmlNode* node) {
    if (!node || data::retail::is_placeholder(node->raw_text)) return {};
    data::tag_trace::used(node); const auto first = node->raw_text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return node->raw_text.substr(first, node->raw_text.find_last_not_of(" \t\r\n") - first + 1);
}
}
core::Result<void> prepare_corruption_host(const data::Catalog& catalog, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup, gc::GalacticAbilitySetup& host,
    const std::function<gc::TypeId(std::string_view)>& identity, core::LoadNoteBuffer* notes) {
    if (!setup.control || !setup.census || !setup.economy_corruption) return invalid("corruption authoring requires live control/census/economy");
    host.multiplayer = setup.economy_corruption->multiplayer;
    const auto campaign = layers(catalog, plan.campaign_name(), data::Category::campaign);
    if (const auto* tutorial = data::retail::single(campaign, "Tutorial")) {
        data::tag_trace::used(tutorial); const auto flag = data::retail::boolean(tutorial->raw_text);
        if (!flag) campaign_load_note(notes, "EAWR-LOAD-1220", tutorial, plan.campaign_name(), "Tutorial", "false");
        if (flag && *flag) host.tutorial_corruption_allowed = false; // named scripted destination-admission seam
    }
    const auto ids = [&](const data::XmlNode* node, std::vector<gc::TypeId>& out) {
        if (!node || data::retail::is_placeholder(node->raw_text)) return true;
        data::tag_trace::used(node);
        for (const auto& name : words(node->raw_text)) { const auto id = identity(name); if (!id) return false; out.push_back(id); }
        return true;
    };
    for (const auto& planet : setup.planets) {
        const auto source = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& l) { return l.planet && l.planet->id == planet.id; });
        const auto political = std::lower_bound(setup.control->planets.begin(), setup.control->planets.end(), planet.id,
            [](const auto& p, auto id) { return p.planet < id; });
        if (source == plan.locations().end() || political == setup.control->planets.end() || political->planet != planet.id) return invalid("unbound corruption planet");
        const auto original = layers(catalog, source->original_type, data::Category::game_object);
        gc::CorruptionPlanet p; p.planet = planet.id; p.type = political->type; p.retains_influence = political->retains_residual;
        for (std::size_t index = 0; index < p.slots.size(); ++index) {
            auto& c = p.slots[index]; const auto prefix = "Corruption_" + std::to_string(index + 1) + "_";
            const auto node = [&](std::string_view suffix) { return data::retail::single(original, prefix + std::string(suffix)); };
            const auto type = corruption_type(node("Type")), required = corruption_type(node("Required_Mission_Type"));
            const data::XmlNode* bad = nullptr;
            std::string bad_tag;
            const auto checked = [&](bool valid, std::string_view suffix) {
                if (!valid) { bad = node(suffix); bad_tag = prefix + std::string(suffix); }
                return valid;
            };
            if (!checked(type.has_value(), "Type") || !checked(required.has_value(), "Required_Mission_Type")
                || !checked(number(node("Credit_Cost"), c.price) && c.price >= Fixed{}, "Credit_Cost")
                || !checked(count(node("Required_Unit_Count"), c.unit_count), "Required_Unit_Count")
                || !checked(count(node("Required_Mission_Count"), c.mission_count), "Required_Mission_Count")
                || !checked(number(node("Success_Credit_Bonus"), c.credit_reward), "Success_Credit_Bonus")
                || !checked(ids(node("Required_Unit_Type"), c.unit_types), "Required_Unit_Type")
                || !checked(ids(node("Required_Planets"), c.planet_types), "Required_Planets")) {
                campaign_load_note(notes, "EAWR-LOAD-1221", bad ? bad : (original.empty() ? nullptr : &original.front()->root),
                    source->original_type, bad_tag, "skip slot", "WGCH-172/173: unknown invalid-data behavior",
                    core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object);
                c = {}; continue;
            }
            c.type = *type; c.required_mission_type = *required; c.briefing = text(node("Mission_Briefing"));
            c.map = text(node("Tactical_Map_Name")); c.script = text(node("Tactical_Script_Name"));
            const auto mode = key(text(node("Tactical_Game_Mode")));
            if (!mode.empty() && mode != "LAND" && mode != "SPACE") {
                campaign_load_note(notes, "EAWR-LOAD-1221", node("Tactical_Game_Mode"), source->original_type,
                    prefix + "Tactical_Game_Mode", "skip slot", "unknown", core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object);
                c = {}; continue;
            }
            c.mode = mode == "LAND" ? 1 : mode == "SPACE" ? 2 : 0;
            const auto unlock = text(node("Success_Unlock_Unit_Type"));
            if (!unlock.empty()) {
                c.build_unlock = identity(unlock);
                if (!c.build_unlock) {
                    campaign_load_note(notes, "EAWR-LOAD-1221", node("Success_Unlock_Unit_Type"), source->original_type,
                        prefix + "Success_Unlock_Unit_Type", "skip slot", "unknown", core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object);
                    c = {}; continue;
                }
                const auto unlocked = layers(catalog, unlock, data::Category::game_object);
                const auto* cost = data::retail::single(unlocked, "Build_Cost_Credits");
                if (!cost || !number(cost, c.unlock_build_cost) || c.unlock_build_cost < Fixed{}) {
                    campaign_load_note(notes, "EAWR-LOAD-1222", cost, unlock, "Build_Cost_Credits", "0"); c.unlock_build_cost = {};
                }
            }
        }
        host.corruption_planets.push_back(std::move(p));
    }
    for (const auto& binding : plan.player_bindings()) {
        gc::CorruptionPlayer p; p.player = binding.player.player_id;
        const auto original = layers(catalog, binding.faction, data::Category::faction);
        const auto* node = data::retail::single(original, "Automatic_Planetary_Corruption_Upgrade_Type");
        if (node && !data::retail::is_placeholder(node->raw_text)) {
            data::tag_trace::used(node); const auto names = words(node->raw_text);
            // Entries are positional by corruption type; a shorter authored list leaves the trailing types without
            // an upgrade, and only entries past the modelled corruption types have nothing to bind to.
            if (names.size() > p.automatic_upgrades.size() - 1)
                campaign_load_note(notes, "EAWR-LOAD-1223", node, binding.faction, node->name, "truncate to corruption type count");
            for (std::size_t i = 0; i < std::min(names.size(), p.automatic_upgrades.size() - 1); ++i) {
                if (data::retail::is_placeholder(names[i])) continue;
                p.automatic_upgrades[i + 1] = identity(names[i]);
                if (!p.automatic_upgrades[i + 1]) campaign_load_note(notes, "EAWR-LOAD-1223", node, binding.faction, node->name, "unresolved entries remain empty");
            }
        }
        host.corruption_players.push_back(p);
    }
    for (const auto& binding : plan.type_bindings()) {
        if (std::none_of(setup.census->types.begin(), setup.census->types.end(), [&](const auto& t) { return t.type == binding.id; })) continue;
        const auto* type = plan.original_type(binding.id); if (!type) {
            campaign_load_note(notes, "EAWR-LOAD-1224", nullptr, binding.original_type, "prerequisite", "skip unresolved prerequisite"); continue;
        }
        const auto original = layers(catalog, binding.original_type, data::Category::game_object);
        bool consume = true; // WGCH-171: qualified original type byte-member default
        if (const auto* node = data::retail::single(original, "Destroy_When_Used_As_Corruption_Prereq")) {
            data::tag_trace::used(node); const auto value = data::retail::boolean(node->raw_text);
            if (!value) campaign_load_note(notes, "EAWR-LOAD-1224", node, binding.original_type, node->name, "true");
            consume = value.value_or(true);
        }
        host.corruption_prerequisites.push_back({binding.id, consume, type->named_hero.value_or(false)});
    }
    std::sort(host.corruption_planets.begin(), host.corruption_planets.end(), [](const auto& a, const auto& b) { return a.planet < b.planet; });
    std::sort(host.corruption_players.begin(), host.corruption_players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    std::sort(host.corruption_prerequisites.begin(), host.corruption_prerequisites.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& planet : setup.planets) {
        const auto player = std::find_if(setup.economy_corruption->players.begin(), setup.economy_corruption->players.end(),
            [&](const auto& p) { return p.player == planet.owner; });
        if (player != setup.economy_corruption->players.end() && player->benefits)
            host.initial_corruption.push_back({planet.id, gc::corruption_fixture::Type::racketeering, planet.owner}); // WGCH-165/168
    }
    for (const auto* node : data::retail::list(campaign, "Corruption_Level_Override", data::retail::ListRule::replace_in_variant)) {
        data::tag_trace::used(node); const auto pair = words(node->raw_text);
        if (pair.size() != 2) { campaign_load_note(notes, "EAWR-LOAD-1225", node, plan.campaign_name(), node->name, "skip override"); continue; }
        const auto type = identity(pair[0]); const auto level = Fixed::from_decimal(pair[1]);
        if (!type || !level) { campaign_load_note(notes, "EAWR-LOAD-1225", node, plan.campaign_name(), node->name, "skip override"); continue; }
        const auto found = std::find_if(host.corruption_planets.begin(), host.corruption_planets.end(), [&](const auto& p) { return p.type == type; });
        if (found == host.corruption_planets.end()) continue;
        if (std::count_if(host.corruption_planets.begin(), host.corruption_planets.end(), [&](const auto& p) { return p.type == type; }) > 1)
        { campaign_load_note(notes, "EAWR-LOAD-1225", node, plan.campaign_name(), node->name, "skip ambiguous override"); continue; }
        const auto planet = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& p) { return p.id == found->planet; });
        host.initial_corruption.push_back({found->planet, level.value() > Fixed{} ? gc::corruption_fixture::Type::racketeering : gc::corruption_fixture::Type::none, planet->owner});
    }
    return core::Result<void>::success();
}
} // namespace eawr::campaign
