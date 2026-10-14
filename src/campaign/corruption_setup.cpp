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
    const std::function<gc::TypeId(std::string_view)>& identity) {
    if (!setup.control || !setup.census || !setup.economy_corruption) return invalid("corruption authoring requires live control/census/economy");
    host.multiplayer = setup.economy_corruption->multiplayer;
    const auto campaign = layers(catalog, plan.campaign_name(), data::Category::campaign);
    if (const auto* tutorial = data::retail::single(campaign, "Tutorial")) {
        data::tag_trace::used(tutorial); const auto flag = data::retail::boolean(tutorial->raw_text);
        if (!flag) return invalid("invalid corruption tutorial mode");
        if (*flag) host.tutorial_corruption_allowed = false; // named scripted destination-admission seam
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
            if (!type || !required || !number(node("Credit_Cost"), c.price) || c.price < Fixed{}
                || !count(node("Required_Unit_Count"), c.unit_count) || !count(node("Required_Mission_Count"), c.mission_count)
                || !number(node("Success_Credit_Bonus"), c.credit_reward) || !ids(node("Required_Unit_Type"), c.unit_types)
                || !ids(node("Required_Planets"), c.planet_types)) return invalid("invalid corruption slot inputs");
            c.type = *type; c.required_mission_type = *required; c.briefing = text(node("Mission_Briefing"));
            c.map = text(node("Tactical_Map_Name")); c.script = text(node("Tactical_Script_Name"));
            const auto mode = key(text(node("Tactical_Game_Mode")));
            if (!mode.empty() && mode != "LAND" && mode != "SPACE") return invalid("invalid corruption mission mode");
            c.mode = mode == "LAND" ? 1 : mode == "SPACE" ? 2 : 0;
            const auto unlock = text(node("Success_Unlock_Unit_Type"));
            if (!unlock.empty()) {
                c.build_unlock = identity(unlock); if (!c.build_unlock) return invalid("unbound corruption build unlock");
                const auto unlocked = layers(catalog, unlock, data::Category::game_object);
                const auto* cost = data::retail::single(unlocked, "Build_Cost_Credits");
                if (!cost || !number(cost, c.unlock_build_cost)) return invalid("unbound corruption unlock build cost");
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
            if (names.size() > p.automatic_upgrades.size() - 1) return invalid("corruption automatic upgrade list names more types than the corruption model has");
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (data::retail::is_placeholder(names[i])) continue;
                p.automatic_upgrades[i + 1] = identity(names[i]);
                if (!p.automatic_upgrades[i + 1]) return invalid("unbound corruption automatic upgrade");
            }
        }
        host.corruption_players.push_back(p);
    }
    for (const auto& binding : plan.type_bindings()) {
        if (std::none_of(setup.census->types.begin(), setup.census->types.end(), [&](const auto& t) { return t.type == binding.id; })) continue;
        const auto* type = plan.original_type(binding.id); if (!type) return invalid("unbound corruption prerequisite original type");
        const auto original = layers(catalog, binding.original_type, data::Category::game_object);
        bool consume = true; // WGCH-171: qualified original type byte-member default
        if (const auto* node = data::retail::single(original, "Destroy_When_Used_As_Corruption_Prereq")) {
            data::tag_trace::used(node); const auto value = data::retail::boolean(node->raw_text);
            if (!value) return invalid("invalid corruption prerequisite destruction flag");
            consume = *value;
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
        if (pair.size() != 2) return invalid("invalid starting corruption override");
        const auto type = identity(pair[0]); const auto level = Fixed::from_decimal(pair[1]);
        if (!type || !level) return invalid("unbound starting corruption override");
        const auto found = std::find_if(host.corruption_planets.begin(), host.corruption_planets.end(), [&](const auto& p) { return p.type == type; });
        if (found == host.corruption_planets.end()) continue;
        if (std::count_if(host.corruption_planets.begin(), host.corruption_planets.end(), [&](const auto& p) { return p.type == type; }) > 1)
            return invalid("starting corruption override has ambiguous active planet type");
        const auto planet = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& p) { return p.id == found->planet; });
        host.initial_corruption.push_back({found->planet, level.value() > Fixed{} ? gc::corruption_fixture::Type::racketeering : gc::corruption_fixture::Type::none, planet->owner});
    }
    return core::Result<void>::success();
}
} // namespace eawr::campaign
