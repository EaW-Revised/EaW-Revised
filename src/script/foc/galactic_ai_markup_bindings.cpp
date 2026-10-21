#include "galactic_ai_markup.hpp"
#include "galactic_ai_world.hpp"

namespace eawr::script::foc::ai {
std::optional<sim::galactic::PlanetId> GalacticWorld::markup_stage(tactical::PlayerId player,
    sim::galactic::PlanetId target, std::string_view policy) const {
    // WGA-226/466: preserve the owning site's published order.
    const auto key = upper_case(policy);
    if (key == "SINGLE_HOP_DISCONNECTED") return target;
    if (const auto id = goal_reach_policy(key)) {
        const auto* found = bound().goal_reach_index_->find(player, target, *id);
        if (!found) return {};
        const auto& sites = bound().connectivity_->goal_reach[found->row].production_sites;
        return sites.empty() ? std::nullopt : std::optional{sites.front()};
    }
    for (const auto& row : bound().connectivity_->goal_reach) // unrecognised spelling: only an identical row spelling can match
        if (row.player == player && row.source == target && upper_case(row.policy) == key)
            return row.production_sites.empty() ? std::nullopt : std::optional{row.production_sites.front()};
    return {};
}
core::Result<void> register_galactic_markup_bindings(authoritative::ScriptScheduler& scripts,
    std::shared_ptr<const GalacticWorld> world, std::shared_ptr<const Engine> engine, GalacticMarkupPath path) {
    using namespace detail;
    if (!world || !engine) return core::Result<void>::failure(make_error(authoritative::codes::invalid_request, "markup bindings need a retained galactic owner"));
    // WGA-471: fixed completed-invocation phase, chosen from installed census
    // measurements. Runtime timings never select or alter this interval.
    if (auto registered = scripts.register_managed_handles({handle_galactic_markup,
        "galactic.markup.inventory", "galactic.markup.release", 64}); !registered) return registered;
    const auto planet = [world, engine](const authoritative::Value& value) -> std::optional<sim::galactic::PlanetId> {
        if (const auto* at = as_handle(value, handle_game_object)) return world->containing_planet(at->id);
        if (const auto* at = as_handle(value, handle_ai_target)) {
            const auto* target = engine->target(at->id);
            return target ? world->containing_planet(target->object) : std::nullopt;
        }
        return {};
    };
    const auto direct_planet = [world, engine](const authoritative::Value& value) -> std::optional<sim::galactic::PlanetId> {
        sim::EntityId id{};
        if (const auto* at = as_handle(value, handle_game_object)) id = at->id;
        else if (const auto* wrapper = as_handle(value, handle_ai_target)) {
            const auto* target = engine->target(wrapper->id); if (target) id = target->object;
        }
        const auto parent = id ? world->containing_planet(id) : std::nullopt;
        return parent && *parent == id ? parent : std::nullopt;
    };
    auto result = scripts.register_method(handle_galactic_markup, "IsFinished",
        [](authoritative::BindingContext&, const authoritative::ValueList&) { return one(boolean(false)); }); // WGA-470.
    if (!result) return result;
    result = scripts.register_method(handle_galactic_markup, "Result",
        [](authoritative::BindingContext&, const authoritative::ValueList&) { return one(boolean(true)); });
    if (!result) return result;
    result = scripts.register_binding("Apply_Markup", [direct_planet](authoritative::BindingContext& context,
        const authoritative::ValueList& args) -> core::Result<authoritative::ValueList> {
        if (args.size() != 3 && args.size() != 4) return fail("Apply_Markup expects 3 or 4 parameters");
        if (!as_handle(args[0], handle_player) || !std::get_if<Real>(&args[2].data)) return fail("Apply_Markup requires a player and number");
        const auto* list = std::get_if<std::vector<authoritative::Value>>(&args[1].data);
        const auto targets = list ? std::span<const authoritative::Value>(*list) : std::span<const authoritative::Value>(&args[1], 1);
        std::vector<authoritative::Value> copied;
        for (const auto& target : targets) {
            if (as_handle(target, handle_player)) copied.push_back(target);
            else if (const auto id = direct_planet(target)) copied.push_back(handle(handle_game_object, *id));
            else return fail("Apply_Markup target requires a live planet or player perception root");
            // WGA-462: non-planet object roots are not hosted by this galactic owner.
        }
        authoritative::Value previous;
        if (args.size() == 4 && as_handle(args[3], handle_galactic_markup)) previous = args[3];
        return context.apply_immediate("galactic.markup", {args[0], authoritative::Value{std::move(copied)}, args[2], previous});
    });
    if (!result) return result;
    result = scripts.register_binding("FindPlanet.Get_All_Planets", [world](authoritative::BindingContext&, const authoritative::ValueList&)
        -> core::Result<authoritative::ValueList> {
        const auto input = world->copy_input(); if (!input) return fail("planet list requires a bound galaxy");
        std::vector<authoritative::Value> planets;
        for (const auto& target : input->targets) planets.push_back(handle(handle_game_object, target.planet));
        return one(authoritative::Value{std::move(planets)});
    });
    if (!result) return result;
    result = scripts.register_binding("Find_Path", [world, direct_planet, path](authoritative::BindingContext&, const authoritative::ValueList& args)
        -> core::Result<authoritative::ValueList> {
        if (args.size() != 3) return fail("Find_Path expects a player and two planets");
        const auto* player = as_handle(args[0], handle_player);
        const auto from = direct_planet(args[1]), to = direct_planet(args[2]);
        if (!player || player->id > std::numeric_limits<tactical::PlayerId>::max() || !from || !to || !path)
            return fail("Find_Path requires live planets and the galactic execution path owner");
        const auto owner = static_cast<tactical::PlayerId>(player->id);
        if (!world->markup()->is_ai_player(owner)) return fail("Find_Path requires an AI player");
        auto found = path(owner, *from, *to);
        if (!found) return core::Result<authoritative::ValueList>::failure(found.error());
        std::vector<authoritative::Value> planets;
        for (const auto id : found.value()) planets.push_back(handle(handle_game_object, id));
        return one(authoritative::Value{std::move(planets)});
    });
    if (!result) return result;
    return scripts.register_binding("_FindStageArea", [world, engine, planet](authoritative::BindingContext&, const authoritative::ValueList& args)
        -> core::Result<authoritative::ValueList> {
        if (args.size() != 3) return fail("_FindStageArea expects a player, target and taskforce");
        const auto* player = as_handle(args[0], handle_player); const auto target = planet(args[1]);
        if (!player || player->id > std::numeric_limits<tactical::PlayerId>::max() || !target) return fail("_FindStageArea requires a player and live planet target");
        const auto owner = static_cast<tactical::PlayerId>(player->id);
        if (!world->markup()->is_ai_player(owner)) return fail("_FindStageArea requires an AI player");
        auto reach = galactic_ai::MovementReach::friendly_only;
        if (const auto* wrapper = as_handle(args[2], handle_taskforce)) {
            const auto* force = engine->taskforce(wrapper->id);
            const auto* plan = force ? engine->plan(force->plan) : nullptr;
            const auto policy = plan ? engine->galactic_movement_reach(plan->id) : std::nullopt;
            if (!plan || plan->player != owner || !policy) return fail("_FindStageArea taskforce lacks its retained goal policy");
            reach = *policy;
        }
        constexpr std::array names{"FRIENDLY_ONLY", "FRIENDLY_IGNORE_THREAT", "ENEMY_DESTINATION", "ENEMY_UNDEFENDED", "SINGLE_HOP_DISCONNECTED", "ANY"};
        const auto stage = world->markup_stage(owner, *target, names.at(static_cast<std::size_t>(reach)));
        if (!stage) return none();
        if (as_handle(args[1], handle_ai_target)) {
            const auto* at = engine->target_of_object(*stage);
            return at ? one(handle(handle_ai_target, at->id)) : none();
        }
        return one(handle(handle_game_object, *stage));
    });
}
} // namespace eawr::script::foc::ai
