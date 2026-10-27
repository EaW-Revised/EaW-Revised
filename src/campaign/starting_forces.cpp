#include "eawr/campaign/starting_forces.hpp"

#include "eawr/sim/galactic/production.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
template<class T> core::Result<T> refuse(const std::string& message) {
    core::Diagnostic error; error.code = std::string(gc::diagnostic_codes::invalid_setup);
    error.message = "WGS-80 starting placement: " + message;
    return core::Result<T>::failure(std::move(error));
}
const units::GalacticType* original(const StartingForceInputs& inputs, gc::TypeId id) {
    const auto at = std::find_if(inputs.bindings.begin(), inputs.bindings.end(),
        [&](const auto& row) { return row.id == id; });
    return at == inputs.bindings.end() ? nullptr : inputs.types.find(at->original_type);
}
gc::TypeId identity(const StartingForceInputs& inputs, std::string_view name) {
    const auto* wanted = inputs.types.find(name);
    if (!wanted) return 0;
    const auto at = std::find_if(inputs.bindings.begin(), inputs.bindings.end(),
        [&](const auto& row) { return inputs.types.find(row.original_type) == wanted; });
    return at == inputs.bindings.end() ? 0 : at->id;
}
const StartingForceRuntimeType* runtime(const StartingForceInputs& inputs, gc::TypeId id) {
    const auto at = std::find_if(inputs.runtime_types.begin(), inputs.runtime_types.end(),
        [&](const auto& row) { return row.type == id; });
    return at == inputs.runtime_types.end() ? nullptr : &*at;
}
bool behaves(const units::GalacticType& type, std::string_view name) {
    return std::find(type.behavior.begin(), type.behavior.end(), name) != type.behavior.end();
}
gc::ActorCombatRole combat_role(const units::GalacticType& type) {
    if (behaves(type, "FLEET")) return gc::ActorCombatRole::fleet;
    if (behaves(type, "TRANSPORT")) return gc::ActorCombatRole::transport;
    if (behaves(type, "SQUADRON") || behaves(type, "DUMMY_SPACE_FIGHTER_SQUADRON")) return gc::ActorCombatRole::squadron;
    return gc::ActorCombatRole::ordinary;
}
} // namespace

core::Result<gc::NestedCreationSetup> prepare_nested_creation(const StartingForceInputs& inputs) {
    gc::NestedCreationSetup result; result.fleet_type = inputs.fleet_type; result.squadron_type = inputs.squadron_type;
    for (const auto& binding : inputs.bindings) {
        const auto* source = original(inputs, binding.id); const auto* live = runtime(inputs, binding.id);
        if (!source || !live) return refuse<gc::NestedCreationSetup>("missing nested original/runtime binding");
        gc::NestedCreationType row; row.type = binding.id;
        row.skips_combat = live->skips_combat; row.always_spawn_in_orbit = live->always_spawn_in_orbit;
        row.combat_role = combat_role(*source); row.abilities = live->abilities;
        if (behaves(*source, "TRANSPORT")) row.role = gc::NestedCreationRole::transport;
        if (behaves(*source, "DUMMY_GROUND_COMPANY")) {
            row.role = gc::NestedCreationRole::company; row.transport = identity(inputs, source->company_transport);
        } else if (behaves(*source, "DUMMY_SPACE_FIGHTER_SQUADRON")) row.role = gc::NestedCreationRole::squadron;
        const auto& members = row.role == gc::NestedCreationRole::company ? source->company_units : source->squadron_units;
        for (const auto& name : members) {
            const auto id = identity(inputs, name);
            if (!id) return refuse<gc::NestedCreationSetup>("unbound nested member: " + name);
            row.members.push_back(id);
        }
        result.types.push_back(std::move(row));
    }
    for (const auto& row : inputs.relations) result.relations.push_back({row.player, row.other, row.enemy});
    return core::Result<gc::NestedCreationSetup>::success(std::move(result));
}

core::Result<InitialCampaignRelations> prepare_initial_campaign_relations(
    const data::galactic::GalacticData& data, std::span<const CampaignPlayerBinding> players) {
    InitialCampaignRelations result;
    std::vector<const data::galactic::Faction*> factions;
    gc::PlayerId previous{};
    for (const auto& player : players) {
        const auto* faction = data.faction(player.faction);
        if (!faction || player.player.player_id <= previous)
            return refuse<InitialCampaignRelations>("missing faction or unordered player relation input");
        previous = player.player.player_id; factions.push_back(faction);
    }
    const auto matches = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
            return upper(x) == upper(y);
        });
    };
    for (std::size_t i = 0; i < players.size(); ++i) for (std::size_t j = 0; j < players.size(); ++j) {
        bool ally = i == j, hostile = false;
        if (i != j && !factions[i]->is_neutral.value_or(false) && !factions[j]->is_neutral.value_or(false)) {
            ally = factions[i] == factions[j] || std::any_of(factions[i]->allies.begin(), factions[i]->allies.end(),
                [&](const auto& name) { return matches(name, factions[j]->name); });
            // Same faction bypasses the list scan; otherwise enemies overwrite allies.
            if (factions[i] != factions[j]) hostile = std::any_of(factions[i]->enemies.begin(), factions[i]->enemies.end(),
                [&](const auto& name) { return matches(name, factions[j]->name); });
            if (hostile) ally = false;
        }
        const auto a = players[i].player.player_id, b = players[j].player.player_id;
        result.control.push_back({a, b, ally}); result.placement.push_back({a, b, hostile});
    }
    return core::Result<InitialCampaignRelations>::success(std::move(result));
}

core::Result<StartingForceInputs> prepare_initial_starting_force_inputs(const units::GalacticTypeTable& types,
    std::span<const FleetTypeBinding> bindings, std::span<const StartingForceRelation> relations,
    gc::TypeId fleet_type, gc::TypeId squadron_type) {
    StartingForceInputs result; result.bindings.assign(bindings.begin(), bindings.end());
    result.relations.assign(relations.begin(), relations.end());
    result.fleet_type = fleet_type; result.squadron_type = squadron_type;
    gc::TypeId previous{};
    for (const auto& binding : bindings) {
        if (binding.id <= previous) return refuse<StartingForceInputs>("unordered or zero original identity");
        previous = binding.id;
        auto resolved = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!resolved) return core::Result<StartingForceInputs>::failure(resolved.error());
        for (const auto& issue : types.issues) if (issue.owner == resolved.value().name && issue.kind == data::galactic::IssueKind::bad_value
            && (issue.tag == "Abilities/Galactic_Stealth_Ability/Initially_Enabled" || issue.tag == "Evade_Detection_Chance"))
            return refuse<StartingForceInputs>(binding.original_type + ": invalid authored galactic stealth value");
        bool skips = false; gc::ActorAbilityState state;
        state.present = !resolved.value().initial_stealth.empty() || behaves(resolved.value(), "SPECIAL_ABILITIES");
        for (const auto& ability : resolved.value().initial_stealth) {
            if (!ability.initially_enabled.value_or(true)) continue;
            // WGM-160: specific-handler validation corrects other styles to automatic.
            skips = true; state.combat_skip = true; state.self_target = true;
            state.evade_chance = std::clamp(ability.evade_chance.value_or(sim::math::Fixed{}),
                sim::math::Fixed{}, sim::math::Fixed::from_raw(sim::math::Fixed::scale));
        }
        result.runtime_types.push_back({binding.id, skips, *resolved.value().always_spawn_in_orbit, state});
        result.types.types.push_back(std::move(resolved).value());
    }
    if (!fleet_type || !squadron_type || !original(result, fleet_type) || !original(result, squadron_type))
        return refuse<StartingForceInputs>("missing fleet/squadron original identity");
    return core::Result<StartingForceInputs>::success(std::move(result));
}

core::Result<MovementInputs> prepare_initial_movement_inputs(const StartingForceInputs& inputs,
    const data::galactic::GalacticData& data, std::span<const MovementPlayerBinding> players) {
    std::vector<MovementTypeBinding> bindings;
    for (const auto& binding : inputs.bindings) {
        const auto* type = original(inputs, binding.id);
        const auto* live = runtime(inputs, binding.id);
        if (!type || !live || !type->hyperspace_capable) return refuse<MovementInputs>("missing resolved initial movement type");
        const bool fleet = behaves(*type, "FLEET"), squadron = behaves(*type, "SQUADRON") || behaves(*type, "DUMMY_SPACE_FIGHTER_SQUADRON"),
            transport = behaves(*type, "TRANSPORT");
        bindings.push_back({binding.id, binding.original_type, *type->hyperspace_capable, squadron, fleet, live->skips_combat, transport});
    }
    return build_movement_inputs(inputs.types, data, bindings, players);
}

static core::Result<StartingForcePlacement> place_starting_force_once(const StartingForceInputs& inputs,
    const StartingForceRequest& request, sim::EntityId first_free, const gc::GalacticSetup& setup,
    const CampaignPlacementContext& context, const std::function<core::Result<bool>(const gc::Fleet&)>& combat_skip) {
    const auto* type = original(inputs, request.type);
    const auto planet = std::find_if(context.control.planets.begin(), context.control.planets.end(),
        [&](const auto& row) { return row.planet == request.planet; });
    const auto production = std::find_if(context.production.planets.begin(), context.production.planets.end(),
        [&](const auto& row) { return row.planet == request.planet; });
    if (!type || inputs.types.find(request.original_type) != type || planet == context.control.planets.end()
        || production == context.production.planets.end() || !request.owner || !first_free)
        return refuse<StartingForcePlacement>("missing original binding or planet presence");
    StartingForcePlacement out; out.next_id = first_free;
    const auto allocate = [&]() -> sim::EntityId {
        if (out.next_id == std::numeric_limits<sim::EntityId>::max()) return 0;
        return out.next_id++;
    };
    const auto metadata = [&](gc::FleetMember member, gc::TypeId original_type, sim::EntityId parent) {
        out.originals.push_back({member.id, original_type, request.owner, request.planet, parent, false, false, member.type, 0, gc::ActorCombatRole::unavailable, {}, {}});
    };
    const auto fleet = [&](gc::FleetMember member, sim::EntityId id, std::uint8_t slot = 0) {
        out.fleets.push_back({id, request.owner, request.planet, {member}, slot, {}});
        metadata({id, inputs.fleet_type}, inputs.fleet_type, request.planet);
    };
    if (behaves(*type, "DUMMY_GROUND_BASE"))
        return refuse<StartingForcePlacement>("CGS-03 direct ground-base dispatch asserts in the debug build");
    if (behaves(*type, "DUMMY_STAR_BASE")) {
        if (!type->base_level || *type->base_level < 1 || *type->base_level > 5)
            return refuse<StartingForcePlacement>("missing or invalid authored starbase level");
        if (*type->base_level < 2) {
            // CGS-10: Init of a base below level two clears the planet's space special structures.
            out.clear_space_structures = true;
            for (const auto& object : production->objects) if (object.domain == gc::ProductionDomain::ground && object.kind == gc::ProductionKind::structure) {
                const auto* source = original(inputs, object.type);
                if (!source) return refuse<StartingForcePlacement>("missing special-structure Political_Control");
                auto resolved = units::resolve_initial_galactic_type(inputs.types, source->name);
                if (!resolved) return core::Result<StartingForcePlacement>::failure(resolved.error());
                if (resolved.value().political_control->raw() != 0) out.special_control.push_back(*resolved.value().political_control);
            }
            if (out.special_control.size() > 3) return refuse<StartingForcePlacement>("special-control category overflow");
        }
        // WGS-180: every admitted record overwrites the base, including equal or
        // lower levels and a different supplied owner. Authored order wins.
        out.starbase = gc::ControlBase{request.owner, static_cast<std::uint32_t>(*type->base_level)};
        out.starbase_type = request.type;
        return core::Result<StartingForcePlacement>::success(std::move(out));
    }
    const bool ground_structure = behaves(*type, "DUMMY_GROUND_STRUCTURE");
    const bool space_structure = behaves(*type, "DUMMY_ORBITAL_STRUCTURE"); // the debug build's token for an orbital special structure
    if (ground_structure || space_structure) {
        if (ground_structure && !planet->surface_accessible)
            return core::Result<StartingForcePlacement>::success(std::move(out));
        const auto domain = ground_structure ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
        const auto count = std::count_if(production->objects.begin(), production->objects.end(),
            [&](const auto& object) { return object.domain == domain && object.kind == gc::ProductionKind::structure; });
        // CGS-20: direct creation retains excess structures. The debug build's
        // capacity diagnostic follows insertion and does not undo it.
        const auto id = allocate();
        if (!id) return refuse<StartingForcePlacement>("allocation frontier exhausted");
        out.structures.push_back({id, request.type, request.owner, {}, domain, gc::ProductionKind::structure});
        out.primary = id;
        metadata({id, request.type}, request.type, request.planet);
        if (ground_structure) out.ground_base = gc::ControlBase{request.owner,
            static_cast<std::uint32_t>(std::min<std::int64_t>(count + 1, 5))};
        auto objects = production->objects;
        objects.push_back(out.structures.front());
        for (const auto layer : {gc::ProductionDomain::ground, gc::ProductionDomain::space})
            for (const auto& object : objects) if (object.domain == layer && object.kind == gc::ProductionKind::structure) {
                const auto* source = original(inputs, object.type);
                if (!source)
                    return refuse<StartingForcePlacement>("missing special-structure Political_Control");
                auto resolved = units::resolve_initial_galactic_type(inputs.types, source->name);
                if (!resolved) return core::Result<StartingForcePlacement>::failure(resolved.error());
                if (resolved.value().political_control->raw() != 0) out.special_control.push_back(*resolved.value().political_control);
            }
        if (out.special_control.size() > 3) return refuse<StartingForcePlacement>("special-control category overflow");
    } else if (behaves(*type, "DUMMY_GROUND_COMPANY") || behaves(*type, "DUMMY_SPACE_FIGHTER_SQUADRON")) {
        // Root prepares these once; standalone placement callers use the same adapter.
        std::optional<gc::NestedCreationSetup> fallback;
        if (!context.production.nested_creation) {
            auto prepared = prepare_nested_creation(inputs);
            if (!prepared) return core::Result<StartingForcePlacement>::failure(prepared.error());
            fallback = std::move(prepared).value();
        }
        const auto& creation = context.production.nested_creation ? *context.production.nested_creation : *fallback;
        auto nested = gc::place_nested_force(creation, request.type, request.owner, request.planet,
            first_free, setup.fleets, setup.movement ? *setup.movement : context.movement, context.control, combat_skip);
        if (!nested) return core::Result<StartingForcePlacement>::failure(nested.error());
        out.next_id = nested.value().next_id;
        out.fleets = std::move(nested.value().fleets); out.contents = std::move(nested.value().contents);
        out.landed = std::move(nested.value().landed); out.originals = std::move(nested.value().originals);
        // WGM-133/134: a company returns its transport; a squadron returns its outer fleet.
        out.primary = behaves(*type, "DUMMY_GROUND_COMPANY") ? first_free : out.fleets.front().id;
    } else if (behaves(*type, "DUMMY_STARSHIP")) {
        const auto outer = allocate(); const gc::FleetMember ship{allocate(), request.type};
        fleet(ship, outer); metadata(ship, request.type, outer);
        out.primary = outer;
    } else return refuse<StartingForcePlacement>("unimplemented original behaviour for " + request.original_type);
    if (!original(inputs, inputs.fleet_type) && !out.fleets.empty())
        return refuse<StartingForcePlacement>("unbound outer fleet original type");
    if (!out.next_id || std::any_of(out.originals.begin(), out.originals.end(), [](const auto& row) { return !row.id; }))
        return refuse<StartingForcePlacement>("allocation frontier exhausted");
    out.created = out.next_id != first_free;
    std::sort(out.originals.begin(), out.originals.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (auto& actor : out.originals) {
        const auto* current = original(inputs, actor.current_type);
        const auto* initialized = runtime(inputs, actor.current_type);
        if (!current || !initialized) return refuse<StartingForcePlacement>("actor constructor facts unavailable");
        actor.combat_role = combat_role(*current); actor.abilities = initialized->abilities;
        actor.garrison = false; // GAG-01: ordinary GC construction has no tactical spawn/escort grant.
    }
    return core::Result<StartingForcePlacement>::success(std::move(out));
}

static core::Result<StartingForcePlacement> place_starting_force_checked(const StartingForceInputs&,
    const StartingForceRequest&, sim::EntityId, const gc::GalacticSetup&, const CampaignPlacementContext&,
    const std::function<core::Result<bool>(const gc::Fleet&)>&);

sim::galactic::GalacticSession::NativeForceBuilder stock_native_force_builder(
    std::shared_ptr<const StartingForceInputs> inputs, gc::GalacticSession::NativeProductionRequest request) {
    return [inputs = std::move(inputs), request](const gc::GalacticSnapshot& snapshot, sim::EntityId frontier) {
        using Result = core::Result<gc::GalacticSession::NativeForcePlacement>;
        if (!inputs || !snapshot.control() || !snapshot.movement() || !snapshot.production())
            return refuse<gc::GalacticSession::NativeForcePlacement>("native placement requires rebuilt original inputs and live presence");
        const auto binding = std::find_if(inputs->bindings.begin(), inputs->bindings.end(),
            [&](const auto& row) { return row.id == request.type; });
        if (binding == inputs->bindings.end()) return refuse<gc::GalacticSession::NativeForcePlacement>("native placement original type is unbound");
        gc::GalacticSetup setup;
        setup.fleets.assign(snapshot.fleets().begin(), snapshot.fleets().end());
        setup.planets.assign(snapshot.planets().begin(), snapshot.planets().end());
        setup.control = *snapshot.control(); setup.movement = *snapshot.movement();
        CampaignPlacementContext context;
        context.control = *setup.control; context.movement = *setup.movement; context.production = *snapshot.production();
        auto placed = place_starting_force(*inputs, {request.planet, request.player, request.type, binding->original_type, false, {}}, frontier, setup, context,
            [&](const gc::Fleet& fleet) { return gc::fleet_combat_skip(fleet, snapshot.actor_records(), snapshot.movement()); });
        if (!placed) return Result::failure(placed.error());
        const auto& row = placed.value();
        if (!row.detached_units.empty()) return refuse<gc::GalacticSession::NativeForcePlacement>("native detached placement requires its owning service");
        return Result::success({row.next_id, row.fleets, row.contents, row.landed, row.structures, row.originals,
            row.starbase, row.ground_base, row.starbase_type, row.special_control, row.primary});
    };
}

static core::Result<gc::GalacticSession::NativeForcePlacement> stock_native_shadow_placement(
    const std::shared_ptr<const StartingForceInputs>& inputs, const gc::GalacticSession::NativeProductionRequest& request,
    const gc::GalacticSession::NativeForceView& view, sim::EntityId frontier) {
    using Result = core::Result<gc::GalacticSession::NativeForcePlacement>;
    if (!inputs || view.planet.id != request.planet)
        return refuse<gc::GalacticSession::NativeForcePlacement>("native shadow placement requires its original inputs and staging planet");
    const auto binding = std::find_if(inputs->bindings.begin(), inputs->bindings.end(),
        [&](const auto& row) { return row.id == request.type; });
    if (binding == inputs->bindings.end()) return refuse<gc::GalacticSession::NativeForcePlacement>("native shadow original type is unbound");
    gc::GalacticSetup setup;
    setup.fleets.assign(view.orbit.begin(), view.orbit.end());
    setup.planets.push_back(view.planet);
    CampaignPlacementContext context;
    context.control.max_ground_forces = view.maximum_ground_forces;
    context.control.planets.push_back(view.presence);
    context.movement.contents.assign(view.contents.begin(), view.contents.end());
    context.production.planets.push_back(view.production);
    context.production.nested_creation = view.creation;
    setup.control = context.control; setup.movement = context.movement;
    auto placed = place_starting_force_checked(*inputs, {request.planet, request.player, request.type, binding->original_type, false, {}},
        frontier, setup, context, view.combat_skip);
    if (!placed) return Result::failure(placed.error());
    const auto& row = placed.value();
    if (!row.detached_units.empty()) return refuse<gc::GalacticSession::NativeForcePlacement>("native detached placement requires its owning service");
    return Result::success({row.next_id, row.fleets, row.contents, row.landed, row.structures, row.originals,
        row.starbase, row.ground_base, row.starbase_type, row.special_control});
}
gc::GalacticSession::NativeShadowForceBuilder stock_native_force_shadow_builder(
    std::shared_ptr<const StartingForceInputs> inputs, gc::GalacticSession::NativeProductionRequest request) {
    return [inputs = std::move(inputs), request](const gc::GalacticSession::NativeForceView& view, sim::EntityId frontier) {
        return stock_native_shadow_placement(inputs, request, view, frontier);
    };
}
gc::GalacticSession::NativeReconciledForceBuilder stock_native_force_reconciliation_builder(
    std::shared_ptr<const StartingForceInputs> inputs, gc::GalacticSession::NativeProductionRequest request) {
    return [inputs = std::move(inputs), request](const gc::GalacticSession::NativeForceView& view, sim::EntityId frontier) {
        using Result = core::Result<std::optional<gc::GalacticSession::NativeForcePlacement>>;
        auto placed = stock_native_shadow_placement(inputs, request, view, frontier);
        if (!placed) return Result::failure(placed.error());
        return Result::success(std::move(placed).value());
    };
}
static core::Result<StartingForcePlacement> place_starting_force_checked(const StartingForceInputs& inputs,
    const StartingForceRequest& request, sim::EntityId first_free, const gc::GalacticSetup& setup,
    const CampaignPlacementContext& context, const std::function<core::Result<bool>(const gc::Fleet&)>& combat_skip) {
    auto staged = setup; auto presence = context;
    staged.control = presence.control; staged.movement = presence.movement;
    StartingForcePlacement result; result.next_id = first_free;
    std::set<gc::TypeId> chain;
    auto current = request;
    for (unsigned depth = 0; ; ++depth) {
        if (depth >= 32 || !chain.insert(current.type).second)
            return refuse<StartingForcePlacement>("cyclic or excessive additional creation chain");
        const auto reader = [&](const gc::Fleet& fleet) -> core::Result<bool> {
            const auto& actors = presence.census.objects;
            const auto at = std::lower_bound(actors.begin(), actors.end(), fleet.id,
                [](const auto& row, auto key) { return row.id < key; });
            if (at != actors.end() && at->id == fleet.id)
                return gc::fleet_combat_skip(fleet, actors, &presence.movement);
            return combat_skip(fleet);
        };
        auto placed = place_starting_force_once(inputs, current, result.next_id, staged, presence,
            combat_skip ? std::function<core::Result<bool>(const gc::Fleet&)>(reader) : std::function<core::Result<bool>(const gc::Fleet&)>{});
        if (!placed) return placed;
        const auto& part = placed.value();
        staged.fleets.insert(staged.fleets.end(), part.fleets.begin(), part.fleets.end());
        if (auto applied = apply_starting_force_placement(inputs, current, part, staged, presence); !applied)
            return core::Result<StartingForcePlacement>::failure(applied.error());
        if (depth == 0) result.primary = part.primary; // WGM-135: recursive success is never primary success.
        result.next_id = part.next_id; result.created |= part.created;
        result.fleets.insert(result.fleets.end(), part.fleets.begin(), part.fleets.end());
        result.contents.insert(result.contents.end(), part.contents.begin(), part.contents.end());
        result.landed.insert(result.landed.end(), part.landed.begin(), part.landed.end());
        if (part.clear_space_structures) {
            // CGS-10: the per-part apply above cleared them from the staged copy; the combined result is applied once, so
            // drop the ones an earlier chain part placed here too.
            const auto is_space = [](const auto& object) {
                return object.domain == gc::ProductionDomain::space && object.kind == gc::ProductionKind::structure;
            };
            for (const auto& object : result.structures) if (is_space(object)) result.cleared_structures.push_back(object.id);
            result.structures.erase(std::remove_if(result.structures.begin(), result.structures.end(), is_space), result.structures.end());
        }
        result.structures.insert(result.structures.end(), part.structures.begin(), part.structures.end());
        result.originals.insert(result.originals.end(), part.originals.begin(), part.originals.end());
        if (part.starbase) { result.starbase = part.starbase; result.starbase_type = part.starbase_type; }
        if (part.ground_base) result.ground_base = part.ground_base;
        if (!part.structures.empty() || part.clear_space_structures) result.special_control = part.special_control;
        result.clear_space_structures |= part.clear_space_structures;
        const auto* source = original(inputs, current.type);
        // WGEP-84 / ET-T053: Create_Additional_Object is an engine tag with
        // no stock XML use; direct creation retains the supplied owner.
        if (source->additional_creation_type.empty()) break;
        current.type = identity(inputs, source->additional_creation_type);
        current.original_type = source->additional_creation_type;
        if (!current.type) return refuse<StartingForcePlacement>("unbound additional creation type");
    }
    return core::Result<StartingForcePlacement>::success(std::move(result));
}
core::Result<StartingForcePlacement> place_starting_force(const StartingForceInputs& inputs,
    const StartingForceRequest& request, sim::EntityId first_free, const gc::GalacticSetup& setup,
    const CampaignPlacementContext& context, const std::function<core::Result<bool>(const gc::Fleet&)>& combat_skip) {
    return place_starting_force_checked(inputs, request, first_free, setup, context, combat_skip);
}

core::Result<void> apply_starting_force_placement(const StartingForceInputs&,
    const StartingForceRequest& request, const StartingForcePlacement& placed,
    gc::GalacticSetup& setup, CampaignPlacementContext& context) {
    if (!setup.control || !setup.movement) return refuse<void>("placement context is absent");
    auto planet = std::find_if(setup.control->planets.begin(), setup.control->planets.end(),
        [&](const auto& row) { return row.planet == request.planet; });
    auto production = std::find_if(context.production.planets.begin(), context.production.planets.end(),
        [&](const auto& row) { return row.planet == request.planet; });
    if (planet == setup.control->planets.end() || production == context.production.planets.end())
        return refuse<void>("planet presence is absent");
    if (placed.starbase) {
        planet->starbase = *placed.starbase; production->starbase_type = placed.starbase_type;
        production->space_level = placed.starbase->level; production->starbase_owner = placed.starbase->owner;
    }
    if (placed.ground_base) { planet->ground_base = *placed.ground_base; production->ground_level = placed.ground_base->level; }
    if (placed.clear_space_structures) {
        // CGS-10: the cleared objects leave the production list and the census.
        std::set<sim::EntityId> cleared;
        for (const auto& object : production->objects)
            if (object.domain == gc::ProductionDomain::space && object.kind == gc::ProductionKind::structure) cleared.insert(object.id);
        production->objects.erase(std::remove_if(production->objects.begin(), production->objects.end(),
            [&](const auto& object) { return cleared.contains(object.id); }), production->objects.end());
        context.census.objects.erase(std::remove_if(context.census.objects.begin(), context.census.objects.end(),
            [&](const auto& object) { return cleared.contains(object.id); }), context.census.objects.end());
    }
    if (!placed.structures.empty() || placed.clear_space_structures) planet->special_control = placed.special_control;
    planet->landed.insert(planet->landed.end(), placed.landed.begin(), placed.landed.end());
    production->objects.insert(production->objects.end(), placed.structures.begin(), placed.structures.end());
    context.census.objects.insert(context.census.objects.end(), placed.originals.begin(), placed.originals.end());
    context.census.objects.erase(std::remove_if(context.census.objects.begin(), context.census.objects.end(),
        [&](const auto& object) { return std::find(placed.cleared_structures.begin(), placed.cleared_structures.end(), object.id)
            != placed.cleared_structures.end(); }), context.census.objects.end());
    setup.movement->contents.insert(setup.movement->contents.end(), placed.contents.begin(), placed.contents.end());
    std::sort(setup.movement->contents.begin(), setup.movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    std::sort(context.census.objects.begin(), context.census.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    context.control = *setup.control; context.movement = *setup.movement;
    return core::Result<void>::success();
}
} // namespace eawr::campaign
