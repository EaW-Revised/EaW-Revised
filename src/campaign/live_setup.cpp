#include "eawr/campaign/live_setup.hpp"
#include "eawr/core/text.hpp"
#include "eawr/diagnostics/gc_memory.hpp"
#include "eawr/campaign/planet_economy.hpp"
#include "eawr/campaign/control.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/load_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/movement.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/observer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
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
} // namespace
core::Result<StructureRequirements> resolve_structure_requirements(const std::string_view authored,
    const std::function<sim::galactic::TypeId(std::string_view)>& identity) {
    StructureRequirements result;
    // The OR token links alternatives; an unlinked entry begins the next AND group. Positional (RGC-09): a null entry that
    // leads a chain is skipped and takes its OR token with it, so the entry after it leads the chain and is tested; a null
    // alternative inside a chain that a known type leads satisfies the whole chain.
    bool join = false, need_entry = false, dropped_lead = false, satisfied = false;
    const auto close = [&] {
        if (!result.groups.empty() && satisfied) result.groups.pop_back();
        satisfied = false;
    };
    for (const auto& name : words(std::string(authored))) {
        if (key(name) == "OR" || name == "|") {
            if (need_entry) return refuse<StructureRequirements>("invalid structure OR grammar");
            need_entry = true;
            if (dropped_lead) { dropped_lead = false; continue; }
            if (result.groups.empty()) return refuse<StructureRequirements>("invalid structure OR grammar");
            join = true;
            continue;
        }
        need_entry = false; dropped_lead = false;
        const auto id = identity(name);
        if (!join) {
            close();
            if (!id) { result.ignored.push_back(name); dropped_lead = true; continue; }
            result.groups.emplace_back();
        } else {
            join = false;
            if (!id) { result.ignored.push_back(name); satisfied = true; continue; }
        }
        result.groups.back().push_back(id);
    }
    if (need_entry) return refuse<StructureRequirements>("incomplete structure OR grammar");
    close();
    return core::Result<StructureRequirements>::success(std::move(result));
}
namespace {
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
    const auto value = data::retail::float_value(node->raw_text);
    if (!value) return refuse<Fixed>("invalid " + node->name);
    return core::Result<Fixed>::success(value->value);
}
struct Prepared {
    mutable core::LoadNoteBuffer load_buffer;
    mutable core::LoadNoteCollector load_collector;
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
    // Load-time observations, each reported once (CGS-07). They leave setup identity unchanged.
    mutable std::vector<core::Diagnostic> diagnostics;
    mutable std::set<std::string> noted;
    void note(const std::string& code, const std::string& message) const {
        if (!noted.insert(code + '|' + message).second) return;
        core::Diagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = message;
        diagnostics.push_back(std::move(diagnostic));
    }
    // Why a name has no identity: no such object, or an object with no galactic data bound for this campaign.
    std::string unresolved(const std::string& name) const {
        return catalog.find(name, data::Category::game_object) ? "type " + name + " (no galactic data)" : "unknown type " + name;
    }
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
    const bool orbital_structure = behaves(type, "DUMMY_ORBITAL_STRUCTURE");
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
    const auto* limit = data::retail::single(original, "Build_Max_Instances_Per_Planet");
    admission.planet_limit = limit ? data::retail::integer_constant(limit->raw_text) : -1; // LSA-06
    for (const auto& name : type.required_orbiting_units) {
        const auto id = p.identity(name);
        if (!id) {
            // CGS-07: a name that resolves to no object is a null reference; the eligibility check skips it.
            p.note("campaign_unresolved_prerequisite", type.name + ": Required_Orbiting_Units names " + p.unresolved(name)
                + "; the entry is ignored (CGS-07)");
            continue;
        }
        admission.required_orbiting_units.push_back(id);
    }
    auto structures = resolve_structure_requirements(type.required_special_structures,
        [&](const std::string_view name) { return p.identity(name); });
    if (!structures) return core::Result<gc::ProductionOffer>::failure(structures.error());
    for (const auto& name : structures.value().ignored)
        p.note("campaign_unresolved_prerequisite", type.name + ": Required_Special_Structures names " + p.unresolved(name)
            + "; the null entry follows the positional eligibility rule (CGS-07, RGC-09)");
    admission.required_structures = std::move(structures.value().groups);
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
        const auto value = data::retail::float_value(entries.front());
        if (!value) return refuse<Fixed>("invalid corruption factor");
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
    time = gc::production_product(adjusted.build_time_seconds.value_or(Fixed{}), p.production_speed);
    adjusted.build_time_seconds = time;
    if (unrounded_price) {
        *unrounded_price = gc::production_product(adjusted.build_cost_credits.value_or(Fixed{}),
            *quantities.corruption_price_multiplier);
    }
    return prepare_production_offer(adjusted, std::move(admission), quantities, &p.load_buffer);
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
        const auto authored_tech = row.tech;
        row.tech = std::clamp(row.tech, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
        if (authored_tech < 0 || row.tech != authored_tech) campaign_type_note(&p.load_buffer, "EAWR-LOAD-1001", *type,
            "Tech_Level", std::to_string(authored_tech), std::to_string(row.tech), "LSA-09: signed technology reaches availability comparison",
            core::LoadCategory::retail_accepts, core::LoadAction::accepted);
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
    std::vector<RaidFactionBinding> raid_factions;
    for (const auto& binding : plan.player_bindings()) raid_factions.push_back({binding.player.faction_id, binding.faction});
    auto raid = build_raid_inputs(p.starting.types, p.data, p.starting.bindings, raid_factions);
    if (!raid) return core::Result<CampaignPlacementContext>::failure(raid.error());
    context.movement.raid = std::move(raid).value();
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
        auto census = prepare_census_type(*type, binding.id, p.starting.bindings, &p.load_buffer);
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
        if (data::retail::single(original, "Planet_Max_Political_Control_For_Faction"))
            return refuse<CampaignPlacementContext>("campaign_planet_caps: explicit authoring needs its owning producer");
        const auto strength = number(data::retail::single(original, "Force_Strength"), Fixed{});
        if (!strength) return core::Result<CampaignPlacementContext>::failure(strength.error());
        if (strength.value().raw() != 0) {
            // CGS-16: fiscal profiles already resolve inherited planet and faction alignment.
            if (!plan.fiscal() || !plan.fiscal()->contributions)
                return refuse<CampaignPlacementContext>("missing planet control alignment profiles");
            const auto& income = *plan.fiscal()->contributions;
            const auto profile = std::find_if(income.planets.begin(), income.planets.end(),
                [&](const auto& row) { return row.planet == location.planet->id; });
            const auto* bonus = p.data.constant("MaxInfluenceTransitionAlignmentBonus");
            const auto* penalty = p.data.constant("MaxInfluenceTransitionAlignmentPenalty");
            if (profile == income.planets.end() || !bonus || !bonus->number || !penalty || !penalty->number)
                return refuse<CampaignPlacementContext>("missing planet control alignment inputs");
            for (const auto& faction : income.factions) {
                const auto player = std::find_if(context.control.players.begin(), context.control.players.end(),
                    [&](const auto& row) { return row.player == faction.player; });
                if (player == context.control.players.end()) return refuse<CampaignPlacementContext>("unbound alignment faction");
                if (player->neutral) continue; // RGC-17: faction identity, not its alignment enum.
                const bool like = faction.alignment == profile->alignment;
                auto multiplier = prepare_control_alignment_multiplier(strength.value(), like,
                    like ? *bonus->number : *penalty->number);
                if (!multiplier) return core::Result<CampaignPlacementContext>::failure(multiplier.error());
                if (multiplier.value() == one) continue;
                context.control.alignment_modifiers.push_back({location.planet->id, faction.player, multiplier.value()});
            }
        }
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
        // WGP-191: planets have resolved object data but are intentionally
        // absent from the ordinary starting-force type table.
        std::optional<Fixed> history_power;
        if (!original.empty()) {
            auto power = number(data::retail::single(original, "AI_Combat_Power"), Fixed{});
            if (!power) return core::Result<CampaignPlacementContext>::failure(power.error());
            history_power = power.value();
        }
        auto census = prepare_census_planet(*source, planet_type, history_power, &p.load_buffer);
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
    if (!campaign || plan.route_names().size() != plan.galaxy().routes.size())
        return refuse<CampaignPlacementContext>("unbound campaign route authoring");
    for (std::size_t i = 0; i < plan.galaxy().routes.size(); ++i) {
        const auto& route = plan.galaxy().routes[i];
        const auto* source = p.data.trade_route(plan.route_names()[i]); // WGS-01 authored graph order.
        if (!source) return refuse<CampaignPlacementContext>("missing route control input");
        if (!source->political_control_gain)
            p.note("campaign_route_control_default", source->name + ": omitted Political_Control_Gain; using 0");
        context.control.routes.push_back({route.id, source->political_control_gain.value_or(Fixed{})});
        control_routes.push_back({route.id, source->name});
    }
    // Binding overlays authored values. Omission defaults are already resolved
    // above, so pass an immutable copy with those planet/route values installed.
    auto effective = p.data;
    for (auto& source : effective.planets) {
        source.political_control = source.political_control.value_or(Fixed{});
        source.planet_capture_bonus_reward = source.planet_capture_bonus_reward.value_or(0);
    }
    for (auto& source : effective.trade_routes)
        source.political_control_gain = source.political_control_gain.value_or(Fixed{});
    auto bound = bind_control_data(effective, p.starting.types, context.control, control_types, control_planets, control_routes, &p.load_buffer);
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

core::Result<void> prepare_live_relations(Prepared& p, const data::galactic::GalacticData& data,
    const CampaignSetupPlan& plan) {
    if (auto seats = validate_live_campaign_seats(data, plan.player_bindings(), plan.galaxy().seats); !seats)
        return core::Result<void>::failure(seats.error());
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
            return refuse<void>("invalid authored " + issue.tag + " on " + issue.owner);
    }
    for (auto& faction : p.data.factions) {
        if (!faction.benefits_from_corruption && std::any_of(plan.player_bindings().begin(), plan.player_bindings().end(),
                [&](const auto& row) { return key(row.faction) == key(faction.name); }))
            p.note("campaign_omitted_faction_tag", faction.name
                + ": Benefits_From_Corruption is omitted; using the false constructor default (WGS-94)");
        faction.hyperspace_speed_factor = faction.hyperspace_speed_factor.value_or(one);
        faction.benefits_from_corruption = faction.benefits_from_corruption.value_or(false);
        faction.minimum_visible_base_level = faction.minimum_visible_base_level.value_or(0);
    }
    if (!plan.fiscal() || plan.human_observers().empty()) return refuse<void>("live root requires fiscal policy and human observers");
    auto relations = prepare_initial_campaign_relations(data, plan.player_bindings());
    if (!relations) return core::Result<void>::failure(relations.error());
    p.relations = std::move(relations).value();
    for (const auto& binding : plan.type_bindings())
        if (!p.identities.emplace(key(binding.original_type), binding.id).second)
            return refuse<void>("duplicate ordinary original identity");
    return core::Result<void>::success();
}

core::Result<void> prepare_live_starting_forces(Prepared& p, const data::Catalog& catalog,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, std::span<const std::string> story_units) {
    // WGS-80's dispatcher looks up these two original names explicitly.
    // Bind the reachable startup/production graph, retaining the plan's explicit
    // original IDs. Tactical-only scene companies without strategic carriers are
    // not inputs to a GC census merely because they share an XML behavior token.
    std::set<gc::TypeId> wanted{p.identity("Galactic_Fleet"), p.identity("Galactic_Squadron")};
    for (const auto& force : plan.forces()) wanted.insert(force.type);
    for (const auto& hero : plan.multiplayer_heroes()) wanted.insert(hero.original_type_id);
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
                    const auto id = p.identity(name);
                    if (!id) {
                        p.note("campaign_unresolved_prerequisite", location.original_type + ": " + prefix
                            + "Required_Unit_Type names " + p.unresolved(name) + "; it is not bound (CGS-07)");
                        continue;
                    }
                    wanted.insert(id);
                }
            }
        }
    }
    // Adds everything a set of types needs to be created: company members, squadron members, orbit prerequisites,
    // the transport, the additional creation and the special structures.
    const auto close = [&](std::set<gc::TypeId>& set) -> core::Result<void> {
        std::vector<gc::TypeId> pending(set.begin(), set.end());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto binding = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
                [&](const auto& b) { return b.id == pending[i]; });
            if (binding == plan.type_bindings().end()) return refuse<void>("unbound reachable original");
            const auto* type = types.find(binding->original_type);
            if (!type) return refuse<void>("missing reachable original");
            auto names = type->company_units;
            names.insert(names.end(), type->squadron_units.begin(), type->squadron_units.end());
            names.insert(names.end(), type->required_orbiting_units.begin(), type->required_orbiting_units.end());
            if (!type->company_transport.empty()) names.push_back(type->company_transport);
            if (!type->additional_creation_type.empty()) names.push_back(type->additional_creation_type);
            for (const auto& name : words(type->required_special_structures)) if (name != "|" && key(name) != "OR") names.push_back(name);
            for (const auto& name : names) {
                const auto id = p.identity(name);
                if (!id) {
                    p.note("campaign_unresolved_dependency", type->name + " depends on " + p.unresolved(name)
                        + "; it is not bound (CGS-07)");
                    continue;
                }
                if (set.insert(id).second) pending.push_back(id);
            }
        }
        return core::Result<void>::success();
    };
    if (auto closed = close(wanted); !closed) return closed;
    // CGS-08: a composite whose members cannot all be resolved is never created, so it takes no part in the census or
    // the production offers.
    for (auto at = wanted.begin(); at != wanted.end();) {
        const auto binding = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
            [&](const auto& b) { return b.id == *at; });
        if (binding != plan.type_bindings().end() && !campaign_type_creatable(types, binding->original_type)) {
            p.note("campaign_type_not_creatable", binding->original_type
                + " names a transport or member that resolves to no type; it is never created (CGS-08)");
            at = wanted.erase(at);
        } else ++at;
    }
    // #683: plot-created units join the creation inputs with their dependencies
    // (WST-40). Unbound rewards still decline by name when they run.
    for (const auto& name : story_units) {
        const auto id = p.identity(name);
        if (!id || wanted.contains(id)) continue;
        std::set<gc::TypeId> extra{id};
        if (!close(extra)) continue;
        wanted.insert(extra.begin(), extra.end());
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
        return refuse<void>("invalid authored " + issue.tag + " on " + issue.owner);
    for (const auto& binding : plan.type_bindings()) if (wanted.contains(binding.id)) live_bindings.push_back(binding);
    auto starting = prepare_initial_starting_force_inputs(types, live_bindings, p.relations.placement,
        p.identity("Galactic_Fleet"), p.identity("Galactic_Squadron"));
    if (!starting) return core::Result<void>::failure(starting.error());
    p.starting = std::move(starting).value();
    // GPC-01: original-type control abilities bind above and the canonical
    // placed actors supply their source-keyed presence at root construction.
    return core::Result<void>::success();
}

core::Result<void> prepare_live_resources(Prepared& p, const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const CampaignSetupPlan& plan, std::string_view difficulty) {
    auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!constants) return core::Result<void>::failure(constants.error());
    const auto constant = [&](std::string_view name) -> const data::XmlNode* {
        const auto at = std::find_if(constants.value().root.children.begin(), constants.value().root.children.end(),
            [&](const auto& node) { return key(node.name) == key(name); });
        return at == constants.value().root.children.end() ? nullptr : &*at;
    };
    auto speed = number(constant("Production_Speed_Factor"));
    if (!speed) return refuse<void>("invalid production speed factor");
    p.production_speed = speed.value();
    for (std::size_t i = 0; i < p.base_time.size(); ++i) {
        auto factor = number(constant("Production_Speed_Mod_Base_Vs_Tech_" + std::to_string(i)));
        if (!factor) return refuse<void>("invalid base-versus-tech time factor");
        p.base_time[i] = factor.value();
    }
    const auto* pay = constant("Pay_As_You_Go");
    if (!pay) return refuse<void>("missing Pay_As_You_Go");
    data::tag_trace::used(pay); const auto pay_flag = data::retail::boolean(pay->raw_text);
    if (!pay_flag) return refuse<void>("invalid Pay_As_You_Go");
    p.pay_as_you_go = *pay_flag;
    auto adjustments = data::load_document(filesystem, "data/xml/difficultyadjustments.xml");
    if (!adjustments) return core::Result<void>::failure(adjustments.error());
    std::optional<Fixed> ai_time;
    for (const auto& adjustment : adjustments.value().root.children) {
        const auto name = std::find_if(adjustment.attributes.begin(), adjustment.attributes.end(),
            [](const auto& attribute) { return key(attribute.name) == "NAME"; });
        if (name == adjustment.attributes.end() || key(name->value) != key(difficulty)) continue;
        for (const auto& node : adjustment.children) if (key(node.name) == "GALACTIC_BUILD_TIME_MULTIPLIER") {
            auto value = number(&node); if (!value) return core::Result<void>::failure(value.error());
            ai_time = value.value();
        }
    }
    if (!ai_time) return refuse<void>("missing selected AI build-time multiplier");
    p.ai_time = *ai_time;
    auto context = prepare_context(filesystem, p, plan);
    if (!context) return core::Result<void>::failure(context.error());
    p.context = std::move(context).value();
    p.economics = units::galactic_economic_table(catalog);
    p.constants = std::move(constants).value();
    auto market = data::load_document(filesystem, "data/xml/blackmarketitems.xml");
    if (!market) return core::Result<void>::failure(market.error());
    p.market = std::move(market).value();
    return core::Result<void>::success();
}

void install_live_placement_services(CampaignSetupServices& services,
    const std::shared_ptr<const Prepared>& owned, const CampaignSetupPlan& plan) {
    services.stock_starting_forces = std::make_shared<const StartingForceInputs>(owned->starting);
    services.placement_context = [owned](const auto&) { return core::Result<CampaignPlacementContext>::success(owned->context); };
    services.location = [](const CampaignLocation&) {
        // RGC-80/81: unresolved locations retain their authored slot, create nothing and do not stop setup.
        // The load note is collected before services are published; planet/force passes skip null slots.
        return core::Result<void>::success();
    };
    services.credit_cap = [owned, per_planet = plan.fiscal()->credit_cap_per_planet](const gc::GalacticPlayer& player, const gc::GalacticSetup& setup) {
        const auto count = static_cast<std::int64_t>(std::count_if(setup.planets.begin(), setup.planets.end(),
            [&](const auto& planet) { return planet.owner == player.player_id; }));
        Fixed cap; const auto planets = Fixed::from_integer(count);
        if (!planets || !sim::math::try_multiply(planets.value(), per_planet, cap)) {
            cap = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
            setup_load_note(owned->load_buffer, "EAWR-LOAD-1021", std::to_string(player.player_id),
                "Credit_Cap_Per_Planet", std::to_string(per_planet.raw()), std::to_string(cap.raw()));
        }
        return core::Result<Fixed>::success(cap);
    };
    // Root retains home records; setup does not publish presentation/Lua pointers.
    services.home = [](const CampaignHome&) { return core::Result<void>::success(); };
    // WGS-14 / WGS-01a: production behavior may come from either authored list.
    std::set<gc::PlanetId> producing_homes;
    for (const auto& home : plan.homes()) if (home.planet) {
        const auto* source = planet_source(*owned, plan, *home.planet);
        if (source && (contains(source->behaviors, "PRODUCTION") || contains(source->galactic_behaviors, "PRODUCTION")))
            producing_homes.insert(*home.planet);
    }
    services.home_has_production = [homes = std::move(producing_homes)](gc::PlanetId home) {
        return core::Result<bool>::success(homes.contains(home));
    };
    services.assign_tech = [](const CampaignTech& tech, const gc::GalacticSetup&) {
        return core::Result<std::int64_t>::success(std::clamp<std::int64_t>(tech.requested_current, 0, 5)); // WGS-13/73 prior fresh max.
    };
    services.stock_production = [owned](const auto& input, const auto& setup) { return complete_production(*owned, input, setup); };
}

void install_live_conflict_services(CampaignSetupServices& services,
    const std::shared_ptr<const Prepared>& owned) {
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
}

void install_live_ability_services(CampaignSetupServices& services,
    const std::shared_ptr<const Prepared>& owned) {
    services.economy_corruption = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        return prepare_corruption_economy(owned->constants, owned->market, owned->catalog, input, setup, &owned->load_buffer);
    };
    services.ability_host = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::GalacticAbilitySetup result;
        const auto at = std::find_if(owned->constants.root.children.begin(), owned->constants.root.children.end(),
            [](const auto& n) { return key(n.name) == "DEFAULT_HERO_RESPAWN_TIME"; });
        auto seconds = number(at == owned->constants.root.children.end() ? nullptr : &*at);
        if (!seconds) return refuse<gc::GalacticAbilitySetup>("missing default hero absence"); // CGS-12: any authored sign
        result.default_hero_absence = seconds.value();
        const auto routing = std::find_if(owned->constants.root.children.begin(), owned->constants.root.children.end(),
            [](const auto& n) { return key(n.name) == "ACTIVATED_CORRUPT_PLANET_ABILITY_NAMES"; });
        if (routing != owned->constants.root.children.end()) {
            data::tag_trace::used(&*routing);
            for (const auto& name : words(routing->raw_text)) result.activated_corruption_abilities.push_back(key(name));
        }
        for (const auto& node : owned->constants.root.children) if (key(node.name) == "ACTIVATED_SYSTEM_SPY_ABILITY_NAMES") {
            data::tag_trace::used(&node);
            for (const auto& name : words(node.raw_text)) result.activated_spy_abilities.push_back(key(name));
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
        if (const auto* node = constant("ACTIVATED_SABOTAGE_ABILITY_NAMES")) {
            data::tag_trace::used(node);
            for (const auto& name : words(node->raw_text)) result.activated_sabotage_abilities.push_back(key(name));
        }
        if (const auto* node = constant("ACTIVATED_NEUTRALIZE_HERO_ABILITY_NAMES")) {
            data::tag_trace::used(node);
            for (const auto& name : words(node->raw_text)) result.activated_neutralize_abilities.push_back(key(name));
        }
        // WGCH-27/28/97 and WGCH-20..22/52: authored per-type sabotage prices and neutralization cost/permissions.
        // An omitted permission flag is unverified and stays false (the least visible choice).
        std::set<gc::TypeId> neutralization_types;
        for (const auto& binding : input.type_bindings()) {
            const auto original = layers(owned->catalog, binding.original_type, data::Category::game_object);
            auto sabotage_cost = number(data::retail::single(original, "Sabotage_Cost_Credits"), Fixed{});
            if (!sabotage_cost || sabotage_cost.value().raw() < 0) return refuse<gc::GalacticAbilitySetup>("invalid sabotage cost");
            if (sabotage_cost.value().raw() > 0) result.sabotage_costs.push_back({binding.id, sabotage_cost.value()});
            auto cost = number(data::retail::single(original, "Neutralization_Cost"), Fixed{});
            if (!cost || cost.value().raw() < 0) return refuse<gc::GalacticAbilitySetup>("invalid neutralization cost");
            if (cost.value().raw() == 0) continue;
            gc::NeutralizationTarget row; row.type = binding.id; row.cost = cost.value();
            row.major_allowed = row.minor_allowed = false;
            for (const auto& [name, flag] : {std::pair{"Can_Be_Neutralized_By_Major_Heroes", &row.major_allowed},
                std::pair{"Can_Be_Neutralized_By_Minor_Heroes", &row.minor_allowed}})
                if (const auto* node = data::retail::single(original, name)) {
                    data::tag_trace::used(node); const auto value = data::retail::boolean(node->raw_text);
                    if (!value) return refuse<gc::GalacticAbilitySetup>("invalid neutralization permission");
                    *flag = *value;
                }
            neutralization_types.insert(binding.id);
            result.neutralization_targets.push_back(row);
        }
        const auto by_type = [](const auto& a, const auto& b) { return a.type < b.type; };
        std::sort(result.sabotage_costs.begin(), result.sabotage_costs.end(), by_type);
        std::sort(result.neutralization_targets.begin(), result.neutralization_targets.end(), by_type);
        const auto same_type = [](const auto& a, const auto& b) { return a.type == b.type; };
        result.sabotage_costs.erase(std::unique(result.sabotage_costs.begin(), result.sabotage_costs.end(), same_type), result.sabotage_costs.end());
        result.neutralization_targets.erase(std::unique(result.neutralization_targets.begin(), result.neutralization_targets.end(), same_type), result.neutralization_targets.end());
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
            const data::XmlNode* market_ability = nullptr;
            std::vector<const data::XmlNode*> corruptors, hackers, destroyers, saboteurs, neutralizers;
            for (const auto* block : data::retail::list(original, "Abilities", data::retail::ListRule::replace_in_variant))
                for (const auto& ability : block->children) {
                    if (key(ability.name) == "SLICER_ABILITY") slicer = &ability;
                    if (key(ability.name) == "CORRUPT_PLANET_ABILITY") corruptors.push_back(&ability);
                    if (key(ability.name) == "HACK_SUPER_WEAPON_ABILITY") hackers.push_back(&ability);
                    if (key(ability.name) == "PLANET_DESTRUCTION_ABILITY") destroyers.push_back(&ability);
                    if (key(ability.name) == "GALACTIC_SABOTAGE_ABILITY") saboteurs.push_back(&ability);
                    if (key(ability.name) == "NEUTRALIZE_HERO_ABILITY") neutralizers.push_back(&ability);
                    if (key(ability.name) != "BLACK_MARKET_ABILITY") continue;
                    market = true; market_ability = &ability;
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
            const data::XmlNode* saboteur = nullptr; std::string sabotage_name;
            for (const auto& name : result.activated_sabotage_abilities) {
                for (const auto* ability : saboteurs) for (const auto& attr : ability->attributes)
                    if (key(attr.name) == "NAME" && key(attr.value) == name) { saboteur = ability; sabotage_name = name; break; }
                if (saboteur) break;
            }
            const data::XmlNode* neutralizer = nullptr; std::string neutralize_name;
            for (const auto& name : result.activated_neutralize_abilities) {
                for (const auto* ability : neutralizers) for (const auto& attr : ability->attributes)
                    if (key(attr.name) == "NAME" && key(attr.value) == name) { neutralizer = ability; neutralize_name = name; break; }
                if (neutralizer) break;
            }
            const auto* original_metadata = input.original_type(binding.id);
            const auto* original_type = owned->starting.types.find(binding.original_type);
            const auto economic = std::find_if(owned->economics.types.begin(), owned->economics.types.end(),
                [&](const auto& row) { return original_type && key(row.name) == key(original_type->name); });
            const auto* spy_metadata = economic == owned->economics.types.end() ? nullptr : &*economic;
            const data::galactic::SystemSpyDefinition* spy = nullptr;
            if (spy_metadata) for (const auto& candidate : spy_metadata->system_spies)
                if (key(candidate.activation_style) == "GROUND_ACTIVATED") {
                    if (!spy) spy = &candidate;
                    if (std::find(result.activated_spy_abilities.begin(), result.activated_spy_abilities.end(), key(candidate.name))
                        != result.activated_spy_abilities.end()) { spy = &candidate; break; }
                }
            std::vector<gc::AutoSpyProfile> automatic; // WGUI-19/101: initially enabled Galactic_Automatic records, authored order
            if (spy_metadata) for (std::size_t i = 0; i < spy_metadata->system_spies.size(); ++i) {
                const auto& candidate = spy_metadata->system_spies[i];
                if (key(candidate.activation_style) != "GALACTIC_AUTOMATIC" || !candidate.initially_enabled) continue;
                const auto validated = gc::validated_spy({gc::SpyStyle::galactic_automatic, candidate.duration, candidate.mask});
                automatic.push_back({spy_metadata->name, static_cast<std::uint64_t>(i) + 1, validated.duration,
                    candidate.influence_range, validated.mask});
            }
            // A neutralization victim needs its hero respawn facts even without any activated ability of its own.
            const bool victim = neutralization_types.contains(binding.id);
            if (!market && !slicer && !corruptor && !hacker && !destroyer && !saboteur && !neutralizer && !spy && !victim && automatic.empty()
                && !(original_metadata && original_metadata->named_hero.value_or(false))) continue;
            gc::GalacticAbilityProfile profile; profile.type = binding.id; profile.has_market = market;
            for (const auto& [ability, kind] : {
                std::pair{market_ability, gc::AbilityKind::black_market}, std::pair{slicer, gc::AbilityKind::technology_theft},
                std::pair{corruptor, gc::AbilityKind::corruption}, std::pair{hacker, gc::AbilityKind::superweapon_hack},
                std::pair{destroyer, gc::AbilityKind::planet_destruction}, std::pair{saboteur, gc::AbilityKind::structure_sabotage},
                std::pair{neutralizer, gc::AbilityKind::hero_neutralization}}) {
                if (!ability) continue;
                for (const auto& value : ability->children) if (key(value.name) == "ACTIVATION_STYLE") {
                    data::tag_trace::used(&value);
                    const auto style = words(value.raw_text);
                    if (style.size() == 1 && key(style.front()) == "GROUND_ACTIVATED") profile.ground_activated.push_back(kind);
                }
            }
            if (spy) {
                const auto validated = gc::validated_spy({gc::SpyStyle::ground_activated, spy->duration, spy->mask});
                profile.spy = gc::SpyAbilityProfile{key(spy->name), spy_metadata->name,
                    static_cast<std::uint64_t>(spy - spy_metadata->system_spies.data()) + 1, validated.duration,
                    validated.mask, spy->initially_enabled, spy->causes_despawn,
                    (original_metadata && original_metadata->named_hero.value_or(false)) || key(spy_metadata->object_class) == "GENERICHEROUNIT"};
                profile.ground_activated.push_back(gc::AbilityKind::system_spy);
            }
            profile.auto_spies = std::move(automatic);
            profile.lifecycle_only = !market && !slicer && !corruptor && !hacker && !destroyer && !saboteur && !neutralizer && !spy;
            if (saboteur) {
                profile.sabotage = gc::SabotageAbilityProfile{sabotage_name, true, Fixed::from_integer(2).value()}; // WGCH-28: stock factor two
                for (const auto& value : saboteur->children) {
                    if (key(value.name) == "INITIALLY_ENABLED") {
                        data::tag_trace::used(&value); const auto flag = data::retail::boolean(value.raw_text);
                        if (!flag) return refuse<gc::GalacticAbilitySetup>("invalid sabotage enabled flag");
                        profile.sabotage->initially_enabled = *flag;
                    }
                    if (key(value.name) == "SABOTAGE_COST_INCREASE_FACTOR") {
                        auto factor = number(&value);
                        if (!factor || factor.value().raw() < 0) return refuse<gc::GalacticAbilitySetup>("invalid sabotage cost factor");
                        profile.sabotage->cost_factor = factor.value();
                    }
                }
            }
            if (neutralizer) {
                profile.neutralize = gc::NeutralizeAbilityProfile{}; auto& n = *profile.neutralize; n.name = neutralize_name;
                const auto range = [&](const data::XmlNode& value, Fixed& low, Fixed& high) {
                    data::tag_trace::used(&value); const auto parts = words(value.raw_text);
                    if (parts.size() != 2) return true; // WGCH-21: an unusable range selects the default time
                    auto a = Fixed::from_decimal(parts[0]), b = Fixed::from_decimal(parts[1]);
                    if (!a || !b) return false;
                    low = a.value(); high = b.value(); return true;
                };
                const auto curve = [&](const data::XmlNode& value, std::vector<gc::AbilityCurvePoint>& points) {
                    data::tag_trace::used(&value); auto parsed = gc::parse_income_curve(value.raw_text);
                    if (!parsed) return false;
                    points = std::move(parsed).value(); return true;
                };
                const auto flag = [&](const data::XmlNode& value, bool& target) {
                    data::tag_trace::used(&value); const auto parsed = data::retail::boolean(value.raw_text);
                    if (!parsed) return false;
                    target = *parsed; return true;
                };
                n.can_minor = n.can_major = false; // unverified omitted default: the least visible choice
                for (const auto& value : neutralizer->children) {
                    const auto tag = key(value.name); bool ok = true;
                    if (tag == "COST_MOD_BY_BASE_LEVEL") ok = curve(value, n.base_curve);
                    else if (tag == "COST_MOD_BY_PREVIOUS_NEUTRALIZATIONS") ok = curve(value, n.history_curve);
                    else if (tag == "COST_MOD_BY_PLANETARY_CORRUPTION_LEVEL") ok = curve(value, n.corruption_curve);
                    else if (tag == "GENERAL_MINOR_HERO_COST_MOD" || tag == "GENERAL_MAJOR_HERO_COST_MOD") {
                        auto modifier = number(&value);
                        ok = static_cast<bool>(modifier);
                        if (ok) (tag == "GENERAL_MINOR_HERO_COST_MOD" ? n.minor_modifier : n.major_modifier) = modifier.value();
                    }
                    else if (tag == "OWNER_RESPAWN_TIME_IN_SECS") ok = range(value, n.actor_absence_min, n.actor_absence_max);
                    else if (tag == "TARGET_RESPAWN_TIME_IN_SECS") ok = range(value, n.target_absence_min, n.target_absence_max);
                    else if (tag == "CAN_NEUTRALIZE_MINOR_HEROES") ok = flag(value, n.can_minor);
                    else if (tag == "CAN_NEUTRALIZE_MAJOR_HEROES") ok = flag(value, n.can_major);
                    else if (tag == "INITIALLY_ENABLED") ok = flag(value, n.initially_enabled);
                    if (!ok) return refuse<gc::GalacticAbilitySetup>("invalid neutralization ability " + value.name);
                }
            }
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
                auto buyer = prepare_black_market_buyer(owned->catalog, binding.original_type, 0, 0, false, &owned->load_buffer);
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
                bool valid_ranges = load_ranges("Min_Respawn_Time_Per_Tech_Level", theft.absence_min);
                valid_ranges = load_ranges("Max_Respawn_Time_Per_Tech_Level", theft.absence_max) && valid_ranges;
                valid_ranges = valid_ranges && theft.absence_min.size() == theft.absence_max.size();
                if (valid_ranges) for (std::size_t i = 0; i < theft.absence_min.size(); ++i)
                    if (theft.absence_min[i] < Fixed{} || theft.absence_min[i] > theft.absence_max[i]) valid_ranges = false;
                if (!valid_ranges) {
                    campaign_load_note(&owned->load_buffer, "EAWR-LOAD-1230", child("Min_Respawn_Time_Per_Tech_Level") ? child("Min_Respawn_Time_Per_Tech_Level") : slicer, binding.original_type,
                        "Min_Respawn_Time_Per_Tech_Level/Max_Respawn_Time_Per_Tech_Level", "both arrays empty");
                    theft.absence_min.clear(); theft.absence_max.clear();
                }
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
                    if (!profile.respawn_structure) campaign_load_note(&owned->load_buffer, "EAWR-LOAD-1231", value,
                        binding.original_type, "Respawn_Tied_To_Structure", "no structure dependency");
                }
            }
            result.profiles.push_back(std::move(profile));
        }
        std::sort(result.profiles.begin(), result.profiles.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
        const bool has_theft = std::any_of(result.profiles.begin(), result.profiles.end(), [](const auto& p) { return p.theft.has_value(); });
        const bool has_neutralize = std::any_of(result.profiles.begin(), result.profiles.end(), [](const auto& p) { return p.neutralize.has_value(); });
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
                const auto* cost_node = data::retail::single(original, "Slice_Cost_Credits");
                // A slicer-unlockable type with no authored slice cost has no theft price: it stays out of the theft list
                // (unverified; the stock data has some, e.g. an Underworld star base, which only matters once a unit with the
                // slicer ability is bound, as the campaign plots' droids are, #683).
                if (!cost_node || data::retail::is_placeholder(cost_node->raw_text)) continue;
                auto cost = number(cost_node);
                const auto* type = input.original_type(type_id);
                if (!cost || !type) return refuse<gc::GalacticAbilitySetup>(std::string("missing registered slice ") + (cost ? "type " : "cost ") + definition.id);
                const bool ground = units::built_on_ground(*type);
                result.technologies.push_back({type_id, cost.value(), !ground});
            }
        }
        if (has_theft || has_neutralize) {
            // The planet table also carries the auto-capture flag that neutralization reads (WGCH-25).
            for (const auto& planet : setup.planets) {
                const auto* source = planet_source(*owned, input, planet.id);
                if (!source) return refuse<gc::GalacticAbilitySetup>("missing theft planet");
                const auto original = layers(owned->catalog, source->name, data::Category::game_object);
                // CGS-11: an omitted rating keeps the zero of a freshly constructed planet type (unverified, reported).
                const auto* availability_node = data::retail::single(original, "Planet_Tech_Availability_Rating");
                const auto* difficulty_node = data::retail::single(original, "Planet_Slice_Difficulty_Rating");
                auto availability = availability_node ? number(availability_node) : core::Result<Fixed>::success(Fixed{});
                auto difficulty = difficulty_node ? number(difficulty_node) : core::Result<Fixed>::success(Fixed{});
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
                        const auto id = owned->identity(name);
                        if (!id) { owned->note("campaign_unresolved_prerequisite", source->name + ": Planet_Slice_Affinity_Types names "
                            + owned->unresolved(name) + "; it is left out (CGS-07)"); continue; }
                        row.affinity_types.push_back(id);
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
            if (source->planet_destroyed_credit_value->raw() < 0) {
                const auto originals = layers(owned->catalog, source->name, data::Category::game_object);
                campaign_load_note(&owned->load_buffer, "EAWR-LOAD-1232", data::retail::single(originals, "Planet_Destroyed_Credit_Value"),
                    source->name, "Planet_Destroyed_Credit_Value", "ordinary income", "WGCH-176: invalid value unknown; retain ordinary income");
                continue;
            }
            result.ruined_income.push_back({planet.id, *source->planet_destroyed_credit_value});
        }
        auto corruption = prepare_corruption_host(owned->catalog, input, setup, result,
            [owned](std::string_view name) { return owned->identity(name); }, &owned->load_buffer);
        if (!corruption) return core::Result<gc::GalacticAbilitySetup>::failure(corruption.error());
        return core::Result<gc::GalacticAbilitySetup>::success(std::move(result));
    };
}

void install_live_income_service(CampaignSetupServices& services,
    const std::shared_ptr<const Prepared>& owned) {
    services.income = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::IncomeSetup result;
        if (!setup.census || !setup.production || !setup.movement)
            return refuse<gc::IncomeSetup>("income requires original owned census, movement contents and production");
        std::vector<EconomicTypeBinding> bindings;
        for (const auto& [name, id] : owned->identities) bindings.push_back({name, id});
        // CGS-21: selection reads original types, including those with no
        // startup creation or purchase. The creation table is only a subset.
        units::GalacticTypeTable targets;
        for (const auto& binding : input.type_bindings()) {
            if (const auto* original = input.original_type(binding.id)) targets.types.push_back(*original);
        }
        std::map<gc::TypeId, const units::GalacticEconomicType*> profiles;
        for (const auto& type : owned->economics.types) {
            const auto id = owned->identity(type.name);
            // WGEP-163: a halt-only profile is retained for the writer but is not a fiscal source.
            const bool income_source = type.mining || type.gambling || !type.reductions.empty() || !type.technology_prices.empty();
            if (!id || (!income_source && !type.credit_halt.value_or(false))) continue;
            const auto object = std::find_if(setup.census->objects.begin(), setup.census->objects.end(),
                [&](const auto& row) { return row.original_type == id; });
            const auto* owner = object == setup.census->objects.end() ? nullptr : player_source(input, object->owner);
            // A type no starting object holds (a story hero such as the Emperor or Mon Mothma, placed later by a plot
            // reward) has no owner yet: it serves every campaign faction its own Affiliation names.
            std::vector<std::string_view> factions;
            if (owner) factions.push_back(owner->faction);
            else if (const auto* original = owned->starting.types.find(type.name))
                for (const auto& player : input.player_bindings())
                    if (contains(original->affiliation, player.faction)
                        && std::none_of(factions.begin(), factions.end(), [&](const auto& f) { return key(f) == key(player.faction); }))
                        factions.push_back(player.faction);
            if (factions.empty()) factions.push_back({});
            for (const auto& ability : type.reductions) for (const auto& name : ability.included_types)
                if (!targets.find(name)) owned->note("campaign_unresolved_economic_target", type.name
                    + ": Applicable_Unit_Types names " + owned->unresolved(name) + "; it is left out (CGS-21)");
            auto economic = prepare_economic_type(type, id, factions.front(), targets, bindings);
            if (!economic) return core::Result<gc::IncomeSetup>::failure(economic.error());
            for (std::size_t i = 1; i < factions.size(); ++i) {
                auto more = prepare_economic_type(type, id, factions[i], targets, bindings);
                if (!more) return core::Result<gc::IncomeSetup>::failure(more.error());
                auto& rows = economic.value().reductions;
                rows.insert(rows.end(), more.value().reductions.begin(), more.value().reductions.end());
            }
            if (factions.size() > 1) { // the ledger keeps one row per (type, price, category, galaxy_wide), the largest reduction
                auto& rows = economic.value().reductions;
                const auto row_key = [](const auto& row) { return std::tuple{row.type, row.price, row.category, row.galaxy_wide}; };
                std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return row_key(a) < row_key(b); });
                std::vector<gc::EconomicReduction> merged;
                for (const auto& row : rows) {
                    if (!merged.empty() && row_key(merged.back()) == row_key(row)) merged.back().reduction = std::max(merged.back().reduction, row.reduction);
                    else merged.push_back(row);
                }
                rows = std::move(merged);
            }
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
}

void install_live_observer_service(CampaignSetupServices& services,
    const std::shared_ptr<const Prepared>& owned) {
    services.observer = [owned](const CampaignSetupPlan& input, const gc::GalacticSetup& setup) {
        gc::ObserverSetup result;
        for (const auto* name : {"Good_Side_Name", "Evil_Side_Name", "Corrupt_Side_Name"}) {
            const auto* configured = owned->data.constant(name);
            if (!configured) continue;
            for (const auto& binding : input.player_bindings())
                if (key(binding.faction) == key(configured->text)) result.history_players.push_back(binding.player.player_id);
        }
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
    if (auto relations = prepare_live_relations(p, data, plan); !relations)
        return core::Result<CampaignSetupServices>::failure(relations.error());
    auto plots = load_story_plots(filesystem, data, plan);
    if (!plots) return core::Result<CampaignSetupServices>::failure(plots.error());
    const auto story_units = story_unit_names(plots.value());
    if (auto starting = prepare_live_starting_forces(p, catalog, types, plan, story_units); !starting)
        return core::Result<CampaignSetupServices>::failure(starting.error());
    if (auto resources = prepare_live_resources(p, filesystem, catalog, plan, difficulty); !resources)
        return core::Result<CampaignSetupServices>::failure(resources.error());
    for (const auto& location : plan.locations())
        if (!catalog.find(location.original_type, data::Category::game_object))
            p.note("campaign_unresolved_location", "Locations names unknown type " + location.original_type
                + "; the entry creates nothing and its placement is skipped (RGC-80/81)");
    for (const auto& location : plan.locations()) if (location.planet) {
        const auto original = layers(catalog, location.original_type, data::Category::game_object);
        for (const auto* tag : {"Planet_Black_Market_Modifier", "Planet_Tech_Availability_Rating", "Planet_Slice_Difficulty_Rating"})
            if (!data::retail::single(original, tag))
                p.note("campaign_omitted_planet_tag", location.original_type + ": " + tag
                    + " is omitted; zero is assumed, unverified (CGS-11)");
    }
    const std::shared_ptr<const Prepared> owned = prepared;
    CampaignSetupServices services;
    services.load_diagnostics = owned->diagnostics;
    services.annotate_load_notes = [owned, campaign = std::string(plan.campaign_name())](std::span<core::LoadNote> notes) {
        for (auto& note : notes) if (note.code >= "EAWR-LOAD-1000" && note.code <= "EAWR-LOAD-1199") {
            if (note.object == "GameConstants") {
                const auto& root = owned->constants.root;
                const auto found = std::find_if(root.children.begin(), root.children.end(), [&](const auto& node) {
                    return core::iequals(node.name, note.tag);
                });
                const auto& source = found == root.children.end() ? root.source : found->source;
                note.layer.mod_id = source.layer_id; note.file = source.logical_path; note.line = source.line;
                if (found != root.children.end()) note.authored_value = found->raw_text;
                continue;
            }
            auto originals = layers(owned->catalog, note.object, data::Category::game_object);
            if (originals.empty()) originals = layers(owned->catalog, note.object, data::Category::faction);
            if (originals.empty()) originals = layers(owned->catalog, campaign, data::Category::campaign);
            if (const auto* node = data::retail::single(originals, note.tag)) {
                note.layer.mod_id = node->source.layer_id;
                note.file = node->source.logical_path; note.line = node->source.line;
                note.authored_value = node->raw_text;
            } else if (!originals.empty() && note.file.empty()) {
                const auto& source = originals.front()->root.source;
                note.layer.mod_id = source.layer_id; note.file = source.logical_path; note.line = source.line;
            }
        }
    };
    services.load_notes = [owned, annotate = services.annotate_load_notes] {
        owned->load_collector.merge(owned->load_buffer);
        auto notes = owned->load_collector.snapshot(); annotate(notes); return notes;
    };
    if (diagnostics::GcMemory::enabled()) services.memory_inventory = [weak = std::weak_ptr<const Prepared>(owned)](diagnostics::GcMemory& memory) {
        if (const auto value = weak.lock()) {
            memory.row("gc.providers.catalog", value->catalog.definitions());
            memory.row("gc.providers.galactic_data", value->data);
            memory.row("gc.providers.economics", value->economics.types);
        }
    };
    install_live_placement_services(services, owned, plan);
    install_live_conflict_services(services, owned);
    install_live_ability_services(services, owned);
    install_live_income_service(services, owned);
    install_live_observer_service(services, owned);
    return core::Result<CampaignSetupServices>::success(std::move(services));
}
} // namespace eawr::campaign
