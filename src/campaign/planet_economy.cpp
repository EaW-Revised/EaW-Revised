#include "eawr/campaign/planet_economy.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <sstream>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
using sim::math::Fixed;
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.message = "corruption economy authoring: " + message;
    return core::Result<T>::failure(std::move(d));
}
std::string key(std::string_view text) { std::string r(text); for (auto& c : r) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); return r; }
std::string trim(std::string_view text) { const auto first = text.find_first_not_of(" \r\n\t"); if (first == std::string_view::npos) return {}; return std::string(text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1)); }
std::vector<const data::Definition*> layers(const data::Catalog& c, std::string_view name, data::Category category) {
    std::vector<const data::Definition*> result; auto resolved = c.resolve(name, category);
    if (resolved) for (const auto& layer : resolved.value().chain) if (const auto* p = c.find(layer, category)) result.push_back(p);
    return result;
}
const data::XmlNode* child(const data::XmlNode& n, std::string_view name) {
    const data::XmlNode* r = nullptr; for (const auto& c : n.children) if (key(c.name) == key(name)) r = &c; return r;
}
core::Result<Fixed> number(const data::XmlNode* n) {
    if (!n || data::retail::is_placeholder(n->raw_text)) return fail<Fixed>("missing required number");
    data::tag_trace::used(n); auto value = data::retail::leading_number(n->raw_text);
    return value ? core::Result<Fixed>::success(value->value) : fail<Fixed>("invalid " + n->name);
}
core::Result<Fixed> number_or(const data::XmlNode* n, Fixed fallback) {
    return n ? number(n) : core::Result<Fixed>::success(fallback);
}
core::Result<std::vector<Fixed>> numbers(const data::XmlNode* n, std::size_t size) {
    if (!n) return fail<std::vector<Fixed>>("missing required numeric list");
    data::tag_trace::used(n); auto text = n->raw_text; for (auto& c : text) if (c == ',') c = ' ';
    std::istringstream stream(text); std::vector<Fixed> r;
    for (std::string word; stream >> word;) {
        auto value = data::retail::leading_number(word);
        if (!value) return fail<std::vector<Fixed>>("invalid " + n->name);
        r.push_back(value->value);
    }
    if (r.size() != size) return fail<std::vector<Fixed>>("wrong list length " + n->name);
    return core::Result<std::vector<Fixed>>::success(std::move(r));
}
core::Result<bool> flag(const data::XmlNode* n) {
    if (!n) { return fail<bool>("missing required flag"); }
    data::tag_trace::used(n);
    auto value = data::retail::boolean(n->raw_text); return value ? core::Result<bool>::success(*value) : fail<bool>("invalid " + n->name);
}
} // namespace
core::Result<gc::EconomyCorruptionState> prepare_corruption_economy(const vfs::Vfs& vfs,
    const data::Catalog& catalog, const CampaignSetupPlan& plan, const gc::GalacticSetup& setup) {
    auto constants = data::load_document(vfs, "Data/XML/GameConstants.xml");
    auto market = data::load_document(vfs, "Data/XML/BlackMarketItems.xml");
    if (!constants) return core::Result<gc::EconomyCorruptionState>::failure(constants.error());
    if (!market) return core::Result<gc::EconomyCorruptionState>::failure(market.error());
    return prepare_corruption_economy(constants.value(), market.value(), catalog, plan, setup);
}
core::Result<gc::EconomyCorruptionState> prepare_corruption_economy(const data::XmlDocument& constants,
    const data::XmlDocument& market, const data::Catalog& catalog, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup) {
    gc::EconomyCorruptionState result;
    auto factors = numbers(child(constants.root, "Corruption_Choice_Income_Percentage"), 8);
    auto low = number(child(constants.root, "Black_Market_Income_Mult_Min"));
    auto high = number(child(constants.root, "Black_Market_Income_Mult_Max"));
    if (!factors || !low || !high) return fail<gc::EconomyCorruptionState>("invalid income constants");
    std::copy(factors.value().begin(), factors.value().end(), result.income_factors.begin() + 1);
    result.factor_min = low.value(); result.factor_max = high.value();
    const auto campaign = layers(catalog, plan.campaign_name(), data::Category::campaign);
    if (const auto* n = data::retail::single(campaign, "Tutorial")) { auto value = flag(n); if (!value) return fail<gc::EconomyCorruptionState>("invalid tutorial flag"); result.tutorial = value.value(); }
    // Exact campaign multiplayer authoring; number of local human seats is not a proxy.
    if (const auto* n = data::retail::single(campaign, "Is_Multiplayer")) { auto value = flag(n); if (!value) return fail<gc::EconomyCorruptionState>("invalid multiplayer flag"); result.multiplayer = value.value(); }
    std::map<std::string, std::uint32_t> factions;
    for (const auto& d : catalog.definitions()) if (d.active && d.category == data::Category::faction) factions.emplace(key(d.id), 0);
    std::uint32_t next = 1; for (auto& [name, id] : factions) { (void)name; id = next++; }
    for (const auto& binding : plan.player_bindings()) {
        const auto original = layers(catalog, binding.faction, data::Category::faction);
        const auto faction = factions.find(key(binding.faction));
        auto playable = flag(data::retail::single(original, "Is_Playable"));
        if (faction == factions.end() || !playable) return fail<gc::EconomyCorruptionState>("missing faction eligibility");
        const auto* eligibility = data::retail::single(original, "Benefits_From_Corruption");
        auto benefits = eligibility ? flag(eligibility) : core::Result<bool>::success(false);
        if (!benefits || (!eligibility && playable.value())) return fail<gc::EconomyCorruptionState>("missing playable faction corruption eligibility");
        gc::CorruptionEconomyPlayer p; p.player = binding.player.player_id; p.faction = faction->second;
        p.benefits = benefits.value(); p.playable = playable.value();
        if (const auto* n = data::retail::single(original, "Displayed_Tech_Level_Adjustment")) {
            auto value = number(n);
            if (!value || value.value().raw() % Fixed::scale) return fail<gc::EconomyCorruptionState>("invalid displayed tech adjustment");
            p.displayed_tech_adjustment = value.value().trunc_to_integer();
        } // WGS-76: constructor adjustment zero; WGEP-99: no GC display clamp.
        p.human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& s) { return s.active && s.player == p.player && s.kind == gc::SeatKind::human; });
        if (!p.benefits && p.playable) {
            auto price = numbers(data::retail::single(original, "Corruption_Galactic_Production_Price_Multipliers"), 2);
            auto time = numbers(data::retail::single(original, "Corruption_Galactic_Production_Time_Multipliers"), 2);
            if (!price || !time) return fail<gc::EconomyCorruptionState>("missing corruption production factors");
            std::copy(price.value().begin(), price.value().end(), p.price.begin()); std::copy(time.value().begin(), time.value().end(), p.time.begin());
        }
        // WGEP-92: fresh empty technology-price list yields one.
        result.players.push_back(std::move(p));
    }
    std::sort(result.players.begin(), result.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (const auto& p : setup.planets) {
        const auto location = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& l) { return l.planet && l.planet->id == p.id; });
        if (location == plan.locations().end()) return fail<gc::EconomyCorruptionState>("missing planet binding");
        const auto original = layers(catalog, location->original_type, data::Category::game_object);
        gc::CorruptionEconomyPlanet row; row.planet = p.id;
        if (const auto* n = data::retail::single(original, "Planet_Black_Market_Influences")) { auto value = flag(n); if (!value) return fail<gc::EconomyCorruptionState>("invalid influenced flag"); row.influenced = value.value(); }
        auto modifier = number(data::retail::single(original, "Planet_Black_Market_Modifier"));
        if (!modifier) return core::Result<gc::EconomyCorruptionState>::failure(modifier.error());
        row.market_modifier = modifier.value(); result.planets.push_back(row);
    }
    std::uint32_t item_id = 1;
    for (const auto& node : market.root.children) {
        if (key(node.name) != "ITEM") continue;
        gc::corruption_fixture::MarketItem item; item.id = item_id++;
        const auto* faction = child(node, "Faction");
        if (!faction || !factions.contains(key(trim(faction->raw_text)))) return fail<gc::EconomyCorruptionState>("unbound item faction");
        data::tag_trace::used(faction); item.faction = factions.at(key(trim(faction->raw_text)));
        // WGEP-105: the ordinary item constructor supplies 1/0/0.
        auto tech = number_or(child(node, "Tech_Level"), gc::corruption_fixture::one);
        auto cost = number_or(child(node, "Base_Cost_Credits"), Fixed{});
        auto uncommon = number_or(child(node, "Uncommon_Price_Multiplier"), Fixed{});
        if (!tech || !cost || !uncommon || tech.value().raw() % Fixed::scale
            || tech.value().trunc_to_integer() < std::numeric_limits<int>::min() || tech.value().trunc_to_integer() > std::numeric_limits<int>::max())
            return fail<gc::EconomyCorruptionState>("invalid item authoring");
        item.tech = static_cast<int>(tech.value().trunc_to_integer()); item.base_cost = cost.value(); item.uncommon_multiplier = uncommon.value();
        // Unverified omission defaults are not applied in tutorial play.
        if (result.tutorial || child(node, "Tutorial_Tech_Level") || child(node, "Tutorial_Base_Cost_Credits")) {
            auto tutorial = number(child(node, "Tutorial_Tech_Level")); auto tutorial_cost = number(child(node, "Tutorial_Base_Cost_Credits"));
            if (!tutorial || !tutorial_cost || tutorial.value().raw() % Fixed::scale
                || tutorial.value().trunc_to_integer() < std::numeric_limits<int>::min() || tutorial.value().trunc_to_integer() > std::numeric_limits<int>::max())
                return fail<gc::EconomyCorruptionState>("tutorial-item-omission seam: explicit tech/cost required");
            item.tutorial_tech = static_cast<int>(tutorial.value().trunc_to_integer()); item.tutorial_base_cost = tutorial_cost.value();
        }
        result.items.push_back(item);
    }
    return core::Result<gc::EconomyCorruptionState>::success(std::move(result));
}
core::Result<gc::BlackMarketBuyer> prepare_black_market_buyer(const data::Catalog& catalog,
    std::string_view type, sim::EntityId id, gc::PlayerId owner, bool opposing) {
    const auto original = layers(catalog, type, data::Category::game_object);
    const data::XmlNode* ability = nullptr;
    for (const auto* layer : original) {
        if (const auto* abilities = child(layer->root, "Abilities")) ability = child(*abilities, "Black_Market_Ability");
        if (ability) break;
    }
    if (!ability) return fail<gc::BlackMarketBuyer>("missing black-market ability");
    auto price = number_or(child(*ability, "Price_Modifier"), gc::corruption_fixture::one); // WGEP-106
    const auto* despawn_node = child(*ability, "Causes_Despawn");
    auto despawn = despawn_node ? flag(despawn_node) : core::Result<bool>::success(true);
    auto low = numbers(child(*ability, "Min_Respawn_Times"), 5); auto high = numbers(child(*ability, "Max_Respawn_Times"), 5);
    if (!price || !despawn) return fail<gc::BlackMarketBuyer>("invalid black-market ability");
    gc::BlackMarketBuyer result; result.buyer = id; result.owner = owner; result.opposing_planet = opposing;
    result.price_modifier = price.value(); result.causes_despawn = true; // WGEP-107 validation forces this route.
    // WGEP-107: absent or invalid respawn lists clear both arrays.
    bool valid = low && high;
    if (valid) for (const auto* values : {&low.value(), &high.value()}) {
        const auto zero = std::count(values->begin(), values->end(), Fixed{});
        if (zero != 0 && zero != 5) valid = false;
    }
    if (valid) for (std::size_t i = 0; i < 5; ++i)
        if (low.value()[i] < Fixed{} || low.value()[i] > high.value()[i]) valid = false;
    if (valid) { result.absence_min = std::move(low).value(); result.absence_max = std::move(high).value(); }
    return core::Result<gc::BlackMarketBuyer>::success(std::move(result));
}
} // namespace eawr::campaign
