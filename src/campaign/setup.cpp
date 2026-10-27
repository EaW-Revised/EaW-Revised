#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/control.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <functional>
#include <set>
#include <utility>
#include <tuple>

namespace eawr::campaign {
namespace gc = sim::galactic;
gc::ObserverPlayer prepare_observer_player(const data::galactic::Faction& faction, gc::ObserverPlayer live) {
    // WGUI-98: canonical definition identity after name lookup, not player/faction ordinal.
    const auto canonical = [&](std::string_view name) {
        return faction.name.size() == name.size() && std::equal(faction.name.begin(), faction.name.end(), name.begin(),
            [](char a, char b) {
                if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
                if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
                return a == b;
            });
    };
    live.canonical_empire = canonical("Empire");
    live.default_intelligence = gc::faction_default_intelligence(canonical("Rebel"), canonical("Underworld"));
    if (faction.minimum_visible_base_level) live.minimum_visible_base_level = *faction.minimum_visible_base_level;
    if (faction.benefits_from_corruption) live.benefits_from_corruption = *faction.benefits_from_corruption;
    return live;
}
gc::ObserverPlanet prepare_observer_planet(const data::galactic::Planet& planet, gc::ObserverPlanet live) {
    // WGUI-10/103: independent effective inherited base-stealth gates.
    if (planet.planet_starbase_stealth) live.space_stealth = *planet.planet_starbase_stealth;
    if (planet.planet_groundbase_stealth) live.ground_stealth = *planet.planet_groundbase_stealth;
    return live;
}
core::Result<gc::EconomicPlanet> prepare_economic_planet(const units::GalacticEconomicType& input,
    gc::PlanetId id, bool destroyed, bool intimidation) {
    if (!id || !input.mining_colony) {
        core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
        d.message = "economic planet requires a sourced mining-colony flag";
        return core::Result<gc::EconomicPlanet>::failure(std::move(d));
    }
    return core::Result<gc::EconomicPlanet>::success({id, *input.mining_colony, destroyed, intimidation});
}
core::Result<gc::EconomicType> prepare_economic_type(const units::GalacticEconomicType& input,
    gc::TypeId id, std::string_view faction, const units::GalacticTypeTable& types,
    std::span<const EconomicTypeBinding> bindings) {
    const auto fail = [&](std::string_view reason = {}) {
        core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
        d.message = "economic profile " + input.name + " requires authored values and resolved target types";
        if (!reason.empty()) d.message += ": " + std::string(reason);
        return core::Result<gc::EconomicType>::failure(std::move(d));
    };
    if (!id) return fail();
    const auto same = [](std::string_view a, std::string_view b) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
    };
    const auto contains = [&](const auto& names, std::string_view name) {
        return std::any_of(names.begin(), names.end(), [&](const auto& value) { return same(value, name); });
    };
    // WGEP-73: All is the authored full category mask. Empty masks have no
    // intersection, and either nonempty side can contain the full mask.
    const auto intersects = [&](const auto& left, const auto& right) {
        return !left.empty() && !right.empty() && (contains(left, "All") || contains(right, "All")
            || std::any_of(left.begin(), left.end(), [&](const auto& category) { return contains(right, category); }));
    };
    gc::EconomicType result; result.type = id;
    // WGEP-166: validation forces Galactic_Automatic and clamps >= 1 to 1;
    // negative percentages remain authored penalties. Only planetary data owns
    // this effect. Distinct named abilities share the planet's aggregate ledger row.
    if (same(input.object_class, "Planet")) for (const auto& ability : input.technology_prices) {
        if (!ability.initially_enabled || !ability.reduction) return fail();
        if (!*ability.initially_enabled) continue;
        const auto reduction = std::min(*ability.reduction, sim::math::Fixed::from_raw(sim::math::Fixed::scale));
        auto total = sim::math::add(result.technology_price_reduction.value_or(sim::math::Fixed{}), reduction);
        if (!total) return fail();
        result.technology_price_reduction = total.value();
    }
    const auto active = [&](const units::GalacticInitialStealth& ability) -> core::Result<bool> {
        if (!ability.initially_enabled) return core::Result<bool>::failure(fail().error());
        if (!*ability.initially_enabled) return core::Result<bool>::success(false);
        std::string style = ability.activation_style;
        std::transform(style.begin(), style.end(), style.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        if (style == "GALACTIC_AUTOMATIC" || style == "GLOBAL_AUTOMATIC") return core::Result<bool>::success(true);
        if (style == "COMBAT_AUTOMATIC" || style == "SPACE_AUTOMATIC" || style == "GROUND_AUTOMATIC"
            || style == "SKIRMISH_AUTOMATIC" || style == "GROUND_ACTIVATED" || style == "HERO_DETECTED"
            || style == "COMBAT_IMMINENT" || style == "SPECIAL_ATTACK" || style == "TAKE_DAMAGE"
            || style == "USER_INPUT" || style == "INVALID") return core::Result<bool>::success(false);
        return core::Result<bool>::failure(fail().error());
    };
    const auto mining_active = active(input.mining_activation), gambling_active = active(input.gambling_activation);
    if (!mining_active || !gambling_active) return fail();
    if (input.mining && mining_active.value()) {
        if (!input.mining_percentage || !input.mining_absolute || !input.mining_multiplier) return fail("invalid mining parameters");
        result.mining = gc::PlanetIncomeAbility{*input.mining_percentage, *input.mining_absolute, *input.mining_multiplier};
    }
    if (input.gambling && gambling_active.value()) {
        if (!input.gambling_chance || !input.gambling_minimum || !input.gambling_maximum || !input.gambling_interval)
            return fail("invalid gambling parameters");
        std::uint32_t frames{};
        if (input.gambling_interval->raw() > 0) {
            const auto raw = input.gambling_interval->raw();
            if (raw > std::int64_t{32767} * sim::math::Fixed::scale / gc::galactic_frames_per_second) return fail();
            frames = static_cast<std::uint32_t>((raw * gc::galactic_frames_per_second + sim::math::Fixed::scale / 2) / sim::math::Fixed::scale);
        }
        result.gambling = gc::GamblingIncomeAbility{*input.gambling_chance, *input.gambling_minimum, *input.gambling_maximum, frames};
    }
    // WGEP-163: data-driven; an authored-true halt without a duration registers a zero-length
    // halt (unqualified default, never an invented value). Negative authored seconds refuse.
    if (input.credit_halt && *input.credit_halt) {
        const auto seconds = input.credit_halt_seconds.value_or(sim::math::Fixed{});
        if (seconds.raw() < 0) return fail();
        result.credit_halt = gc::CreditHaltAbility{seconds};
    }
    for (const auto& ability : input.reductions) {
        // WGEP-93: fresh automatic reduction abilities apply during object
        // initialization; disabled or manual/tactical profiles have no effect.
        if (!ability.initially_enabled) return fail();
        if (!*ability.initially_enabled) continue;
        std::string activation = ability.activation_style;
        std::transform(activation.begin(), activation.end(), activation.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        if (activation != "GALACTIC_AUTOMATIC" && activation != "GLOBAL_AUTOMATIC") {
            if (activation == "COMBAT_AUTOMATIC" || activation == "SPACE_AUTOMATIC" || activation == "GROUND_AUTOMATIC"
                || activation == "SKIRMISH_AUTOMATIC" || activation == "GROUND_ACTIVATED" || activation == "HERO_DETECTED"
                || activation == "COMBAT_IMMINENT" || activation == "SPECIAL_ATTACK" || activation == "TAKE_DAMAGE"
                || activation == "USER_INPUT" || activation == "INVALID") continue;
            return fail();
        }
        if (!ability.category || *ability.category < 0 || *ability.category > std::numeric_limits<std::uint32_t>::max())
            return fail("invalid Stacking_Category");
        if (!ability.reduction) return fail(ability.price ? "invalid Price_Reduction_Percentage" : "invalid Time_Reduction_Percentage");
        if (!ability.galaxy_wide) return fail("invalid Galaxy_Wide");
        std::vector<const units::GalacticType*> affected;
        for (const auto& type : types.types) {
            const bool included = intersects(type.category_mask, ability.included_categories);
            const bool excluded = intersects(type.category_mask, ability.excluded_categories);
            if (included && !excluded && !contains(ability.excluded_types, type.name)
                && (same(input.object_class, "Planet") || contains(type.affiliation, faction))) affected.push_back(&type);
        }
        // Explicit entries bypass category/exclusion/affiliation checks.
        for (const auto& name : ability.included_types) {
            // CGS-21: unresolved explicit references contribute no target; the
            // remaining entries and abilities still apply.
            if (const auto* type = types.find(name)) affected.push_back(type);
        }
        for (const auto* type : affected) {
            if (ability.price && (!type->build_cost_credits || type->build_cost_credits->raw() <= 0)) continue;
            const auto at = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) { return same(binding.name, type->name); });
            if (at == bindings.end() || !at->type) return fail("missing identity for target " + type->name);
            result.reductions.push_back({at->type, static_cast<std::uint32_t>(*ability.category), *ability.reduction, ability.price, *ability.galaxy_wide});
        }
    }
    const auto key = [](const auto& row) { return std::tuple{row.type, row.price, row.category, row.galaxy_wide}; };
    std::sort(result.reductions.begin(), result.reductions.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    std::vector<gc::EconomicReduction> reductions;
    for (const auto& row : result.reductions) {
        if (!reductions.empty() && key(reductions.back()) == key(row))
            reductions.back().reduction = std::max(reductions.back().reduction, row.reduction);
        else reductions.push_back(row);
    }
    result.reductions = std::move(reductions);
    return core::Result<gc::EconomicType>::success(std::move(result));
}
core::Result<gc::CensusType> prepare_census_type(const units::GalacticType& type, gc::TypeId id,
    std::span<const FleetTypeBinding> bindings) {
    // WGEP-69: the table resolves variant inheritance; GC ignores tactical overrides.
    const auto population = sim::math::Fixed::from_integer(type.population_value.value_or(sim::math::Fixed{}).trunc_to_integer()).value();
    const auto additional_capacity = type.additional_population_capacity.value_or(0);
    if (id == 0 || population.raw() < 0
        || population.raw() > (std::int64_t{1} << 40)
        || additional_capacity < std::numeric_limits<std::int32_t>::min()
        || additional_capacity > std::numeric_limits<std::int32_t>::max()) {
        core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.severity = core::Severity::error;
        d.logical_path = type.file;
        d.message = "census " + type.name + ": rejected Population_Value=" + std::to_string(population.trunc_to_integer())
            + ", Additional_Population_Capacity=" + std::to_string(additional_capacity) + ", type id=" + std::to_string(id);
        return core::Result<gc::CensusType>::failure(std::move(d));
    }
    gc::CensusType result{id, population, additional_capacity};
    const auto behaves = [&](std::string_view name) {
        return std::find(type.behavior.begin(), type.behavior.end(), name) != type.behavior.end();
    };
    // WGP-191: resolved galactic type metric, without tactical team-size scaling.
    result.history = gc::HistoryType{type.ai_combat_power.value_or(sim::math::Fixed{}),
        behaves("FLEET") || behaves("SQUADRON"), behaves("TRANSPORT"), behaves("SQUADRON") || behaves("DUMMY_SPACE_FIGHTER_SQUADRON")};
    const auto normalized = [](std::string text) {
        for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    };
    // GHUD-S93: retain permission on every bound type, including transports and cargo.
    const auto sale = normalized(type.sellable);
    if (!sale.empty() && sale != "SELLABLE_DEFAULT" && sale != "SELLABLE_ALWAYS" && sale != "SELLABLE_NEVER") {
        core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
        d.message = "unknown authored sale policy";
        return core::Result<gc::CensusType>::failure(std::move(d));
    }
    result.sale_policy = sale == "SELLABLE_ALWAYS" ? 1 : sale == "SELLABLE_NEVER" ? 2 : 0;
    result.named_hero = type.named_hero.value_or(false);
    result.superweapon = std::any_of(type.category_mask.begin(), type.category_mask.end(),
        [&](const auto& category) { return normalized(category) == "SUPER"; });
    result.authored_price = type.build_cost_credits.value_or(sim::math::Fixed{});
    std::map<std::string, gc::TypeId> identities;
    for (const auto& binding : bindings) {
        if (!binding.id || !identities.emplace(normalized(binding.original_type), binding.id).second) {
            core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.severity = core::Severity::error;
            d.message = "census requires unique nonzero original-type bindings";
            return core::Result<gc::CensusType>::failure(std::move(d));
        }
    }
    auto names = type.company_units;
    if (!type.company_transport.empty()) names.push_back(type.company_transport);
    names.insert(names.end(), type.squadron_units.begin(), type.squadron_units.end());
    for (const auto& name : names) {
        const auto at = identities.find(normalized(name));
        if (at == identities.end()) {
            core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup); d.severity = core::Severity::error;
            d.message = "census composite member requires an original-type binding: " + name;
            return core::Result<gc::CensusType>::failure(std::move(d));
        }
        result.members.push_back(at->second);
    }
    std::sort(result.members.begin(), result.members.end());
    result.members.erase(std::unique(result.members.begin(), result.members.end()), result.members.end());
    result.company = std::find(type.behavior.begin(), type.behavior.end(), "DUMMY_GROUND_COMPANY") != type.behavior.end();
    if (result.company) {
        const auto at = identities.find(normalized(type.company_transport));
        if (at == identities.end() || type.company_units.empty()) {
            core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
            d.message = "census company requires its exact authored transport and members";
            return core::Result<gc::CensusType>::failure(std::move(d));
        }
        result.transport = at->second;
    }
    return core::Result<gc::CensusType>::success(std::move(result));
}
core::Result<gc::CensusType> prepare_census_planet(const data::galactic::Planet& planet, gc::TypeId id,
    std::optional<sim::math::Fixed> combat_power) {
    units::GalacticType original;
    auto population = sim::math::Fixed::from_integer(planet.population_value.value_or(0));
    if (!population) return core::Result<gc::CensusType>::failure(population.error());
    original.population_value = population.value();
    original.additional_population_capacity = planet.additional_population_capacity;
    auto result = prepare_census_type(original, id);
    if (result) {
        if (combat_power) result.value().history->power = *combat_power;
        else result.value().history.reset();
    }
    return result;
}
gc::CensusPlayer prepare_census_player(const data::galactic::Faction& faction, gc::PlayerId id,
    std::vector<gc::PlanetId> unusable_planets) {
    return {id, faction.base_population_capacity.value_or(0), faction.ai_uses_galactic_pop_cap.value_or(false), std::move(unusable_planets)};
}
core::Result<gc::ProductionOffer> prepare_production_offer(const units::GalacticType& type,
    gc::ProductionOffer admission, const ProductionPreparation& inputs) {
    using sim::math::Fixed;
    const auto fail = [&](const char* why) {
        core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
        d.severity = core::Severity::error; d.message = "production offer requires sourced price/time and complete modifier inputs: " + type.name + " (" + why + ")";
        return core::Result<gc::ProductionOffer>::failure(std::move(d));
    };
    if (!inputs.price_multiplier
        || !inputs.corruption_price_multiplier || !inputs.tech_purchase_multiplier || !inputs.time_multiplier
        || !inputs.base_tech_multiplier || !inputs.ai_time_multiplier || !inputs.corruption_time_multiplier
        || !inputs.factory_count || *inputs.factory_count < 1) return fail("modifier inputs");
    // WGEP-42: authoring supplies an unsigned target, not an increment/flag.
    if (type.build_advances_tech_level) {
        if (*type.build_advances_tech_level < 0 || *type.build_advances_tech_level > std::numeric_limits<std::int32_t>::max()) return fail("tech target");
        if (*type.build_advances_tech_level > 0) {
            admission.kind = gc::ProductionKind::technology;
            admission.target_tech = *type.build_advances_tech_level;
        }
    }
    // WGEP-92: defaults apply at consumption, never to hashed authored facts.
    auto price = type.build_cost_credits.value_or(Fixed{});
    admission.authored_price = price; // GHUD-S57: the card's price colour compares against this, not the scaled price
    // GHUD-S84: explicit Sellable_Always permits default-excluded types.
    auto sale = type.sellable;
    std::transform(sale.begin(), sale.end(), sale.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
    if (!sale.empty() && sale != "sellable_default" && sale != "sellable_always" && sale != "sellable_never") return fail("sale policy");
    const bool superweapon = std::any_of(type.category_mask.begin(), type.category_mask.end(), [](std::string category) {
        std::transform(category.begin(), category.end(), category.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
        return category == "super";
    });
    admission.sellable = sale == "sellable_always" || (sale != "sellable_never"
        && !type.named_hero.value_or(false) && !superweapon);
    for (auto factor : {*inputs.price_multiplier, *inputs.corruption_price_multiplier,
            admission.kind == gc::ProductionKind::technology ? *inputs.tech_purchase_multiplier : Fixed::from_raw(Fixed::scale)}) {
        price = gc::production_product(price, factor);
    }
    // WGEP-30: positive sub-credit prices become one; half ties go away from zero.
    admission.price = gc::production_price(price);
    auto seconds = type.build_time_seconds.value_or(Fixed{});
    if (inputs.tutorial) seconds = Fixed::from_integer(5).value(); // WGEP-32: hardcoded tutorial duration
    else {
        for (auto factor : {*inputs.time_multiplier, *inputs.base_tech_multiplier, *inputs.ai_time_multiplier, *inputs.corruption_time_multiplier})
            seconds = gc::production_product(seconds, factor);
        const auto factories = Fixed::from_integer(*inputs.factory_count);
        if (!factories || !sim::math::try_divide(seconds, factories.value(), seconds)) return fail("factory division");
    }
    admission.seconds = seconds;
    if (type.tech_level) admission.tech = *type.tech_level;
    // WGEP-69: GC population is the original type's integer, never a tactical override.
    admission.population = sim::math::Fixed::from_integer(type.population_value.value_or(sim::math::Fixed{}).trunc_to_integer()).value();
    if (type.required_ground_base_level) admission.required_ground_level = *type.required_ground_base_level;
    if (type.required_star_base_level) admission.required_space_level = *type.required_star_base_level;
    if (type.base_level) admission.base_level = *type.base_level;
    if (type.build_limit_current_per_player) admission.current_limit = *type.build_limit_current_per_player;
    if (type.build_limit_lifetime_per_player) admission.lifetime_limit = *type.build_limit_lifetime_per_player;
    if (type.build_initially_locked) admission.initially_locked = *type.build_initially_locked;
    if (type.build_requires_initial_placement) admission.requires_initial_placement = *type.build_requires_initial_placement;
    return core::Result<gc::ProductionOffer>::success(std::move(admission));
}
namespace {
std::string key(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return result;
}
template <typename T> core::Result<T> invalid(std::string message, std::string_view file = {}) {
    core::Diagnostic d;
    d.code = std::string(gc::diagnostic_codes::invalid_setup);
    d.severity = core::Severity::error;
    d.logical_path = std::string(file);
    d.message = "campaign setup: " + std::move(message);
    return core::Result<T>::failure(std::move(d));
}
class Identity {
public:
    void integer(std::uint64_t n) {
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(n >> shift));
    }
    void text(std::string_view s) { integer(s.size()); bytes.insert(bytes.end(), s.begin(), s.end()); }
    std::vector<std::uint8_t> bytes;
};
std::int64_t authored_number(std::span<const data::galactic::FactionNumber> rows,
    std::string_view faction, std::int64_t absent) {
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return key(row.faction) == key(faction); });
    return found == rows.end() ? absent : found->value;
}
bool source_integer(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
}
core::Result<sim::EntityId> placement_frontier(const StartingForcePlacement& placed, const sim::EntityId first_free,
    const StartingForceRequest& request, gc::GalacticSetup& setup, const CampaignSetupPlan& plan) {
    std::set<sim::EntityId> ids;
    const auto resolve_type = [&](gc::TypeId type) -> core::Result<void> {
        auto& traits = setup.fleet_control->types;
        const auto position = std::lower_bound(traits.begin(), traits.end(), type,
            [](const auto& row, gc::TypeId id) { return row.type < id; });
        if (position != traits.end() && position->type == type) return core::Result<void>::success();
        auto resolved = plan.resolve_fleet_type(type);
        if (!resolved) return core::Result<void>::failure(resolved.error());
        traits.insert(position, resolved.value());
        return core::Result<void>::success();
    };
    const auto claim = [&](sim::EntityId id) {
        return id >= first_free && id < placed.next_id && ids.insert(id).second;
    };
    for (const auto& fleet : placed.fleets) {
        if (!claim(fleet.id) || fleet.owner != request.owner || fleet.orbit != request.planet || fleet.members.empty())
            return invalid<sim::EntityId>("placement provider returned an invalid fleet for " + request.original_type);
        for (const auto& member : fleet.members) {
            if (!claim(member.id)) return invalid<sim::EntityId>("placement provider returned an invalid member for " + request.original_type);
            if (auto resolved = resolve_type(member.type); !resolved) return core::Result<sim::EntityId>::failure(resolved.error());
        }
    }
    for (const auto& unit : placed.detached_units) {
        if (!claim(unit.member.id) || unit.owner != request.owner || unit.planet != request.planet)
            return invalid<sim::EntityId>("placement provider returned an invalid detached unit for " + request.original_type);
        if (auto resolved = resolve_type(unit.member.type); !resolved) return core::Result<sim::EntityId>::failure(resolved.error());
    }
    for (const auto& unit : placed.landed) {
        if (!claim(unit.id) || unit.owner != request.owner)
            return invalid<sim::EntityId>("placement provider returned an invalid landed force");
        if (auto resolved = resolve_type(unit.type); !resolved) return core::Result<sim::EntityId>::failure(resolved.error());
    }
    for (const auto& object : placed.structures) {
        if (!claim(object.id) || object.owner != request.owner || object.kind != gc::ProductionKind::structure)
            return invalid<sim::EntityId>("placement provider returned an invalid structure");
        if (!plan.original_type(object.type)) return invalid<sim::EntityId>("placement provider returned an unbound structure type");
    }
    for (const auto& contents : placed.contents) for (const auto& child : contents.children) {
        if (!claim(child.id)) return invalid<sim::EntityId>("placement provider duplicated a nested member");
        if (auto resolved = resolve_type(child.type); !resolved) return core::Result<sim::EntityId>::failure(resolved.error());
    }
    std::set<sim::EntityId> original_ids;
    for (const auto& original : placed.originals) {
        if (!ids.contains(original.id) || !original_ids.insert(original.id).second
            || original.owner != request.owner || original.planet != request.planet || original.dead || original.delete_pending)
            return invalid<sim::EntityId>("placement provider returned invalid original-type metadata");
        if (!plan.original_type(original.original_type))
            return invalid<sim::EntityId>("placement provider returned unbound original metadata");
    }
    if (placed.next_id < first_free || placed.next_id == std::numeric_limits<sim::EntityId>::max()
        || placed.next_id - first_free != ids.size() || placed.created != !ids.empty())
        return invalid<sim::EntityId>("placement provider must account for every allocated identity and live creation");
    return core::Result<sim::EntityId>::success(placed.next_id);
}
} // namespace

const units::GalacticType* CampaignSetupPlan::original_type(gc::TypeId type) const noexcept {
    const auto binding = std::lower_bound(type_bindings_.begin(), type_bindings_.end(), type,
        [](const auto& row, gc::TypeId id) { return row.id < id; });
    return binding == type_bindings_.end() || binding->id != type ? nullptr : bound_types_.find(binding->original_type);
}
core::Result<gc::FleetTypeTraits> CampaignSetupPlan::resolve_fleet_type(gc::TypeId type) const {
    const auto binding = std::lower_bound(type_bindings_.begin(), type_bindings_.end(), type,
        [](const auto& row, gc::TypeId id) { return row.id < id; });
    if (binding == type_bindings_.end() || binding->id != type)
        return invalid<gc::FleetTypeTraits>("placement provider returned an unbound type identity " + std::to_string(type));
    auto traits = build_fleet_type_traits(bound_types_, std::span(&*binding, 1));
    if (!traits) return core::Result<gc::FleetTypeTraits>::failure(traits.error());
    return core::Result<gc::FleetTypeTraits>::success(traits.value().front());
}

namespace {

core::Result<void> plan_fiscal_setup(const data::galactic::GalacticData& data, const CampaignSetupOptions& options, const auto* campaign,
    auto& model, auto& fiscal_setup) {
    const auto* cap = data.constant("Credit_Cap_Per_Planet");
    if (!cap || !cap->number) return invalid<void>("missing Credit_Cap_Per_Planet", campaign->file);
    fiscal_setup = gc::FiscalSetup{*cap->number, *options.fiscal_players, {}};
    // WGEP-05: current, intact planet basic income. Destroyed/blocked state,
    // alignment, politics, trade and structure modifiers are separate seams.
    for (const auto& planet : model.value().planets) {
        const auto named = std::find_if(model.value().names.begin(), model.value().names.end(),
            [&](const auto& row) { return row.id == planet.id; });
        const auto* source = data.planet(named->name);
        if (!source || !source->planet_credit_value)
            return invalid<void>("missing Planet_Credit_Value for " + named->name, campaign->file);
        fiscal_setup->planets.push_back({planet.id, *source->planet_credit_value});
    }
    // Old/synthetic data without the new constant group keeps the legacy slice.
    const auto* maximum_bonus = data.constant("MaxCreditIncomeAlignmentBonus");
    const auto* maximum_penalty = data.constant("MaxCreditIncomeAlignmentPenalty");
    const auto* curve = data.constant("Political_Income_Curve");
    if (maximum_bonus || maximum_penalty || curve) {
        if (!maximum_bonus || !maximum_bonus->number || !maximum_penalty || !maximum_penalty->number || !curve)
            return invalid<void>("incomplete WGEP-07/63 income constants", campaign->file);
        auto points = gc::parse_income_curve(curve->text);
        if (!points) return core::Result<void>::failure(points.error());
        auto& rules = fiscal_setup->contributions.emplace();
        rules.maximum_alignment_bonus = *maximum_bonus->number;
        rules.maximum_alignment_penalty = *maximum_penalty->number;
        rules.political_curve = std::move(points.value());
        const auto alignment = [](std::string_view name) -> std::optional<gc::IncomeAlignment> {
            if (key(name) == "NEUTRAL") return gc::IncomeAlignment::neutral;
            if (key(name) == "LIGHT") return gc::IncomeAlignment::light;
            if (key(name) == "DARK") return gc::IncomeAlignment::dark;
            return std::nullopt;
        };
        for (const auto& player : options.players) {
            const auto value = alignment(data.faction(player.faction)->force_alignment);
            if (!value) return invalid<void>("invalid or absent Force_Alignment", campaign->file);
            rules.factions.push_back({player.player.player_id, *value});
        }
        for (const auto& planet : model.value().planets) {
            const auto named = std::find_if(model.value().names.begin(), model.value().names.end(),
                [&](const auto& row) { return row.id == planet.id; });
            const auto* source = data.planet(named->name);
            const auto value = alignment(source->force_alignment);
            if (!value || !source->force_strength) return invalid<void>("invalid or absent planet alignment/strength", source->file);
            rules.planets.push_back({planet.id, *value, *source->force_strength});
        }
        for (const auto& route : model.value().routes) {
            const auto named = std::find_if(model.value().names.begin(), model.value().names.end(),
                [&](const auto& row) { return row.id == route.id; });
            const auto* source = data.trade_route(named->name);
            if (!source || !source->credit_gain_factor) return invalid<void>("missing Credit_Gain_Factor", campaign->file);
            rules.routes.push_back({route.id, *source->credit_gain_factor});
        }
    }
    return core::Result<void>::success();
}

core::Result<void> plan_location_requests(const auto* campaign, gc::PlayerId neutral, auto& planets,
    auto& galaxy_setup, auto& locations) {
    for (const auto& name : campaign->locations) {
        const auto id = planets.find(key(name));
        CampaignLocation location{name, neutral, std::nullopt};
        if (id != planets.end()) location.planet = *std::find_if(galaxy_setup.planets.begin(), galaxy_setup.planets.end(),
            [&](const auto& row) { return row.id == id->second; });
        locations.push_back(std::move(location));
    }
    if (campaign->locations.empty() || std::any_of(campaign->planets.begin(), campaign->planets.end(), [&](const auto& name) {
            return std::none_of(campaign->locations.begin(), campaign->locations.end(), [&](const auto& location) { return key(name) == key(location); });
        })) return invalid<void>("planet list is not a subset of authored Locations", campaign->file);
    return core::Result<void>::success();
}

// CGS-05: a record naming a type that resolves to no object is reported and skipped; the campaign still starts.
void unknown_record_type(const auto* campaign, auto& diagnostics, const std::string_view tag, const std::string& location,
    const std::string& type) {
    core::Diagnostic diagnostic;
    diagnostic.code = "campaign_unknown_" + std::string(tag) + "_type";
    diagnostic.severity = core::Severity::warning;
    diagnostic.logical_path = campaign->file;
    diagnostic.message = campaign->name + ": " + std::string(tag) + " at " + location + " names unknown type " + type
        + "; the record is skipped (CGS-05)";
    diagnostics.push_back(std::move(diagnostic));
}

core::Result<void> plan_starting_placements(const auto* campaign, const CampaignSetupOptions& options, const units::GalacticTypeTable& types,
    auto& locations, auto& original_types, auto& type_ids, auto& players, auto& referenced_types,
    auto& placement_plan, auto& force_plan, auto& diagnostics) {
    // WGS-70: per matching planet, special placements precede stored force traversal. Missing
    // or inactive owners are skipped before consulting the placement provider.
    for (const auto& location : locations) {
        if (!location.planet) continue;
        std::set<gc::TypeId> marked;
        for (const auto& record : campaign->special_case_production) {
            if (key(record.location) != key(location.original_type)) continue;
            const auto original = original_types.find(key(record.type));
            if (original == original_types.end()) {
                unknown_record_type(campaign, diagnostics, "Special_Case_Production", record.location, record.type);
                continue;
            }
            const auto* type = original->second;
            const auto id = type_ids.find(key(record.type));
            if (id == type_ids.end()) return invalid<void>("missing original identity for " + record.type, campaign->file);
            referenced_types.insert(id->second);
            // WGS-72: use the inherited/authored value before the shared false default.
            if (!type->build_requires_initial_placement.value_or(false)) continue;
            if (marked.insert(id->second).second) placement_plan.push_back({location.planet->id, id->second});
        }
        for (const auto& record : campaign->starting_forces) {
            if (key(record.location) != key(location.original_type)) continue;
            const auto owner = players.find(key(record.faction));
            if (owner == players.end() || !owner->second->active) continue;
            const auto id = type_ids.find(key(record.type));
            if (id == type_ids.end()) {
                // Debug build: the placement pass skips a record whose unit type did not resolve at load.
                unknown_record_type(campaign, diagnostics, "Starting_Forces", record.location, record.type);
                continue;
            }
            // CGS-09: creation dispatches on the dummy behaviours; any other type is "unknown", returns null and the
            // pass goes on with the next record.
            const auto& behavior = original_types.find(key(record.type))->second->behavior;
            const bool dispatched = std::any_of(behavior.begin(), behavior.end(), [](const auto& role) {
                return role == "DUMMY_STAR_BASE" || role == "DUMMY_GROUND_BASE" || role == "DUMMY_GROUND_STRUCTURE"
                    || role == "DUMMY_ORBITAL_STRUCTURE" || role == "DUMMY_GROUND_COMPANY"
                    || role == "DUMMY_SPACE_FIGHTER_SQUADRON" || role == "DUMMY_STARSHIP";
            });
            // A type with no behaviour list at all is a placeholder, not an authored object; only a type that names
            // behaviours and none of the creation ones is skipped.
            if (!behavior.empty() && !dispatched) {
                unknown_record_type(campaign, diagnostics, "Starting_Forces", record.location, record.type + " (no creation behaviour, CGS-09)");
                continue;
            }
            if (!campaign_type_creatable(types, record.type)) {
                // CGS-08: the placement pass would create a composite with a null transport or member.
                unknown_record_type(campaign, diagnostics, "Starting_Forces", record.location, record.type + " (a composite member does not resolve)");
                continue;
            }
            referenced_types.insert(id->second);
            const auto player = owner->second->player.player_id;
            const bool human = std::any_of(options.seats.begin(), options.seats.end(), [=](const auto& seat) {
                return seat.player == player && seat.kind == gc::SeatKind::human;
            });
            force_plan.push_back({location.planet->id, player, id->second, record.type, human,
                sim::math::Fixed::from_raw(sim::math::Fixed::scale)});
        }
    }
    return core::Result<void>::success();
}

void plan_home_locations(const auto* campaign, const CampaignSetupOptions& options, auto& planets, auto& homes) {
    for (const auto& binding : options.players) {
        CampaignHome home{binding.player.player_id, {}, std::nullopt};
        const auto record = std::find_if(campaign->home_locations.begin(), campaign->home_locations.end(),
            [&](const auto& row) { return key(row.faction) == key(binding.faction); });
        if (record != campaign->home_locations.end()) {
            home.original_type = record->name;
            if (const auto planet = planets.find(key(record->name)); planet != planets.end()) home.planet = planet->second;
        }
        homes.push_back(std::move(home));
    }
}

core::Result<void> plan_player_resources(const data::galactic::GalacticData& data, const auto* campaign, const CampaignSetupOptions& options,
    auto& homes, auto& type_ids, auto& referenced_types, auto& credits_plan,
    auto& tech_plan, auto& hero_plan) {
    for (std::size_t index = 0; index < options.players.size(); ++index) {
        const auto& binding = options.players[index];
        const auto* faction = data.faction(binding.faction);
        const auto amount = options.custom ? options.custom->credits : authored_number(campaign->starting_credits, binding.faction, 0);
        // WGS-74: the runtime custom-credit getter supplies a signed 32-bit integer.
        if (options.custom && !source_integer(amount))
            return invalid<void>("custom credits do not fit the source integer", campaign->file);
        auto credits = sim::math::Fixed::from_integer(amount);
        if (!credits) return invalid<void>("starting credits are outside the represented numeric range", campaign->file);
        credits_plan.push_back({binding.player.player_id, credits.value()});
        auto current = options.custom ? options.custom->starting_tech : authored_number(campaign->starting_tech_levels, binding.faction, 0);
        // WGS-73: max-tech getter's missing-record default is the code value 5.
        auto maximum = options.custom ? options.custom->maximum_tech : authored_number(campaign->max_tech_levels, binding.faction, 5);
        if (!source_integer(current) || !source_integer(maximum)) return invalid<void>("tech input does not fit the source integer", campaign->file);
        // WGS-76: the faction constructor initializes this adjustment to zero.
        if (options.custom && faction->displayed_tech_level_adjustment.value_or(0) != 0) {
            --current; --maximum; // WGS-13: subtract exactly one, regardless of adjustment magnitude/sign.
            if (!source_integer(current) || !source_integer(maximum)) return invalid<void>("custom tech adjustment exceeds the source integer", campaign->file);
        }
        tech_plan.push_back({binding.player.player_id, current, maximum});
        const auto& home = homes[index];
        if (options.mode == CampaignSetupMode::multiplayer && (!options.custom || options.custom->allow_heroes) && home.planet) {
            for (const auto& original : faction->multiplayer_campaign_heroes) {
                const auto type = type_ids.find(key(original));
                if (type == type_ids.end()) return invalid<void>("missing original identity for multiplayer hero " + original, campaign->file);
                referenced_types.insert(type->second);
                hero_plan.push_back({binding.player.player_id, *home.planet, type->second, original});
            }
        }
    }
    return core::Result<void>::success();
}

void plan_fleet_traits(const CampaignSetupOptions& options, auto& referenced_types, auto& bound_types,
    auto& diagnostics, auto& galaxy_setup) {
    for (const auto& binding : options.types) {
        if (!referenced_types.contains(binding.id)) continue;
        auto traits = build_fleet_type_traits(bound_types, std::span(&binding, 1));
        if (!traits) {
            // Planning records placement intent without deciding fleet movement. An unresolved
            // trait is diagnosed and absent from the live table; placement_frontier refuses a
            // provider that tries to create a persistent member with that unresolved identity.
            auto diagnostic = traits.error();
            diagnostic.severity = core::Severity::warning;
            diagnostics.push_back(std::move(diagnostic));
        } else {
            galaxy_setup.fleet_control->types.push_back(traits.value().front());
        }
    }
}

core::Result<void> plan_victory_setup(const data::galactic::GalacticData& data, const auto* campaign, const CampaignSetupOptions& options,
    auto& original_types, auto& type_ids, auto& planets, auto& model,
    auto& bound_types, auto& galaxy_setup) {
    gc::VictoryAuthoring authoring;
    const auto parse_conditions = [&](const auto& names, auto& out) -> core::Result<void> {
        for (const auto& name : names) {
            const auto n = key(name);
            if (n == "GALACTIC_ALL_PLANETS_CONTROLLED") out.push_back(gc::VictoryKind::all_planets);
            else if (n == "GALACTIC_CONTROL_NAMED_PLANETS") out.push_back(gc::VictoryKind::named_planets);
            else if (n == "GALACTIC_CYCLES_ELAPSED") out.push_back(gc::VictoryKind::cycles);
            else if (n == "GALACTIC_SUPER_WEAPON_DESTRUCTION") out.push_back(gc::VictoryKind::destroy_superweapon);
            else if (n == "GALACTIC_OPPONENT_CONTROLS_NO_PLANETS") out.push_back(gc::VictoryKind::no_enemy_planets);
            else if (n == "GALACTIC_CREDITS_ACCRUED") out.push_back(gc::VictoryKind::credits);
            else if (n == "GALACTIC_PERCENTAGE_CONTROL") out.push_back(gc::VictoryKind::control_fraction);
            else if (n == "GALACTIC_CONQUER_ENEMY_HOMEWORLD_AND_LEADER") out.push_back(gc::VictoryKind::homeworld_and_leader);
            else if (n == "GALACTIC_SUPER_WEAPON_DESTROYS_LAST_ENEMY_PLANET") out.push_back(gc::VictoryKind::superweapon_last_planet);
            else if (n == "GALACTIC_SUPER_WEAPON_KILLS_ENEMY_LEADER") out.push_back(gc::VictoryKind::superweapon_kill_leader);
            else if (n == "GALACTIC_KILL_ENEMY_LEADER") out.push_back(gc::VictoryKind::kill_leader);
            else return invalid<void>("unsupported authored victory condition " + name, campaign->file);
        }
        return core::Result<void>::success();
    };
    if (auto parsed = parse_conditions(campaign->human_victory_conditions, authoring.human); !parsed)
        return core::Result<void>::failure(parsed.error());
    if (auto parsed = parse_conditions(campaign->ai_victory_conditions, authoring.ai); !parsed)
        return core::Result<void>::failure(parsed.error());
    const auto one = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
    authoring.human_parameters = {campaign->human_victory_credit_target.value_or(0), campaign->human_victory_cycle_limit.value_or(0),
        campaign->human_victory_galactic_control.value_or(one), campaign->human_victory_faction_names,
        campaign->human_victory_leader_names, campaign->human_victory_planet_names};
    authoring.ai_parameters = {campaign->ai_victory_credit_target.value_or(0), campaign->ai_victory_cycle_limit.value_or(0),
        campaign->ai_victory_galactic_control.value_or(one), campaign->ai_victory_faction_names,
        campaign->ai_victory_leader_names, campaign->ai_victory_planet_names};
    gc::VictoryContext context;
    context.multiplayer = options.mode == CampaignSetupMode::multiplayer;
    gc::PlayerId starting{};
    for (const auto& binding : options.players) {
        const auto* faction = data.faction(binding.faction);
        gc::VictoryPlayer p; p.player = binding.player.player_id; p.faction = faction->name;
        p.neutral = faction->is_neutral.value_or(false); p.playable = faction->is_playable.value_or(false);
        p.human = std::any_of(options.seats.begin(), options.seats.end(), [&](const auto& seat) { return seat.player == p.player && seat.kind == gc::SeatKind::human; });
        p.ai = std::any_of(options.seats.begin(), options.seats.end(), [&](const auto& seat) { return seat.player == p.player && seat.kind == gc::SeatKind::ai; });
        if (key(binding.faction) == key(campaign->starting_active_player)) starting = p.player;
        for (const auto& other : options.players) if (key(other.faction) == key(faction->primary_enemy)) p.primary_enemy = other.player.player_id;
        p.planet_leader_unit = faction->faction_leader;
        if (const auto company = original_types.find(key(faction->faction_leader_company)); company != original_types.end()
            && !company->second->company_units.empty()) p.leader_unit = company->second->company_units.front();
        if (const auto t = type_ids.find(key(faction->faction_leader_company)); t != type_ids.end()) p.leader_company = t->second;
        if (const auto t = type_ids.find(key(p.leader_unit)); t != type_ids.end()) p.leader_unit_type = t->second;
        if (const auto t = type_ids.find(key(p.planet_leader_unit)); t != type_ids.end()) p.planet_leader_type = t->second;
        if (const auto home = planets.find(key(faction->home_planet)); home != planets.end()) p.home = home->second;
        context.players.push_back(std::move(p));
    }
    for (const auto& named : model.value().names) {
        const auto p = std::find_if(galaxy_setup.planets.begin(), galaxy_setup.planets.end(), [&](const auto& row) { return row.id == named.id; });
        if (p == galaxy_setup.planets.end()) continue;
        context.types.push_back({named.name, named.id});
        context.planets.push_back({named.id, named.id, p->owner, p->owner, p->owner, false});
    }
    // WGS-25: a valid original planet type with no active instances remains resolvable.
    gc::TypeId next_victory_type = model.value().next_id;
    for (const auto& original : data.planets)
        if (std::none_of(context.types.begin(), context.types.end(), [&](const auto& t) { return key(t.name) == key(original.name); }))
            context.types.push_back({original.name, next_victory_type++});
    // Political relations and leader lifecycle remain owning-provider inputs, gathered live.
    for (const auto& p : context.players) context.relations.push_back({p.player, p.player, true});
    for (std::size_t i = 0; i < bound_types.types.size(); ++i)
        if (bound_types.types[i].superweapon_killer.value_or(false)) context.superweapon_killers.push_back(options.types[i].id);
    galaxy_setup.victory = gc::install_victory(authoring, context, starting);
    return core::Result<void>::success();
}

void seal_campaign_identity(const auto* campaign, const CampaignSetupOptions& options, auto& galaxy_setup) {
    // Coordinator-reserved identity domain. Explicit bindings and owner activity join the
    // selected definition/content, so two menus selecting different roots cannot alias.
    Identity identity;
    identity.text("eawr-campaign-setup-v1");
    identity.bytes.insert(identity.bytes.end(), galaxy_setup.content_identity.begin(), galaxy_setup.content_identity.end());
    identity.text(key(campaign->name));
    identity.integer(options.players.size());
    for (const auto& player : options.players) {
        identity.integer(player.player.player_id); identity.integer(player.player.faction_id);
        identity.integer(player.player.flags); identity.text(key(player.faction)); identity.integer(player.active ? 1 : 0);
    }
    identity.integer(options.types.size());
    for (const auto& type : options.types) { identity.integer(type.id); identity.text(key(type.original_type)); }
    identity.integer(options.mode == CampaignSetupMode::multiplayer ? 1 : 0);
    identity.integer(options.custom ? 1 : 0);
    if (options.custom) {
        identity.integer(static_cast<std::uint64_t>(options.custom->credits));
        identity.integer(static_cast<std::uint64_t>(options.custom->starting_tech));
        identity.integer(static_cast<std::uint64_t>(options.custom->maximum_tech));
        identity.integer(options.custom->allow_heroes ? 1 : 0);
    }
    if (options.seat_control) {
        // Appended only when present, so a single-player identity keeps its bytes.
        identity.text("seat-control");
        identity.integer(options.seat_control->teams.size());
        for (const auto& team : options.seat_control->teams) {
            identity.integer(team.side); identity.integer(team.lead.value); identity.integer(team.observer_ai ? 1 : 0);
        }
        identity.integer(options.seat_control->controllers.size());
        for (const auto& controller : options.seat_control->controllers) {
            identity.integer(controller.player); identity.integer(static_cast<std::uint64_t>(controller.kind));
            identity.integer(static_cast<std::uint64_t>(controller.dropout));
            identity.integer(controller.configuration.size());
            identity.bytes.insert(identity.bytes.end(), controller.configuration.begin(), controller.configuration.end());
        }
    }
    if (options.pause_policy) {
        // Appended only when chosen explicitly; the mode default is already part of the identity above.
        identity.text("pause-policy");
        identity.integer(static_cast<std::uint64_t>(options.pause_policy->fleet_management));
        identity.integer(static_cast<std::uint64_t>(options.pause_policy->surface_deploy));
        identity.integer(static_cast<std::uint64_t>(options.pause_policy->fleet_move));
    }
    galaxy_setup.campaign_identity = core::sha256(identity.bytes);
}

} // namespace

core::Result<CampaignSetupPlan> plan_campaign_setup(const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const std::string_view campaign_name, const CampaignSetupOptions& options) {
    const auto* campaign = data.campaign(campaign_name);
    if (!campaign) return invalid<CampaignSetupPlan>("unknown selected campaign " + std::string(campaign_name));
    if (options.mode != CampaignSetupMode::single_player && options.mode != CampaignSetupMode::multiplayer)
        return invalid<CampaignSetupPlan>("unknown galactic mode", campaign->file);
    if (options.seat_control && options.mode != CampaignSetupMode::multiplayer)
        return invalid<CampaignSetupPlan>("seat control requires multiplayer mode", campaign->file);
    // WGS-75: campaign staging resets custom options on selection and exposes its
    // option dialog only for definitions that support it. Omission retains WGP-74's true default.
    if (options.custom && !campaign->supports_custom_settings.value_or(true))
        return invalid<CampaignSetupPlan>("selected definition does not admit custom campaign options", campaign->file);
    CampaignSetupPlan plan;
    plan.player_bindings_ = options.players;
    if (options.mode == CampaignSetupMode::single_player) plan.solo_reference_human_ = options.solo_reference_human;
    for (const auto& seat : options.seats) if (seat.active && seat.kind == gc::SeatKind::human)
        plan.human_observers_.push_back(seat.player);
    std::sort(plan.human_observers_.begin(), plan.human_observers_.end());
    plan.human_observers_.erase(std::unique(plan.human_observers_.begin(), plan.human_observers_.end()), plan.human_observers_.end());
    if (plan.human_observers_.empty() && plan.solo_reference_human_) plan.human_observers_.push_back(*plan.solo_reference_human_);
    plan.mode_ = options.mode;
    // GOBS-01 / WGS-04: campaign definition construction defaults auto-reveal true.
    plan.observer_auto_reveal_ = campaign->planet_auto_reveal.value_or(true);
    if (const auto* delay = data.constant("Planet_Reveal_Delay_Time")) plan.observer_reveal_delay_ = delay->number;
    plan.campaign_name_ = campaign->name;
    plan.galaxy_.seed = options.forced_seed.value_or(options.seed);
    plan.galaxy_.seats = options.seats;
    plan.galaxy_.seat_control = options.seat_control;
    std::map<std::string, const CampaignPlayerBinding*> players;
    std::set<gc::FactionId> faction_ids;
    gc::PlayerId neutral{};
    for (const auto& binding : options.players) {
        const auto* faction = data.faction(binding.faction);
        if (!faction || binding.player.faction_id == 0 || !faction_ids.insert(binding.player.faction_id).second
            || !players.emplace(key(faction->name), &binding).second)
            return invalid<CampaignSetupPlan>("unknown or multiply bound faction " + binding.faction, campaign->file);
        if (faction->is_neutral.value_or(false)) {
            if (neutral != 0 || !binding.active) return invalid<CampaignSetupPlan>("root needs one active neutral player", campaign->file);
            neutral = binding.player.player_id;
        }
        plan.galaxy_.players.push_back(binding.player);
    }
    if (neutral == 0) return invalid<CampaignSetupPlan>("root needs one active neutral player", campaign->file);
    if (options.solo_reference_human && std::none_of(options.players.begin(), options.players.end(), [&](const auto& binding) {
            return binding.active && binding.player.player_id == *options.solo_reference_human;
        })) return invalid<CampaignSetupPlan>("local observer is not an active bound player", campaign->file);
    for (const auto& seat : options.seats) {
        const auto player = std::find_if(options.players.begin(), options.players.end(),
            [&](const auto& row) { return row.player.player_id == seat.player; });
        if (player == options.players.end() || (seat.active && !player->active))
            return invalid<CampaignSetupPlan>("active seat names an absent or inactive player", campaign->file);
    }
    // WGS-02/03: only a new root starts its allocator at one. Child construction never calls here.
    auto model = build_campaign_galaxy(data, types, *campaign, neutral, 1);
    if (!model) return core::Result<CampaignSetupPlan>::failure(model.error());
    plan.diagnostics_ = model.value().diagnostics;
    plan.galaxy_.planets = model.value().planets;
    plan.galaxy_.routes = model.value().routes;
    for (const auto& route : model.value().routes) {
        const auto named = std::find_if(model.value().names.begin(), model.value().names.end(),
            [&](const auto& row) { return row.id == route.id; });
        if (named == model.value().names.end()) return invalid<CampaignSetupPlan>("galaxy model has an unnamed route", campaign->file);
        plan.route_names_.push_back(named->name);
    }
    plan.galaxy_.content_identity = model.value().content_identity;
    plan.galaxy_.rules.maximum_fleet_movement_distance = model.value().maximum_movement_distance;
    plan.first_free_ = model.value().next_id;
    const auto* fiscal = data.constant("Fiscal_Cycle_Time_In_Secs");
    if (!fiscal || !fiscal->number) return invalid<CampaignSetupPlan>("missing Fiscal_Cycle_Time_In_Secs", campaign->file);
    auto frames = gc::fiscal_cycle_frames(*fiscal->number);
    if (!frames) return core::Result<CampaignSetupPlan>::failure(frames.error());
    plan.galaxy_.rules.fiscal_cycle_frames = frames.value();
    plan.galaxy_.fleet_control.emplace();
    // WGM-07 deviation (owner default): single player and co-op manage their forces during a player pause;
    // a versus lobby, or a multiplayer setup that does not say, keeps retail's refusal.
    plan.galaxy_.fleet_control->pause = options.pause_policy ? *options.pause_policy
        : options.mode == CampaignSetupMode::single_player || (options.seat_control && options.seat_control->teams.size() <= 1)
            ? gc::PausePolicy::admit_all() : gc::PausePolicy{};
    std::map<std::string, const units::GalacticType*> original_types;
    for (const auto& type : types.types) original_types.emplace(key(type.name), &type);
    std::map<std::string, gc::TypeId> type_ids;
    gc::TypeId previous{};
    for (const auto& binding : options.types) {
        const auto original = original_types.find(key(binding.original_type));
        if (original == original_types.end() || binding.id <= previous
            || !type_ids.emplace(key(binding.original_type), binding.id).second)
            return invalid<CampaignSetupPlan>("unknown, unordered or multiply bound original type " + binding.original_type, campaign->file);
        previous = binding.id;
        plan.bound_types_.types.push_back(*original->second);
        // WGM-100: the loader has already resolved inheritance; only a chain omission
        // receives the shared true default. Keep explicit and inherited false values.
        auto& resolved = plan.bound_types_.types.back();
        if (!resolved.can_hyperspace_without_ability) resolved.can_hyperspace_without_ability = true;
    }
    plan.type_bindings_ = options.types;
    std::set<gc::TypeId> referenced_types;
    std::map<std::string, gc::PlanetId> planets;
    for (const auto& planet : model.value().planets) {
        const auto named = std::find_if(model.value().names.begin(), model.value().names.end(), [&](const auto& row) { return row.id == planet.id; });
        // WGS-184 qualifies route lookup only. Full setup duplicate Locations
        // continuation (including repeated force placement) remains unverified;
        // see campaign-setup.md before relaxing this binding boundary.
        if (named == model.value().names.end() || !planets.emplace(key(named->name), planet.id).second)
            return invalid<CampaignSetupPlan>("galaxy model has missing or duplicate planet names", campaign->file);
    }
    if (options.fiscal_players) {
        if (auto phase = plan_fiscal_setup(data, options, campaign, model, plan.fiscal_); !phase)
            return core::Result<CampaignSetupPlan>::failure(phase.error());
    }
    if (auto phase = plan_location_requests(campaign, neutral, planets, plan.galaxy_, plan.locations_); !phase)
        return core::Result<CampaignSetupPlan>::failure(phase.error());
    if (auto phase = plan_starting_placements(campaign, options, types, plan.locations_, original_types, type_ids, players, referenced_types, plan.initial_placements_, plan.forces_, plan.diagnostics_); !phase)
        return core::Result<CampaignSetupPlan>::failure(phase.error());
    plan_home_locations(campaign, options, planets, plan.homes_);
    if (auto phase = plan_player_resources(data, campaign, options, plan.homes_, type_ids, referenced_types, plan.credits_, plan.tech_, plan.heroes_); !phase)
        return core::Result<CampaignSetupPlan>::failure(phase.error());
    plan_fleet_traits(options, referenced_types, plan.bound_types_, plan.diagnostics_, plan.galaxy_);
    if (auto phase = plan_victory_setup(data, campaign, options, original_types, type_ids, planets, model, plan.bound_types_, plan.galaxy_); !phase)
        return core::Result<CampaignSetupPlan>::failure(phase.error());
    if (auto valid = gc::validate_setup(plan.galaxy_); !valid) return core::Result<CampaignSetupPlan>::failure(valid.error());
    seal_campaign_identity(campaign, options, plan.galaxy_);
    return core::Result<CampaignSetupPlan>::success(std::move(plan));
}

bool campaign_type_creatable(const units::GalacticTypeTable& types, const std::string_view name) {
    std::set<std::string> visiting;
    const std::function<bool(std::string_view)> creatable = [&](const std::string_view current) {
        const auto* type = types.find(current);
        if (!type) return false;
        if (!visiting.insert(key(current)).second) return true; // a cycle adds no unresolved member
        bool ok = true;
        // A ground company with no authored transport or no members has nothing to create.
        if (std::find(type->behavior.begin(), type->behavior.end(), "DUMMY_GROUND_COMPANY") != type->behavior.end()
            && (type->company_transport.empty() || type->company_units.empty())) ok = false;
        for (const auto* names : {&type->company_units, &type->squadron_units})
            for (const auto& member : *names) ok = ok && creatable(member);
        if (!type->company_transport.empty()) ok = ok && creatable(type->company_transport);
        visiting.erase(key(current));
        return ok;
    };
    return creatable(name);
}

std::vector<std::string> campaign_referenced_types(const data::galactic::GalacticData& data, const std::string_view campaign) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    const auto add = [&](const std::string& name) {
        if (!name.empty() && seen.insert(key(name)).second) names.push_back(name);
    };
    if (const auto* source = data.campaign(campaign)) {
        for (const auto& record : source->starting_forces) add(record.type);
        for (const auto& record : source->special_case_production) add(record.type);
    }
    return names;
}

core::Result<CampaignSetupPlan> plan_live_campaign_setup(const vfs::Vfs& filesystem,
    const data::galactic::GalacticData& data, const units::GalacticTypeTable& types,
    std::string_view campaign, CampaignSetupOptions options, std::string_view difficulty) {
    auto document = data::load_document(filesystem, "data/xml/difficultyadjustments.xml");
    if (!document) return core::Result<CampaignSetupPlan>::failure(document.error());
    std::optional<sim::math::Fixed> multiplier;
    for (const auto& adjustment : document.value().root.children) {
        const auto name = std::find_if(adjustment.attributes.begin(), adjustment.attributes.end(),
            [](const auto& attribute) { return key(attribute.name) == "NAME"; });
        if (name == adjustment.attributes.end() || key(name->value) != key(difficulty)) continue;
        for (const auto& node : adjustment.children) if (key(node.name) == "CREDIT_MULTIPLIER") {
            data::tag_trace::used(&node);
            auto value = data::fixed_value(node);
            if (!value || value.value().raw() < 0)
                return invalid<CampaignSetupPlan>("invalid difficulty Credit_Multiplier");
            multiplier = value.value();
        }
    }
    if (!multiplier) return invalid<CampaignSetupPlan>("missing selected difficulty Credit_Multiplier");
    options.fiscal_players.emplace();
    const auto one = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
    for (const auto& binding : options.players) {
        const auto* faction = data.faction(binding.faction);
        if (!faction) return invalid<CampaignSetupPlan>("unknown fiscal faction " + binding.faction);
        const bool playable = binding.active && !faction->is_neutral.value_or(false) && faction->is_playable.value_or(false);
        if (playable && !faction->can_control_planets)
            return invalid<CampaignSetupPlan>("missing Can_Control_Planets for " + binding.faction);
        const bool eligible = playable && faction->can_control_planets.value_or(false);
        const auto controller = std::find_if(options.seats.begin(), options.seats.end(),
            [&](const auto& seat) { return seat.active && seat.player == binding.player.player_id; });
        if (eligible && controller == options.seats.end())
            return invalid<CampaignSetupPlan>("missing active fiscal controller for " + binding.faction);
        const bool human = std::any_of(options.seats.begin(), options.seats.end(), [&](const auto& seat) {
            return seat.active && seat.player == binding.player.player_id && seat.kind == gc::SeatKind::human;
        });
        // WGEP-16 also scales AI refunds; eligibility gates planet collection,
        // independently of the account's positive-addition difficulty policy.
        const bool ai = controller != options.seats.end() && !human;
        options.fiscal_players->push_back({binding.player.player_id, eligible, ai ? *multiplier : one});
    }
    auto planned = plan_campaign_setup(data, types, campaign, options);
    if (!planned) return planned;
    auto& plan = planned.value();
    plan.galaxy_.fiscal = plan.fiscal_;
    plan.galaxy_.player_economy.emplace();
    for (const auto& credit : plan.credits_) {
        const auto count = std::count_if(plan.galaxy_.planets.begin(), plan.galaxy_.planets.end(),
            [&](const auto& planet) { return planet.owner == credit.player; });
        const auto planets = sim::math::Fixed::from_integer(std::max<std::int64_t>(1, count));
        sim::math::Fixed cap;
        if (!planets || !sim::math::try_multiply(planets.value(), plan.fiscal_->credit_cap_per_planet, cap))
            return invalid<CampaignSetupPlan>("live credit cap exceeds represented range");
        // WGS-74: initial credit assignment is not clamped. Tech/story unlocks
        // still require the owning assignment provider (WGS-13/WGS-U08).
        plan.galaxy_.player_economy->players.push_back({credit.player, credit.amount, cap, 0, 0});
    }
    if (auto valid = gc::validate_setup(plan.galaxy_); !valid)
        return core::Result<CampaignSetupPlan>::failure(valid.error());
    return planned;
}

core::Result<void> initialize_layout_observer(const data::galactic::GalacticData& data,
    const CampaignSetupPlan& plan, const gc::PlayerId local_player, gc::GalacticSetup& setup) {
    if (setup.observer || !setup.fleets.empty() || setup.movement || setup.control || setup.production
        || !setup.fleet_control || !setup.fleet_control->detached_units.empty() || setup.planets != plan.galaxy().planets)
        return invalid<void>("observer_layout_only requires the untouched neutral campaign layout");
    const auto local = std::find_if(plan.player_bindings().begin(), plan.player_bindings().end(), [&](const auto& row) {
        return row.active && row.player.player_id == local_player;
    });
    if (local == plan.player_bindings().end()) return invalid<void>("layout observer requires an active bound local player");
    if (!plan.observer_reveal_delay() || !setup.rules.maximum_fleet_movement_distance || !plan.observer_auto_reveal())
        return invalid<void>("layout observer requires authored reveal delay, range and auto-reveal policy");
    gc::ObserverSetup observer; observer.human_observers = {local_player}; observer.solo_reference_human = local_player;
    observer.reveal_delay_seconds = *plan.observer_reveal_delay();
    observer.automatic_reveal_range = *setup.rules.maximum_fleet_movement_distance;
    for (const auto& binding : plan.player_bindings()) {
        const auto* faction = data.faction(binding.faction);
        if (!faction) return invalid<void>("layout observer faction metadata is absent");
        gc::ObserverPlayer player; player.player = binding.player.player_id;
        // observer_layout_nonlocal: this preview publishes local discovery only;
        // AI and other client initialization require their own live producers.
        player.ai = player.player != local_player; player.allies = {player.player};
        // observer_layout_empty_bases: there are no constructed bases in this
        // untouched layout. An absent faction threshold grants no size inference.
        player.minimum_visible_base_level = std::numeric_limits<std::int64_t>::max();
        observer.players.push_back(prepare_observer_player(*faction, std::move(player)));
    }
    // Resolve layout identities once; do not rescan every location per planet.
    std::map<gc::PlanetId, std::string_view> planet_sources;
    for (const auto& location : plan.locations())
        if (location.planet) planet_sources.emplace(location.planet->id, location.original_type);
    for (const auto& planet : setup.planets) {
        const auto location = planet_sources.find(planet.id);
        if (location == planet_sources.end()) return invalid<void>("layout observer planet metadata is absent");
        const auto* source = data.planet(location->second);
        if (!source) return invalid<void>("layout observer planet metadata is unresolved");
        gc::ObserverPlanet facts; facts.planet = planet.id;
        // WGS-09: real freshly created layout owner, before the uninstalled
        // placement/control providers. No authored force record becomes ownership.
        facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = planet.owner;
        facts.automatic_reveal = *plan.observer_auto_reveal();
        observer.planets.push_back(prepare_observer_planet(*source, std::move(facts)));
    }
    auto staged = setup; staged.observer = std::move(observer);
    const auto home = std::find_if(plan.homes().begin(), plan.homes().end(), [&](const auto& row) { return row.player == local_player; });
    auto initialized = gc::initialize_campaign_observer(staged, home == plan.homes().end() ? std::nullopt : home->planet);
    if (!initialized) return initialized;
    setup.observer = std::move(staged.observer);
    return core::Result<void>::success();
}

CampaignRoot::CampaignRoot(CampaignSetupPlan plan, GameSession game, std::vector<CampaignCreditCap> caps)
    : plan_(std::move(plan)), game_(std::move(game)), caps_(std::move(caps)) {}
core::Result<CampaignRoot> CampaignRoot::create(CampaignSetupPlan plan, const CampaignSetupServices& services) {
    if (!services.location || (!services.political_control && !services.initial_political_control && !services.stock_starting_forces) || !services.credit_cap || !services.home)
        return invalid<CampaignRoot>("unimplemented location/control/credit-cap/home setup provider");
    if (!plan.tech().empty() && !services.assign_tech)
        return invalid<CampaignRoot>("unimplemented tech/story/production setup provider");
    if (!plan.multiplayer_heroes().empty() && !services.stock_starting_forces && (!services.home_has_production || !services.owned_original_types || !services.place_hero))
        return invalid<CampaignRoot>("unimplemented multiplayer hero/home-production provider");
    if (!plan.initial_placements().empty() && !services.mark_initial_placement && !services.stock_starting_forces)
        return invalid<CampaignRoot>("unimplemented initial-placement setup provider");
    if (!plan.forces().empty() && !services.place && !services.stock_starting_forces)
        return invalid<CampaignRoot>("unimplemented WGS-U08 nested placement provider");
    if (!services.unify_human_guard_fleets && !services.stock_starting_forces && std::any_of(plan.forces().begin(), plan.forces().end(), [](const auto& row) { return row.human; }))
        return invalid<CampaignRoot>("unimplemented human guard-fleet unification provider");
    auto setup = plan.galaxy();
    // WGS-08/09: the live layout convenience must not expose its prepared
    // accounts before root placement/control/cap passes. Root assigns credits
    // and installs fiscal policy below, after the owning setup providers run.
    setup.player_economy.reset();
    setup.fiscal.reset();
    std::optional<CampaignPlacementContext> placement_context;
    if (services.stock_starting_forces) {
        if (!services.placement_context || services.place || services.production || services.census || services.unify_human_guard_fleets)
            return invalid<CampaignRoot>("stock starting forces require one owning placement context");
        auto prepared = services.placement_context(plan);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        placement_context = std::move(prepared).value();
        auto nested = prepare_nested_creation(*services.stock_starting_forces);
        if (!nested) return core::Result<CampaignRoot>::failure(nested.error());
        placement_context->production.nested_creation = std::move(nested).value();
        if (!placement_context->control.state.empty() || !placement_context->control.captured_types.empty()
            || !placement_context->control.absent_forces.empty() || !placement_context->movement.contents.empty()
            || !placement_context->census.objects.empty())
            return invalid<CampaignRoot>("stock placement context must be fresh");
        for (const auto& planet : placement_context->control.planets)
            if (planet.ground_base.level || planet.starbase.level || !planet.landed.empty() || !planet.special_control.empty())
                return invalid<CampaignRoot>("stock placement context already contains forces");
        for (const auto& planet : placement_context->production.planets)
            if (!planet.objects.empty() || !planet.queues[0].empty() || !planet.queues[1].empty() || planet.starbase_type)
                return invalid<CampaignRoot>("stock placement context already contains production");
        setup.control = placement_context->control; setup.movement = placement_context->movement;
        auto traits = build_fleet_type_traits(services.stock_starting_forces->types,
            services.stock_starting_forces->bindings);
        if (!traits) return core::Result<CampaignRoot>::failure(traits.error());
        setup.fleet_control->types = std::move(traits).value();
        if (auto valid = gc::validate_setup(setup); !valid) return core::Result<CampaignRoot>::failure(valid.error());
    }
    auto next = plan.first_free_entity_id();
    std::map<gc::PlanetId, std::vector<StartingAllegiance>> allegiance;
    // WGS-09: complete the neutral location pass before any force pass.
    for (const auto& location : plan.locations())
        if (auto initialized = services.location(location); !initialized) return core::Result<CampaignRoot>::failure(initialized.error());
    for (const auto& location : plan.locations()) {
        if (!location.planet) continue;
        const auto planet = location.planet->id;
        for (const auto& mark : plan.initial_placements()) if (mark.planet == planet) {
            if (services.stock_starting_forces) {
                const auto at = std::find_if(placement_context->production.planets.begin(), placement_context->production.planets.end(),
                    [&](const auto& row) { return row.planet == mark.planet; });
                if (at == placement_context->production.planets.end()) return invalid<CampaignRoot>("initial-placement planet context is absent");
                const auto offer = std::find_if(at->offers.begin(), at->offers.end(), [&](const auto& row) { return row.type == mark.type; });
                if (offer == at->offers.end()) return invalid<CampaignRoot>("initial-placement production type is absent");
                offer->initially_placed = true;
            }
            if (services.mark_initial_placement)
                if (auto result = services.mark_initial_placement(mark); !result) return core::Result<CampaignRoot>::failure(result.error());
        }
        for (const auto& request : plan.forces()) {
            if (request.planet != planet) continue;
            auto& targets = allegiance[planet];
            auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& row) { return row.player == request.owner; });
            // WGS-70's target amount is code 1.0; it is not a current political-owner assignment.
            const StartingAllegiance value{request.owner, request.target_allegiance};
            if (target == targets.end()) targets.push_back(value); else *target = value;
            const auto first_free = next;
            auto placed = services.stock_starting_forces
                ? place_starting_force(*services.stock_starting_forces, request, next, setup, *placement_context)
                : services.place(request, next, setup);
            if (!placed) return core::Result<CampaignRoot>::failure(placed.error());
            auto frontier = placement_frontier(placed.value(), next, request, setup, plan);
            if (!frontier) return core::Result<CampaignRoot>::failure(frontier.error());
            next = frontier.value();
            setup.fleets.insert(setup.fleets.end(), placed.value().fleets.begin(), placed.value().fleets.end());
            auto& detached = setup.fleet_control->detached_units;
            detached.insert(detached.end(), placed.value().detached_units.begin(), placed.value().detached_units.end());
            if (services.stock_starting_forces) {
                if (placed.value().originals.size() != next - first_free)
                    return invalid<CampaignRoot>("stock placement requires original metadata for every allocation");
                if (auto applied = apply_starting_force_placement(*services.stock_starting_forces, request,
                    placed.value(), setup, *placement_context); !applied)
                    return core::Result<CampaignRoot>::failure(applied.error());
            } else if (!placed.value().contents.empty() || !placed.value().landed.empty() || !placed.value().structures.empty()
                || !placed.value().originals.empty() || placed.value().starbase || placed.value().ground_base)
                return invalid<CampaignRoot>("nested placement requires its persistent owning context");
            // WGS-80: the debug-build human guard callback is empty. Retail
            // qualification of this retained-fleet behaviour remains WGS-U08.
            if (request.human && placed.value().created && services.unify_human_guard_fleets) {
                auto unified = services.unify_human_guard_fleets(planet, request.owner, setup);
                if (!unified) return core::Result<CampaignRoot>::failure(unified.error());
                // Providers may merge containers, but may not invent/discard/retype persistent
                // units, change other owners/orbits or rewind the allocator.
                std::map<sim::EntityId, std::pair<gc::FleetMember, std::pair<gc::PlayerId, gc::PlanetId>>> before, after;
                for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
                    before.emplace(member.id, std::pair{member, std::pair{fleet.owner, fleet.orbit}});
                std::set<gc::FleetId> old_ids;
                for (const auto& fleet : setup.fleets) old_ids.insert(fleet.id);
                for (const auto& fleet : setup.fleets) if (fleet.owner != request.owner || fleet.orbit != planet) {
                    const auto unchanged = std::find_if(unified.value().begin(), unified.value().end(),
                        [&](const auto& row) { return row.id == fleet.id; });
                    if (unchanged == unified.value().end() || *unchanged != fleet)
                        return invalid<CampaignRoot>("guard unification changed an unrelated fleet");
                }
                for (const auto& fleet : unified.value()) {
                    if (!old_ids.contains(fleet.id)) return invalid<CampaignRoot>("guard unification invented a fleet identity");
                    for (const auto& member : fleet.members)
                        if (!after.emplace(member.id, std::pair{member, std::pair{fleet.owner, fleet.orbit}}).second)
                            return invalid<CampaignRoot>("guard unification duplicated a member");
                }
                if (before != after) return invalid<CampaignRoot>("guard unification changed persistent force identity/location/owner");
                setup.fleets = std::move(unified).value();
            }
            std::sort(setup.fleets.begin(), setup.fleets.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            std::sort(detached.begin(), detached.end(), [](const auto& a, const auto& b) { return a.member.id < b.member.id; });
            if (auto valid = gc::validate_setup(setup); !valid) return core::Result<CampaignRoot>::failure(valid.error());
        }
    }
    // Separate control pass: every force on every planet exists before it begins.
    if (services.initial_political_control || services.stock_starting_forces) {
        auto control = services.initial_political_control ? services.initial_political_control(plan, setup)
            : prepare_initial_control(setup, *setup.control);
        if (!control) return core::Result<CampaignRoot>::failure(control.error());
        if (!control.value().captured_types.empty() || !control.value().absent_forces.empty())
            return invalid<CampaignRoot>("initial political provider returned a conquest/death journal");
        if (control.value().state.size() != setup.planets.size())
            return invalid<CampaignRoot>("initial political provider must initialize every planet");
        for (std::size_t i = 0; i < setup.planets.size(); ++i) {
            const auto& state = control.value().state[i];
            if (!state.initialized || state.planet != setup.planets[i].id
                || state.current_player != state.target_player || state.current != state.target)
                return invalid<CampaignRoot>("initial political provider returned incomplete current/target allegiance");
            setup.planets[i].owner = state.current_player;
        }
        setup.control = std::move(control).value();
    } else for (auto& planet : setup.planets) {
        auto owner = services.political_control(planet, allegiance[planet.id], setup);
        if (!owner) return core::Result<CampaignRoot>::failure(owner.error());
        if (std::none_of(setup.players.begin(), setup.players.end(), [&](const auto& player) { return player.player_id == owner.value(); }))
            return invalid<CampaignRoot>("political-control provider returned an unknown player");
        planet.owner = owner.value();
    }
    if (auto valid = gc::validate_setup(setup); !valid) return core::Result<CampaignRoot>::failure(valid.error());
    std::vector<CampaignCreditCap> caps;
    for (const auto& player : setup.players) {
        auto cap = services.credit_cap(player, setup);
        if (!cap) return core::Result<CampaignRoot>::failure(cap.error());
        caps.push_back({player.player_id, cap.value()});
    }
    for (const auto& home : plan.homes())
        if (auto assigned = services.home(home); !assigned) return core::Result<CampaignRoot>::failure(assigned.error());
    // WGS-14: inside object assignment, after homes/caps and before the WGS-08 credit pass.
    // Keep duplicate authored records; a newly live original type guards later repeats.
    std::map<gc::PlayerId, std::set<gc::TypeId>> created_originals;
    for (const auto& hero : plan.multiplayer_heroes()) {
        const StartingForceRequest context{hero.home, hero.player, hero.original_type_id, hero.original_type, false, {}};
        if (services.home_has_production) {
            auto production = services.home_has_production(hero.home);
            if (!production) return core::Result<CampaignRoot>::failure(production.error());
            if (!production.value()) continue;
        }
        if (services.stock_starting_forces) {
            // WGS-14: home_has_production above qualifies authored production behavior.
            // production.planets holds every location; census retains placed original identities.
            const auto& producers = placement_context->production.planets;
            if (std::none_of(producers.begin(), producers.end(), [&](const auto& row) { return row.planet == hero.home; })) continue;
            const auto& originals = placement_context->census.objects;
            if (std::any_of(originals.begin(), originals.end(), [&](const auto& row) {
                return row.owner == hero.player && row.original_type == hero.original_type_id;
            })) continue;
            placement_context->control = *setup.control;
        } else {
            auto owned = services.owned_original_types(hero.player, setup);
            if (!owned) return core::Result<CampaignRoot>::failure(owned.error());
            if (std::find(owned.value().begin(), owned.value().end(), hero.original_type_id) != owned.value().end()) continue;
        }
        if (created_originals[hero.player].contains(hero.original_type_id)) continue;
        const auto first_free = next;
        auto placed = services.stock_starting_forces
            ? place_starting_force(*services.stock_starting_forces, context, next, setup, *placement_context)
            : services.place_hero(hero, next, setup);
        if (!placed) return core::Result<CampaignRoot>::failure(placed.error());
        auto frontier = placement_frontier(placed.value(), next, context, setup, plan);
        if (!frontier) return core::Result<CampaignRoot>::failure(frontier.error());
        next = frontier.value();
        setup.fleets.insert(setup.fleets.end(), placed.value().fleets.begin(), placed.value().fleets.end());
        auto& detached = setup.fleet_control->detached_units;
        detached.insert(detached.end(), placed.value().detached_units.begin(), placed.value().detached_units.end());
        if (services.stock_starting_forces) {
            if (placed.value().originals.size() != next - first_free)
                return invalid<CampaignRoot>("stock hero placement requires original metadata for every allocation");
            if (auto applied = apply_starting_force_placement(*services.stock_starting_forces, context,
                placed.value(), setup, *placement_context); !applied)
                return core::Result<CampaignRoot>::failure(applied.error());
        }
        std::sort(setup.fleets.begin(), setup.fleets.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(detached.begin(), detached.end(), [](const auto& a, const auto& b) { return a.member.id < b.member.id; });
        if (auto valid = gc::validate_setup(setup); !valid) return core::Result<CampaignRoot>::failure(valid.error());
        if (placed.value().created) created_originals[hero.player].insert(hero.original_type_id);
    }
    setup.player_economy.emplace();
    for (const auto& cap : caps) setup.player_economy->players.push_back({cap.player, {}, cap.value, 0, 0});
    // WGS-74: Set_Credits is direct assignment, not cap clamping or income.
    for (std::size_t index = 0; index < plan.credits().size(); ++index)
        setup.player_economy->players[index].credits = plan.credits()[index].amount;
    for (std::size_t index = 0; index < plan.tech().size(); ++index) {
        const auto& request = plan.tech()[index];
        auto applied = services.assign_tech(request, setup);
        if (!applied) return core::Result<CampaignRoot>::failure(applied.error());
        if (!source_integer(applied.value())) return invalid<CampaignRoot>("tech provider returned a value outside the source integer");
        auto& row = setup.player_economy->players[index];
        row.current_tech = applied.value(); row.maximum_tech = request.requested_maximum;
    }
    setup.fiscal = plan.fiscal();
    if (placement_context) {
        setup.production = std::move(placement_context->production);
        setup.census = std::move(placement_context->census);
        for (auto original : placement_context->location_originals) {
            const auto planet = std::find_if(setup.planets.begin(), setup.planets.end(),
                [&](const auto& row) { return row.id == original.id; });
            if (planet == setup.planets.end() || original.planet != original.id || original.container
                || original.dead || original.delete_pending || !original.original_type)
                return invalid<CampaignRoot>("invalid planet original identity in stock placement context");
            original.owner = planet->owner;
            // CRP-01: this is the actual typed planet constructor binding.
            original.current_type = original.original_type;
            original.combat_role = sim::galactic::ActorCombatRole::ordinary;
            original.abilities = {}; // qualified planet constructor, no initialized stealth grant
            original.garrison = false; // GAG-01 ordinary constructor default.
            setup.census->objects.push_back(original);
        }
        std::sort(setup.census->objects.begin(), setup.census->objects.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        if (services.stock_production) {
            auto prepared = services.stock_production(plan, setup);
            if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
            setup.production = std::move(prepared).value();
        }
        if (services.stock_access) {
            auto prepared = services.stock_access(plan, setup);
            if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
            setup.movement->access = std::move(prepared).value();
        }
    }
    if (services.production) {
        auto prepared = services.production(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.production = std::move(prepared).value();
    }
    if (services.census) {
        auto prepared = services.census(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.census = std::move(prepared).value();
    }
    // GPC-01: the control pass above runs before the census exists, so the live actors seed the source ledger here.
    if (setup.control && setup.census) {
        setup.control->ability_sources.clear();
        for (const auto& actor : setup.census->objects) gc::update_control_actor(*setup.control, actor);
    }
    if (services.income) {
        auto prepared = services.income(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.income = std::move(prepared).value();
    }
    if (services.economy_corruption) {
        auto prepared = services.economy_corruption(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.economy_corruption = std::move(prepared).value();
    }
    if (services.ability_host) {
        auto prepared = services.ability_host(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.ability_host = std::move(prepared).value();
    }
    if (services.observer) {
        if (plan.human_observers().empty()) return invalid<CampaignRoot>("campaign observer requires a human roster");
        auto prepared = services.observer(plan, setup);
        if (!prepared) return core::Result<CampaignRoot>::failure(prepared.error());
        setup.observer = std::move(prepared).value();
        setup.observer->human_observers.assign(plan.human_observers().begin(), plan.human_observers().end());
        setup.observer->solo_reference_human = plan.solo_reference_human().value_or(0);
        if (plan.observer_reveal_delay()) setup.observer->reveal_delay_seconds = *plan.observer_reveal_delay();
        setup.observer->automatic_reveal_range = *setup.rules.maximum_fleet_movement_distance;
        if (plan.observer_auto_reveal()) for (auto& planet : setup.observer->planets)
            planet.automatic_reveal = *plan.observer_auto_reveal();
        std::vector<gc::ObserverHome> homes;
        for (const auto human : plan.human_observers()) {
            const auto home = std::find_if(plan.homes().begin(), plan.homes().end(), [&](const auto& row) { return row.player == human; });
            homes.push_back({human, home == plan.homes().end() ? std::nullopt : home->planet});
        }
        auto initialized = gc::initialize_campaign_observers(setup, homes);
        if (!initialized) return core::Result<CampaignRoot>::failure(initialized.error());
    }
    if (auto valid = gc::validate_setup(setup); !valid) return core::Result<CampaignRoot>::failure(valid.error());
    auto game = GameSession::create(setup);
    if (!game) return core::Result<CampaignRoot>::failure(game.error());
    bind_story_identities(game.value(), plan);
    if (game.value().galaxy().next_entity_id() != next)
        return invalid<CampaignRoot>("setup provider lost its allocator frontier; an unrepresented object requires its owning subsystem");
    return core::Result<CampaignRoot>::success(CampaignRoot(std::move(plan), std::move(game).value(), std::move(caps)));
}
void bind_story_identities(GameSession& game, const CampaignSetupPlan& plan) {
    std::map<gc::PlanetId, std::string> planets;
    std::map<gc::PlayerId, std::string> factions;
    for (const auto& location : plan.locations()) if (location.planet)
        planets.emplace(location.planet->id, location.original_type);
    for (const auto& binding : plan.player_bindings())
        factions.emplace(binding.player.player_id, binding.faction);
    game.set_story_identities(std::move(planets), std::move(factions));
}
void CampaignRoot::stop_root() noexcept {
    game_.reset(); plan_.reset(); caps_.clear();
}
} // namespace eawr::campaign
