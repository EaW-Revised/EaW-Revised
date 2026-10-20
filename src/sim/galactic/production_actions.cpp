#include "session_impl.hpp"
#include "native_action_shadow_data.hpp"
#include "native_actor_change_checkpoint.hpp"
#include "native_force_validation.hpp"
#include "native_control_admission.hpp"
#include "native_production_admission.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/core/gc_profile.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <variant>

namespace eawr::sim::galactic {
using session_detail::invalid;
using session_detail::ordered_births;
using session_detail::validate_native_force;
void GalacticSession::Impl::record_actor(CensusObject row, NativeActorChange::Kind kind) {
    if (kind == NativeActorChange::Kind::created && row.combat_role == ActorCombatRole::unavailable
        && production && production->nested_creation) {
        const auto& types = production->nested_creation->types;
        const auto fact = std::lower_bound(types.begin(), types.end(), row.current_type,
            [](const auto& value, auto key) { return value.type < key; });
        if (fact != types.end() && fact->type == row.current_type && fact->combat_role != ActorCombatRole::unavailable) {
            row.combat_role = fact->combat_role; row.abilities = fact->abilities; row.garrison = false;
        }
    }
    const auto at = actor_records.find(row.id);
    if (kind == NativeActorChange::Kind::changed && at != actor_records.end() && at->second == row) return;
    if (ability_host && (at == actor_records.end() || at->second.owner != row.owner || at->second.planet != row.planet
        || at->second.current_type != row.current_type || at->second.dead != row.dead || at->second.delete_pending != row.delete_pending))
        ++ability_input_generation;
    actor_records.insert_or_assign(row.id, row);
    refresh_ability_actor(row);
    update_victory_actor(row); // WGS-24: sparse qualified lifecycle/current-type write.
    published_actor_records.reset();
    invalidate_observer_projection();
    dirty[static_cast<std::size_t>(StateSection::census)] = true;
    if (native_actor_changes_handler) pending_actor_changes.push_back({kind, row.id,
        kind == NativeActorChange::Kind::created ? row.generated_source : 0, {}, {}, {}});
}
void GalacticSession::Impl::relocate_actor(EntityId id, PlayerId owner, PlanetId planet, EntityId parent) {
    if (const auto at = actor_records.find(id); at != actor_records.end()) {
        auto row = at->second; row.owner = owner; row.planet = planet; row.container = parent;
        record_actor(row, NativeActorChange::Kind::changed);
    }
    if (movement_inputs) {
        const auto& rows = movement_inputs->contents;
        const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.container < key; });
        if (at != rows.end() && at->container == id)
            for (const auto& child : at->children) relocate_actor(child.id, owner, planet, id);
    }
}
void GalacticSession::Impl::record_fleet_shell(EntityId id, PlayerId owner, PlanetId planet) {
    // Without an actor owner (no census, no nested creation facts) a lone fleet-shell record would turn every
    // later combat-skip reader onto member records the session never had (#2880).
    if (!setup.census && !(production && production->nested_creation)) return;
    const auto type = production && production->nested_creation ? production->nested_creation->fleet_type : TypeId{};
    record_actor({id, type, owner, planet, planet, false, false, type, 0, ActorCombatRole::unavailable, {}, {}}, NativeActorChange::Kind::created);
}
void GalacticSession::Impl::lose_actor(EntityId id) {
    if (movement_inputs) {
        const auto& rows = movement_inputs->contents;
        const auto at = std::lower_bound(rows.begin(), rows.end(), id,
            [](const auto& row, auto key) { return row.container < key; });
        if (at != rows.end() && at->container == id)
            for (const auto& child : at->children) lose_actor(child.id);
    }
    if (!actor_records.erase(id)) return;
    lose_ability_actor(id);
    remove_victory_actor(id); // A loss is removal, never proof of an attributed leader kill.
    published_actor_records.reset(); dirty[static_cast<std::size_t>(StateSection::census)] = true;
    if (!native_actor_changes_handler) return;
    // #4039: a batch is delivered against the completed snapshot, which no longer holds this actor. Earlier
    // writes of it in the same batch (a generated fleet created and destroyed by one denied launch) are moot:
    // the owner never saw a birth to retire, and a retained actor only needs the loss.
    const auto stale = [id](const NativeActorChange& change) {
        return change.actor == id && change.kind != NativeActorChange::Kind::lost;
    };
    const bool born_here = std::any_of(pending_actor_changes.begin(), pending_actor_changes.end(),
        [id](const NativeActorChange& change) { return change.actor == id && change.kind == NativeActorChange::Kind::created; });
    std::erase_if(pending_actor_changes, stale);
    if (!born_here) pending_actor_changes.push_back({NativeActorChange::Kind::lost, id, 0, {}, {}, {}});
}
core::Result<void> GalacticSession::Impl::deliver_actor_changes() {
    if (pending_actor_changes.empty()) return core::Result<void>::success();
    auto changes = std::move(pending_actor_changes); pending_actor_changes.clear();
    if (!native_actor_changes_handler) return core::Result<void>::success();
    auto delivered = native_actor_changes_handler(*snapshot, changes);
    if (!delivered) failed = true;
    return delivered;
}
core::Result<void> GalacticSession::change_actor_current_type(EntityId actor, TypeId type) {
    auto& impl = *impl_;
    const auto at = impl.actor_records.find(actor);
    if (impl.failed || !type || !impl.setup.census || at == impl.actor_records.end()
        || std::none_of(impl.setup.census->types.begin(), impl.setup.census->types.end(),
            [&](const auto& row) { return row.type == type; }))
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "current_representation_type_change_unqualified"));
    auto row = at->second; row.current_type = type;
    // A type-owner change must supply fresh dispatch facts; never keep a former
    // carrier role or silently recreate its archived live ability state.
    row.combat_role = ActorCombatRole::unavailable;
    if (impl.production && impl.production->nested_creation) {
        const auto& types = impl.production->nested_creation->types;
        const auto fact = std::lower_bound(types.begin(), types.end(), type,
            [](const auto& value, auto key) { return value.type < key; });
        if (fact != types.end() && fact->type == type) row.combat_role = fact->combat_role;
    }
    // The live ability registry/service state follows representation writes. Retain
    // this qualified transition before mutation so fresh script hashes see it too.
    if (impl.ability_host && !impl.native_replaying) {
        std::vector<std::uint8_t> bytes; detail::put_u8(bytes, 1);
        detail::put_u64(bytes, actor); detail::put_u64(bytes, type); detail::put_u64(bytes, at->second.current_type);
        auto recorded = append_native_effect(NativeEffectKind::ability_lifecycle, std::move(bytes));
        if (!recorded) return recorded;
    }
    impl.record_actor(row, NativeActorChange::Kind::changed);
    impl.refresh_containing_fleet_combat_skip(actor);
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return impl.deliver_actor_changes();
}
core::Result<void> GalacticSession::apply_actor_stealth(EntityId owner, EntityId target, math::Fixed chance, bool apply) {
    auto& impl = *impl_;
    const auto at = impl.actor_records.find(owner);
    if (impl.failed || !owner || (target && target != owner) || at == impl.actor_records.end()
        || at->second.combat_role == ActorCombatRole::unavailable || !at->second.abilities.present)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "actor_stealth_effect_unqualified"));
    auto row = at->second;
    if (apply) {
        row.abilities.combat_skip = true;
        row.abilities.evade_chance = std::clamp(chance, math::Fixed{}, math::Fixed::from_raw(math::Fixed::scale));
        row.abilities.self_target = true;
    } else {
        row.abilities.evade_chance = {};
        row.abilities.combat_skip = false;
        row.abilities.self_target = false;
    }
    impl.record_actor(row, NativeActorChange::Kind::changed);
    impl.refresh_containing_fleet_combat_skip(owner);
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return impl.deliver_actor_changes();
}
void GalacticSession::Impl::refresh_containing_fleet_combat_skip(EntityId actor) {
    auto parent = actor;
    for (unsigned depth = 0; parent && depth < 32; ++depth) {
        const auto at_parent = actor_records.find(parent);
        if (at_parent == actor_records.end()) break;
        if (fleets.contains(parent)) {
            refresh_fleet_combat_skip(parent); break;
        }
        parent = at_parent->second.container;
    }
}
void GalacticSession::Impl::refresh_fleet_combat_skip(FleetId id) {
    if (!production || !production->fleet_blockers || !fleets.contains(id)) return;
    auto& blockers = *production->fleet_blockers;
    const auto blocker = std::find_if(blockers.begin(), blockers.end(), [&](const auto& row) { return row.fleet == id; });
    if (blocker == blockers.end()) return;
    const auto& value = registry.get<session_detail::FleetData>(fleets.at(id));
    auto skip = fleet_combat_skip(Fleet{id, value.owner, value.orbit, value.members}, actor_inputs(), movement_inputs.get());
    if (skip && blocker->skips_space_combat != skip.value()) {
        blocker->skips_space_combat = skip.value();
        dirty[static_cast<std::size_t>(StateSection::production)] = true;
    }
}
core::Result<void> GalacticSession::set_actor_garrison(EntityId id, bool value) {
    auto& impl = *impl_;
    const auto at = impl.actor_records.find(id);
    if (impl.failed || at == impl.actor_records.end())
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "actor_garrison_write_unqualified"));
    auto row = at->second; row.garrison = value;
    impl.record_actor(row, NativeActorChange::Kind::changed);
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return impl.deliver_actor_changes();
}
core::Result<void> GalacticSession::restore_actor_representations(std::span<const std::uint8_t> bytes) {
    const auto refuse = [] { return core::Result<void>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "current_representation_restore_unqualified")); };
    auto& impl = *impl_;
    if (impl.failed || !impl.setup.census || bytes.size() < 4 || !impl.pending_actor_changes.empty()) return refuse();
    std::uint32_t size{};
    for (unsigned i = 0; i < 4; ++i) size |= std::uint32_t{bytes[i]} << (i * 8);
    if (size > bytes.size() - 4 || bytes.size() - 4 - size < 4) return refuse();
    const auto totals = bytes.subspan(4 + size);
    std::uint32_t count{};
    for (unsigned i = 0; i < 4; ++i) count |= std::uint32_t{totals[i]} << (i * 8);
    if (count > galactic_max_players || totals.size() != 4 + std::size_t{count} * 29) return refuse();
    for (std::uint32_t i = 0; i < count; ++i) if (totals[4 + std::size_t{i} * 29 + 28] > 1) return refuse();
    auto census = detail::decode_census(bytes.subspan(4, size));
    if (!census || census->types != impl.setup.census->types || census->players != impl.setup.census->players
        || census->objects.size() != impl.actor_records.size()) return refuse();
    auto existing = impl.actor_records.begin();
    for (const auto& row : census->objects) {
        const auto& before = existing->second;
        if (row.id != existing->first || row.original_type != before.original_type || row.owner != before.owner
            || row.planet != before.planet || row.container != before.container || row.dead != before.dead
            || row.delete_pending != before.delete_pending || row.generated_source == row.id
            || row.generated_source != before.generated_source
            || row.generated_source >= impl.next_id
            || (impl.ability_host && row.current_type != before.current_type) // live bindings must have replayed before script hashes
            || (!row.current_type && before.current_type)
            || (row.current_type && std::none_of(census->types.begin(), census->types.end(),
                [&](const auto& type) { return type.type == row.current_type; }))) return refuse();
        ++existing;
    }
    // All references validate before mutation; restoration never dispatches births.
    for (const auto& row : census->objects) impl.actor_records.at(row.id) = row;
    impl.published_actor_records.reset();
    impl.invalidate_observer_projection();
    impl.dirty[static_cast<std::size_t>(StateSection::census)] = true;
    for (const auto& [id, handle] : impl.fleets) { (void)handle; impl.refresh_fleet_combat_skip(id); }
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return core::Result<void>::success();
}
namespace session_detail {
core::Result<std::vector<GalacticSession::NativeActorChange>> ordered_births(std::span<const CensusObject> rows) {
    using Change = GalacticSession::NativeActorChange;
    std::map<EntityId, const CensusObject*> records;
    std::map<EntityId, std::uint8_t> visited;
    for (const auto& row : rows) records.emplace(row.id, &row);
    std::vector<Change> changes; changes.reserve(rows.size());
    const auto visit = [&](auto&& self, EntityId id, unsigned depth) -> bool {
        if (!records.contains(id)) return true; // an already constructed source
        auto& mark = visited[id];
        if (mark == 2) return true;
        if (mark == 1 || depth > 32) return false;
        mark = 1;
        const auto& row = *records.at(id);
        if (row.generated_source && !self(self, row.generated_source, depth + 1)) return false;
        mark = 2; changes.push_back({Change::Kind::created, id, row.generated_source, {}, {}, {}});
        return true;
    };
    for (const auto& row : rows) if (!visit(visit, row.id, 0))
        return invalid<std::vector<Change>>("current_representation_generated_source_cycle");
    return core::Result<std::vector<Change>>::success(std::move(changes));
}
core::Result<std::vector<EntityId>> validate_native_force(const GalacticSession::NativeProductionRequest& request,
    const GalacticSession::NativeForcePlacement& placement, EntityId frontier, std::size_t live_objects,
    const NestedCreationSetup& creation, const std::function<bool(EntityId)>& existing_source) {
    using Result = core::Result<std::vector<EntityId>>;
    if (placement.next_id < frontier || placement.next_id >= std::numeric_limits<EntityId>::max()
        || placement.next_id - frontier > galactic_max_objects
        || live_objects > galactic_max_objects - (placement.next_id - frontier))
        return invalid<std::vector<EntityId>>("native creation exceeds the object frontier");
    std::set<EntityId> identities;
    std::map<EntityId, const CensusObject*> records;
    std::vector<EntityId> actors;
    for (const auto& object : placement.originals) {
        const auto bound = [&](TypeId type) {
            const auto at = std::lower_bound(creation.types.begin(), creation.types.end(), type,
                [](const auto& row, auto id) { return row.type < id; });
            return at != creation.types.end() && at->type == type;
        };
        if (object.id < frontier || object.id >= placement.next_id || !identities.insert(object.id).second
            || object.owner != request.player || object.planet != request.planet
            || !bound(object.original_type) || !bound(object.current_type)
            || object.combat_role > ActorCombatRole::squadron
            || object.abilities.evade_chance.raw() < 0 || object.abilities.evade_chance.raw() > math::Fixed::scale
            || (!object.abilities.present && (object.abilities.combat_skip || object.abilities.self_target || object.abilities.evade_chance.raw())))
            return invalid<std::vector<EntityId>>("native creation returned an invalid original identity");
        if (object.original_type == request.type) actors.push_back(object.id);
        records.emplace(object.id, &object);
    }
    if (identities.size() != placement.next_id - frontier)
        return invalid<std::vector<EntityId>>("native creation must describe every allocated identity");
    for (const auto& object : placement.originals)
        if (object.generated_source && ((!identities.contains(object.generated_source) && !existing_source(object.generated_source)) || object.generated_source == object.id))
            return invalid<std::vector<EntityId>>("current_representation_generated_source_unqualified");
    if (auto ordered = ordered_births(placement.originals); !ordered) return Result::failure(ordered.error());
    for (const auto& object : placement.originals) {
        auto parent = object.container; unsigned depth{};
        while (records.contains(parent)) {
            if (parent == object.id || ++depth > 32)
                return invalid<std::vector<EntityId>>("current_representation_parent_cycle");
            parent = records.at(parent)->container;
        }
        if (parent && parent != request.planet)
            return invalid<std::vector<EntityId>>("current_representation_parent_unqualified");
    }
    const auto matches = [&](const FleetMember& member, EntityId parent) {
        if (!records.contains(member.id) || records.at(member.id)->container != parent) return false;
        const auto& actor = *records.at(member.id);
        if (actor.current_type == member.type) return true;
        const auto role = std::lower_bound(creation.types.begin(), creation.types.end(), member.type,
            [](const auto& row, auto id) { return row.type < id; });
        return role != creation.types.end() && role->type == member.type && role->role == NestedCreationRole::squadron
            && actor.original_type == member.type && actor.current_type == creation.squadron_type;
    };
    for (const auto& fleet : placement.fleets) {
        if (!identities.contains(fleet.id) || fleet.owner != request.player || fleet.orbit != request.planet)
            return invalid<std::vector<EntityId>>("native creation returned an invalid fleet");
        if (records.at(fleet.id)->current_type != creation.fleet_type || records.at(fleet.id)->container != request.planet)
            return invalid<std::vector<EntityId>>("current_representation_fleet_constructor_unqualified");
        for (const auto& member : fleet.members) if (!matches(member, fleet.id))
            return invalid<std::vector<EntityId>>("native creation returned an unresolved member");
    }
    for (const auto& row : placement.contents) {
        if (!identities.contains(row.container)) return invalid<std::vector<EntityId>>("native creation returned an unresolved container");
        for (const auto& child : row.children) if (!matches(child, row.container))
            return invalid<std::vector<EntityId>>("native creation returned an unresolved child");
    }
    for (const auto& row : placement.landed) if (!matches({row.id, row.type}, request.planet) || row.owner != request.player)
        return invalid<std::vector<EntityId>>("native creation returned an invalid landed actor");
    for (const auto& row : placement.structures) if (!matches({row.id, row.type}, request.planet) || row.owner != request.player)
        return invalid<std::vector<EntityId>>("native creation returned an invalid structure");
    std::sort(actors.begin(), actors.end());
    return Result::success(std::move(actors));
}
}
void GalacticSession::set_native_production_handler(NativeProductionHandler handler) {
    impl_->native_production_handler = std::move(handler);
}
core::Result<session_detail::NativeProductionAdmission> session_detail::prepare_native_production(
    const GalacticSession::NativeProductionRequest& request, const ProductionSetup& state, const PlayerEconomySetup& economy,
    std::span<const Planet> planets, std::span<const Fleet> fleets, std::span<const PopulationTotal> population, bool census,
    const IncomeSetup* sources, EntityId frontier, std::size_t live_objects, std::span<const TypeId> local_unlocks) {
    using Result = core::Result<NativeProductionAdmission>;
    auto planet = std::lower_bound(state.planets.begin(), state.planets.end(), request.planet,
        [](const auto& row, auto id) { return row.planet < id; });
    auto player = std::lower_bound(state.players.begin(), state.players.end(), request.player,
        [](const auto& row, auto id) { return row.player < id; });
    if (planet == state.planets.end() || planet->planet != request.planet || player == state.players.end()
        || player->player != request.player || player->human)
        return invalid<NativeProductionAdmission>("native production requires its AI player and planet");
    const auto index = static_cast<std::size_t>(player - state.players.begin());
    auto original = std::lower_bound(planet->offers.begin(), planet->offers.end(), request.type,
        [](const auto& row, auto id) { return row.type < id; });
    if (original == planet->offers.end() || original->type != request.type) return Result::success({});
    auto prepared = prepared_production_offer(*original, sources,
        planets, request.planet, request.player);
    if (!prepared) return Result::failure(prepared.error());
    const auto& offer = prepared.value();
    // The native request has its own ledger payment route. Reuse content,
    // ownership and population gates without pretending it is a human order.
    auto admission_player = *player; admission_player.human = true; admission_player.pay_as_you_go = true;
    const auto owner = std::lower_bound(planets.begin(), planets.end(), request.planet, [](const auto& row, auto id) { return row.id < id; })->owner;
    const auto admission = production_admission({state, *planet, admission_player, economy.players[index],
        fleets, owner, census ? &population[index] : nullptr,
        frontier, live_objects, nullptr, local_unlocks}, &offer, offer.domain);
    return Result::success({admission, static_cast<std::size_t>(planet - state.planets.begin()), index, offer, owner});
}
core::Result<GalacticSession::NativeProductionReceipt> GalacticSession::admit_native_production(const NativeProductionRequest& request, const PartitionExecutor& executor) {
    using Result = core::Result<NativeProductionReceipt>;
    auto& impl = *impl_;
    if (impl.failed || !impl.production || !impl.economy || !impl.planets.contains(request.planet))
        return invalid<NativeProductionReceipt>("native production requires a live producer");
    if (auto* shadow = native_shadow()) return stage_native_production(*shadow, request, executor);
    if (auto refreshed = impl.refresh_population(executor); !refreshed) return Result::failure(refreshed.error());
    auto& state = *impl.production;
    const auto sources = impl.income ? std::optional(impl.live_income_inputs()) : std::nullopt;
    auto checked = session_detail::prepare_native_production(request, state, *impl.economy,
        impl.sorted_planets(), impl.sorted_fleets(), impl.population, impl.setup.census.has_value(),
        sources ? &*sources : nullptr, impl.next_id, impl.live_object_count, impl.corruption_local_unlocks(request.planet, request.player));
    if (!checked) return Result::failure(checked.error());
    const auto& prepared = checked.value();
    if (!prepared.offer) return Result::success({});
    const auto& offer = *prepared.offer;
    const auto owner = prepared.owner;
    auto planet = state.planets.begin() + static_cast<std::ptrdiff_t>(prepared.planet_index);
    if (prepared.admission != ProductionAdmission::allowed) return Result::success({prepared.admission, 0, offer.price});
    if (!impl.native_production_handler) return invalid<NativeProductionReceipt>("native production requires its ledger handler");
    if (!request.mark_cost || request.mark_cost->raw() < 0 || request.mark_cost->raw() > (std::int64_t{1} << 48)
        || request.mark_cost->raw() % math::Fixed::scale != 0)
        return invalid<NativeProductionReceipt>("native mark requires the resolved null-location integer type cost");
    const auto id = impl.next_id;
    if (auto marked = impl.native_production_handler({NativeProductionNotice::Kind::marked, request.player, id, *request.mark_cost, offer.price, offer.seconds, owner}); !marked)
        return Result::failure(marked.error());
    auto& queue = planet->queues[static_cast<std::size_t>(offer.domain)];
    queue.push_back({offer.type, request.player, id, offer.price, offer.seconds, offer.seconds,
        impl.clock.galactic_frame, production_frames(offer.seconds).value(), true});
    ++impl.next_id;
    impl.active_producers.insert(static_cast<std::size_t>(planet - state.planets.begin()));
    impl.population_stale = true;
    impl.dirty[0] = impl.dirty[8] = true;
    const auto old = impl.snapshot->events();
    std::vector<GalacticEvent> events(old.begin(), old.end());
    events.push_back({impl.completed_tick, GalacticEventKind::production_started, {}, request.planet, id, request.type, GalacticRejectReason::none});
    impl.snapshot = impl.publish_native_snapshot(std::move(events));
    return Result::success({ProductionAdmission::allowed, id, offer.price});
}

core::Result<std::vector<EntityId>> GalacticSession::create_native_force(const NativeProductionRequest& request,
    const NativeForceBuilder& builder) {
    using Result = core::Result<std::vector<EntityId>>;
    auto& impl = *impl_;
    if (impl.failed || !builder || !impl.production || !impl.production->nested_creation || !impl.control || !impl.movement_inputs || !impl.setup.census
        || !impl.planets.contains(request.planet) || !request.type
        || std::none_of(impl.setup.players.begin(), impl.setup.players.end(), [&](const auto& p) { return p.player_id == request.player; }))
        return invalid<std::vector<EntityId>>("native creation requires qualified live placement services");
    if (auto* shadow = native_shadow()) return stage_native_force(*shadow, request, builder);
    auto staged = builder(*impl.snapshot, impl.next_id);
    if (!staged) return Result::failure(staged.error());
    const auto& placement = staged.value();
    // Prepared replay borrows final staged columns; source validation must stay monotonic (existence only).
    auto actors = validate_native_force(request, placement, impl.next_id, impl.live_object_count,
        *impl.production->nested_creation, [&](EntityId id) { return bool(impl.snapshot->current_type(id)); });
    if (!actors) return actors;
    auto movement = std::make_shared<MovementSetup>(*impl.movement_inputs);
    if (auto applied = impl.apply_native_force(request, placement, *movement); !applied)
        return Result::failure(applied.error());
    std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    impl.movement_inputs = std::move(movement);
    const auto old = impl.snapshot->events();
    impl.snapshot = impl.publish_native_snapshot({old.begin(), old.end()});
    if (auto delivered = impl.deliver_actor_changes(); !delivered) return Result::failure(delivered.error());
    return actors;
}
core::Result<void> GalacticSession::Impl::apply_native_force(const NativeProductionRequest& request, const NativeForcePlacement& placement, MovementSetup& movement) {
    auto& impl = *this;
    const auto at = std::lower_bound(impl.production->planets.begin(), impl.production->planets.end(), request.planet,
        [](const auto& row, auto id) { return row.planet < id; });
    const auto index = static_cast<std::size_t>(at - impl.production->planets.begin());
    // WGA-158: magic removes scheduled-spawn records, never paid producer entries.
    // The separate scheduled-spawn-list owner is not yet installed in this session.
    impl.next_id = placement.next_id;
    impl.live_object_count += placement.originals.size();
    for (const auto& fleet : placement.fleets) impl.insert(fleet);
    movement.contents.insert(movement.contents.end(), placement.contents.begin(), placement.contents.end());
    auto& presence = impl.control->planets[index];
    presence.landed.insert(presence.landed.end(), placement.landed.begin(), placement.landed.end());
    at->landed_transports = static_cast<std::int64_t>(presence.landed.size());
    at->objects.insert(at->objects.end(), placement.structures.begin(), placement.structures.end());
    if (!placement.structures.empty()) presence.special_control = placement.special_control;
    if (placement.ground_base) { presence.ground_base = *placement.ground_base; at->ground_level = placement.ground_base->level; }
    if (placement.starbase) {
        presence.starbase = *placement.starbase; at->space_level = placement.starbase->level;
        at->starbase_owner = placement.starbase->owner; at->starbase_type = placement.starbase_type;
    }
    impl.production_originals.insert(impl.production_originals.end(), placement.originals.begin(), placement.originals.end());
    // CRP-02: actual sources precede their generated children, regardless of ID order.
    const auto births = ordered_births(placement.originals).value(); // validated before world mutation
    std::map<EntityId, const CensusObject*> records;
    for (const auto& row : placement.originals) records.emplace(row.id, &row);
    for (const auto& birth : births) {
        impl.record_actor(*records.at(birth.actor), NativeActorChange::Kind::created);
    }
    impl.published_production_originals.reset();
    if (impl.production->fleet_blockers) for (const auto& fleet : placement.fleets) {
        // CRP-02: construction has now installed the actual actor flags and
        // payload tree. Qualified live predicates supersede original-type
        // defaults, including recursive stealth through container shells.
        const auto actual = fleet_combat_skip(fleet, impl.actor_inputs(), &movement);
        if (!actual) {
            // Constructor output is already committed; reject further service
            // rather than exposing a partially constructed session as playable.
            impl.failed = true;
            return core::Result<void>::failure(actual.error());
        }
        impl.production->fleet_blockers->push_back({fleet.id, false, actual.value(), true});
    }
    impl.publish_surface_arrivals(request.planet, placement.landed);
    impl.population_stale = true;
    impl.invalidate_observer_projection();
    for (const auto section : {StateSection::session, StateSection::fleets, StateSection::fleet_control,
        StateSection::movement, StateSection::control, StateSection::census, StateSection::production})
        impl.dirty[static_cast<std::size_t>(section)] = true;
    return core::Result<void>::success();
}

namespace {
thread_local std::weak_ptr<void> native_action_barrier_context;
}
struct GalacticSession::NativeActionBarrier::State {
    GalacticSession* world{};
    const void* owner{};
    std::vector<PlayerId> players;
    std::vector<std::unique_ptr<NativeActionShadow>> segments;
    std::size_t max_requests{}, actions{}, journal_entries{};
    std::optional<NativeEffectPhase> last_phase;
    bool prelude{true}, tail{}, finished{};
    ~State() { while (!segments.empty()) segments.pop_back(); }
};
GalacticSession::NativeActionBarrier::NativeActionBarrier(std::shared_ptr<State> state) : state_(std::move(state)) {}
GalacticSession::NativeActionBarrier::~NativeActionBarrier() {
    if (native_action_barrier_context.lock().get() == state_.get()) native_action_barrier_context.reset();
    if (state_ && !state_->finished) {
        state_->world->impl_->failed = true;
        state_->world->impl_->native_barrier_active.store(false, std::memory_order_release);
    }
}
GalacticSession::NativeActionBarrier::State* GalacticSession::native_barrier() const noexcept {
    const auto context = native_action_barrier_context.lock();
    auto* state = static_cast<NativeActionBarrier::State*>(context.get());
    return state && state->owner == impl_.get() && !state->finished ? state : nullptr;
}
GalacticSession::NativeActionShadow* GalacticSession::native_shadow() const noexcept {
    auto* state = native_barrier();
    assert((state || !impl_->native_barrier_active.load(std::memory_order_acquire))
        && "native barrier reads and writes require its decision thread");
    return state && !state->segments.empty() ? state->segments.back().get() : nullptr;
}
core::Result<std::unique_ptr<GalacticSession::NativeActionBarrier>> GalacticSession::begin_native_barrier(
    std::span<const PlayerId> players, std::size_t max_requests) {
    using Result = core::Result<std::unique_ptr<NativeActionBarrier>>;
    if (!native_action_barrier_context.expired() || impl_->native_barrier_active.load(std::memory_order_acquire)
        || impl_->failed || !max_requests || max_requests > galactic_max_commands
        || !std::is_sorted(players.begin(), players.end()) || std::adjacent_find(players.begin(), players.end()) != players.end())
        return invalid<std::unique_ptr<NativeActionBarrier>>("native barrier requires one live owner, ordered players and bounded requests");
    if (players.empty()) return Result::success({});
    auto root = begin_native_actions(players.front(), std::min<std::size_t>(max_requests, galactic_max_objects));
    if (!root) return Result::failure(root.error());
    root.value().data_->movement_listener = impl_->native_movement_listener; root.value().data_->prelude = true;
    root.value().data_->max_requests = max_requests;
    root.value().data_->invasion_records = impl_->native_invasions.size(); root.value().data_->invasion_transports = impl_->native_invasion_transport_count;
    root.value().data_->next_invasion = impl_->next_native_invasion; root.value().data_->invasions_enabled = impl_->native_invasions_enabled;
    auto state = std::make_shared<NativeActionBarrier::State>();
    state->world = this; state->owner = impl_.get(); state->players.assign(players.begin(), players.end()); state->max_requests = max_requests;
    state->segments.push_back(std::make_unique<NativeActionShadow>(std::move(root).value()));
    if (!impl_->native_effects.empty() && impl_->native_effects.back().frontier == impl_->completed_tick)
        state->last_phase = impl_->native_effects.back().phase;
    auto barrier = std::unique_ptr<NativeActionBarrier>(new NativeActionBarrier(std::move(state)));
    impl_->native_barrier_active.store(true, std::memory_order_release);
    native_action_barrier_context = barrier->state_;
    return Result::success(std::move(barrier));
}
core::Result<void> GalacticSession::NativeActionBarrier::begin_players() {
    if (!state_ || state_->finished || native_action_barrier_context.lock().get() != state_.get())
        return invalid<void>("native player phase requires its active barrier and owning thread");
    if (!state_->prelude) return core::Result<void>::success();
    state_->prelude = false;
    return state_->world->advance_native_segment(false, 0);
}
core::Result<void> GalacticSession::NativeActionBarrier::select_player(PlayerId player) {
    if (!state_ || state_->finished || native_action_barrier_context.lock().get() != state_.get()
        || !std::binary_search(state_->players.begin(), state_->players.end(), player))
        return invalid<void>("native barrier selection requires its admitted player and owning thread");
    if (auto started = begin_players(); !started) return started;
    if (state_->tail || state_->segments.back()->player() == player) return core::Result<void>::success();
    auto next = state_->segments.back()->for_player(player, std::min<std::size_t>(state_->max_requests, galactic_max_objects));
    if (!next) return core::Result<void>::failure(next.error());
    next.value().data_->max_requests = state_->max_requests;
    state_->segments.push_back(std::make_unique<NativeActionShadow>(std::move(next).value()));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::NativeActionBarrier::begin_tail() {
    if (!state_ || state_->finished || native_action_barrier_context.lock().get() != state_.get())
        return invalid<void>("native tail requires its active barrier and owning thread");
    if (state_->tail) return core::Result<void>::success();
    if (auto started = begin_players(); !started) return started;
    state_->tail = true;
    return state_->world->advance_native_segment(true, 0);
}
core::Result<void> GalacticSession::NativeActionBarrier::commit() {
    gc_profile::Scope profile_scope(gc_profile::Phase::barrier_commit);
    if (!state_ || state_->finished || native_action_barrier_context.lock().get() != state_.get())
        return invalid<void>("native commit requires its active barrier and owning thread");
    std::vector<NativeActionShadow> layers; layers.reserve(state_->segments.size());
    for (const auto& segment : state_->segments) layers.push_back(*segment);
    native_action_barrier_context.reset(); state_->finished = true;
    state_->world->impl_->native_barrier_active.store(false, std::memory_order_release);
    auto committed = state_->world->commit_native_actions(layers, true, true);
    if (!committed) state_->world->impl_->failed = true;
    return committed;
}
GalacticSession::NativeActionBarrier::Work GalacticSession::NativeActionBarrier::work() const noexcept {
    Work result;
    if (!state_) return result;
    result.layers = state_->segments.size();
    for (const auto& segment : state_->segments) result.read_checkpoints += segment->data_->read_checkpoints;
    return result;
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::continue_native_actions(
    const NativeActionShadow& previous, std::size_t max_requests, bool immediate, bool tail) const {
    if (impl_->failed || previous.data_->owner != impl_.get() || !previous.player() || !max_requests || max_requests > galactic_max_objects)
        return invalid<NativeActionShadow>("native continuation requires its player owner and bounded requests");
    auto data = std::make_shared<NativeActionShadow::Data>(); const auto& before = *previous.data_;
    data->owner = before.owner; data->base = before.base; data->parent = previous.data_; data->player = before.player;
    data->inherit_reads(previous.data_);
    data->first_id = data->next_id = before.next_id; data->live_objects = before.live_objects; data->max_requests = max_requests;
    data->can_create = before.can_create; data->continuation = true; data->prelude = native_barrier() && native_barrier()->prelude; data->immediate_batch = immediate; data->tail = tail;
    data->graph = before.graph; data->movement = before.movement; data->observer_generation = before.observer_generation;
    data->population_schema = before.population_schema; data->income_schema = before.income_schema; data->fiscal_schema = before.fiscal_schema;
    data->base_income = before.base_income; data->income_edges = before.income_edges; data->movement_listener = before.movement_listener;
    data->production_projection = before.production_projection; data->population_projection = before.population_projection;
    data->population_stale = before.population_stale;
    data->invasion_records = before.invasion_records; data->invasion_transports = before.invasion_transports;
    data->next_invasion = before.next_invasion; data->invasions_enabled = before.invasions_enabled;
    data->publication = before.view();
    if (data->publication->victory()) data->victory = std::shared_ptr<const VictoryState>(data->publication, &*data->publication->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(data)));
}
core::Result<void> GalacticSession::advance_native_segment(bool tail, std::size_t actions) {
    auto* state = native_barrier();
    if (!state || actions > state->max_requests - std::min(state->actions, state->max_requests)) {
        impl_->failed = true;
        return invalid<void>("native barrier exhausted its ordered action bound");
    }
    auto& previous = *state->segments.back();
    // Ordinary writes reuse the current player's sparse frontier. Only an
    // immediate transaction or an explicit phase boundary freezes a new layer.
    if (!previous.data_->immediate_batch && previous.data_->prelude == state->prelude
        && previous.data_->tail == (tail || state->tail)) {
        state->actions += actions;
        return core::Result<void>::success();
    }
    auto next = continue_native_actions(previous, std::min<std::size_t>(state->max_requests, galactic_max_objects), false, tail || state->tail);
    if (!next) return core::Result<void>::failure(next.error());
    next.value().data_->max_requests = state->max_requests;
    state->actions += actions; state->segments.push_back(std::make_unique<NativeActionShadow>(std::move(next).value()));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::deliver_native_shadow_changes(const NativeActionShadow& shadow, std::size_t first) {
    const auto changes = shadow.actor_changes().subspan(first);
    if (changes.empty() || !impl_->native_actor_changes_handler) return core::Result<void>::success();
    auto delivered = impl_->native_actor_changes_handler(*shadow.snapshot(), changes);
    if (!delivered) impl_->failed = true;
    return delivered;
}
core::Result<void> GalacticSession::publish_native_batch(const NativeActionShadow& shadow, bool actors_prepared) {
    auto* state = native_barrier(); const auto* previous = native_shadow();
    if (!state || !previous || !shadow.data_->immediate_batch || !shadow.data_->continuation
        || shadow.data_->owner != impl_.get() || shadow.data_->base != impl_->snapshot
        || shadow.data_->parent != previous->data_ || shadow.data_->first_id != previous->next_entity_id()
        || shadow.data_->player != previous->player() || shadow.data_->tail != state->tail || shadow.data_->prelude != state->prelude)
        return invalid<void>("immediate native batch is stale or belongs to another barrier");
    const auto requests = std::max<std::size_t>(1, shadow.work().requests);
    if (requests > state->max_requests - std::min(state->actions, state->max_requests)) {
        impl_->failed = true;
        return invalid<void>("immediate native batch exhausted its barrier retention bound");
    }
    // The ordinary immediate owner always publishes, even for an empty batch.
    shadow.data_->publication.reset();
    state->segments.push_back(std::make_unique<NativeActionShadow>(shadow));
    if (auto continued = advance_native_segment(false, requests); !continued) return continued;
    return actors_prepared ? core::Result<void>::success() : deliver_native_shadow_changes(shadow, 0);
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::stage_native_fleet(
    NativeActionShadow& shadow, PlayerId player, const CommandPayload& payload, bool pending) {
    const auto first = shadow.actor_changes().size();
    auto result = shadow.admit_fleet(player, payload, pending);
    if (!result) return result;
    if (auto saved = impl_->journal_native_receipt(*this, NativeEffectKind::fleet, player, 0, 0, pending, &payload, result.value()); !saved)
        return core::Result<NativeFleetReceipt>::failure(saved.error());
    if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered) return core::Result<NativeFleetReceipt>::failure(delivered.error());
    if (auto continued = advance_native_segment(); !continued) return core::Result<NativeFleetReceipt>::failure(continued.error());
    return result;
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::stage_native_launch(
    NativeActionShadow& shadow, PlayerId player, EntityId actor, FleetId target, bool pending) {
    const auto first = shadow.actor_changes().size();
    auto result = shadow.launch_transport(player, actor, target, pending);
    if (!result) return result;
    if (shadow.data_->last_admission_ran) {
        if (auto saved = impl_->journal_native_receipt(*this, NativeEffectKind::launch, player, actor, target, pending, nullptr, result.value()); !saved)
            return core::Result<NativeFleetReceipt>::failure(saved.error());
        if (result.value().admitted) if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered)
            return core::Result<NativeFleetReceipt>::failure(delivered.error());
    }
    if (auto continued = advance_native_segment(); !continued) return core::Result<NativeFleetReceipt>::failure(continued.error());
    return result;
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::stage_native_land(
    NativeActionShadow& shadow, PlayerId player, FleetId fleet, bool pending) {
    const auto first = shadow.actor_changes().size();
    auto result = shadow.land_transports(player, fleet, pending);
    if (!result) return result;
    if (shadow.data_->last_published) {
        if (auto saved = impl_->journal_native_receipt(*this, NativeEffectKind::land, player, fleet, 0, pending, nullptr, result.value()); !saved)
            return core::Result<NativeFleetReceipt>::failure(saved.error());
        if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered) return core::Result<NativeFleetReceipt>::failure(delivered.error());
    }
    if (auto continued = advance_native_segment(); !continued) return core::Result<NativeFleetReceipt>::failure(continued.error());
    return result;
}
core::Result<void> GalacticSession::stage_native_orbit_recheck(NativeActionShadow& shadow, PlayerId player, FleetId fleet) {
    const auto first = shadow.actor_changes().size();
    if (auto result = shadow.recheck_orbit(player, fleet); !result) return result;
    if (auto saved = impl_->journal_native_receipt(*this, NativeEffectKind::orbit_recheck,
        player, fleet, 0, false, nullptr, {true, fleet}); !saved) return saved;
    if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered) return delivered;
    return advance_native_segment();
}
core::Result<GalacticSession::NativeTransportLandingReceipt> GalacticSession::stage_native_actor_land(
    NativeActionShadow& shadow, PlayerId player, EntityId actor, PlanetId planet, bool pending, std::optional<std::uint32_t> live_landed) {
    const auto first = shadow.actor_changes().size();
    auto result = shadow.land_transport(player, actor, planet, pending, live_landed);
    if (!result) return result;
    if (auto saved = impl_->journal_native_receipt(*this, NativeEffectKind::actor_land, player, actor, planet, pending, nullptr, result.value().event); !saved)
        return core::Result<NativeTransportLandingReceipt>::failure(saved.error());
    if (shadow.data_->last_published) if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered)
        return core::Result<NativeTransportLandingReceipt>::failure(delivered.error());
    if (auto continued = advance_native_segment(); !continued) return core::Result<NativeTransportLandingReceipt>::failure(continued.error());
    return result;
}
core::Result<GalacticSession::NativeProductionReceipt> GalacticSession::stage_native_production(
    NativeActionShadow& shadow, const NativeProductionRequest& request, const PartitionExecutor& executor) {
    auto result = shadow.admit_production(request, executor, impl_->native_production_handler, impl_->corruption_local_unlocks(request.planet, request.player));
    if (!result) return result;
    if (auto continued = advance_native_segment(); !continued) return core::Result<NativeProductionReceipt>::failure(continued.error());
    return result;
}
core::Result<math::Fixed> GalacticSession::stage_native_credits(
    NativeActionShadow& shadow, PlayerId player, math::Fixed delta, math::Fixed multiplier, bool advance, bool record) {
    const auto before = shadow.credits(player);
    if (!before || !shadow.data_->base->player_economy()) return invalid<math::Fixed>("credit addition names a missing player");
    const auto& accounts = shadow.data_->base->player_economy()->players;
    const auto at = std::lower_bound(accounts.begin(), accounts.end(), player, [](const auto& row, auto id) { return row.player < id; });
    auto balance = adjusted_credits(*before, delta, multiplier, at->credit_cap);
    if (!balance) return balance;
    auto actual = math::subtract(balance.value(), *before);
    if (!actual) return actual;
    if (impl_->credit_notification_handler) {
        auto notified = impl_->credit_notification_handler({CreditNotification::Kind::windfall, player, balance.value(), actual.value(), {}});
        if (!notified) return core::Result<math::Fixed>::failure(notified.error());
    }
    const auto suppressed = shadow.data_->suppress_cash_request; shadow.data_->suppress_cash_request = !record;
    auto result = shadow.add_credits(player, delta, multiplier);
    shadow.data_->suppress_cash_request = suppressed;
    if (!result) return result;
    if (advance) if (auto continued = advance_native_segment(); !continued) return core::Result<math::Fixed>::failure(continued.error());
    return result;
}
core::Result<void> GalacticSession::stage_native_production_finish(NativeActionShadow& shadow, EntityId build, bool salvage, math::Fixed multiplier) {
    const auto before = shadow.snapshot();
    auto checked = session_detail::prepare_native_production_finish(*before->production(), before->planets(), build, salvage);
    if (!checked) return core::Result<void>::failure(checked.error());
    const auto& prepared = checked.value();
    if (!prepared.cancel) return core::Result<void>::success();
    auto refund = stage_native_credits(shadow, prepared.owner, session_detail::native_production_paid(prepared.entry), multiplier, false, false);
    if (!refund) return core::Result<void>::failure(refund.error());
    const auto refund_view = shadow.snapshot(); // outcome notice sees the refund publication, before queue removal publishes
    auto row = refund_view->production()->planets[prepared.planet_index]; auto& queue = row.queues[prepared.domain_index];
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(prepared.entry_index));
    if (prepared.entry_index == 0 && !queue.empty()) session_detail::native_production_start(queue.front(), shadow.data_->base->clock().galactic_frame);
    shadow.data_->production_rows.insert_or_assign(row.planet, std::move(row)); shadow.data_->population_stale = true;
    shadow.data_->events.push_back({shadow.data_->base->completed_tick(), GalacticEventKind::production_cancelled, {},
        refund_view->production()->planets[prepared.planet_index].planet, prepared.entry.build_id, prepared.entry.type, GalacticRejectReason::none});
    shadow.data_->action_requests.emplace_back(NativeActionShadow::Data::FinishRequest{build, salvage, multiplier});
    if (impl_->native_production_outcome_handler)
        impl_->native_production_outcome_handler({prepared.entry.builder, prepared.entry.build_id, 0, false});
    shadow.data_->publication.reset();
    return advance_native_segment();
}
core::Result<std::vector<EntityId>> GalacticSession::stage_native_force(
    NativeActionShadow& shadow, const NativeProductionRequest& request, const NativeForceBuilder& builder) {
    using Result = core::Result<std::vector<EntityId>>;
    const auto first = shadow.actor_changes().size(); const auto before = shadow.snapshot();
    const InlineExecutor executor;
    auto result = shadow.create_force(request, [&](const NativeForceView&, EntityId frontier) { return builder(*before, frontier); }, executor);
    if (!result) return result;
    if (auto delivered = deliver_native_shadow_changes(shadow, first); !delivered) return Result::failure(delivered.error());
    if (auto continued = advance_native_segment(); !continued) return Result::failure(continued.error());
    return result;
}
core::Result<void> GalacticSession::stage_native_effect(
    NativeActionShadow& shadow, NativeEffectKind kind, std::vector<std::uint8_t> payload, NativeEffectPhase phase) {
    auto* state = native_barrier();
    if (!state || impl_->failed || impl_->native_replaying || kind < NativeEffectKind::fleet || kind > NativeEffectKind::story_reward
        || phase > NativeEffectPhase::world_service || payload.size() > galactic_replay_max_payload
        || impl_->native_dropped + impl_->native_effects.size() + state->journal_entries + impl_->executed_dropped + impl_->executed.size() + impl_->pending.size() >= galactic_max_commands
        || state->journal_entries >= std::numeric_limits<std::uint64_t>::max() - impl_->native_sequence) {
        impl_->failed = true;
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command, "native journal requires a live ordered owner and bounded payload"));
    }
    if (state->last_phase && phase < *state->last_phase) {
        impl_->failed = true;
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::command_order, "native journal phase rewound"));
    }
    const auto published = shadow.data_->publication;
    shadow.detach(); shadow.data_->action_requests.emplace_back(NativeActionShadow::Data::JournalRequest{kind, std::move(payload), phase});
    shadow.data_->publication = published; // journaling never publishes private rows
    ++state->journal_entries; state->last_phase = phase;
    return core::Result<void>::success();
}
const GalacticSession::NativeInvasion* GalacticSession::native_shadow_invasion(const NativeActionShadow& shadow, std::uint64_t number) const noexcept {
    for (const NativeActionShadow::Data* layer = shadow.data_.get(); layer; layer = layer->read_parent.get())
        if (const auto at = layer->invasion_rows.find(number); at != layer->invasion_rows.end()) return at->second ? &*at->second : nullptr;
    const auto at = impl_->native_invasions.find(number);
    return at == impl_->native_invasions.end() ? nullptr : &at->second;
}
const GalacticSession::NativeInvasion* GalacticSession::native_shadow_pending_invasion(const NativeActionShadow& shadow) const noexcept {
    if (!impl_->native_invasion_queue.empty()) return native_shadow_invasion(shadow, impl_->native_invasion_queue.front());
    std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
    for (const NativeActionShadow::Data* layer = shadow.data_.get(); layer; layer = layer->read_parent.get())
        if (!layer->new_invasion_queue.empty()) first = std::min(first, layer->new_invasion_queue.front());
    return first == std::numeric_limits<std::uint64_t>::max() ? nullptr : native_shadow_invasion(shadow, first);
}
core::Result<void> GalacticSession::stage_native_retire_invasion(NativeActionShadow& shadow, std::uint64_t number) {
    const auto* current = native_shadow_invasion(shadow, number);
    if (impl_->failed || !current) return invalid<void>("invasion reader release requires its retained identity");
    if (current->reader_released) return core::Result<void>::success();
    const auto published = shadow.snapshot(); auto request = *current; request.reader_released = true;
    shadow.detach();
    if (request.state == NativeInvasion::State::completed || request.state == NativeInvasion::State::unqueued) {
        shadow.data_->invasion_rows[number] = std::nullopt; --shadow.data_->invasion_records;
    } else shadow.data_->invasion_rows[number] = std::move(request);
    shadow.data_->publication = published;
    shadow.data_->action_requests.emplace_back(NativeActionShadow::Data::InvasionRequest{true, 0, 0, {}, false, {}, number});
    std::vector<std::uint8_t> bytes; for (const auto word : std::array<std::uint64_t, 3>{1, 3, number}) detail::put_u64(bytes, word);
    if (auto saved = append_native_effect(NativeEffectKind::invasion, std::move(bytes)); !saved) return saved;
    return advance_native_segment();
}
core::Result<GalacticSession::NativeTransportLandingGate> GalacticSession::native_shadow_landing_available(
    const NativeActionShadow& shadow, PlayerId /* ownership safety belongs to the caller */, PlanetId planet, std::optional<std::uint32_t> hint) const {
    using Result = core::Result<NativeTransportLandingGate>;
    const auto snapshot = shadow.snapshot();
    if (!snapshot->control()) return invalid<NativeTransportLandingGate>("automatic landing surface owner unavailable");
    const auto* surface = session_detail::native_find_row(snapshot->control()->planets, planet, [](const auto& row) { return row.planet; });
    if (!surface) return invalid<NativeTransportLandingGate>("automatic landing surface identity unavailable");
    std::uint32_t count{};
    for (const auto& unit : surface->landed) if (unit.id) {
        const auto* actor = shadow.actor(unit.id); if (!actor || !actor->delete_pending) ++count;
    }
    assert(!hint || *hint == count); (void)hint;
    if (count >= snapshot->control()->max_ground_forces) return Result::success(NativeTransportLandingGate::capacity);
    if (!surface->surface_accessible) return Result::success(NativeTransportLandingGate::surface);
    // WGS-187: a mixed surface prefix does not refuse a later link.
    return Result::success(NativeTransportLandingGate::admitted);
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::begin_native_actions(std::size_t max_requests) const {
    if (const auto* current = native_shadow())
        return continue_native_actions(*current, max_requests, true, current->data_->tail);
    if (impl_->failed || !max_requests || max_requests > galactic_max_objects)
        return invalid<NativeActionShadow>("native shadow requires a live campaign and bounded requests");
    auto state = std::make_shared<NativeActionShadow::Data>();
    state->owner = impl_.get(); state->base = impl_->snapshot;
    state->population_schema = impl_->native_population_schema; state->income_schema = impl_->native_income_schema;
    state->observer_generation = state->base->observer_input_generation();
    state->first_id = state->next_id = impl_->next_id;
    state->live_objects = impl_->live_object_count; state->max_requests = max_requests;
    state->can_create = impl_->setup.census.has_value(); state->population_stale = impl_->population_stale;
    state->publication = state->base;
    if (impl_->production && state->base->production() && impl_->production->players != state->base->production()->players)
        state->production_projection = std::make_shared<const std::vector<ProductionPlayer>>(impl_->production->players);
    if (impl_->population != std::vector<PopulationTotal>(state->base->population().begin(), state->base->population().end()))
        state->population_projection = std::make_shared<const std::vector<PopulationTotal>>(impl_->population);
    if (state->base->movement()) state->movement = std::shared_ptr<const MovementSetup>(state->base, state->base->movement());
    if (state->base->victory()) state->victory = std::shared_ptr<const VictoryState>(state->base, &*state->base->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(state)));
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::begin_native_actions(
    PlayerId player, std::size_t max_requests) const {
    if (native_shadow()) return invalid<NativeActionShadow>("explicit native player roots require a completed world barrier");
    auto shadow = begin_native_actions(max_requests);
    if (!shadow) return shadow;
    if (!player || std::none_of(impl_->setup.players.begin(), impl_->setup.players.end(),
        [player](const auto& row) { return row.player_id == player; }))
        return invalid<NativeActionShadow>("native player layer requires a known player");
    shadow.value().data_->player = player;
    shadow.value().data_->fiscal_schema = impl_->native_fiscal_schema;
    if (impl_->setup.fiscal) {
        auto base_income = std::make_shared<std::vector<math::Fixed>>(); base_income->reserve(impl_->setup.planets.size());
        for (std::size_t i = 0; i < impl_->setup.planets.size(); ++i) base_income->push_back(impl_->planet_base_credits(i));
        shadow.value().data_->base_income = std::move(base_income);
        shadow.value().data_->income_edges = std::make_shared<const session_detail::NativeIncomeEdges>(impl_->income_edges);
    }
    shadow.value().data_->graph = impl_->graph ? std::make_shared<const PlanetGraph>(*impl_->graph) : nullptr;
    return shadow;
}
core::Result<void> GalacticSession::commit_native_actions(const NativeActionShadow& shadow, bool actor_changes_prepared) {
    if (native_shadow()) return publish_native_batch(shadow, actor_changes_prepared);
    return commit_native_actions(std::span<const NativeActionShadow>(&shadow, 1), actor_changes_prepared);
}
core::Result<void> GalacticSession::commit_native_actions(std::span<const NativeActionShadow> shadows,
    bool actor_changes_prepared, bool native_effects_prepared) {
    gc_profile::Scope profile_scope(gc_profile::Phase::layer_replay);
    if (shadows.empty()) return core::Result<void>::success();
    EntityId next_id = impl_->next_id;
    const NativeActionShadow::Data* previous{};
    for (const auto& shadow : shadows) {
        const auto& data = *shadow.data_;
        if (impl_->failed || data.owner != impl_.get() || data.base != impl_->snapshot || data.first_id != next_id
            || data.parent.get() != previous || (previous && (data.player < previous->player
                || (data.player == previous->player && !data.continuation) || (previous->tail && !data.tail)
                || (!previous->prelude && data.prelude))) || (data.prelude && data.tail))
            return invalid<void>("native shadow chain is stale, unordered or belongs to another campaign");
        previous = &data; next_id = data.next_id;
    }
    if (shadows.front().player()) {
        if (impl_->native_commit_publication) return invalid<void>("native barrier replay cannot nest publication batches");
        if (actor_changes_prepared && !impl_->pending_actor_changes.empty())
            return invalid<void>("prepared player layers require a settled actor notification frontier");
        struct RestoreHandlers {
            Impl& owner;
            NativeActorChangesHandler actors;
            NativeMovementListener movement;
            CreditNotificationHandler credits;
            NativeProductionHandler production;
            NativeProductionOutcomeHandler outcomes;
            bool applying;
            std::shared_ptr<const GalacticSnapshot> failure_snapshot;
            bool committed{};
            ~RestoreHandlers() {
                owner.native_commit_publication.reset();
                if (!committed && failure_snapshot) owner.snapshot = std::move(failure_snapshot);
                owner.native_actor_changes_handler = std::move(actors); owner.native_movement_listener = std::move(movement);
                owner.credit_notification_handler = std::move(credits); owner.native_production_handler = std::move(production);
                owner.native_production_outcome_handler = std::move(outcomes);
                owner.native_applying = applying;
            }
        } restore{*impl_, impl_->native_actor_changes_handler, impl_->native_movement_listener,
            impl_->credit_notification_handler, impl_->native_production_handler, impl_->native_production_outcome_handler, impl_->native_applying,
            native_effects_prepared ? impl_->snapshot : nullptr};
        if (actor_changes_prepared) impl_->native_actor_changes_handler = {};
        if (native_effects_prepared) {
            impl_->native_commit_publications = 0;
            impl_->native_commit_publication = impl_->snapshot;
            impl_->native_applying = true; impl_->native_movement_listener = {}; impl_->credit_notification_handler = {}; impl_->native_production_outcome_handler = {};
        }
        const auto disagreed = [&]() {
            impl_->failed = true; return invalid<void>("native action layer disagrees with ordered commit");
        };
        for (const auto& shadow : shadows) {
            const auto& data = *shadow.data_;
            if (native_effects_prepared) impl_->native_commit_publication = data.view();
            if (data.immediate_batch) {
                const session_detail::NativeActorChangeCheckpoint pending_before(impl_->pending_actor_changes, actor_changes_prepared);
                std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
                if (!data.forces.empty()) {
                    auto movement = std::make_shared<MovementSetup>(*impl_->movement_inputs);
                    for (const auto& force : data.forces)
                        if (auto applied = impl_->apply_native_force(force->request, force->placement, *movement); !applied) return applied;
                    std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& x, const auto& y) { return x.container < y.container; });
                    impl_->movement_inputs = std::move(movement);
                }
                pending_before.restore();
                for (const auto& [player, balance] : data.accounts) {
                    auto& accounts = impl_->economy->players;
                    const auto at = std::lower_bound(accounts.begin(), accounts.end(), player, [](const auto& row, auto id) { return row.player < id; });
                    at->credits = balance; impl_->dirty[static_cast<std::size_t>(StateSection::players)] = true;
                }
                if (!data.accounts.empty() && data.victory) {
                    impl_->victory = *data.victory; impl_->dirty[static_cast<std::size_t>(StateSection::victory)] = true;
                }
                events.insert(events.end(), data.events.begin(), data.events.end());
                impl_->snapshot = impl_->publish_native_snapshot(std::move(events));
                if (auto delivered = impl_->deliver_actor_changes(); !delivered) return delivered;
            }
            for (const auto& effect : data.action_requests) {
            auto applied = std::visit([&](const auto& request) -> core::Result<void> {
                using Request = std::remove_cvref_t<decltype(request)>;
                if constexpr (std::is_same_v<Request, NativeActionShadow::Data::CashRequest>) {
                    auto result = add_credits(request.player, request.delta, request.multiplier);
                    if (!result) return core::Result<void>::failure(result.error());
                    return result.value() == request.actual ? core::Result<void>::success() : disagreed();
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::ForceRequest>) {
                    auto result = create_native_force(request.force->request, [&](const GalacticSnapshot&, EntityId) {
                        return core::Result<NativeForcePlacement>::success(request.force->placement);
                    });
                    if (!result) return core::Result<void>::failure(result.error());
                    return result.value() == request.actors ? core::Result<void>::success() : disagreed();
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::ProductionRequest>) {
                    const auto handler = impl_->native_production_handler;
                    impl_->native_production_handler = [](const NativeProductionNotice&) {
                        return core::Result<NativeProductionPayment>::success({});
                    };
                    const InlineExecutor executor;
                    auto result = admit_native_production(request.request, executor);
                    impl_->native_production_handler = handler;
                    if (!result) return core::Result<void>::failure(result.error());
                    return result.value().admission == request.receipt.admission && result.value().build == request.receipt.build
                        && result.value().price == request.receipt.price ? core::Result<void>::success() : disagreed();
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::FinishRequest>) {
                    return finish_native_production_task(request.build, request.salvage, request.multiplier);
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::InvasionRequest>) {
                    if (request.retire) return retire_native_invasion(request.number);
                    auto result = admit_native_invasion(request.player, request.fleet, request.transports, request.pending);
                    if (!result) return core::Result<void>::failure(result.error());
                    return result.value() == request.receipt ? core::Result<void>::success() : disagreed();
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::JournalRequest>) {
                    const auto applying = impl_->native_applying; impl_->native_applying = false;
                    auto result = append_native_effect(request.kind, request.payload, request.phase);
                    impl_->native_applying = applying; return result;
                } else if constexpr (std::is_same_v<Request, NativeActionShadow::Data::OrbitRecheckRequest>) {
                    return recheck_native_orbit(request.player, request.fleet);
                } else {
                    auto result = [&]() -> core::Result<NativeFleetReceipt> {
                        if constexpr (std::is_same_v<Request, NativeActionShadow::Data::FleetRequest>)
                            return admit_native_fleet(request.player, request.payload, request.pending);
                        else {
                            using Kind = NativeActionShadow::Data::ControlRequest::Kind;
                            if (request.kind == Kind::launch) return launch_native_transport(request.player, request.object, request.target, request.pending);
                            if (request.kind == Kind::fleet_land) return land_native_transports(request.player, request.object, request.pending);
                            auto landed = land_native_transport(request.player, request.object, request.target, request.pending, request.live_landed);
                            if (!landed) return core::Result<NativeFleetReceipt>::failure(landed.error());
                            if (landed.value().linked != request.linked) {
                                impl_->failed = true; return invalid<NativeFleetReceipt>("native actor landing layer disagrees with ordered commit");
                            }
                            return core::Result<NativeFleetReceipt>::success(landed.value().event);
                        }
                    }();
                    if (!result) return core::Result<void>::failure(result.error());
                    return result.value().admitted == request.receipt.admitted && result.value().object == request.receipt.object
                        && result.value().reason == request.receipt.reason ? core::Result<void>::success() : disagreed();
                }
            }, effect);
            if (!applied) return applied;
        }
        }
        impl_->native_commit_publication.reset();
        if (native_effects_prepared && impl_->native_commit_publications) {
            const auto published = impl_->snapshot;
            bool unpublished_counts{};
            {
                struct RestoreCounts {
                    Impl& owner;
                    std::vector<ProductionPlayer> players;
                    std::vector<PopulationTotal> totals;
                    bool restore_players{}, restore_totals{};
                    ~RestoreCounts() {
                        if (restore_players) owner.production->players.swap(players);
                        if (restore_totals) owner.population.swap(totals);
                    }
                } counts{*impl_, {}, {}, false, false};
                // A final refused producer can refresh private counts without
                // publishing them. Preserve the last logical publication while
                // retaining the refreshed raw rows for the next native request.
                if (impl_->production && published->production()
                    && impl_->production->players != published->production()->players) {
                    counts.players = published->production()->players;
                    impl_->production->players.swap(counts.players); counts.restore_players = true;
                }
                if (!std::ranges::equal(impl_->population, published->population())) {
                    counts.totals.assign(published->population().begin(), published->population().end());
                    impl_->population.swap(counts.totals); counts.restore_totals = true;
                }
                unpublished_counts = counts.restore_players || counts.restore_totals;
                impl_->snapshot = impl_->make_snapshot({published->events().begin(), published->events().end()});
            }
            if (unpublished_counts) {
                // The new immutable snapshot contains the published counts;
                // subsequent raw refreshes must not share its generation key.
                auto generation = *impl_->economy_input_generation;
                ++generation.census; ++generation.accounts;
                impl_->economy_input_generation = std::make_shared<const EconomyInputGeneration>(std::move(generation));
            }
        }
        restore.committed = true;
        return core::Result<void>::success();
    }
    // Validation/placement and notification staging are already complete.
    // This ordered commit applies only new output entities, never a scene copy.
    const session_detail::NativeActorChangeCheckpoint pending_before(impl_->pending_actor_changes, actor_changes_prepared);
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    for (const auto& shadow : shadows) {
        const auto& data = *shadow.data_;
        if (!data.forces.empty()) {
            auto movement = std::make_shared<MovementSetup>(*impl_->movement_inputs);
            for (const auto& force : data.forces)
                if (auto applied = impl_->apply_native_force(force->request, force->placement, *movement); !applied) return applied;
            std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
            impl_->movement_inputs = std::move(movement);
        }
        pending_before.restore();
        for (const auto& [id, balance] : data.accounts) {
            auto& accounts = impl_->economy->players;
            const auto at = std::lower_bound(accounts.begin(), accounts.end(), id, [](const auto& row, auto key) { return row.player < key; });
            at->credits = balance;
            impl_->dirty[static_cast<std::size_t>(StateSection::players)] = true;
        }
        if (!data.accounts.empty() && data.victory) {
            impl_->victory = *data.victory;
            impl_->dirty[static_cast<std::size_t>(StateSection::victory)] = true;
        }
        events.insert(events.end(), data.events.begin(), data.events.end());
    }
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return impl_->deliver_actor_changes();
}

core::Result<GalacticSession::DueHeroPlacement> GalacticSession::place_due_hero(
    const NativeProductionRequest& request, const NativeForceBuilder& builder, const PartitionExecutor& executor) {
    using Result = core::Result<DueHeroPlacement>;
    auto& impl = *impl_;
    if (impl.failed || !builder || !impl.production || !impl.production->nested_creation || !impl.control
        || !impl.movement_inputs || !impl.setup.census)
        return invalid<DueHeroPlacement>("due placement requires qualified live production services");
    const auto site = std::find_if(impl.production->planets.begin(), impl.production->planets.end(),
        [&](const auto& row) { return row.planet == request.planet; });
    if (!request.type || !impl.planets.contains(request.planet) || site == impl.production->planets.end()
        || std::none_of(impl.setup.players.begin(), impl.setup.players.end(), [&](const auto& p) { return p.player_id == request.player; }))
        return Result::success({}); // WGM-132: an invalid placement destination has no primary.
    auto staged = builder(*impl.snapshot, impl.next_id);
    if (!staged) return Result::failure(staged.error());
    auto& placement = staged.value();
    const auto center = impl.registry.get<session_detail::PlanetData>(impl.planets.at(request.planet)).position;
    for (auto& fleet : placement.fleets) fleet.motion.position = center; // WGM-133/134: destination center, zero facing.
    auto validated = validate_native_force(request, placement, impl.next_id, impl.live_object_count,
        *impl.production->nested_creation, [&](EntityId id) { return bool(impl.snapshot->current_type(id)); });
    if (!validated) return Result::failure(validated.error());
    if (placement.primary && std::none_of(placement.originals.begin(), placement.originals.end(),
        [&](const auto& row) { return row.id == placement.primary; }))
        return invalid<DueHeroPlacement>("due placement primary must belong to its committed object tree");
    auto movement = std::make_shared<MovementSetup>(*impl.movement_inputs);
    if (auto applied = impl.apply_native_force(request, placement, *movement); !applied)
        return Result::failure(applied.error());
    std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    impl.movement_inputs = std::move(movement);
    // The next due record reads the newly committed census through the same partition executor.
    if (auto refreshed = impl.refresh_population(executor); !refreshed) return Result::failure(refreshed.error());
    const auto old = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({old.begin(), old.end()}, &executor);
    if (auto delivered = impl.deliver_actor_changes(); !delivered) return Result::failure(delivered.error());
    return Result::success({placement.primary, placement.originals});
}

core::Result<HeroStructureState> GalacticSession::due_hero_structure_state(
    const HeroReturnRecord& record, const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.production || !impl.setup.census)
        return invalid<HeroStructureState>("due hero revalidation requires the live original census");
    if (auto refreshed = impl.refresh_population(executor); !refreshed)
        return core::Result<HeroStructureState>::failure(refreshed.error());
    HeroStructureState state;
    const auto owner = impl.original_owned_counts.find(record.owner);
    if (owner == impl.original_owned_counts.end()) return core::Result<HeroStructureState>::success(state);
    if (const auto row = owner->second.find(record.type); row != owner->second.end()) state.owned_instances = row->second;
    if (const auto row = owner->second.find(record.structure); row != owner->second.end()) state.owned_structures = row->second;
    for (const auto& planet : impl.production->planets) {
        if (!impl.planets.contains(planet.planet)
            || impl.registry.get<session_detail::PlanetData>(impl.planets.at(planet.planet)).owner != record.owner) continue;
        if ((planet.starbase_owner == record.owner && planet.starbase_type == record.structure)
            || std::any_of(planet.objects.begin(), planet.objects.end(), [&](const auto& row) {
                return row.type == record.structure && row.owner == record.owner;
            })) state.planets.push_back(planet.planet);
    }
    return core::Result<HeroStructureState>::success(std::move(state));
}
} // namespace eawr::sim::galactic
