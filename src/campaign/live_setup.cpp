#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/planet_economy.hpp"
#include "eawr/campaign/control.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/movement.hpp"
#include "eawr/sim/galactic/income.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
using Fixed = sim::math::Fixed;
const auto one = Fixed::from_raw(Fixed::scale);
std::string key(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return result;
}
template<class T> core::Result<T> refuse(const std::string& message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
    d.message = "stock campaign setup: " + message;
    return core::Result<T>::failure(std::move(d));
}
std::vector<std::string> words(std::string text) {
    for (auto& c : text) if (c == ',') c = ' ';
    std::istringstream stream(text); std::vector<std::string> result;
    for (std::string word; stream >> word;) result.push_back(std::move(word));
    return result;
}
bool behaves(const units::GalacticType& type, std::string_view token) {
    return std::find(type.behavior.begin(), type.behavior.end(), token) != type.behavior.end();
}
bool contains(std::span<const std::string> names, std::string_view name) {
    return std::any_of(names.begin(), names.end(), [&](const auto& n) { return key(n) == key(name); });
}
std::vector<const data::Definition*> layers(const data::Catalog& catalog, std::string_view name, data::Category category) {
    std::vector<const data::Definition*> result;
    auto resolved = catalog.resolve(name, category);
    if (resolved) for (const auto& original : resolved.value().chain)
        if (const auto* definition = catalog.find(original, category)) result.push_back(definition);
    return result;
}
core::Result<Fixed> number(const data::XmlNode* node, std::optional<Fixed> omitted = {}) {
    if (!node || data::retail::is_placeholder(node->raw_text)) {
        if (omitted) return core::Result<Fixed>::success(*omitted);
        return refuse<Fixed>("missing numeric input");
    }
    data::tag_trace::used(node);
    const auto value = data::retail::leading_number(node->raw_text);
    if (!value) return refuse<Fixed>("invalid " + node->name);
    return core::Result<Fixed>::success(value->value);
}
struct Prepared {
    data::Catalog catalog;
    data::galactic::GalacticData data;
    StartingForceInputs starting;
    InitialCampaignRelations relations;
    CampaignPlacementContext context;
    units::GalacticEconomicTable economics;
    std::map<std::string, gc::TypeId> identities;
    Fixed production_speed, ai_time;
    std::array<Fixed, 5> base_time;
    bool pay_as_you_go{};
    data::XmlDocument constants, market;
    gc::TypeId identity(std::string_view name) const {
        const auto at = identities.find(key(name)); return at == identities.end() ? 0 : at->second;
    }
};
const data::galactic::Planet* planet_source(const Prepared& p, const CampaignSetupPlan& plan, gc::PlanetId id) {
    const auto at = std::find_if(plan.locations().begin(), plan.locations().end(),
        [&](const auto& location) { return location.planet && location.planet->id == id; });
    return at == plan.locations().end() ? nullptr : p.data.planet(at->original_type);
}
const CampaignPlayerBinding* player_source(const CampaignSetupPlan& plan, gc::PlayerId id) {
    const auto at = std::find_if(plan.player_bindings().begin(), plan.player_bindings().end(),
        [&](const auto& player) { return player.player.player_id == id; });
    return at == plan.player_bindings().end() ? nullptr : &*at;
}
bool human(const gc::GalacticSetup& setup, gc::PlayerId id) {
    return std::any_of(setup.seats.begin(), setup.seats.end(),
        [&](const auto& seat) { return seat.active && seat.player == id && seat.kind == gc::SeatKind::human; });
}
core::Result<gc::ProductionOffer> offer(const Prepared& p, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup, const gc::PlanetProduction& location,
    const FleetTypeBinding& binding, const units::GalacticType& type, Fixed* unrounded_price = nullptr) {
    gc::ProductionOffer admission; admission.type = binding.id;
    const bool company = behaves(type, "DUMMY_GROUND_COMPANY");
    const bool squadron = behaves(type, "DUMMY_SPACE_FIGHTER_SQUADRON");
    const bool station = behaves(type, "DUMMY_STAR_BASE");
    const bool ground_structure = behaves(type, "DUMMY_GROUND_STRUCTURE");
    const bool orbital_structure = behaves(type, "DUMMY_SPACE_STRUCTURE");
    const bool ground_base = behaves(type, "DUMMY_GROUND_BASE");
    const bool ordinary_ship = behaves(type, "DUMMY_STARSHIP");
    admission.domain = units::built_on_ground(type) ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
    admission.kind = type.build_advances_tech_level.value_or(0) > 0 ? gc::ProductionKind::technology : station ? gc::ProductionKind::starbase : ground_structure || orbital_structure
        ? gc::ProductionKind::structure : gc::ProductionKind::unit;
    admission.direct_build_allowed = station || ground_structure || orbital_structure || ordinary_ship || company || squadron;
    if (ground_base) admission.direct_build_allowed = false;
    if (admission.kind == gc::ProductionKind::technology) {
        admission.direct_build_allowed = true; admission.target_tech = *type.build_advances_tech_level;
    }
    const auto* owner = player_source(plan, location.target_owner);
    if (!owner) return refuse<gc::ProductionOffer>("production owner is unbound");
    admission.affiliated = contains(type.affiliation, owner->faction);
    const auto original = layers(p.catalog, type.name, data::Category::game_object);
    if (const auto* node = data::retail::single(original, "Cinematic_Object_Only")) {
        data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
        if (!flag) return refuse<gc::ProductionOffer>("invalid cinematic flag");
        admission.cinematic = *flag;
    } // WGS-90: fresh false.
    if (const auto* node = data::retail::single(original, "Build_Can_Be_Unlocked_By_Slicer")) {
        data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
        if (!flag) return refuse<gc::ProductionOffer>("invalid slicer flag");
        admission.slicer_unlockable = *flag;
    }
    auto limit = number(data::retail::single(original, "Build_Max_Instances_Per_Planet"), Fixed::from_integer(-1).value());
    if (!limit || limit.value().raw() % Fixed::scale) return refuse<gc::ProductionOffer>("invalid per-planet production limit");
    admission.planet_limit = limit.value().trunc_to_integer();
    for (const auto& name : type.required_orbiting_units) {
        const auto id = p.identity(name); if (!id) return refuse<gc::ProductionOffer>("unbound orbit prerequisite " + name);
        admission.required_orbiting_units.push_back(id);
    }
    // The OR token links alternatives; an unlinked entry begins the next AND group.
    bool alternative = false;
    for (const auto& name : words(type.required_special_structures)) {
        if (key(name) == "OR" || name == "|") {
            if (admission.required_structures.empty() || alternative) return refuse<gc::ProductionOffer>("invalid structure OR grammar");
            alternative = true; continue;
        }
        const auto id = p.identity(name); if (!id) return refuse<gc::ProductionOffer>("unbound structure prerequisite " + name);
        if (!alternative) admission.required_structures.emplace_back();
        admission.required_structures.back().push_back(id); alternative = false;
    }
    if (alternative) return refuse<gc::ProductionOffer>("incomplete structure OR grammar");
    const auto* source = planet_source(p, plan, location.planet);
    if (!source) return refuse<gc::ProductionOffer>("unbound production planet");
    if (!type.required_planets.empty() && !contains(type.required_planets, source->name)) admission.direct_build_allowed = false;
    // WGEP-158..160: an authored positive cost never admits alone. Ground construction also needs a nonzero
    // required ground-base level or the ground-structure role; space construction a nonzero required
    // starbase level or the level-one starbase role. Heroes and variants follow their own resolved tags,
    // so a mod that authors a level makes them buildable. Every reader of the offer table (human list,
    // queue command, Lua production, AI producer options) sees this through the shared admission.
    const bool positive_cost = type.build_cost_credits.value_or(Fixed{}).raw() >= Fixed::scale;
    const bool built_on_ground = units::built_on_ground(type);
    const bool built_in_space = positive_cost
        && (type.required_star_base_level.value_or(0) != 0 || (station && type.base_level.value_or(0) == 1));
    if (!built_on_ground && !built_in_space) admission.direct_build_allowed = false;
    ProductionPreparation quantities;
    quantities.price_multiplier = quantities.time_multiplier = quantities.tech_purchase_multiplier = one;
    quantities.corruption_price_multiplier = quantities.corruption_time_multiplier = one;
    const auto faction = layers(p.catalog, owner->faction, data::Category::faction);
    const auto corruption_factor = [&](std::string_view tag) -> core::Result<Fixed> {
        const auto* node = data::retail::single(faction, tag);
        if (!node) return core::Result<Fixed>::success(one); // WGS-94: empty fresh vector getter fallback.
        data::tag_trace::used(node); const auto entries = words(node->raw_text);
        if (entries.empty()) return core::Result<Fixed>::success(one);
        const auto value = data::retail::leading_number(entries.front());
        if (!value || value->value.raw() < 0) return refuse<Fixed>("invalid corruption factor");
        return core::Result<Fixed>::success(value->value); // Fresh uncorrupted planet, WGEP-29/32.
    };
    const auto* faction_data = p.data.faction(owner->faction);
    if (!faction_data || !faction_data->benefits_from_corruption) return refuse<gc::ProductionOffer>("unbound corruption eligibility");
    if (!*faction_data->benefits_from_corruption && admission.affiliated) {
        auto price = corruption_factor("Corruption_Galactic_Production_Price_Multipliers");
        auto time = corruption_factor("Corruption_Galactic_Production_Time_Multipliers");
        if (!price) return core::Result<gc::ProductionOffer>::failure(price.error());
        if (!time) return core::Result<gc::ProductionOffer>::failure(time.error());
        quantities.corruption_price_multiplier = price.value(); quantities.corruption_time_multiplier = time.value();
    }
    quantities.ai_time_multiplier = human(setup, location.target_owner) ? one : p.ai_time;
    quantities.base_tech_multiplier = one;
    const auto difference = location.space_level - type.tech_level.value_or(0) - 1;
    if (admission.domain == gc::ProductionDomain::space && location.space_level > 0 && difference >= 0)
        quantities.base_tech_multiplier = p.base_time[static_cast<std::size_t>(std::min<std::int64_t>(difference, 4))];
    quantities.factory_count = 1;
    if (type.build_time_reduced_by_multiple_factories.value_or(false)) {
        std::int64_t count{};
        for (const auto& group : admission.required_structures) for (const auto id : group)
            count += static_cast<std::int64_t>(std::count_if(location.objects.begin(), location.objects.end(),
                [&](const auto& object) { return object.type == id && object.kind == gc::ProductionKind::structure; }));
        quantities.factory_count = std::max<std::int64_t>(1, count);
    }
    const auto* campaign = p.data.campaign(plan.campaign_name());
    quantities.tutorial = campaign && campaign->tutorial.value_or(false);
    auto adjusted = type;
    Fixed time;
    // WGEP-92: consume constructor defaults without mutating original facts.
    if (!sim::math::try_multiply(adjusted.build_time_seconds.value_or(Fixed{}), p.production_speed, time))
        return refuse<gc::ProductionOffer>("invalid unlocated production time");
    adjusted.build_time_seconds = time;
    if (unrounded_price) {
        if (!sim::math::try_multiply(adjusted.build_cost_credits.value_or(Fixed{}),
                *quantities.corruption_price_multiplier, *unrounded_price))
            return refuse<gc::ProductionOffer>("unrounded production price overflow");
    }
    return prepare_production_offer(adjusted, std::move(admission), quantities);
}
core::Result<bool> fleet_skips(const Prepared& p, const gc::GalacticSetup& setup, const gc::Fleet& fleet) {
    if (!setup.movement) return refuse<bool>("missing movement contents");
    std::vector<gc::FleetMember> pending(fleet.members.rbegin(), fleet.members.rend());
    std::set<sim::EntityId> seen; bool any = false;
    while (!pending.empty()) {
        const auto member = pending.back(); pending.pop_back();
        if (!seen.insert(member.id).second) return refuse<bool>("repeated or cyclic fleet contents");
        const auto binding = std::find_if(p.starting.bindings.begin(), p.starting.bindings.end(),
            [&](const auto& b) { return b.id == member.type; });
        if (binding == p.starting.bindings.end()) return refuse<bool>("unbound live fleet member");
        const auto* type = p.starting.types.find(binding->original_type);
        const auto contents = std::find_if(setup.movement->contents.begin(), setup.movement->contents.end(),
            [&](const auto& c) { return c.container == member.id; });
        if (behaves(*type, "TRANSPORT") && (contents == setup.movement->contents.end() || contents->children.empty()))
            return core::Result<bool>::success(false);
        if (contents != setup.movement->contents.end()) {
            pending.insert(pending.end(), contents->children.rbegin(), contents->children.rend()); continue;
        }
        const auto runtime = std::find_if(p.starting.runtime_types.begin(), p.starting.runtime_types.end(),
            [&](const auto& r) { return r.type == member.type; });
        if (runtime == p.starting.runtime_types.end()) return refuse<bool>("missing initial combat skip fact");
        any = true; if (!runtime->skips_combat) return core::Result<bool>::success(false);
    }
    return core::Result<bool>::success(any);
}
core::Result<gc::ProductionSetup> complete_production(const Prepared& p, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup) {
    if (!setup.production || !setup.control || !setup.player_economy) return refuse<gc::ProductionSetup>("missing post-placement production state");
    auto production = *setup.production;
    production.technology_types.clear();
    // WGEP-147: use every registered original, not the reachable build graph.
    for (const auto& binding : plan.type_bindings()) {
        const auto* type = plan.original_type(binding.id);
        if (!type) return refuse<gc::ProductionSetup>("missing registered technology type");
        gc::TechnologyType row;
        row.type = binding.id; row.tech = type->tech_level.value_or(0);
        row.initially_locked = type->build_initially_locked.value_or(false);
        const auto original = layers(p.catalog, type->name, data::Category::game_object);
        if (const auto* node = data::retail::single(original, "Build_Can_Be_Unlocked_By_Slicer")) {
            data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
            if (!flag) return refuse<gc::ProductionSetup>("invalid registered slicer flag");
            row.slicer_unlockable = *flag;
        }
        if (row.tech < 0 || row.tech > std::numeric_limits<std::int32_t>::max())
            return refuse<gc::ProductionSetup>("invalid registered technology level");
        for (const auto& owner : plan.player_bindings())
            if (contains(type->affiliation, owner.faction)) row.affiliated_players.push_back(owner.player.player_id);
        std::sort(row.affiliated_players.begin(), row.affiliated_players.end());
        production.technology_types.push_back(std::move(row));
    }
    std::sort(production.technology_types.begin(), production.technology_types.end(),
        [](const auto& a, const auto& b) { return a.type < b.type; });
    std::set<gc::TypeId> direct_members;
    for (const auto& type : p.starting.types.types) {
        for (const auto& name : type.company_units) direct_members.insert(p.identity(name));
        for (const auto& name : type.squadron_units) direct_members.insert(p.identity(name));
    }
    for (std::size_t i = 0; i < production.planets.size(); ++i) {
        auto& location = production.planets[i]; const auto& control = setup.control->planets[i]; const auto& state = setup.control->state[i];
        location.target_owner = state.target_player; location.starbase_owner = control.starbase.owner;
        location.ground_occupier = state.controller; location.orbit_occupier = state.orbital_player;
        location.ground_level = control.ground_base.level; location.space_level = control.starbase.level;
        location.landed_transports = static_cast<std::int64_t>(control.landed.size());
        const auto marked = location.offers; location.offers.clear();
        for (const auto& binding : p.starting.bindings) {
            const auto* type = p.starting.types.find(binding.original_type);
            if (!type) return refuse<gc::ProductionSetup>("missing production original type");
            if (type->build_cost_credits.value_or(Fixed{}).raw() <= 0 && !type->build_requires_initial_placement.value_or(false)) continue;
            auto built = offer(p, plan, setup, location, binding, *type);
            if (!built) return core::Result<gc::ProductionSetup>::failure(built.error());
            auto value = std::move(built).value();
            if (direct_members.contains(binding.id) || behaves(*type, "TRANSPORT")) value.direct_build_allowed = false; // WGEP-20.
            const auto before = std::find_if(marked.begin(), marked.end(), [&](const auto& m) { return m.type == binding.id; });
            value.initially_placed = before != marked.end() && before->initially_placed;
            location.offers.push_back(std::move(value));
        }
    }
    for (auto& player : production.players) {
        const auto economy = std::find_if(setup.player_economy->players.begin(), setup.player_economy->players.end(),
            [&](const auto& e) { return e.player == player.player; });
        if (economy == setup.player_economy->players.end()) return refuse<gc::ProductionSetup>("unbound production player economy");
        auto account = *economy;
        // WGS-13: startup current was assigned before its authored maximum;
        // replay that unlock pass without reclamping the retained current.
        account.maximum_tech = std::max(account.current_tech, account.maximum_tech);
        auto assigned = gc::assign_technology(account, player, production.technology_types, account.current_tech, true);
        if (!assigned) return core::Result<gc::ProductionSetup>::failure(assigned.error());
    }
    production.fleet_blockers.emplace();
    for (const auto& fleet : setup.fleets) {
        auto skips = fleet_skips(p, setup, fleet);
        if (!skips) return core::Result<gc::ProductionSetup>::failure(skips.error());
        production.fleet_blockers->push_back({fleet.id, false, skips.value(), true}); // WGEP-24 fresh fleet declaration.
    }
    return core::Result<gc::ProductionSetup>::success(std::move(production));
}
core::Result<CampaignPlacementContext> prepare_context(const vfs::Vfs& filesystem,
    Prepared& p, const CampaignSetupPlan& plan) {
    CampaignPlacementContext context;
    std::vector<MovementPlayerBinding> players;
    for (const auto& binding : plan.player_bindings()) players.push_back({binding.player.player_id, binding.faction});
    auto movement = prepare_initial_movement_inputs(p.starting, p.data, players);
    if (!movement) return core::Result<CampaignPlacementContext>::failure(movement.error());
    context.movement.types = movement.value().types; context.movement.players = movement.value().players;
    context.movement.mode_modifier = movement.value().base_mode_modifier;
    context.control.relations = p.relations.control;
    context.production.maximum_human_queue = 5; // WGEP-27 / WPR-30 fresh queue default.
    context.production.fleet_blockers.emplace();
    for (const auto& binding : plan.player_bindings()) {
        const auto* faction = p.data.faction(binding.faction);
        if (!faction) return refuse<CampaignPlacementContext>("missing faction " + binding.faction);
        context.control.players.push_back({binding.player.player_id, faction->is_neutral.value_or(false),
            faction->is_playable.value_or(false), binding.player.faction_id});
        context.census.players.push_back(prepare_census_player(*faction, binding.player.player_id, {}));
        if (context.census.players.back().ai_uses_cap)
            return refuse<CampaignPlacementContext>("campaign_ai_population_usability: explicit AI usability producer required");
        gc::ProductionPlayer production; production.player = binding.player.player_id;
        production.human = human(plan.galaxy(), production.player);
        production.pay_as_you_go = !production.human || p.pay_as_you_go;
        production.uses_population_cap = production.human || context.census.players.back().ai_uses_cap; // WGEP-50/69.
        context.production.players.push_back(std::move(production));
    }
    std::vector<ControlTypeBinding> control_types;
    for (std::size_t i = 0; i < p.starting.bindings.size(); ++i) {
        const auto& binding = p.starting.bindings[i]; const auto* type = p.starting.types.find(binding.original_type);
        if (!type) return refuse<CampaignPlacementContext>("unbound original type");
        auto census = prepare_census_type(*type, binding.id, p.starting.bindings);
        if (!census) return core::Result<CampaignPlacementContext>::failure(census.error());
        context.census.types.push_back(std::move(census).value());
        control_types.push_back({binding.id, binding.original_type, p.starting.runtime_types[i].skips_combat, false});
    }
    std::vector<ControlObjectBinding> control_planets, control_routes;
    gc::TypeId planet_type = plan.type_bindings().empty() ? 1 : plan.type_bindings().back().id + 1;
    for (const auto& location : plan.locations()) {
        if (!location.planet) continue;
        const auto* source = p.data.planet(location.original_type);
        if (!source) return refuse<CampaignPlacementContext>("missing planet " + location.original_type);
        const auto original = layers(p.catalog, source->name, data::Category::game_object);
        // Authored caps/alignment need their respective resolved faction lifetime
        // providers. Fresh zero Force_Strength does not create an alignment effect.
        auto strength = number(data::retail::single(original, "Force_Strength"), Fixed{});
        if (!strength || strength.value().raw() != 0 || data::retail::single(original, "Planet_Max_Political_Control_For_Faction"))
            return refuse<CampaignPlacementContext>("campaign_planet_alignment_and_caps: explicit authoring needs its owning producer");
        gc::ControlPlanet control; control.planet = location.planet->id; control.type = planet_type;
        control.surface_accessible = source->planet_surface_accessible.value_or(true); // WGS-92 constructor.
        control.retains_residual = source->planet_retains_residual_influence.value_or(true); // WGS-92 constructor.
        control.leaning = source->political_control.value_or(Fixed{});
        if (!source->political_faction.empty()) {
            const auto leaning = std::find_if(plan.player_bindings().begin(), plan.player_bindings().end(),
                [&](const auto& b) { return key(b.faction) == key(source->political_faction); });
            if (leaning == plan.player_bindings().end()) return refuse<CampaignPlacementContext>("unbound planet political faction");
            control.leaning_player = leaning->player.player_id;
        }
        auto reward = Fixed::from_integer(source->planet_capture_bonus_reward.value_or(0)); // WGS-90 constructor.
        if (!reward) return core::Result<CampaignPlacementContext>::failure(reward.error());
        control.capture_reward = reward.value();
        control.production_owner = {location.initial_owner, location.initial_owner};
        context.control.planets.push_back(control); control_planets.push_back({control.planet, source->name});
        gc::PlanetProduction production; production.planet = control.planet;
        production.target_owner = location.initial_owner;
        production.maximum_space_level = source->max_space_base.value_or(5); // WGS-90 constructor.
        production.ground_slots = source->special_structures_land.value_or(3); // WGS-90 constructor.
        production.space_slots = source->special_structures_space.value_or(3); // WGS-90 constructor.
        production.maximum_ground_forces = 0; // installed from the control constant below.
        production.enabled = true; production.surface_accessible = control.surface_accessible;
        context.production.planets.push_back(std::move(production));
        auto census = prepare_census_planet(*source, planet_type);
        if (!census) return core::Result<CampaignPlacementContext>::failure(census.error());
        context.census.types.push_back(std::move(census).value());
        context.location_originals.push_back({control.planet, planet_type, location.initial_owner, control.planet, 0, false, false, 0, 0, gc::ActorCombatRole::unavailable, {}, {}});
        if (!p.identities.emplace(key(source->name), planet_type++).second)
            return refuse<CampaignPlacementContext>("planet and ordinary original identities overlap");
        std::string model = source->galactic_model_name;
        if (model.empty()) return refuse<CampaignPlacementContext>("campaign_planet_model_bounds: missing model " + source->name);
        if (key(model).ends_with(".ALO")) model.resize(model.size() - 4);
        auto geometry = assets::load_model(filesystem, "data/art/models/" + model + ".alo");
        if (!geometry) return core::Result<CampaignPlacementContext>::failure(geometry.error());
        auto bounds = prepare_initial_movement_planet(control.planet, geometry.value(), source->scale_factor.value_or(one));
        if (!bounds) return core::Result<CampaignPlacementContext>::failure(bounds.error());
        context.movement.planets.push_back(bounds.value());
    }
    const auto* campaign = p.data.campaign(plan.campaign_name());
    if (!campaign || campaign->trade_routes.size() != plan.galaxy().routes.size())
        return refuse<CampaignPlacementContext>("unbound campaign route authoring");
    for (std::size_t i = 0; i < plan.galaxy().routes.size(); ++i) {
        const auto& route = plan.galaxy().routes[i];
        const auto* source = p.data.trade_route(campaign->trade_routes[i]); // WGS-01 authored graph order.
        if (!source || !source->political_control_gain) return refuse<CampaignPlacementContext>("missing route control input");
        context.control.routes.push_back({route.id, *source->political_control_gain});
        control_routes.push_back({route.id, source->name});
    }
    // Binding overlays authored values. Omission defaults are already resolved
    // above, so pass an immutable copy with those planet/route values installed.
    auto effective = p.data;
    for (auto& source : effective.planets) {
        source.political_control = source.political_control.value_or(Fixed{});
        source.planet_capture_bonus_reward = source.planet_capture_bonus_reward.value_or(0);
    }
    auto bound = bind_control_data(effective, p.starting.types, context.control, control_types, control_planets, control_routes);
    if (!bound) return core::Result<CampaignPlacementContext>::failure(bound.error());
    context.control = std::move(bound).value();
    auto ground = prepare_ground_capability_types(p.starting.types, p.starting.bindings);
    if (!ground) return core::Result<CampaignPlacementContext>::failure(ground.error());
    context.control.ground_capability_types = std::move(ground).value();
    for (auto& planet : context.production.planets) {
        planet.maximum_ground_forces = context.control.max_ground_forces;
        for (const auto& binding : p.starting.bindings) {
            const auto* type = p.starting.types.find(binding.original_type);
            if (!type) return refuse<CampaignPlacementContext>("missing original offer type");
            if (!type->build_requires_initial_placement.value_or(false)) continue;
            auto prepared = offer(p, plan, plan.galaxy(), planet, binding, *type);
            if (!prepared) return core::Result<CampaignPlacementContext>::failure(prepared.error());
            planet.offers.push_back(std::move(prepared).value());
        }
    }
    for (const auto& player : plan.galaxy().players) for (const auto& planet : plan.galaxy().planets)
        context.movement.access.push_back({player.player_id, planet.id, true, false}); // WGS-90 fresh restriction; conflict set after placement.
    return core::Result<CampaignPlacementContext>::success(std::move(context));
}
} // namespace
core::Result<void> validate_live_campaign_seats(const data::galactic::GalacticData& data,
    std::span<const CampaignPlayerBinding> players, std::span<const gc::Seat> seats) {
    for (const auto& seat : seats) if (seat.active && seat.kind == gc::SeatKind::human) {
        const auto binding = std::find_if(players.begin(), players.end(),
            [&](const auto& row) { return row.player.player_id == seat.player; });
        const auto* faction = binding == players.end() ? nullptr : data.faction(binding->faction);
        if (!faction || !faction->is_playable.value_or(false))
            return refuse<void>("active human seat requires a playable faction");
    }
    return core::Result<void>::success();
}
core::Result<CampaignSetupServices> prepare_live_campaign_services(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, std::string_view difficulty) {
    auto prepared = std::make_shared<Prepared>(); auto& p = *prepared;
    p.catalog = catalog; p.data = data;
    if (auto seats = validate_live_campaign_seats(data, plan.player_bindings(), plan.galaxy().seats); !seats)
        return core::Result<CampaignSetupServices>::failure(seats.error());
    // Omission defaults do not hide malformed authoring on consumed providers.
    const std::set<std::string> data_inputs{"HYPERSPACE_SPEED_FACTOR", "BENEFITS_FROM_CORRUPTION",
        "MINIMUM_VISIBLE_BASE_LEVEL", "BASE_POPULATION_CAPACITY", "AI_USES_GALACTIC_POP_CAP",
        "PLANET_SURFACE_ACCESSIBLE", "PLANET_RETAINS_RESIDUAL_INFLUENCE", "POLITICAL_CONTROL",
        "PLANET_CAPTURE_BONUS_REWARD", "MAX_SPACE_BASE", "SPECIAL_STRUCTURES_LAND",
        "SPECIAL_STRUCTURES_SPACE", "SCALE_FACTOR", "POLITICAL_CONTROL_GAIN", "GALACTIC_INFLUENCE_RANGE"};
    for (const auto& issue : data.issues) if (issue.kind == data::galactic::IssueKind::bad_value
            && (data_inputs.contains(key(issue.tag)) || key(issue.tag).starts_with("ABILITIES/SYSTEM_SPY_ABILITY/"))) {
        const bool bound_player = std::any_of(plan.player_bindings().begin(), plan.player_bindings().end(),
            [&](const auto& row) { return key(row.faction) == key(issue.owner); });
        const bool bound_planet = std::any_of(plan.locations().begin(), plan.locations().end(),
            [&](const auto& row) { return row.planet && key(row.original_type) == key(issue.owner); });
        const auto* selected = data.campaign(plan.campaign_name());
        if (bound_player || bound_planet || (selected && contains(selected->trade_routes, issue.owner)))
            return refuse<CampaignSetupServices>("invalid authored " + issue.tag + " on " + issue.owner);
    }
    for (auto& faction : p.data.factions) {
        faction.hyperspace_speed_factor = faction.hyperspace_speed_factor.value_or(one);
        faction.benefits_from_corruption = faction.benefits_from_corruption.value_or(false);
        faction.minimum_visible_base_level = faction.minimum_visible_base_level.value_or(0);
    }
    if (!plan.fiscal() || !plan.local_observer()) return refuse<CampaignSetupServices>("live root requires fiscal policy and a local observer");
    auto relations = prepare_initial_campaign_relations(data, plan.player_bindings());
    if (!relations) return core::Result<CampaignSetupServices>::failure(relations.error());
    p.relations = std::move(relations).value();
    for (const auto& binding : plan.type_bindings())
        if (!p.identities.emplace(key(binding.original_type), binding.id).second)
            return refuse<CampaignSetupServices>("duplicate ordinary original identity");
    // WGS-80's dispatcher looks up these two original names explicitly.
    // Bind the reachable startup/production graph, retaining the plan's explicit
    // original IDs. Tactical-only scene companies without strategic carriers are
    // not inputs to a GC census merely because they share an XML behavior token.
    std::set<gc::TypeId> wanted{p.identity("Galactic_Fleet"), p.identity("Galactic_Squadron")};
    for (const auto& force : plan.forces()) wanted.insert(force.type);
    for (const auto& placement : plan.initial_placements()) wanted.insert(placement.type);
    for (const auto& binding : plan.type_bindings()) {
        const auto* type = types.find(binding.original_type);
        if (type && ((type->build_cost_credits && type->build_cost_credits->raw() > 0)
                || type->build_requires_initial_placement.value_or(false))) wanted.insert(binding.id);
    }
    // Corruption's authored prerequisite alternatives are canonical original
    // types even when their current owned count is zero. Keep the exact plan IDs.
    for (const auto& location : plan.locations()) if (location.planet) {
        const auto source = layers(catalog, location.original_type, data::Category::game_object);
        for (unsigned slot = 1; slot <= 3; ++slot) {
            const auto prefix = "Corruption_" + std::to_string(slot) + "_";
            if (const auto* node = data::retail::single(source, prefix + "Required_Unit_Type")) {
                if (!data::retail::is_placeholder(node->raw_text)) for (const auto& name : words(node->raw_text)) {
                    const auto id = p.identity(name); if (!id) return refuse<CampaignSetupServices>("unbound corruption prerequisite " + name);
                    wanted.insert(id);
                }
            }
        }
    }
    std::vector<gc::TypeId> pending(wanted.begin(), wanted.end());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto binding = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
            [&](const auto& b) { return b.id == pending[i]; });
        if (binding == plan.type_bindings().end()) return refuse<CampaignSetupServices>("unbound reachable original");
        const auto* type = types.find(binding->original_type);
        if (!type) return refuse<CampaignSetupServices>("missing reachable original");
        auto names = type->company_units;
        names.insert(names.end(), type->squadron_units.begin(), type->squadron_units.end());
        names.insert(names.end(), type->required_orbiting_units.begin(), type->required_orbiting_units.end());
        if (!type->company_transport.empty()) names.push_back(type->company_transport);
        if (!type->additional_creation_type.empty()) names.push_back(type->additional_creation_type);
        for (const auto& name : words(type->required_special_structures)) if (name != "|" && key(name) != "OR") names.push_back(name);
        for (const auto& name : names) {
            const auto id = p.identity(name); if (!id) return refuse<CampaignSetupServices>("unbound reachable dependency " + name);
            if (wanted.insert(id).second) pending.push_back(id);
        }
    }
    std::vector<FleetTypeBinding> live_bindings;
    const std::set<std::string> type_inputs{"BUILD_COST_CREDITS", "BUILD_TIME_SECONDS", "TECH_LEVEL",
        "REQUIRED_GROUND_BASE_LEVEL", "REQUIRED_STAR_BASE_LEVEL", "BASE_LEVEL", "POPULATION_VALUE",
        "BUILD_LIMIT_CURRENT_PER_PLAYER", "BUILD_LIMIT_LIFETIME_PER_PLAYER", "BUILD_INITIALLY_LOCKED",
        "BUILD_REQUIRES_INITIAL_PLACEMENT", "BUILD_TIME_REDUCED_BY_MULTIPLE_FACTORIES",
        "BUILD_ADVANCES_TECH_LEVEL", "HYPERSPACE", "ALWAYS_SPAWN_IN_ORBIT", "CAN_HYPERSPACE_WITHOUT_ACTIVATING_ABILITY"};
    for (const auto& issue : types.issues) if (issue.kind == data::galactic::IssueKind::bad_value
            && type_inputs.contains(key(issue.tag)) && (wanted.contains(p.identity(issue.owner))
                || key(issue.tag) == "BUILD_COST_CREDITS" || key(issue.tag) == "TECH_LEVEL"
                || key(issue.tag) == "BUILD_INITIALLY_LOCKED"))
        return refuse<CampaignSetupServices>("invalid authored " + issue.tag + " on " + issue.owner);
    for (const auto& binding : plan.type_bindings()) if (wanted.contains(binding.id)) live_bindings.push_back(binding);
    auto starting = prepare_initial_starting_force_inputs(types, live_bindings, p.relations.placement,
        p.identity("Galactic_Fleet"), p.identity("Galactic_Squadron"));
    if (!starting) return core::Result<CampaignSetupServices>::failure(starting.error());
    p.starting = std::move(starting).value();
    // campaign_political_protection_sources (#2359): source identity and removal
    // belong to the protection lifecycle, never to a sticky startup Boolean.
    for (const auto& binding : p.starting.bindings) {
        const auto original = layers(p.catalog, binding.original_type, data::Category::game_object);
        for (const auto* block : data::retail::list(original, "Abilities", data::retail::ListRule::replace_in_variant))
            for (const auto& ability : block->children) if (key(ability.name) == "POLITICAL_CONTROL_PROTECTION_ABILITY")
                return refuse<CampaignSetupServices>("campaign_political_protection_sources: source lifecycle provider required");
    }
    auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!constants) return core::Result<CampaignSetupServices>::failure(constants.error());
    const auto constant = [&](std::string_view name) -> const data::XmlNode* {
        const auto at = std::find_if(constants.value().root.children.begin(), constants.value().root.children.end(),
            [&](const auto& node) { return key(node.name) == key(name); });
        return at == constants.value().root.children.end() ? nullptr : &*at;
    };
    auto speed = number(constant("Production_Speed_Factor"));
    if (!speed || speed.value().raw() < 0) return refuse<CampaignSetupServices>("invalid production speed factor");
    p.production_speed = speed.value();
    for (std::size_t i = 0; i < p.base_time.size(); ++i) {
        auto factor = number(constant("Production_Speed_Mod_Base_Vs_Tech_" + std::to_string(i)));
        if (!factor || factor.value().raw() < 0) return refuse<CampaignSetupServices>("invalid base-versus-tech time factor");
        p.base_time[i] = factor.value();
    }
    const auto* pay = constant("Pay_As_You_Go");
    if (!pay) return refuse<CampaignSetupServices>("missing Pay_As_You_Go");
    data::tag_trace::used(pay); const auto pay_flag = data::retail::boolean(pay->raw_text);
    if (!pay_flag) return refuse<CampaignSetupServices>("invalid Pay_As_You_Go");
    p.pay_as_you_go = *pay_flag;
    auto adjustments = data::load_document(filesystem, "data/xml/difficultyadjustments.xml");
    if (!adjustments) return core::Result<CampaignSetupServices>::failure(adjustments.error());
    std::optional<Fixed> ai_time;
    for (const auto& adjustment : adjustments.value().root.children) {
        const auto name = std::find_if(adjustment.attributes.begin(), adjustment.attributes.end(),
            [](const auto& attribute) { return key(attribute.name) == "NAME"; });
        if (name == adjustment.attributes.end() || key(name->value) != key(difficulty)) continue;
        for (const auto& node : adjustment.children) if (key(node.name) == "GALACTIC_BUILD_TIME_MULTIPLIER") {
            auto value = number(&node); if (!value) return core::Result<CampaignSetupServices>::failure(value.error());
            ai_time = value.value();
        }
    }
    if (!ai_time || ai_time->raw() < 0) return refuse<CampaignSetupServices>("missing selected AI build-time multiplier");
    p.ai_time = *ai_time;
    auto context = prepare_context(filesystem, p, plan);
    if (!context) return core::Result<CampaignSetupServices>::failure(context.error());
    p.context = std::move(context).value();
    p.economics = units::galactic_economic_table(catalog);
    p.constants = std::move(constants).value();
    auto market = data::load_document(filesystem, "data/xml/blackmarketitems.xml");
    if (!market) return core::Result<CampaignSetupServices>::failure(market.error());
    p.market = std::move(market).value();
    const std::shared_ptr<const Prepared> owned = prepared;
    CampaignSetupServices services;
    services.stock_starting_forces = std::make_shared<const StartingForceInputs>(p.starting);
    services.placement_context = [owned](const auto&) { return core::Result<CampaignPlacementContext>::success(owned->context); };
    services.location = [owned](const CampaignLocation& location) {
        if (!owned->catalog.find(location.original_type, data::Category::game_object))
            return refuse<void>("unbound scene location " + location.original_type);
        return core::Result<void>::success();
    };
    services.credit_cap = [per_planet = plan.fiscal()->credit_cap_per_planet](const gc::GalacticPlayer& player, const gc::GalacticSetup& setup) {
        const auto count = static_cast<std::int64_t>(std::count_if(setup.planets.begin(), setup.planets.end(),
            [&](const auto& planet) { return planet.owner == player.player_id; }));
        Fixed cap; const auto planets = Fixed::from_integer(count);
        if (!planets || !sim::math::try_multiply(planets.value(), per_planet, cap))
            return refuse<Fixed>("credit-cap multiplication overflow");
        return core::Result<Fixed>::success(cap);
    };
    // Root retains home records; setup does not publish presentation/Lua pointers.
    services.home = [](const CampaignHome&) { return core::Result<void>::success(); };
    services.assign_tech = [](const CampaignTech& tech, const gc::GalacticSetup&) {
        return core::Result<std::int64_t>::success(std::clamp<std::int64_t>(tech.requested_current, 0, 5)); // WGS-13/73 prior fresh max.
    };
    services.stock_production = [owned](const auto& input, const auto& setup) { return complete_production(*owned, input, setup); };
    services.stock_access = [owned](const CampaignSetupPlan&, const gc::GalacticSetup& setup) {
        using Rows = std::vector<gc::MovementAccess>; Rows result;
        if (!setup.control) return refuse<Rows>("missing complete conflict presence");
        const auto enemy = [&](gc::PlayerId a, gc::PlayerId b) {
            return std::any_of(owned->relations.placement.begin(), owned->relations.placement.end(),
                [&](const auto& r) { return r.player == a && r.other == b && r.enemy; });
        };
        for (const auto& player : setup.players) for (const auto& planet : setup.control->planets) {
            bool conflict = planet.starbase.level != 0 && enemy(player.player_id, planet.starbase.owner);
            for (const auto& fleet : setup.fleets) if (fleet.orbit == planet.planet && enemy(player.player_id, fleet.owner)) {
                auto skips = fleet_skips(*owned, setup, fleet);
                if (!skips) return core::Result<Rows>::failure(skips.error());
                conflict = conflict || !skips.value();
            }
            result.push_back({player.player_id, planet.planet, true, conflict});
        }
        return core::Result<Rows>::success(std::move(result));
    };
    services.economy_corruption = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        return prepare_corruption_economy(owned->constants, owned->market, owned->catalog, input, setup);
    };
    services.ability_host = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::GalacticAbilitySetup result;
        const auto at = std::find_if(owned->constants.root.children.begin(), owned->constants.root.children.end(),
            [](const auto& n) { return key(n.name) == "DEFAULT_HERO_RESPAWN_TIME"; });
        auto seconds = number(at == owned->constants.root.children.end() ? nullptr : &*at);
        if (!seconds || seconds.value().raw() <= 0) return refuse<gc::GalacticAbilitySetup>("missing default hero absence");
        result.default_hero_absence = seconds.value();
        const auto routing = std::find_if(owned->constants.root.children.begin(), owned->constants.root.children.end(),
            [](const auto& n) { return key(n.name) == "ACTIVATED_CORRUPT_PLANET_ABILITY_NAMES"; });
        if (routing != owned->constants.root.children.end()) {
            data::tag_trace::used(&*routing);
            for (const auto& name : words(routing->raw_text)) result.activated_corruption_abilities.push_back(key(name));
        }
        // WGCH-109/110/139: authored hack constants. An unresolved required type never matches (no fallback).
        const auto constant = [&](std::string_view name) -> const data::XmlNode* {
            const auto found = std::find_if(owned->constants.root.children.begin(), owned->constants.root.children.end(),
                [&](const auto& n) { return key(n.name) == name; });
            return found == owned->constants.root.children.end() ? nullptr : &*found;
        };
        if (const auto* node = constant("ACTIVATED_HACK_SUPER_WEAPON_ABILITY_NAMES")) {
            data::tag_trace::used(node);
            for (const auto& name : words(node->raw_text)) result.activated_hack_abilities.push_back(key(name));
        }
        if (const auto* node = constant("ACTIVATED_DESTROY_PLANET_ABILITY_NAMES")) {
            data::tag_trace::used(node);
            for (const auto& name : words(node->raw_text)) result.activated_destruction_abilities.push_back(key(name));
        }
        if (const auto* node = constant("HACK_SUPER_WEAPON_REQUIRED_TYPE")) {
            data::tag_trace::used(node);
            const auto names = words(node->raw_text);
            if (!names.empty() && !data::retail::is_placeholder(node->raw_text)) result.hack_required_type = owned->identity(names.front());
        }
        if (const auto* node = constant("HACK_SUPER_WEAPON_COST")) {
            data::tag_trace::used(node);
            auto cost = number(node, Fixed{});
            if (!cost || cost.value().raw() < 0) return refuse<gc::GalacticAbilitySetup>("invalid hack cost");
            result.hack_cost = cost.value();
        }
        for (const auto& binding : input.type_bindings()) {
            if (!setup.census || std::none_of(setup.census->types.begin(), setup.census->types.end(),
                [&](const auto& row) { return row.type == binding.id; })) continue;
            const auto original = layers(owned->catalog, binding.original_type, data::Category::game_object);
            bool market = false; std::optional<bool> market_enabled{true}; // WGCH-170/WGS-88 common enabled default
            const data::XmlNode* slicer = nullptr;
            std::vector<const data::XmlNode*> corruptors, hackers, destroyers;
            for (const auto* block : data::retail::list(original, "Abilities", data::retail::ListRule::replace_in_variant))
                for (const auto& ability : block->children) {
                    if (key(ability.name) == "SLICER_ABILITY") slicer = &ability;
                    if (key(ability.name) == "CORRUPT_PLANET_ABILITY") corruptors.push_back(&ability);
                    if (key(ability.name) == "HACK_SUPER_WEAPON_ABILITY") hackers.push_back(&ability);
                    if (key(ability.name) == "PLANET_DESTRUCTION_ABILITY") destroyers.push_back(&ability);
                    if (key(ability.name) != "BLACK_MARKET_ABILITY") continue;
                    market = true;
                    for (const auto& value : ability.children) if (key(value.name) == "INITIALLY_ENABLED") {
                        data::tag_trace::used(&value); market_enabled = data::retail::boolean(value.raw_text);
                    }
                }
            const data::XmlNode* corruptor = nullptr; std::string corruption_name;
            for (const auto& name : result.activated_corruption_abilities) {
                for (const auto* ability : corruptors) for (const auto& attr : ability->attributes)
                    if (key(attr.name) == "NAME" && key(attr.value) == name) { corruptor = ability; corruption_name = name; break; }
                if (corruptor) break;
            }
            const data::XmlNode* hacker = nullptr; std::string hack_name;
            for (const auto& name : result.activated_hack_abilities) {
                for (const auto* ability : hackers) for (const auto& attr : ability->attributes)
                    if (key(attr.name) == "NAME" && key(attr.value) == name) { hacker = ability; hack_name = name; break; }
                if (hacker) break;
            }
            const data::XmlNode* destroyer = nullptr; std::string destroy_name;
            for (const auto& name : result.activated_destruction_abilities) {
                for (const auto* ability : destroyers) for (const auto& attr : ability->attributes)
                    if (key(attr.name) == "NAME" && key(attr.value) == name) { destroyer = ability; destroy_name = name; break; }
                if (destroyer) break;
            }
            const auto* original_metadata = input.original_type(binding.id);
            if (!market && !slicer && !corruptor && !hacker && !destroyer && !(original_metadata && original_metadata->named_hero.value_or(false))) continue;
            gc::GalacticAbilityProfile profile; profile.type = binding.id; profile.has_market = market;
            profile.lifecycle_only = !market && !slicer && !corruptor && !hacker && !destroyer;
            if (destroyer) {
                profile.destruction = gc::DestructionAbilityProfile{destroy_name, true}; // WGCH-170: common constructor default
                for (const auto& value : destroyer->children) if (key(value.name) == "INITIALLY_ENABLED") {
                    data::tag_trace::used(&value); const auto flag = data::retail::boolean(value.raw_text);
                    if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid destruction enabled flag");
                    profile.destruction->initially_enabled = *flag;
                }
            }
            if (hacker) {
                profile.hack = gc::HackAbilityProfile{hack_name, true}; // WGCH-170: common constructor default
                for (const auto& value : hacker->children) if (key(value.name) == "INITIALLY_ENABLED") {
                    data::tag_trace::used(&value); const auto flag = data::retail::boolean(value.raw_text);
                    if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid hack enabled flag");
                    profile.hack->initially_enabled = *flag;
                }
            }
            if (corruptor) {
                profile.corruption = gc::CorruptionAbilityProfile{corruption_name, true};
                for (const auto& value : corruptor->children) if (key(value.name) == "INITIALLY_ENABLED") {
                    data::tag_trace::used(&value); const auto flag = data::retail::boolean(value.raw_text);
                    if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid corruption enabled flag");
                    profile.corruption->initially_enabled = *flag;
                }
            }
            if (market) {
                if (!market_enabled) return refuse<gc::GalacticAbilitySetup>("missing black-market enabled fact");
                auto buyer = prepare_black_market_buyer(owned->catalog, binding.original_type, 0, 0, false);
                if (!buyer) return core::Result<gc::GalacticAbilitySetup>::failure(buyer.error());
                profile.market = std::move(buyer).value(); profile.initially_enabled = *market_enabled;
            }
            if (slicer) {
                auto& theft = profile.theft.emplace();
                const auto child = [&](std::string_view name) -> const data::XmlNode* {
                    const data::XmlNode* result_node = nullptr;
                    for (const auto& value : slicer->children) if (key(value.name) == key(name)) result_node = &value;
                    return result_node;
                };
                const auto load_curve = [&](std::string_view name, std::vector<gc::AbilityCurvePoint>& points) {
                    const auto* node = child(name);
                    if (!node) return true; // WGCH-169: empty authored evaluator returns zero
                    data::tag_trace::used(node); auto parsed = gc::parse_income_curve(node->raw_text);
                    if (!parsed) return false;
                    points = std::move(parsed).value(); return true;
                };
                if (!load_curve("Cost_Multiplier_By_Planet_Tech_Availability", theft.availability)
                    || !load_curve("Chance_Modifier_By_Base_Level", theft.ground)
                    || !load_curve("Chance_Modifier_By_Planet_Difficulty", theft.difficulty))
                    return refuse<gc::GalacticAbilitySetup>("missing or invalid theft curve");
                theft.space = theft.ground;
                auto price = number(child("Cost_Affinity_Modifier"), Fixed{}), chance = number(child("Chance_Affinity_Modifier"), Fixed{}); // WGCH-170
                if (!price || !chance) return refuse<gc::GalacticAbilitySetup>("missing theft affinity modifiers");
                theft.affinity_price = price.value(); theft.affinity_chance = chance.value();
                const auto* enabled = child("Initially_Enabled");
                theft.initially_enabled = true; // WGCH-170/WGS-88: shared constructor default
                if (enabled) {
                    data::tag_trace::used(enabled); const auto flag = data::retail::boolean(enabled->raw_text);
                    if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid theft enabled flag");
                    theft.initially_enabled = *flag;
                }
                const auto load_ranges = [&](std::string_view name, std::vector<Fixed>& values) {
                    const auto* node = child(name); if (!node) return true;
                    data::tag_trace::used(node);
                    for (const auto& text : words(node->raw_text)) {
                        auto value = Fixed::from_decimal(text); if (!value) return false; values.push_back(value.value());
                    }
                    return values.empty() || values.size() == 5;
                };
                if (!load_ranges("Min_Respawn_Time_Per_Tech_Level", theft.absence_min)
                    || !load_ranges("Max_Respawn_Time_Per_Tech_Level", theft.absence_max)
                    || theft.absence_min.size() != theft.absence_max.size()) return refuse<gc::GalacticAbilitySetup>("invalid theft absence arrays");
                for (std::size_t i = 0; i < theft.absence_min.size(); ++i)
                    if (theft.absence_min[i] < Fixed{} || theft.absence_min[i] > theft.absence_max[i]) return refuse<gc::GalacticAbilitySetup>("invalid theft absence range");
            }
            const auto* type = input.original_type(binding.id);
            if (!type) return refuse<gc::GalacticAbilitySetup>("unbound ability original type");
            profile.named = type->named_hero.value_or(false);
            if (const auto* value = data::retail::single(original, "Respawn_Whole_Team_When_Killed")) {
                data::tag_trace::used(value); const auto flag = data::retail::boolean(value->raw_text);
                if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid whole-team respawn flag");
                profile.respawn_whole_team = *flag;
            }
            if (const auto* value = data::retail::single(original, "Respawn_Tied_To_Structure")) {
                data::tag_trace::used(value); auto name = words(value->raw_text);
                if (!name.empty() && !data::retail::is_placeholder(value->raw_text)) {
                    profile.respawn_structure = owned->identity(name.front());
                    if (!profile.respawn_structure) return refuse<gc::GalacticAbilitySetup>("unbound hero respawn structure");
                }
            }
            result.profiles.push_back(std::move(profile));
        }
        std::sort(result.profiles.begin(), result.profiles.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
        const bool has_theft = std::any_of(result.profiles.begin(), result.profiles.end(), [](const auto& p) { return p.theft.has_value(); });
        if (has_theft) {
            if (!setup.production) return refuse<gc::GalacticAbilitySetup>("theft requires registered production types");
            // WGCH-54: preserve the active XML registry order independently of
            // the caller's arbitrary numeric identity order.
            for (const auto& definition : owned->catalog.definitions()) {
                if (definition.category != data::Category::game_object || !definition.active || !definition.namespace_winner) continue;
                const auto type_id = owned->identity(definition.id);
                if (!type_id) continue;
                const auto registered = std::lower_bound(setup.production->technology_types.begin(), setup.production->technology_types.end(), type_id,
                    [](const auto& t, auto id) { return t.type < id; });
                if (registered == setup.production->technology_types.end() || registered->type != type_id || !registered->slicer_unlockable) continue;
                const auto original = layers(owned->catalog, definition.id, data::Category::game_object);
                auto cost = number(data::retail::single(original, "Slice_Cost_Credits"));
                const auto* type = input.original_type(type_id);
                if (!cost || !type) return refuse<gc::GalacticAbilitySetup>("missing registered slice cost/type");
                const bool ground = units::built_on_ground(*type);
                result.technologies.push_back({type_id, cost.value(), !ground});
            }
            for (const auto& planet : setup.planets) {
                const auto* source = planet_source(*owned, input, planet.id);
                if (!source) return refuse<gc::GalacticAbilitySetup>("missing theft planet");
                const auto original = layers(owned->catalog, source->name, data::Category::game_object);
                auto availability = number(data::retail::single(original, "Planet_Tech_Availability_Rating"));
                auto difficulty = number(data::retail::single(original, "Planet_Slice_Difficulty_Rating"));
                if (!availability || !difficulty) return refuse<gc::GalacticAbilitySetup>("missing theft planet ratings");
                gc::TheftPlanet row; row.planet = planet.id; row.availability = availability.value(); row.difficulty = difficulty.value();
                if (const auto* node = data::retail::single(original, "Planet_Auto_Capture_Minor_Heroes")) {
                    data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
                    if (!flag) { return refuse<gc::GalacticAbilitySetup>("invalid auto-capture flag"); }
                    row.auto_capture_minor = *flag;
                }
                for (const auto* node : data::retail::list(original, "Planet_Slice_Affinity_Types", data::retail::ListRule::replace_in_variant)) {
                    data::tag_trace::used(node);
                    if (!data::retail::is_placeholder(node->raw_text)) for (const auto& name : words(node->raw_text)) {
                        const auto id = owned->identity(name); if (!id) return refuse<gc::GalacticAbilitySetup>("unbound slice affinity type"); row.affinity_types.push_back(id);
                    }
                }
                std::sort(row.affinity_types.begin(), row.affinity_types.end());
                row.affinity_types.erase(std::unique(row.affinity_types.begin(), row.affinity_types.end()), row.affinity_types.end());
                result.theft_planets.push_back(std::move(row));
            }
        }
        // WGCH-143/176: the ruined base income is the authored destroyed value; an omitted value retains the
        // ordinary base (unverified constructor default, least visible) and is never invented here.
        for (const auto& planet : setup.planets) {
            const auto* source = planet_source(*owned, input, planet.id);
            if (!source || !source->planet_destroyed_credit_value) continue;
            if (source->planet_destroyed_credit_value->raw() < 0) return refuse<gc::GalacticAbilitySetup>("invalid destroyed planet income");
            result.ruined_income.push_back({planet.id, *source->planet_destroyed_credit_value});
        }
        auto corruption = prepare_corruption_host(owned->catalog, input, setup, result,
            [owned](std::string_view name) { return owned->identity(name); });
        if (!corruption) return core::Result<gc::GalacticAbilitySetup>::failure(corruption.error());
        return core::Result<gc::GalacticAbilitySetup>::success(std::move(result));
    };
    services.income = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::IncomeSetup result;
        if (!setup.census || !setup.production || !setup.movement)
            return refuse<gc::IncomeSetup>("income requires original owned census, movement contents and production");
        std::vector<EconomicTypeBinding> bindings;
        for (const auto& [name, id] : owned->identities) bindings.push_back({name, id});
        std::map<gc::TypeId, const units::GalacticEconomicType*> profiles;
        for (const auto& type : owned->economics.types) {
            const auto id = owned->identity(type.name);
            // WGEP-163: a halt-only profile is retained for the writer but is not a fiscal source.
            const bool income_source = type.mining || type.gambling || !type.reductions.empty() || !type.technology_prices.empty();
            if (!id || (!income_source && !type.credit_halt.value_or(false))) continue;
            const auto object = std::find_if(setup.census->objects.begin(), setup.census->objects.end(),
                [&](const auto& row) { return row.original_type == id; });
            const auto* owner = object == setup.census->objects.end() ? nullptr : player_source(input, object->owner);
            auto economic = prepare_economic_type(type, id, owner ? owner->faction : std::string_view{}, owned->starting.types, bindings);
            if (!economic) return core::Result<gc::IncomeSetup>::failure(economic.error());
            if (income_source) profiles.emplace(id, &type);
            result.types.push_back(std::move(economic).value());
        }
        std::sort(result.types.begin(), result.types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
        for (const auto& planet : setup.planets) {
            const auto* source = planet_source(*owned, input, planet.id);
            if (!source) return refuse<gc::IncomeSetup>("missing income planet source");
            const auto economic = std::find_if(owned->economics.types.begin(), owned->economics.types.end(),
                [&](const auto& row) { return key(row.name) == key(source->name); });
            if (economic == owned->economics.types.end()) return refuse<gc::IncomeSetup>("missing economic planet metadata");
            auto prepared_planet = prepare_economic_planet(*economic, planet.id, false, false);
            if (!prepared_planet) return core::Result<gc::IncomeSetup>::failure(prepared_planet.error());
            result.planets.push_back(prepared_planet.value());
        }
        for (const auto& object : setup.census->objects) if (profiles.contains(object.original_type))
            result.sources.push_back({object.id, object.original_type, object.planet, object.owner, object.id == object.planet, true});
        for (const auto& planet : setup.production->planets) for (const auto& value : planet.offers) {
            const auto binding = std::find_if(owned->starting.bindings.begin(), owned->starting.bindings.end(),
                [&](const auto& b) { return b.id == value.type; });
            if (binding == owned->starting.bindings.end()) return refuse<gc::IncomeSetup>("unbound income offer");
            const auto* type = owned->starting.types.find(binding->original_type); Fixed price;
            auto basis = offer(*owned, input, setup, planet, *binding, *type, &price);
            if (!basis) return core::Result<gc::IncomeSetup>::failure(basis.error());
            result.offers.push_back({planet.planet, value.type, price, basis.value().seconds});
        }
        return core::Result<gc::IncomeSetup>::success(std::move(result));
    };
    services.observer = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::ObserverSetup result;
        if (!setup.control) return refuse<gc::ObserverSetup>("observer requires complete control presence");
        for (const auto& binding : input.player_bindings()) {
            const auto* faction = owned->data.faction(binding.faction);
            if (!faction) return refuse<gc::ObserverSetup>("missing observer faction");
            gc::ObserverPlayer player; player.player = binding.player.player_id;
            player.ai = !human(setup, player.player);
            for (const auto& relation : owned->relations.control) if (relation.player == player.player && relation.allied)
                player.allies.push_back(relation.other);
            result.players.push_back(prepare_observer_player(*faction, std::move(player)));
        }
        for (std::size_t i = 0; i < setup.control->planets.size(); ++i) {
            const auto& presence = setup.control->planets[i]; const auto& state = setup.control->state[i];
            const auto* source = planet_source(*owned, input, presence.planet);
            if (!source) return refuse<gc::ObserverSetup>("missing observer planet");
            gc::ObserverPlanet planet; planet.planet = presence.planet;
            planet.allegiance = state.current_player; planet.target_owner = state.target_player;
            planet.current_owner = state.current_player; planet.current_controller = state.controller;
            planet.space_owner = presence.starbase.owner; planet.ground_owner = presence.ground_base.owner;
            planet.space_level = presence.starbase.level; planet.ground_level = presence.ground_base.level;
            planet.political_control = state.current;
            for (const auto& transport : presence.landed) planet.landed_transports.push_back({transport.id, transport.owner, false, false});
            result.planets.push_back(prepare_observer_planet(*source, std::move(planet)));
            for (std::size_t ability = 0; ability < source->system_spies.size(); ++ability) {
                const auto& spy = source->system_spies[ability];
                const auto style = key(spy.activation_style) == "GALACTIC_AUTOMATIC" ? gc::SpyStyle::galactic_automatic
                    : key(spy.activation_style) == "GROUND_ACTIVATED" ? gc::SpyStyle::ground_activated : gc::SpyStyle::disabled;
                result.planet_spies.push_back({presence.planet, ability + 1, source->name, spy.mask,
                    spy.duration, spy.influence_range, style, spy.initially_enabled, spy.causes_despawn});
            }
        }
        for (std::size_t i = 0; i < owned->starting.bindings.size(); ++i) {
            const auto& binding = owned->starting.bindings[i]; const auto* type = owned->starting.types.find(binding.original_type);
            const bool stealth = owned->starting.runtime_types[i].skips_combat;
            const auto original = layers(owned->catalog, type->name, data::Category::game_object);
            bool generic = false; // WGUI-18 fresh generic-hero flag.
            if (const auto* node = data::retail::single(original, "Is_Generic_Hero")) {
                data::tag_trace::used(node); const auto value = data::retail::boolean(node->raw_text);
                if (!value) return refuse<gc::ObserverSetup>("invalid generic-hero flag");
                generic = *value;
            }
            result.types.push_back({binding.id, stealth, stealth && (generic || type->named_hero.value_or(false)), contains(type->category_mask, "Super")});
        }
        return core::Result<gc::ObserverSetup>::success(std::move(result));
    };
    return core::Result<CampaignSetupServices>::success(std::move(services));
}
} // namespace eawr::campaign
