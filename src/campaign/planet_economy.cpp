#include "eawr/campaign/planet_economy.hpp"
#include "eawr/core/text.hpp"
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
core::Result<Fixed> retail_number(const data::XmlNode* n, core::LoadNoteBuffer* notes = nullptr, std::string_view object = {}) {
    if (!n || data::retail::is_placeholder(n->raw_text)) return fail<Fixed>("missing required number");
    data::tag_trace::used(n); auto value = data::retail::float_value(n->raw_text);
    if (value && !value->exact) campaign_load_note(notes, "EAWR-LOAD-1208", n, object, n->name,
        std::to_string(value->value.raw()) + "/" + std::to_string(Fixed::scale), "LSA-P3: float parse", core::LoadCategory::retail_accepts);
    return value ? core::Result<Fixed>::success(value->value) : fail<Fixed>("invalid " + n->name);
}
core::Result<Fixed> number_or(const data::XmlNode* n, Fixed fallback, core::LoadNoteBuffer* notes = nullptr, std::string_view object = {}) {
    return n ? retail_number(n, notes, object) : core::Result<Fixed>::success(fallback);
}
core::Result<std::vector<Fixed>> numbers(const data::XmlNode* n, std::size_t size, core::LoadNoteBuffer* notes = nullptr, std::string_view object = {}) {
    if (!n) return fail<std::vector<Fixed>>("missing required numeric list");
    data::tag_trace::used(n); auto text = n->raw_text; for (auto& c : text) if (c == ',') c = ' ';
    std::istringstream stream(text); std::vector<Fixed> r;
    for (std::string word; stream >> word;) {
        auto value = data::retail::float_value(word);
        if (!value) return fail<std::vector<Fixed>>("invalid " + n->name);
        if (!value->exact) campaign_load_note(notes, "EAWR-LOAD-1208", n, object, n->name,
            "retail float tokens", "LSA-P3: float parse", core::LoadCategory::retail_accepts);
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
    const data::Catalog& catalog, const CampaignSetupPlan& plan, const gc::GalacticSetup& setup, core::LoadNoteBuffer* notes) {
    auto constants = data::load_document(vfs, "Data/XML/GameConstants.xml");
    auto market = data::load_document(vfs, "Data/XML/BlackMarketItems.xml");
    if (!constants) return core::Result<gc::EconomyCorruptionState>::failure(constants.error());
    if (!market) return core::Result<gc::EconomyCorruptionState>::failure(market.error());
    return prepare_corruption_economy(constants.value(), market.value(), catalog, plan, setup, notes);
}
core::Result<gc::EconomyCorruptionState> prepare_corruption_economy(const data::XmlDocument& constants,
    const data::XmlDocument& market, const data::Catalog& catalog, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup, core::LoadNoteBuffer* notes) {
    gc::EconomyCorruptionState result;
    auto factors = numbers(child(constants.root, "Corruption_Choice_Income_Percentage"), 8, notes, "GameConstants");
    auto low = retail_number(child(constants.root, "Black_Market_Income_Mult_Min"), notes, "GameConstants");
    auto high = retail_number(child(constants.root, "Black_Market_Income_Mult_Max"), notes, "GameConstants");
    if (!factors) {
        campaign_load_note(notes, "EAWR-LOAD-1201", child(constants.root, "Corruption_Choice_Income_Percentage") ? child(constants.root, "Corruption_Choice_Income_Percentage") : &constants.root,
            "GameConstants", "Corruption_Choice_Income_Percentage", "eight neutral factors (1)");
        factors = core::Result<std::vector<Fixed>>::success(std::vector<Fixed>(8, gc::corruption_fixture::one));
    }
    if (!low || !high || low.value() < Fixed{} || high.value() < low.value()) {
        campaign_load_note(notes, "EAWR-LOAD-1201", &constants.root, "GameConstants",
            "Black_Market_Income_Mult_Min/Max", "1/1");
        low = high = core::Result<Fixed>::success(gc::corruption_fixture::one);
    }
    const auto bounded = [&](Fixed value, const data::XmlNode* node, std::string_view object, std::string_view tag) {
        const auto effective = std::clamp(value, Fixed::from_raw(-(std::int64_t{1} << 48)), Fixed::from_raw(std::int64_t{1} << 48));
        if (effective != value) campaign_load_note(notes, "EAWR-LOAD-1209", node, object, tag,
            std::to_string(effective.raw()) + "/" + std::to_string(Fixed::scale), "unknown: project arithmetic support range",
            core::LoadCategory::unknown_degrade, core::LoadAction::clamped);
        return effective;
    };
    for (auto& factor : factors.value()) factor = bounded(factor, child(constants.root, "Corruption_Choice_Income_Percentage"),
        "GameConstants", "Corruption_Choice_Income_Percentage");
    low = core::Result<Fixed>::success(bounded(low.value(), child(constants.root, "Black_Market_Income_Mult_Min"), "GameConstants", "Black_Market_Income_Mult_Min"));
    high = core::Result<Fixed>::success(bounded(high.value(), child(constants.root, "Black_Market_Income_Mult_Max"), "GameConstants", "Black_Market_Income_Mult_Max"));
    std::copy(factors.value().begin(), factors.value().end(), result.income_factors.begin() + 1);
    result.factor_min = low.value(); result.factor_max = high.value();
    const auto campaign = layers(catalog, plan.campaign_name(), data::Category::campaign);
    if (const auto* n = data::retail::single(campaign, "Tutorial")) { auto value = flag(n); if (!value) campaign_load_note(notes, "EAWR-LOAD-1202", n, plan.campaign_name(), "Tutorial", "false"); result.tutorial = value ? value.value() : false; }
    // The setup's multiplayer mode, the one flag every multiplayer rule branches on (GCMP-24). Neither the
    // campaign's Is_Multiplayer menu tag nor the number of local human seats is a proxy for it.
    result.multiplayer = plan.mode() == CampaignSetupMode::multiplayer;
    std::map<std::string, std::uint32_t> factions;
    for (const auto& d : catalog.definitions()) if (d.active && d.category == data::Category::faction) factions.emplace(key(d.id), 0);
    std::uint32_t next = 1; for (auto& [name, id] : factions) { (void)name; id = next++; }
    for (const auto& binding : plan.player_bindings()) {
        const auto original = layers(catalog, binding.faction, data::Category::faction);
        auto playable = flag(data::retail::single(original, "Is_Playable"));
        if (!factions.contains(key(binding.faction))) {
            campaign_load_note(notes, "EAWR-LOAD-1202", nullptr, binding.faction, "Faction", "retain player with corruption disabled");
            factions.emplace(key(binding.faction), next++);
        }
        const auto faction_id = factions.at(key(binding.faction));
        if (!playable) { campaign_load_note(notes, "EAWR-LOAD-1202", data::retail::single(original, "Is_Playable"), binding.faction,
            "Is_Playable", "false"); playable = core::Result<bool>::success(false); }
        const auto* eligibility = data::retail::single(original, "Benefits_From_Corruption");
        // WGS-94: raw catalog consumers also retain the false constructor default on omission.
        auto benefits = eligibility ? flag(eligibility) : core::Result<bool>::success(false);
        if (!benefits) { campaign_load_note(notes, "EAWR-LOAD-1202", eligibility, binding.faction, "Benefits_From_Corruption", "false");
            benefits = core::Result<bool>::success(false); }
        gc::CorruptionEconomyPlayer p; p.player = binding.player.player_id; p.faction = faction_id;
        p.benefits = benefits.value(); p.playable = playable.value();
        if (const auto* n = data::retail::single(original, "Displayed_Tech_Level_Adjustment")) {
            data::tag_trace::used(n);
            p.displayed_tech_adjustment = data::retail::integer_constant(n->raw_text);
        } // WGS-76: constructor adjustment zero; WGEP-99: no GC display clamp.
        p.human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& s) { return s.active && s.player == p.player && s.kind == gc::SeatKind::human; });
        if (!p.benefits && p.playable) {
            auto price = numbers(data::retail::single(original, "Corruption_Galactic_Production_Price_Multipliers"), 2, notes, binding.faction);
            auto time = numbers(data::retail::single(original, "Corruption_Galactic_Production_Time_Multipliers"), 2, notes, binding.faction);
            const auto safe_factors = [&](auto& parsed, std::string_view tag) {
                if (parsed) return;
                campaign_load_note(notes, "EAWR-LOAD-1203", data::retail::single(original, tag) ? data::retail::single(original, tag) : (original.empty() ? nullptr : &original.front()->root), binding.faction, tag, "1,1");
                parsed = core::Result<std::vector<Fixed>>::success(std::vector<Fixed>(2, gc::corruption_fixture::one));
            };
            safe_factors(price, "Corruption_Galactic_Production_Price_Multipliers");
            safe_factors(time, "Corruption_Galactic_Production_Time_Multipliers");
            std::copy(price.value().begin(), price.value().end(), p.price.begin()); std::copy(time.value().begin(), time.value().end(), p.time.begin());
        }
        // WGEP-92: fresh empty technology-price list yields one.
        result.players.push_back(std::move(p));
    }
    std::sort(result.players.begin(), result.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (const auto& p : setup.planets) {
        const auto location = std::find_if(plan.locations().begin(), plan.locations().end(), [&](const auto& l) { return l.planet && l.planet->id == p.id; });
        if (location == plan.locations().end()) {
            campaign_load_note(notes, "EAWR-LOAD-1204", nullptr, std::to_string(p.id), "Planet", "zero modifier, no influence");
            gc::CorruptionEconomyPlanet row; row.planet = p.id; result.planets.push_back(row); continue;
        }
        const auto original = layers(catalog, location->original_type, data::Category::game_object);
        gc::CorruptionEconomyPlanet row; row.planet = p.id;
        if (const auto* n = data::retail::single(original, "Planet_Black_Market_Influences")) { auto value = flag(n); if (!value) campaign_load_note(notes, "EAWR-LOAD-1204", n, location->original_type, "Planet_Black_Market_Influences", "false"); row.influenced = value ? value.value() : false; }
        // CGS-11: an omitted modifier keeps the zero a freshly constructed planet type holds. Unverified in the debug
        // build; the omission is reported when the services are prepared.
        const auto* modifier_node = data::retail::single(original, "Planet_Black_Market_Modifier");
        auto modifier = modifier_node ? retail_number(modifier_node, notes, location->original_type) : core::Result<Fixed>::success(Fixed{});
        if (!modifier || !modifier_node) {
            campaign_load_note(notes, "EAWR-LOAD-1204", modifier_node ? modifier_node : (original.empty() ? nullptr : &original.front()->root), location->original_type, "Planet_Black_Market_Modifier", "0");
            modifier = core::Result<Fixed>::success(Fixed{});
        }
        row.market_modifier = bounded(modifier.value(), modifier_node, location->original_type, "Planet_Black_Market_Modifier"); result.planets.push_back(row);
    }
    std::uint32_t item_id = 1;
    for (const auto& node : market.root.children) {
        if (key(node.name) != "ITEM") continue;
        gc::corruption_fixture::MarketItem item; item.id = item_id++;
        const auto* faction = child(node, "Faction");
        if (!faction || !factions.contains(key(core::trim(faction->raw_text, core::xml_whitespace)))) {
            campaign_load_note(notes, "EAWR-LOAD-1205", faction ? faction : &node, std::to_string(item.id), "Faction",
                "skip item", "unknown", core::LoadCategory::unknown_degrade, core::LoadAction::skipped_object); continue;
        }
        data::tag_trace::used(faction); item.faction = factions.at(key(core::trim(faction->raw_text, core::xml_whitespace)));
        // WGEP-105: the ordinary item constructor supplies 1/0/0.
        const auto ordinary = [&](std::string_view tag, Fixed fallback, bool integral = false) {
            const auto* n = child(node, tag);
            if (!n || data::retail::is_placeholder(n->raw_text)) {
                campaign_load_note(notes, "EAWR-LOAD-1200", n ? n : &node, std::to_string(item.id), tag,
                    std::to_string(fallback.trunc_to_integer()), "WGEP-105: ordinary constructor 1/0/0", core::LoadCategory::retail_accepts);
                return fallback;
            }
            data::tag_trace::used(n);
            if (integral) {
                const auto parsed = data::retail::integer_value(n->raw_text);
                if (parsed && !parsed->exact) campaign_load_note(notes, "EAWR-LOAD-1200", n, std::to_string(item.id), tag,
                    std::to_string(parsed->value), "LSA-P1: signed integer parse", core::LoadCategory::retail_accepts);
                return Fixed::from_raw(static_cast<std::int64_t>(parsed->value) * Fixed::scale);
            }
            const auto parsed = data::retail::float_value(n->raw_text);
            if (parsed && !parsed->exact) campaign_load_note(notes, "EAWR-LOAD-1200", n, std::to_string(item.id), tag,
                std::to_string(parsed->value.raw()) + "/" + std::to_string(Fixed::scale), "LSA-P3: float parse", core::LoadCategory::retail_accepts);
            return parsed->value;
        };
        item.tech = static_cast<int>(ordinary("Tech_Level", gc::corruption_fixture::one, true).trunc_to_integer());
        item.base_cost = ordinary("Base_Cost_Credits", Fixed{});
        item.uncommon_multiplier = ordinary("Uncommon_Price_Multiplier", Fixed{});
        // Unverified omission defaults are not applied in tutorial play.
        if (result.tutorial || child(node, "Tutorial_Tech_Level") || child(node, "Tutorial_Base_Cost_Credits")) {
            auto tutorial = number(child(node, "Tutorial_Tech_Level")); auto tutorial_cost = number(child(node, "Tutorial_Base_Cost_Credits"));
            if (!tutorial || !tutorial_cost || tutorial.value().raw() % Fixed::scale
                || tutorial.value().trunc_to_integer() < std::numeric_limits<int>::min() || tutorial.value().trunc_to_integer() > std::numeric_limits<int>::max())
                return fail<gc::EconomyCorruptionState>("tutorial-item-omission seam: explicit tech/cost required");
            item.tutorial_tech = static_cast<int>(tutorial.value().trunc_to_integer()); item.tutorial_base_cost = tutorial_cost.value();
        }
        const auto supported = [&](Fixed value, std::string_view tag, Fixed minimum) {
            const auto bounded = std::clamp(value, minimum, Fixed::from_raw(std::int64_t{1} << 48));
            if (bounded != value) campaign_load_note(notes, "EAWR-LOAD-1209", child(node, tag) ? child(node, tag) : &node,
                std::to_string(item.id), tag, std::to_string(bounded.raw()) + "/" + std::to_string(Fixed::scale),
                "unknown: project arithmetic support range", core::LoadCategory::unknown_degrade, core::LoadAction::clamped);
            return bounded;
        };
        item.base_cost = supported(item.base_cost, "Base_Cost_Credits", Fixed::from_raw(-(std::int64_t{1} << 48)));
        item.uncommon_multiplier = supported(item.uncommon_multiplier, "Uncommon_Price_Multiplier", Fixed{});
        result.items.push_back(item);
    }
    return core::Result<gc::EconomyCorruptionState>::success(std::move(result));
}
core::Result<gc::BlackMarketBuyer> prepare_black_market_buyer(const data::Catalog& catalog,
    std::string_view type, sim::EntityId id, gc::PlayerId owner, bool opposing, core::LoadNoteBuffer* notes) {
    const auto original = layers(catalog, type, data::Category::game_object);
    const data::XmlNode* ability = nullptr;
    for (const auto* layer : original) {
        if (const auto* abilities = child(layer->root, "Abilities")) ability = child(*abilities, "Black_Market_Ability");
        if (ability) break;
    }
    if (!ability) {
        campaign_load_note(notes, "EAWR-LOAD-1206", nullptr, type, "Black_Market_Ability", "inert buyer");
        gc::BlackMarketBuyer inert; inert.buyer = id; inert.owner = owner; inert.opposing_planet = opposing; inert.present = false;
        return core::Result<gc::BlackMarketBuyer>::success(std::move(inert));
    }
    auto price = number_or(child(*ability, "Price_Modifier"), gc::corruption_fixture::one, notes, type); // WGEP-106
    const auto* despawn_node = child(*ability, "Causes_Despawn");
    auto despawn = despawn_node ? flag(despawn_node) : core::Result<bool>::success(true);
    auto low = numbers(child(*ability, "Min_Respawn_Times"), 5, notes, type); auto high = numbers(child(*ability, "Max_Respawn_Times"), 5, notes, type);
    if (!price || price.value() < Fixed{} || price.value().raw() > (std::int64_t{1} << 48) || !despawn) {
        campaign_load_note(notes, "EAWR-LOAD-1206", ability, type, "Black_Market_Ability", "price 1; causes despawn");
        price = core::Result<Fixed>::success(gc::corruption_fixture::one);
    }
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
    if (!valid) campaign_load_note(notes, "EAWR-LOAD-1207", child(*ability, "Min_Respawn_Times") ? child(*ability, "Min_Respawn_Times") : ability, type, "Min_Respawn_Times/Max_Respawn_Times",
        "both arrays empty", "WGEP-107: invalid respawn arrays clear both", core::LoadCategory::retail_accepts);
    if (valid && std::any_of(high.value().begin(), high.value().end(), [](Fixed value) { return value.raw() > (std::int64_t{1} << 48); })) {
        campaign_load_note(notes, "EAWR-LOAD-1209", child(*ability, "Max_Respawn_Times"), type,
            "Min_Respawn_Times/Max_Respawn_Times", "both arrays empty", "unknown: project arithmetic support range");
        valid = false;
    }
    if (valid) { result.absence_min = std::move(low).value(); result.absence_max = std::move(high).value(); }
    return core::Result<gc::BlackMarketBuyer>::success(std::move(result));
}
} // namespace eawr::campaign
