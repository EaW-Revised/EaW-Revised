#include "session_impl.hpp"
#include "encoding.hpp"
#include "native_fleet_admission.hpp"
#include "native_control_admission.hpp"
#include "native_production_admission.hpp"
#include "native_movement_admission.hpp"
#include "native_victory_projection.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/core/gc_profile.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <set>
#include <type_traits>
#include <variant>

namespace eawr::sim::galactic {
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
namespace {
template<class T> core::Result<T> invalid(std::string message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
}
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

struct GalacticSession::NativeActionShadow::Data {
    struct Location {
        Planet planet;
        ControlPlanet presence;
        PlanetProduction production;
        std::shared_ptr<const std::vector<Fleet>> orbit;
        std::shared_ptr<const std::vector<MovementContents>> contents;
    };
    struct Force { NativeProductionRequest request; NativeForcePlacement placement; };
    struct FleetRequest { PlayerId player{}; CommandPayload payload; bool pending{}; NativeFleetReceipt receipt; };
    struct ControlRequest {
        enum class Kind { launch, fleet_land, actor_land } kind{};
        PlayerId player{};
        EntityId object{}, target{};
        bool pending{};
        std::optional<std::uint32_t> live_landed;
        NativeFleetReceipt receipt;
        bool linked{};
    };
    struct CashRequest { PlayerId player{}; math::Fixed delta, multiplier, actual; };
    struct ForceRequest { std::shared_ptr<const Force> force; std::vector<EntityId> actors; };
    struct FinishRequest { EntityId build{}; bool salvage{}; math::Fixed multiplier; };
    struct ProductionRequest { NativeProductionRequest request; NativeProductionReceipt receipt; };
    struct InvasionRequest { bool retire{}; PlayerId player{}; FleetId fleet{}; std::vector<EntityId> transports;
        bool pending{}; NativeInvasionReceipt receipt; std::uint64_t number{}; };
    struct JournalRequest { NativeEffectKind kind{}; std::vector<std::uint8_t> payload; NativeEffectPhase phase{}; };
    struct OrbitRecheckRequest { PlayerId player{}; FleetId fleet{}; };
    std::map<FleetId, std::optional<Fleet>> fleet_rows;
    std::map<EntityId, std::optional<CensusObject>> actor_rows;
    std::map<EntityId, std::optional<DetachedUnit>> detached_rows;
    std::map<FleetId, bool> fleet_blockers;
    std::map<PlanetId, ControlPlanet> control_rows;
    std::map<PlanetId, PlanetProduction> production_rows;
    std::vector<FleetId> created_blockers;
    bool immediate_batch{}, continuation{}, prelude{}, tail{}, last_published{}, last_admission_ran{};
    NativeMovementListener movement_listener;
    std::map<std::uint64_t, std::optional<NativeInvasion>> invasion_rows;
    std::vector<std::uint64_t> new_invasion_queue;
    std::size_t invasion_records{}, invasion_transports{};
    std::uint64_t next_invasion{1};
    bool invasions_enabled{}, suppress_cash_request{};
    using ObserverPlanet = decltype(ObserverSetup{}.planets)::value_type;
    std::map<PlanetId, ObserverPlanet> observer_rows;
    bool population_stale{};
    std::vector<std::variant<FleetRequest, ControlRequest, CashRequest, ForceRequest, ProductionRequest, FinishRequest, InvasionRequest, JournalRequest, OrbitRecheckRequest>> action_requests;
    std::shared_ptr<const session_detail::NativePopulationSchema> population_schema;
    std::shared_ptr<const IncomeSetup> income_schema;
    std::shared_ptr<const FiscalSetup> fiscal_schema;
    std::shared_ptr<const std::vector<math::Fixed>> base_income;
    std::shared_ptr<const session_detail::NativeIncomeEdges> income_edges;
    std::shared_ptr<const std::vector<ProductionPlayer>> production_projection;
    std::shared_ptr<const std::vector<PopulationTotal>> population_projection;
    std::shared_ptr<const PlanetGraph> graph;
    std::shared_ptr<const MovementSetup> movement;
    std::uint64_t observer_generation{};
    mutable std::shared_ptr<const GalacticSnapshot> publication;
    [[nodiscard]] std::shared_ptr<const GalacticSnapshot> view() const;
    [[nodiscard]] const Fleet* fleet(FleetId) const;
    [[nodiscard]] const CensusObject* actor(EntityId) const;
    [[nodiscard]] const DetachedUnit* detached(EntityId) const;
    [[nodiscard]] const MovementContents* contents(EntityId) const;
    class FleetOwner;
    class ControlOwner;
    const void* owner{};
    std::shared_ptr<const GalacticSnapshot> base;
    std::shared_ptr<const Data> parent;
    std::shared_ptr<const Data> read_parent;
    std::size_t read_depth{}, read_checkpoints{};
    void inherit_reads(const std::shared_ptr<const Data>&);
    mutable std::shared_ptr<const EconomyInputGeneration> last_generation;
    PlayerId player{};
    EntityId first_id{}, next_id{};
    std::size_t live_objects{}, max_requests{};
    bool can_create{};
    std::map<PlayerId, math::Fixed> accounts;
    std::map<PlanetId, std::shared_ptr<const Location>> locations;
    std::vector<std::shared_ptr<const Force>> forces;
    std::vector<NativeActorChange> actor_changes;
    std::shared_ptr<const VictoryState> victory;
    std::vector<GalacticEvent> events;
    Work work;
};
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
    if (!state || impl_->failed || impl_->native_replaying || kind < NativeEffectKind::fleet || kind > NativeEffectKind::income_block
        || phase > NativeEffectPhase::world_service || payload.size() > galactic_replay_max_payload
        || impl_->native_effects.size() + state->journal_entries + impl_->executed.size() + impl_->pending.size() >= galactic_max_commands
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
void GalacticSession::NativeActionShadow::Data::inherit_reads(const std::shared_ptr<const Data>& previous) {
    // This is a storage/lookup bound, not a limit on authored objects or actions.
    // Freeze only the sparse written frontier after 32 links; the replay chain
    // remains intact and preserves every immediate transaction's ordering.
    constexpr std::size_t max_read_links = 32;
    if (previous->read_depth + 1 < max_read_links) {
        read_parent = previous; read_depth = previous->read_depth + 1;
        return;
    }
    auto checkpoint = std::make_shared<Data>(); checkpoint->base = previous->base;
    const auto merge_newest = [](auto& into, const auto& from) {
        for (const auto& [key, value] : from) into.try_emplace(key, value);
    };
    for (auto layer = previous.get(); layer; layer = layer->read_parent.get()) {
        merge_newest(checkpoint->fleet_rows, layer->fleet_rows);
        merge_newest(checkpoint->actor_rows, layer->actor_rows);
        merge_newest(checkpoint->detached_rows, layer->detached_rows);
        merge_newest(checkpoint->fleet_blockers, layer->fleet_blockers);
        merge_newest(checkpoint->accounts, layer->accounts);
        merge_newest(checkpoint->locations, layer->locations);
        merge_newest(checkpoint->invasion_rows, layer->invasion_rows);
        if (!layer->new_invasion_queue.empty() && (checkpoint->new_invasion_queue.empty()
            || layer->new_invasion_queue.front() < checkpoint->new_invasion_queue.front()))
            checkpoint->new_invasion_queue.assign(1, layer->new_invasion_queue.front());
        checkpoint->forces.insert(checkpoint->forces.end(), layer->forces.begin(), layer->forces.end());
    }
    work.copied_fleet_rows += checkpoint->fleet_rows.size();
    work.copied_actor_rows += checkpoint->actor_rows.size();
    read_parent = std::move(checkpoint); read_depth = 1; ++read_checkpoints;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::NativeActionShadow::Data::view() const {
    if (publication) return publication;
    const auto inherited = parent ? parent->view() : base;
    gc_profile::Scope profile_scope(gc_profile::Phase::shadow_view);
    NativeSnapshotChanges changes;
    changes.touched_planets.emplace();
    auto& touched = *changes.touched_planets;
    for (const auto& [id, row] : fleet_rows) {
        if (row) touched.push_back(row->orbit);
        if (const auto* old = session_detail::native_find_row(inherited->fleets(), id, [](const auto& item) { return item.id; })) touched.push_back(old->orbit);
    }
    for (const auto& [id, row] : actor_rows) {
        if (row) touched.push_back(row->planet);
        if (const auto* old = session_detail::native_find_row(inherited->actor_records(), id, [](const auto& item) { return item.id; })) touched.push_back(old->planet);
    }
    for (const auto& [id, row] : detached_rows) {
        if (row) touched.push_back(row->planet);
        if (inherited->fleet_control()) if (const auto* old = session_detail::native_find_row(
            inherited->fleet_control()->detached_units, id, [](const auto& item) { return item.member.id; })) touched.push_back(old->planet);
    }
    for (const auto& [id, row] : locations) { (void)row; touched.push_back(id); }
    for (const auto& [id, row] : control_rows) { (void)row; touched.push_back(id); }
    for (const auto& [id, row] : production_rows) { (void)row; touched.push_back(id); }
    for (const auto& force : forces) touched.push_back(force->request.planet);
    for (const auto& [id, row] : fleet_blockers) {
        (void)row;
        if (const auto* fleet = this->fleet(id)) touched.push_back(fleet->orbit);
    }
    if (production_projection && inherited->production()) {
        changes.production = *inherited->production(); changes.production->players = *production_projection;
    }
    if (population_projection) changes.population = *population_projection;
    const auto patch = []<class T, class Key>(std::vector<T>& rows, const T& row, Key key) {
        const auto at = std::lower_bound(rows.begin(), rows.end(), key(row), [&](const auto& item, auto id) { return key(item) < id; });
        if (at != rows.end() && key(*at) == key(row)) *at = row;
        else rows.insert(at, row);
    };
    if (!fleet_rows.empty()) {
        changes.fleets.emplace(inherited->fleets().begin(), inherited->fleets().end());
        for (const auto& [id, row] : fleet_rows) {
            if (row) patch(*changes.fleets, *row, [](const auto& item) { return item.id; });
            else std::erase_if(*changes.fleets, [id](const auto& item) { return item.id == id; });
        }
    }
    if (!actor_rows.empty()) {
        auto rows = std::make_shared<std::vector<CensusObject>>(inherited->actor_records().begin(), inherited->actor_records().end());
        for (const auto& [id, row] : actor_rows) {
            if (row) patch(*rows, *row, [](const auto& item) { return item.id; });
            else std::erase_if(*rows, [id](const auto& item) { return item.id == id; });
        }
        changes.actors = std::move(rows);
    }
    if (!detached_rows.empty() && inherited->fleet_control()) {
        changes.fleet_control = *inherited->fleet_control();
        for (const auto& [id, row] : detached_rows) {
            if (row) patch(changes.fleet_control->detached_units, *row, [](const auto& item) { return item.member.id; });
            else std::erase_if(changes.fleet_control->detached_units, [id](const auto& item) { return item.member.id == id; });
        }
    }
    if (!accounts.empty() && inherited->player_economy()) {
        changes.economy = *inherited->player_economy();
        for (auto& row : changes.economy->players)
            if (const auto at = accounts.find(row.player); at != accounts.end()) row.credits = at->second;
    }
    if (!locations.empty()) {
        if (inherited->control()) changes.control = *inherited->control();
        if (!changes.production && inherited->production()) changes.production = *inherited->production();
        for (const auto& [id, location] : locations) {
            (void)id;
            if (changes.control) patch(changes.control->planets, location->presence, [](const auto& row) { return row.planet; });
            if (changes.production) patch(changes.production->planets, location->production, [](const auto& row) { return row.planet; });
        }
    }
    if (!forces.empty()) {
        auto originals = std::make_shared<std::vector<CensusObject>>(inherited->commissioned_originals().begin(), inherited->commissioned_originals().end());
        for (const auto& force : forces) for (const auto& row : force->placement.originals)
            patch(*originals, row, [](const auto& item) { return item.id; });
        changes.originals = std::move(originals);
    }
    if (!production_rows.empty() && inherited->production()) {
        if (!changes.production) changes.production = *inherited->production();
        for (const auto& [id, row] : production_rows) { (void)id; patch(changes.production->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!control_rows.empty() && inherited->control()) {
        if (!changes.control) changes.control = *inherited->control();
        for (const auto& [id, row] : control_rows) { (void)id; patch(changes.control->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!observer_rows.empty() && inherited->observer()) {
        changes.observer = *inherited->observer();
        for (const auto& [id, row] : observer_rows) { (void)id; patch(changes.observer->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!fleet_blockers.empty() && inherited->production() && inherited->production()->fleet_blockers) {
        if (!changes.production) changes.production = *inherited->production();
        for (auto& row : *changes.production->fleet_blockers)
            if (const auto at = fleet_blockers.find(row.fleet); at != fleet_blockers.end()) row.skips_space_combat = at->second;
        for (const auto id : created_blockers)
            if (std::none_of(changes.production->fleet_blockers->begin(), changes.production->fleet_blockers->end(), [id](const auto& row) { return row.fleet == id; }))
                changes.production->fleet_blockers->push_back({id, false, fleet_blockers.at(id), true});
    }
    if (!actor_rows.empty() && inherited->victory()) {
        auto projected = victory ? *victory : *inherited->victory();
        for (const auto& [id, row] : actor_rows) {
            if (row) static_cast<void>(session_detail::update_native_victory_actor(projected, *row));
            else static_cast<void>(session_detail::remove_native_victory_actor(projected, id));
        }
        changes.victory = std::move(projected);
    }
    // Immediate batches preserve their existing publication grouping: force
    // births first, then the scalar credit/victory projection.
    if (!accounts.empty() && victory && (immediate_batch || actor_rows.empty())) changes.victory = *victory;
    if (movement && movement.get() != inherited->movement()) changes.movement = movement;
    if (!events.empty()) {
        changes.events.emplace(inherited->events().begin(), inherited->events().end());
        changes.events->insert(changes.events->end(), events.begin(), events.end());
    }
    if (observer_generation != inherited->observer_input_generation()) {
        changes.observer_generation = observer_generation;
        if (inherited->observer()) {
            const auto fleets = changes.fleets ? std::span<const Fleet>(*changes.fleets) : inherited->fleets();
            const auto actors = changes.actors ? std::span<const CensusObject>(*changes.actors) : inherited->actor_records();
            changes.observations = std::make_shared<const std::vector<ObservedPlanet>>(
                observed_planets(changes.observer ? *changes.observer : *inherited->observer(), fleets, movement ? movement.get() : inherited->movement(), actors));
        }
    }
    // Generations are derived invalidation keys, excluded from canonical bytes.
    const bool physical_changes = !actor_rows.empty() || !fleet_rows.empty() || !control_rows.empty() || !forces.empty()
        || !production_rows.empty() || production_projection || population_projection;
    if (inherited->economy_input_generation() && (physical_changes || !accounts.empty())) {
        auto generation = *inherited->economy_input_generation();
        if (last_generation) {
            generation.census = std::max(generation.census, last_generation->census);
            generation.accounts = std::max(generation.accounts, last_generation->accounts);
        }
        if (physical_changes) ++generation.census;
        if (!accounts.empty()) ++generation.accounts;
        if (physical_changes && fiscal_schema && base_income && income_edges) {
            const auto fleets = changes.fleets ? std::span<const Fleet>(*changes.fleets) : inherited->fleets();
            const auto actors = changes.actors ? std::span<const CensusObject>(*changes.actors) : inherited->actor_records();
            const auto* control = changes.control ? &*changes.control : (inherited->control() ? &*inherited->control() : nullptr);
            const auto* production = changes.production ? &*changes.production : (inherited->production() ? &*inherited->production() : nullptr);
            const auto* fleet_control = changes.fleet_control ? &*changes.fleet_control : (inherited->fleet_control() ? &*inherited->fleet_control() : nullptr);
            const auto detached = fleet_control ? std::span<const DetachedUnit>(fleet_control->detached_units) : std::span<const DetachedUnit>{};
            std::optional<IncomeSetup> inputs;
            std::vector<session_detail::NativeIncomeSourceCandidate> candidates;
            if (income_schema) {
                inputs = *income_schema;
                session_detail::project_native_income(*inputs, *income_schema, control, inherited->planets(), fleets, detached, production, actors, candidates);
            }
            const auto* income = inherited->income() ? &*inherited->income() : nullptr;
            const auto gambling = income ? std::span<const GamblingIncomeState>(income->gambling) : std::span<const GamblingIncomeState>{};
            const InlineExecutor executor;
            auto bonuses = session_detail::project_native_income_bonuses(inputs ? &*inputs : nullptr, gambling, inherited->planets(), *base_income, executor);
            generation.income.reset();
            if (bonuses) {
                auto contributions = session_detail::project_native_income_contributions(*fiscal_schema, inherited->planets(), *base_income, control,
                    inherited->economy_corruption() ? &*inherited->economy_corruption() : nullptr, income, bonuses.value(), *income_edges, executor);
                if (contributions) {
                    generation.income.emplace();
                    for (std::size_t i = 0; i < contributions.value().size(); ++i)
                        generation.income->emplace_back(fiscal_schema->planets[i].planet, contributions.value()[i].total);
                }
            }
        }
        last_generation = std::make_shared<const EconomyInputGeneration>(std::move(generation));
        changes.economy_generation = last_generation;
    }
    publication = GalacticSnapshot::native_view(inherited, std::move(changes));
    return publication;
}
const Fleet* GalacticSession::NativeActionShadow::Data::fleet(FleetId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get()) {
        if (const auto at = layer->fleet_rows.find(id); at != layer->fleet_rows.end())
            return at->second ? &*at->second : nullptr;
        for (const auto& force : layer->forces) for (const auto& row : force->placement.fleets)
            if (row.id == id) return &row;
    }
    const auto rows = base->fleets();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
const CensusObject* GalacticSession::NativeActionShadow::Data::actor(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get()) {
        if (const auto at = layer->actor_rows.find(id); at != layer->actor_rows.end())
            return at->second ? &*at->second : nullptr;
        for (const auto& force : layer->forces) for (const auto& row : force->placement.originals)
            if (row.id == id) return &row;
    }
    const auto rows = base->actor_records();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
const DetachedUnit* GalacticSession::NativeActionShadow::Data::detached(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get())
        if (const auto at = layer->detached_rows.find(id); at != layer->detached_rows.end())
            return at->second ? &*at->second : nullptr;
    if (base->fleet_control()) for (const auto& row : base->fleet_control()->detached_units)
        if (row.member.id == id) return &row;
    return nullptr;
}
const MovementContents* GalacticSession::NativeActionShadow::Data::contents(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get())
        for (const auto& force : layer->forces) for (const auto& row : force->placement.contents)
            if (row.container == id) return &row;
    if (base->movement()) {
        const auto& rows = base->movement()->contents;
        const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.container < key; });
        if (at != rows.end() && at->container == id) return &*at;
    }
    return nullptr;
}
// The kernel's owner adapter borrows immutable schemas and resolves only the
// command's actual fleet/actor/detached frontier. It has no ECS registry.
class GalacticSession::NativeActionShadow::Data::FleetOwner {
protected:
    Data& data_;
    std::map<FleetId, session_detail::FleetData> loaded_;
    std::set<FleetId> removed_;
    std::map<PlanetId, session_detail::PlanetData> loaded_planets_;
public:
    struct Schemas {
        const std::optional<FleetControlSetup>& fleet_control;
        const MovementSetup* movement{};
    } setup;
    std::span<const Seat> seats;
    const GalacticClock& clock;
    EntityId& next_id;
    std::size_t& live_object_count;
    std::shared_ptr<const MovementSetup> movement_inputs;
    bool population_stale{};
    std::array<bool, 32> dirty{};
    const PlanetGraph* graph{};
    NativeMovementListener native_movement_listener;
    struct Registry {
        FleetOwner& owner;
        template<class T> T& get(FleetId id) {
            static_assert(std::is_same_v<T, session_detail::FleetData> || std::is_same_v<T, session_detail::PlanetData>);
            if constexpr (std::is_same_v<T, session_detail::PlanetData>) {
                const auto rows = owner.data_.base->planets();
                const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
                return owner.loaded_planets_.try_emplace(id, session_detail::PlanetData{at->position, at->owner}).first->second;
            } else {
            auto at = owner.loaded_.find(id);
            if (at == owner.loaded_.end()) {
                const auto* row = owner.data_.fleet(id);
                ++owner.data_.work.copied_fleet_rows;
                at = owner.loaded_.emplace(id, session_detail::FleetData{row->owner, row->orbit, row->members,
                    row->orbit_slot, row->motion}).first;
            }
            return at->second;
            }
        }
        void destroy(FleetId id) { owner.loaded_.erase(id); owner.removed_.insert(id); }
    } registry;
    struct FleetIds {
        FleetOwner& owner;
        std::map<FleetId, FleetId> selected;
        auto find(FleetId id) {
            if (contains(id)) selected.try_emplace(id, id);
            else selected.erase(id);
            return selected.find(id);
        }
        auto end() { return selected.end(); }
        void erase(std::map<FleetId, FleetId>::iterator at) { selected.erase(at); }
        bool contains(FleetId id) const { return !owner.removed_.contains(id) && (owner.loaded_.contains(id) || owner.data_.fleet(id)); }
        FleetId at(FleetId id) const { return id; }
        void erase(FleetId) const {}
    } fleets;
    struct PlanetIds {
        const GalacticSnapshot& base;
        PlanetId at(PlanetId id) const { return id; }
        bool contains(PlanetId id) const {
            const auto rows = base.planets();
            const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
            return at != rows.end() && at->id == id;
        }
    } planets;
    class Detached {
        Data& data_;
        std::map<EntityId, DetachedUnit> loaded_;
        std::set<EntityId> removed_;
    public:
        explicit Detached(Data& data) : data_(data) {}
        auto find(EntityId id) {
            if (!removed_.contains(id) && !loaded_.contains(id))
                if (const auto* row = data_.detached(id)) loaded_.emplace(id, *row);
            return loaded_.find(id);
        }
        auto end() { return loaded_.end(); }
        DetachedUnit& at(EntityId id) { return find(id)->second; }
        void erase(EntityId id) { loaded_.erase(id); removed_.insert(id); }
        void erase(std::map<EntityId, DetachedUnit>::const_iterator at) { erase(at->first); }
        template<class... Args> auto emplace(EntityId id, Args&&... args) {
            find(id); removed_.erase(id); return loaded_.emplace(id, std::forward<Args>(args)...);
        }
        void finish() {
            for (const auto& [id, row] : loaded_) data_.detached_rows[id] = row;
            for (const auto id : removed_) data_.detached_rows[id] = std::nullopt;
        }
    } detached_units;
    explicit FleetOwner(Data& data)
        : data_(data), setup{data.base->fleet_control(), data.base->movement()}, seats(data.base->seats()),
          clock(data.base->clock()), next_id(data.next_id), live_object_count(data.live_objects),
          movement_inputs(data.movement),
          graph(data.graph.get()), native_movement_listener(data.movement_listener), registry{*this}, fleets{*this, {}}, planets{*data.base}, detached_units(data) {}
    const Seat* seat(SeatId id) const {
        const auto at = std::find_if(seats.begin(), seats.end(), [id](const auto& row) { return row.seat_id == id; });
        return at != seats.end() ? &*at : nullptr;
    }
    std::vector<Planet> sorted_planets() const { return {data_.base->planets().begin(), data_.base->planets().end()}; }
    std::vector<Route> sorted_routes() const { return {data_.base->routes().begin(), data_.base->routes().end()}; }
    void invalidate_observer_projection() { ++data_.observer_generation; }
    void relocate_actor(EntityId id, PlayerId player, PlanetId planet, EntityId container) {
        if (const auto* before = data_.actor(id)) {
            auto row = *before; row.owner = player; row.planet = planet; row.container = container;
            if (row != *before) {
                data_.actor_rows[id] = std::move(row); ++data_.observer_generation;
                ++data_.work.copied_actor_rows;
                data_.actor_changes.push_back({NativeActorChange::Kind::changed, id, 0, {}, {}, {}});
            }
        }
        if (const auto* contents = data_.contents(id))
            for (const auto& child : contents->children) relocate_actor(child.id, player, planet, id);
    }
    void record_fleet_shell(FleetId id, PlayerId player, PlanetId planet) {
        if (!data_.can_create && !(data_.base->production() && data_.base->production()->nested_creation)) return;
        const auto type = data_.base->production() && data_.base->production()->nested_creation
            ? data_.base->production()->nested_creation->fleet_type : TypeId{};
        CensusObject row{id, type, player, planet, planet, false, false, type, 0, ActorCombatRole::unavailable, {}, {}};
        if (data_.base->production() && data_.base->production()->nested_creation) {
            const auto& types = data_.base->production()->nested_creation->types;
            const auto fact = std::lower_bound(types.begin(), types.end(), type, [](const auto& value, auto key) { return value.type < key; });
            if (fact != types.end() && fact->type == type && fact->combat_role != ActorCombatRole::unavailable) {
                row.combat_role = fact->combat_role; row.abilities = fact->abilities; row.garrison = false;
            }
        }
        data_.actor_rows[id] = std::move(row); ++data_.observer_generation;
        data_.actor_changes.push_back({NativeActorChange::Kind::created, id, 0, {}, {}, {}});
    }
    void lose_actor(EntityId id) {
        if (const auto* row = data_.contents(id)) for (const auto& child : row->children) lose_actor(child.id);
        if (!data_.actor(id)) return;
        data_.actor_rows[id] = std::nullopt;
        data_.actor_changes.push_back({NativeActorChange::Kind::lost, id, 0, {}, {}, {}});
    }
    void insert(const Fleet& row) {
        auto motion = row.motion;
        if (setup.movement && !motion.orbit_entry) {
            std::set<FleetId> ids;
            for (const auto& fleet : data_.base->fleets()) ids.insert(fleet.id);
            for (const Data* layer = &data_; layer; layer = layer->read_parent.get()) {
                for (const auto& [id, value] : layer->fleet_rows) { (void)value; ids.insert(id); }
                for (const auto& force : layer->forces) for (const auto& fleet : force->placement.fleets) ids.insert(fleet.id);
            }
            for (const auto& [id, value] : loaded_) { (void)value; ids.insert(id); }
            std::uint64_t latest{};
            for (const auto id : ids) if (fleets.contains(id)) {
                const auto at = loaded_.find(id);
                latest = std::max(latest, at != loaded_.end() ? at->second.motion.orbit_entry : data_.fleet(id)->motion.orbit_entry);
            }
            motion.orbit_entry = latest + 1;
        }
        loaded_[row.id] = {row.owner, row.orbit, row.members, row.orbit_slot, std::move(motion)};
    }
    void refresh_fleet_combat_skip(FleetId id) {
        if (!data_.base->production() || !data_.base->production()->fleet_blockers || !fleets.contains(id)) return;
        const auto& blockers = *data_.base->production()->fleet_blockers;
        bool present = std::any_of(blockers.begin(), blockers.end(), [id](const auto& row) { return row.fleet == id; });
        for (const Data* layer = &data_; !present && layer; layer = layer->read_parent.get()) present = layer->fleet_blockers.contains(id);
        if (!present) return;
        const auto& row = registry.get<session_detail::FleetData>(id);
        std::vector<CensusObject> records;
        MovementSetup skip_movement;
        std::set<EntityId> visited;
        const auto gather = [&](auto&& self, EntityId actor) -> void {
            if (!visited.insert(actor).second) return;
            if (const auto* value = data_.actor(actor)) records.push_back(*value);
            if (const auto* value = data_.contents(actor)) {
                skip_movement.contents.push_back(*value);
                for (const auto& child : value->children) self(self, child.id);
            }
        };
        gather(gather, id);
        for (const auto& member : row.members) gather(gather, member.id);
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(skip_movement.contents.begin(), skip_movement.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        auto skip = fleet_combat_skip(Fleet{id, row.owner, row.orbit, row.members}, records, &skip_movement);
        if (skip) data_.fleet_blockers[id] = skip.value();
    }
    std::set<FleetId> movement_fleet_ids() const {
        std::set<FleetId> ids;
        for (const auto& fleet : data_.base->fleets()) ids.insert(fleet.id);
        for (const Data* layer = &data_; layer; layer = layer->read_parent.get()) {
            for (const auto& [id, row] : layer->fleet_rows) { (void)row; ids.insert(id); }
            for (const auto& force : layer->forces) for (const auto& fleet : force->placement.fleets) ids.insert(fleet.id);
        }
        return ids;
    }
    template<class Consume> void visit_movement_fleets(const std::set<FleetId>& ids, Consume consume) const {
        for (const auto id : ids) {
            if (removed_.contains(id)) continue;
            if (const auto at = loaded_.find(id); at != loaded_.end()) consume(id, at->second);
            else if (const auto* row = data_.fleet(id)) consume(id, *row);
        }
    }
    core::Result<bool> orbit_conflict(const Fleet& fleet) const {
        std::vector<CensusObject> records;
        MovementSetup contents;
        std::set<EntityId> visited;
        const auto gather = [&](auto&& self, EntityId id) -> void {
            if (!visited.insert(id).second) return;
            if (const auto* row = data_.actor(id)) records.push_back(*row);
            if (const auto* row = data_.contents(id)) {
                contents.contents.push_back(*row);
                for (const auto& child : row->children) self(self, child.id);
            }
        };
        gather(gather, fleet.id);
        for (const auto& member : fleet.members) gather(gather, member.id);
        bool has_actors = !records.empty();
        // Preserve the live owner's schema fallback only for a world without
        // actor facts. Missing facts in an otherwise qualified world refuse.
        if (!has_actors) {
            for (const auto& row : data_.base->actor_records()) if (data_.actor(row.id)) { has_actors = true; break; }
            for (const Data* layer = &data_; !has_actors && layer; layer = layer->read_parent.get()) {
                for (const auto& [id, row] : layer->actor_rows) if (row && data_.actor(id)) { has_actors = true; break; }
                for (const auto& force : layer->forces) for (const auto& row : force->placement.originals)
                    if (data_.actor(row.id)) { has_actors = true; break; }
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(contents.contents.begin(), contents.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        return session_detail::native_orbit_conflict(fleet, has_actors
            ? std::optional<std::span<const CensusObject>>(records) : std::nullopt, *movement_inputs, &contents);
    }
    void finish() {
        for (auto& [id, row] : loaded_) {
            const auto* previous = data_.fleet(id);
            if (previous && previous->owner == row.owner && previous->orbit == row.orbit
                && previous->members == row.members && previous->orbit_slot == row.orbit_slot && previous->motion == row.motion)
                continue;
            data_.fleet_rows[id] = Fleet{id, row.owner, row.orbit, std::move(row.members), row.orbit_slot, std::move(row.motion)};
        }
        for (const auto id : removed_) data_.fleet_rows[id] = std::nullopt;
        detached_units.finish();
        if (population_stale) data_.population_stale = true;
        data_.publication.reset();
    }
};
class GalacticSession::NativeActionShadow::Data::ControlOwner final : public FleetOwner {
    std::shared_ptr<const GalacticSnapshot> input_;
public:
    struct ControlView {
        std::uint32_t max_ground_forces{};
        std::span<const ControlPlayer> players;
        std::span<const ControlRelation> relations;
        std::span<const ControlType> types;
        std::vector<ControlPlanet> planets;
        std::span<const PlanetControl> state;
    };
    struct ObserverView { decltype(ObserverSetup{}.planets) planets; };
    std::optional<ControlView> control;
    std::optional<ObserverView> observer;
    const std::uint64_t completed_tick;
    const bool failed{};
    class Actors {
        const Data& data_;
        mutable std::map<EntityId, CensusObject> selected_;
    public:
        explicit Actors(const Data& data) : data_(data) {}
        auto find(EntityId id) const {
            if (!selected_.contains(id)) if (const auto* row = data_.actor(id)) selected_.emplace(id, *row);
            return selected_.find(id);
        }
        auto end() const { return selected_.end(); }
    } actor_records;
    bool native_invasions_enabled{}, native_applying{};
    std::uint64_t& next_native_invasion;
    std::size_t& native_invasion_transport_count;
    std::vector<std::uint64_t>& native_invasion_queue;
    struct Invasions {
        Data& data;
        std::size_t size() const { return data.invasion_records; }
        void emplace(std::uint64_t number, NativeInvasion request) {
            data.invasion_rows.emplace(number, std::move(request)); ++data.invasion_records;
        }
    } native_invasions;
    ControlOwner(Data& data, std::span<const PlanetId> selected)
        : FleetOwner(data), input_(data.view()), completed_tick(data.base->completed_tick()), actor_records(data), native_invasions_enabled(data.invasions_enabled),
          next_native_invasion(data.next_invasion), native_invasion_transport_count(data.invasion_transports),
          native_invasion_queue(data.new_invasion_queue), native_invasions{data} {
        if (const auto& schema = input_->control()) {
            control.emplace(ControlView{schema->max_ground_forces, schema->players, schema->relations, schema->types, {}, schema->state});
            for (const auto id : selected)
                if (const auto* row = session_detail::native_find_row(schema->planets, id, [](const auto& item) { return item.planet; }))
                    control->planets.push_back(*row);
        }
        if (const auto& schema = input_->observer()) {
            observer.emplace();
            for (const auto id : selected)
                if (const auto* row = session_detail::native_find_row(schema->planets, id, [](const auto& item) { return item.planet; }))
                    observer->planets.push_back(*row);
        }
    }
    void publish_surface_arrivals(PlanetId planet, std::span<const SurfaceForce> arrivals) {
        if (!observer || arrivals.empty()) return;
        auto at = std::lower_bound(observer->planets.begin(), observer->planets.end(), planet,
            [](const auto& row, auto id) { return row.planet < id; });
        if (at == observer->planets.end() || at->planet != planet) return;
        for (const auto& row : arrivals) at->landed_transports.push_back({row.id, row.owner, false, false});
        dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    }
    void finish() {
        FleetOwner::finish();
        if (control && dirty[static_cast<std::size_t>(StateSection::control)])
            for (auto& row : control->planets) data_.control_rows.insert_or_assign(row.planet, std::move(row));
        if (observer && dirty[static_cast<std::size_t>(StateSection::observer)])
            for (auto& row : observer->planets) data_.observer_rows.insert_or_assign(row.planet, std::move(row));
    }
};
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
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::stage_native_invasion(
    NativeActionShadow& shadow, PlayerId player, FleetId fleet, std::span<const EntityId> transports, bool pending) {
    using Result = core::Result<NativeInvasionReceipt>;
    const auto published = shadow.snapshot(); const auto* row = shadow.fleet(fleet);
    const std::array<PlanetId, 1> selected{row ? row->orbit : PlanetId{}};
    shadow.detach(); NativeActionShadow::Data::ControlOwner owner(*shadow.data_, selected);
    auto result = session_detail::admit_native_invasion_rows(owner, player, fleet, transports, pending);
    owner.finish(); shadow.data_->publication = published; // invasion records have no snapshot publication
    if (!result || !result.value().admitted) return result;
    const auto& receipt = result.value();
    shadow.data_->action_requests.emplace_back(NativeActionShadow::Data::InvasionRequest{
        false, player, fleet, std::vector<EntityId>(transports.begin(), transports.end()), pending, receipt, 0});
    std::vector<std::uint8_t> bytes;
    for (const auto word : std::array<std::uint64_t, 9>{1, transports.empty() ? 1U : 4U, player, fleet,
        static_cast<std::uint64_t>(pending), static_cast<std::uint64_t>(receipt.admitted), receipt.request,
        static_cast<std::uint64_t>(receipt.queued), static_cast<std::uint64_t>(receipt.reason)}) detail::put_u64(bytes, word);
    for (const auto transport : transports) detail::put_u64(bytes, transport);
    if (auto saved = append_native_effect(NativeEffectKind::invasion, std::move(bytes)); !saved) return Result::failure(saved.error());
    if (auto continued = advance_native_segment(); !continued) return Result::failure(continued.error());
    return result;
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
std::shared_ptr<const GalacticSnapshot> GalacticSession::NativeActionShadow::snapshot() const { return data_->player ? data_->view() : nullptr; }
const Fleet* GalacticSession::NativeActionShadow::fleet(FleetId id) const { return data_->fleet(id); }
const CensusObject* GalacticSession::NativeActionShadow::actor(EntityId id) const { return data_->actor(id); }
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::admit_fleet(
    PlayerId player, const CommandPayload& payload, bool pending_local_conflict) {
    if (!std::holds_alternative<CreateFleet>(payload) && !std::holds_alternative<AddFleetMembers>(payload)
        && !std::holds_alternative<RemoveFleetMembers>(payload) && !std::holds_alternative<MergeFleets>(payload)
        && !std::holds_alternative<SetFleetOrbitSlot>(payload) && !std::holds_alternative<TransferFleetMembers>(payload)
        && !std::holds_alternative<MoveFleet>(payload))
        return invalid<NativeFleetReceipt>("native fleet layer requires a fleet command");
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests || encode_payload(payload).size() > galactic_replay_max_payload - 38
        || decode_payload(layer_of(payload), opcode_of(payload), encode_payload(payload)) != std::optional(payload))
        return invalid<NativeFleetReceipt>("invalid native fleet layer request or exhausted budget");
    const auto seats = data_->base->seats();
    const auto issuer = std::find_if(seats.begin(), seats.end(), [player](const auto& row) { return row.player == player && row.active; });
    if (issuer == seats.end()) return core::Result<NativeFleetReceipt>::success({false, 0, GalacticRejectReason::inactive_seat});
    const auto published = data_->view();
    detach(); data_->publication = published; // movement notification sees the pre-publication view
    Data::FleetOwner owner(*data_);
    const auto first = data_->events.size();
    const GameCommand command{{data_->base->completed_tick(), issuer->seat_id, 0}, payload};
    if (std::holds_alternative<MoveFleet>(payload))
        session_detail::execute_native_movement(owner, command, data_->base->completed_tick(), data_->events, pending_local_conflict, {});
    else session_detail::execute_native_fleet(owner, command, data_->base->completed_tick(), data_->events, pending_local_conflict, false);
    NativeFleetReceipt receipt;
    for (auto i = first; i < data_->events.size(); ++i) {
        const auto& event = data_->events[i];
        if (event.kind == GalacticEventKind::command_rejected) receipt.reason = event.reason;
        if (event.kind == GalacticEventKind::command_executed) { receipt.admitted = true; receipt.object = event.subject; }
    }
    owner.finish(); ++data_->work.requests;
    data_->action_requests.emplace_back(Data::FleetRequest{player, payload, pending_local_conflict, receipt});
    return core::Result<NativeFleetReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::launch_transport(
    PlayerId player, EntityId id, FleetId target, bool pending_local_conflict) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeFleetReceipt>("invalid native launch layer request or exhausted budget");
    const auto before = data_->view();
    std::vector<PlanetId> selected;
    if (before->control()) for (const auto& row : before->control()->planets)
        if (std::any_of(row.landed.begin(), row.landed.end(), [id](const auto& unit) { return unit.id == id; })) {
            selected.push_back(row.planet); break;
        }
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    const std::array<EntityId, 1> units{id}; bool admission_ran{};
    auto launched = session_detail::launch_native_surface_units(owner, player, units, target, 0,
        [&](const CommandPayload& payload) {
            const auto issuer = std::find_if(owner.seats.begin(), owner.seats.end(),
                [player](const auto& row) { return row.player == player && row.active; });
            if (issuer == owner.seats.end()) return core::Result<NativeFleetReceipt>::success({false, 0, GalacticRejectReason::inactive_seat});
            const auto first = data_->events.size();
            session_detail::execute_native_fleet(owner, GameCommand{{data_->base->completed_tick(), issuer->seat_id, 0}, payload},
                data_->base->completed_tick(), data_->events, pending_local_conflict, false);
            NativeFleetReceipt receipt;
            for (auto i = first; i < data_->events.size(); ++i) {
                if (data_->events[i].kind == GalacticEventKind::command_rejected) receipt.reason = data_->events[i].reason;
                else if (data_->events[i].kind == GalacticEventKind::command_executed) {
                    receipt.admitted = true; receipt.object = data_->events[i].subject;
                }
            }
            return core::Result<NativeFleetReceipt>::success(receipt);
        }, admission_ran);
    owner.finish(); data_->last_admission_ran = admission_ran; data_->last_published = admission_ran;
    if (!launched) return launched;
    if (!admission_ran) data_->publication = before;
    data_->action_requests.emplace_back(Data::ControlRequest{Data::ControlRequest::Kind::launch,
        player, id, target, pending_local_conflict, {}, launched.value(), false});
    return launched;
}
core::Result<void> GalacticSession::NativeActionShadow::recheck_orbit(PlayerId player, FleetId id) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player)
        || data_->work.requests >= data_->max_requests || !data_->movement)
        return invalid<void>("invalid native orbit recheck layer request or exhausted budget");
    const auto* fleet = data_->fleet(id);
    if (!fleet || fleet->owner != player || !fleet->orbit || fleet->motion.transit)
        return invalid<void>("native orbit recheck differs from its owned planet parent");
    const auto destination = fleet->orbit;
    const auto before = data_->view();
    detach(); data_->publication = before; ++data_->work.requests;
    Data::FleetOwner owner(*data_);
    auto conflict = owner.orbit_conflict(*fleet);
    if (!conflict) return core::Result<void>::failure(conflict.error());
    const auto ids = owner.movement_fleet_ids();
    const auto visit = [&](auto consume) { owner.visit_movement_fleets(ids, consume); };
    if (auto linked = session_detail::link_native_movement_orbit(owner, visit, id, destination,
        data_->base->completed_tick(), data_->events); !linked) return linked;
    if (conflict.value()) {
        if (auto gathered = session_detail::gather_native_movement_orbit(owner, visit, id, destination,
            data_->base->completed_tick(), data_->events); !gathered) return gathered;
        data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::movement_conflict, {}, id, 0, destination, GalacticRejectReason::none});
        if (data_->movement_listener) data_->movement_listener(data_->events.back());
    }
    owner.finish();
    data_->action_requests.emplace_back(Data::OrbitRecheckRequest{player, id});
    return core::Result<void>::success();
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::land_transports(
    PlayerId player, FleetId id, bool pending_local_conflict) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeFleetReceipt>("invalid native fleet landing layer request or exhausted budget");
    const auto before = data_->view();
    const auto* fleet = data_->fleet(id);
    const std::array<PlanetId, 1> selected{fleet ? fleet->orbit : PlanetId{}};
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    auto landed = session_detail::land_native_transports_rows(owner, player, id, pending_local_conflict, data_->events);
    owner.finish();
    if (!landed) return core::Result<NativeFleetReceipt>::failure(landed.error());
    data_->last_published = landed.value().published;
    if (!landed.value().published) data_->publication = before;
    const auto receipt = landed.value().receipt;
    data_->action_requests.emplace_back(Data::ControlRequest{Data::ControlRequest::Kind::fleet_land,
        player, id, 0, pending_local_conflict, {}, receipt, false});
    return core::Result<NativeFleetReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeTransportLandingReceipt> GalacticSession::NativeActionShadow::land_transport(
    PlayerId player, EntityId actor, PlanetId planet, bool pending_local_conflict, std::optional<std::uint32_t> live_landed) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeTransportLandingReceipt>("invalid native actor landing layer request or exhausted budget");
    const auto before = data_->view();
    const std::array<PlanetId, 1> selected{planet};
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    auto landed = session_detail::land_native_transport_rows(owner, player, actor, planet, pending_local_conflict, live_landed, data_->events);
    owner.finish();
    if (!landed) return core::Result<NativeTransportLandingReceipt>::failure(landed.error());
    data_->last_published = landed.value().published;
    if (!landed.value().published) data_->publication = before;
    const auto receipt = landed.value().receipt;
    data_->action_requests.emplace_back(Data::ControlRequest{Data::ControlRequest::Kind::actor_land,
        player, actor, planet, pending_local_conflict, live_landed, receipt.event, receipt.linked});
    return core::Result<NativeTransportLandingReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeProductionReceipt> GalacticSession::NativeActionShadow::admit_production(
    const NativeProductionRequest& request, const PartitionExecutor& executor, const NativeProductionHandler& notify,
    std::span<const TypeId> local_unlocks) {
    using Result = core::Result<NativeProductionReceipt>;
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != request.player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeProductionReceipt>("invalid native production layer request or exhausted budget");
    const auto before = data_->view();
    if (!before->production() || !before->player_economy() || !planet(request.planet))
        return invalid<NativeProductionReceipt>("native production requires a live producer");
    detach(); ++data_->work.requests;
    auto current = data_->view();
    if (data_->population_schema && data_->population_stale) {
        const auto detached = current->fleet_control() ? std::span<const DetachedUnit>(current->fleet_control()->detached_units) : std::span<const DetachedUnit>{};
        auto projected = session_detail::project_native_population(*data_->population_schema, current->planets(), current->fleets(), detached,
            current->control() ? &*current->control() : nullptr, current->movement(), current->commissioned_originals(), *current->production(), executor);
        if (!projected) { data_->publication = before; return Result::failure(projected.error()); }
        auto result = std::move(projected).value();
        auto production = std::make_shared<std::vector<ProductionPlayer>>(current->production()->players);
        static_cast<void>(session_detail::apply_native_population(*production, result.totals, result.resolved_counts));
        data_->production_projection = std::move(production);
        data_->population_projection = std::make_shared<const std::vector<PopulationTotal>>(std::move(result.totals));
        data_->population_stale = false; data_->publication.reset(); current = data_->view();
    }
    std::optional<IncomeSetup> sources;
    std::vector<session_detail::NativeIncomeSourceCandidate> candidates;
    if (current->income() && data_->income_schema) {
        sources = *data_->income_schema;
        const auto detached = current->fleet_control() ? std::span<const DetachedUnit>(current->fleet_control()->detached_units) : std::span<const DetachedUnit>{};
        session_detail::project_native_income(*sources, *data_->income_schema, current->control() ? &*current->control() : nullptr,
            current->planets(), current->fleets(), detached, &*current->production(), current->actor_records(), candidates);
    }
    auto checked = session_detail::prepare_native_production(request, *current->production(), *current->player_economy(), current->planets(),
        current->fleets(), current->population(), bool(data_->population_schema), sources ? &*sources : nullptr,
        data_->next_id, data_->live_objects, local_unlocks);
    // A refused offer retains the refreshed private census, but no publication.
    data_->publication = before;
    if (!checked) return Result::failure(checked.error());
    const auto& prepared = checked.value();
    if (!prepared.offer || prepared.admission != ProductionAdmission::allowed) {
        const NativeProductionReceipt receipt{prepared.admission, 0, prepared.offer ? prepared.offer->price : math::Fixed{}};
        data_->action_requests.emplace_back(Data::ProductionRequest{request, receipt});
        return Result::success(receipt);
    }
    if (!notify) return invalid<NativeProductionReceipt>("native production requires its ledger handler");
    if (!request.mark_cost || request.mark_cost->raw() < 0 || request.mark_cost->raw() > (std::int64_t{1} << 48)
        || request.mark_cost->raw() % math::Fixed::scale != 0)
        return invalid<NativeProductionReceipt>("native mark requires the resolved null-location integer type cost");
    const auto& offer = *prepared.offer; const auto id = data_->next_id;
    if (auto marked = notify({NativeProductionNotice::Kind::marked, request.player, id, *request.mark_cost, offer.price, offer.seconds, prepared.owner}); !marked)
        return Result::failure(marked.error());
    auto row = current->production()->planets[prepared.planet_index];
    row.queues[static_cast<std::size_t>(offer.domain)].push_back({offer.type, request.player, id, offer.price, offer.seconds, offer.seconds,
        data_->base->clock().galactic_frame, production_frames(offer.seconds).value(), true});
    data_->production_rows.insert_or_assign(request.planet, std::move(row)); ++data_->next_id;
    data_->population_stale = true; data_->publication.reset();
    data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::production_started, {}, request.planet, id, request.type, GalacticRejectReason::none});
    const NativeProductionReceipt receipt{ProductionAdmission::allowed, id, offer.price};
    data_->action_requests.emplace_back(Data::ProductionRequest{request, receipt});
    return Result::success(receipt);
}
void GalacticSession::NativeActionShadow::detach() {
    if (data_.use_count() != 1) {
        data_ = std::make_shared<Data>(*data_);
        data_->work.copied_fleet_rows += data_->fleet_rows.size();
        data_->work.copied_actor_rows += data_->actor_rows.size();
    }
    data_->publication.reset();
}
EntityId GalacticSession::NativeActionShadow::next_entity_id() const noexcept { return data_->next_id; }
PlayerId GalacticSession::NativeActionShadow::player() const noexcept { return data_->player; }
core::Result<GalacticSession::NativeActionShadow> GalacticSession::NativeActionShadow::for_player(
    PlayerId player, std::size_t max_requests) const {
    if (data_->tail || !data_->player || !player || player <= data_->player || !max_requests || max_requests > galactic_max_objects
        || std::none_of(data_->base->players().begin(), data_->base->players().end(),
            [player](const auto& row) { return row.player_id == player; }))
        return invalid<NativeActionShadow>("native player layers require ascending known players and bounded requests");
    auto state = std::make_shared<Data>();
    state->owner = data_->owner; state->base = data_->base; state->parent = data_;
    state->inherit_reads(data_);
    state->player = player; state->first_id = state->next_id = data_->next_id;
    state->live_objects = data_->live_objects; state->max_requests = max_requests;
    state->can_create = data_->can_create; state->victory = data_->victory;
    state->graph = data_->graph; state->movement = data_->movement; state->movement_listener = data_->movement_listener;
    state->population_schema = data_->population_schema; state->income_schema = data_->income_schema;
    state->fiscal_schema = data_->fiscal_schema; state->base_income = data_->base_income; state->income_edges = data_->income_edges;
    state->production_projection = data_->production_projection; state->population_projection = data_->population_projection;
    state->observer_generation = data_->observer_generation; state->population_stale = data_->population_stale;
    state->invasion_records = data_->invasion_records; state->invasion_transports = data_->invasion_transports;
    state->next_invasion = data_->next_invasion; state->invasions_enabled = data_->invasions_enabled;
    state->publication = data_->view(); // freeze the predecessor publication before sharing it
    if (state->publication->victory()) state->victory = std::shared_ptr<const VictoryState>(state->publication, &*state->publication->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(state)));
}
const GalacticSession::NativeActionShadow::Work& GalacticSession::NativeActionShadow::work() const noexcept { return data_->work; }
std::span<const GalacticSession::NativeActorChange> GalacticSession::NativeActionShadow::actor_changes() const noexcept { return data_->actor_changes; }
const Planet* GalacticSession::NativeActionShadow::planet(PlanetId id) const {
    const auto rows = data_->base->planets();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
std::optional<math::Fixed> GalacticSession::NativeActionShadow::credits(PlayerId id) const {
    for (const Data* layer = data_.get(); layer; layer = layer->read_parent.get()) {
        const auto changed = layer->accounts.find(id);
        if (changed != layer->accounts.end()) return changed->second;
    }
    if (!data_->base->player_economy()) return {};
    const auto& rows = data_->base->player_economy()->players;
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.player < key; });
    return at != rows.end() && at->player == id ? std::optional(at->credits) : std::nullopt;
}
const CensusObject* GalacticSession::NativeActionShadow::created_object(EntityId id) const {
    for (const Data* layer = data_.get(); layer; layer = layer->read_parent.get()) for (const auto& force : layer->forces) {
        const auto& rows = force->placement.originals;
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        if (at != rows.end()) return &*at;
    }
    return nullptr;
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
core::Result<math::Fixed> GalacticSession::NativeActionShadow::add_credits(PlayerId id, math::Fixed delta, math::Fixed multiplier) {
    auto before = credits(id);
    if (!before || data_->work.requests >= data_->max_requests)
        return invalid<math::Fixed>("native cash needs a known account within the request budget");
    const auto& accounts = data_->base->player_economy()->players;
    const auto at = std::lower_bound(accounts.begin(), accounts.end(), id, [](const auto& row, auto key) { return row.player < key; });
    auto balance = adjusted_credits(*before, delta, multiplier, at->credit_cap);
    if (!balance) return balance;
    auto actual = math::subtract(balance.value(), *before);
    if (!actual) return actual;
    std::shared_ptr<const VictoryState> staged_victory = data_->victory;
    if (data_->player && !data_->immediate_batch) {
        const auto current = data_->view();
        if (current->victory()) staged_victory = std::shared_ptr<const VictoryState>(current, &*current->victory());
    }
    bool victory_pending = false;
    // WGEP-16 / WGS-18: same scalar credit-condition admission as add_credits.
    if (staged_victory && (!staged_victory->pending || staged_victory->waits)) {
        auto victory = std::make_shared<VictoryState>(*staged_victory);
        VictoryContext context;
        context.multiplayer = victory->context.multiplayer;
        context.tutorial_delayed = victory->context.tutorial_delayed;
        context.players = victory->context.players;
        for (auto& player : context.players) {
            if (player.player == id) player.credits = balance.value();
            else if (auto value = credits(player.player)) player.credits = *value;
            if (data_->base->seat_control()) for (const auto& owner : data_->base->seat_control()->control.controllers) if (owner.player == player.player) {
                player.ai = owner.kind == FactionControl::ai; player.human = owner.kind == FactionControl::human;
            }
        }
        const InlineExecutor scalar;
        auto tested = test_victory(*victory, {VictoryKind::credits, id}, context, scalar, data_->base->clock().galactic_frame);
        if (!tested) return core::Result<math::Fixed>::failure(tested.error());
        victory_pending = tested.value();
        staged_victory = std::move(victory);
    }
    detach(); ++data_->work.requests; ++data_->work.copied_accounts;
    data_->accounts[id] = balance.value();
    data_->victory = std::move(staged_victory);
    if (victory_pending) data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::victory_pending, {}, id, 0,
        static_cast<std::uint64_t>(VictoryKind::credits), GalacticRejectReason::none});
    if (data_->player && !data_->immediate_batch && !data_->suppress_cash_request) data_->action_requests.emplace_back(Data::CashRequest{id, delta, multiplier, actual.value()});
    return actual;
}
core::Result<std::vector<EntityId>> GalacticSession::NativeActionShadow::create_force(const NativeProductionRequest& request,
    const NativeShadowForceBuilder& builder, const PartitionExecutor& executor) {
    using Result = core::Result<std::vector<EntityId>>;
    const auto current = data_->player ? data_->view() : data_->base;
    const auto& base = *current;
    if (!builder || !data_->can_create || !request.type || !base.production() || !base.production()->nested_creation || !base.control() || !base.movement()
        || !planet(request.planet) || data_->work.requests >= data_->max_requests
        || std::none_of(base.players().begin(), base.players().end(), [&](const auto& row) { return row.player_id == request.player; }))
        return invalid<std::vector<EntityId>>("native placement needs a qualified local provider and request budget");
    detach();
    std::shared_ptr<Data::Location> location;
    for (const Data* layer = data_->player ? nullptr : data_.get(); layer && !location; layer = layer->read_parent.get()) {
        const auto existing = layer->locations.find(request.planet);
        if (existing != layer->locations.end()) location = std::make_shared<Data::Location>(*existing->second);
    }
    if (!location) {
        const auto& planets = base.production()->planets;
        const auto production = std::lower_bound(planets.begin(), planets.end(), request.planet, [](const auto& row, auto id) { return row.planet < id; });
        const auto& presence = base.control()->planets;
        const auto control = std::lower_bound(presence.begin(), presence.end(), request.planet, [](const auto& row, auto id) { return row.planet < id; });
        if (production == planets.end() || production->planet != request.planet || control == presence.end() || control->planet != request.planet)
            return invalid<std::vector<EntityId>>("native placement location has no presence");
        location = std::make_shared<Data::Location>(); location->planet = *planet(request.planet);
        location->presence = *control; location->production = *production;
        // Read-only scene selection stays partitioned. Only this planet's
        // orbit is copied; neither its neighbours nor an ECS registry clone.
        std::vector<std::vector<Fleet>> selected(tick_partition_count);
        auto gathered = executor.execute_phase("native-force-local-inputs", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, base.fleets().size());
            for (auto index = range.begin; index < range.end; ++index) if (base.fleets()[index].orbit == request.planet)
                selected[partition].push_back(base.fleets()[index]);
        });
        if (!gathered) return Result::failure(gathered.error());
        auto orbit = std::make_shared<std::vector<Fleet>>();
        for (auto& rows : selected) for (auto& row : rows) orbit->push_back(std::move(row));
        auto contents = std::make_shared<std::vector<MovementContents>>();
        std::vector<EntityId> pending;
        for (const auto& fleet : *orbit) for (const auto& member : fleet.members) pending.push_back(member.id);
        std::set<EntityId> visited;
        while (!pending.empty()) {
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            std::erase_if(pending, [&](auto id) { return !visited.insert(id).second; });
            if (pending.empty()) break;
            std::vector<std::vector<MovementContents>> nested(tick_partition_count);
            auto collected = executor.execute_phase("native-force-local-contents", tick_partition_count, [&](std::size_t partition) {
                const auto range = partition_range(partition, pending.size());
                const auto& rows = base.movement()->contents;
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto id = pending[index];
                    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.container < key; });
                    if (at != rows.end() && at->container == id) nested[partition].push_back(*at);
                }
            });
            if (!collected) return Result::failure(collected.error());
            pending.clear();
            for (auto& rows : nested) for (auto& row : rows) {
                for (const auto& child : row.children) pending.push_back(child.id);
                contents->push_back(std::move(row));
            }
        }
        std::sort(contents->begin(), contents->end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        data_->work.copied_orbital_objects += orbit->size() + visited.size();
        location->orbit = std::move(orbit); location->contents = std::move(contents);
    }
    const auto skip_reader = [&](const Fleet& fleet) -> core::Result<bool> {
        std::vector<CensusObject> records;
        MovementSetup movement;
        std::vector<EntityId> pending;
        for (const auto& member : fleet.members) pending.push_back(member.id);
        std::set<EntityId> visited;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!visited.insert(id).second) return invalid<bool>("actor_combat_skip_container_cycle");
            const auto* actor = data_->player ? this->actor(id) : created_object(id);
            if (!actor) {
                const auto rows = base.actor_records();
                const auto at = std::lower_bound(rows.begin(), rows.end(), id,
                    [](const auto& row, auto key) { return row.id < key; });
                if (at == rows.end() || at->id != id) return invalid<bool>("actor_combat_skip_unavailable");
                actor = &*at;
            }
            records.push_back(*actor);
            if (actor->delete_pending || actor->combat_role == ActorCombatRole::ordinary) continue;
            const auto& contents = *location->contents;
            const auto at = std::lower_bound(contents.begin(), contents.end(), id,
                [](const auto& row, auto key) { return row.container < key; });
            if (at == contents.end() || at->container != id) return invalid<bool>("actor_payload_unavailable");
            movement.contents.push_back(*at);
            for (const auto& child : at->children) pending.push_back(child.id);
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(movement.contents.begin(), movement.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        return fleet_combat_skip(fleet, records, &movement);
    };
    NativeForceView view{location->planet, location->presence, location->production, *base.production()->nested_creation,
        base.control()->max_ground_forces, *location->orbit, *location->contents, skip_reader};
    auto made = builder(view, data_->next_id);
    if (!made) return Result::failure(made.error());
    auto actors = validate_native_force(request, made.value(), data_->next_id, data_->live_objects,
        *base.production()->nested_creation, [&](EntityId id) {
            const auto* private_actor = created_object(id);
            return private_actor ? private_actor->current_type != 0 : bool(base.current_type(id));
        });
    if (!actors) return actors;
    const auto& placement = made.value();
    auto orbit = std::make_shared<std::vector<Fleet>>(*location->orbit);
    orbit->insert(orbit->end(), placement.fleets.begin(), placement.fleets.end());
    std::sort(orbit->begin(), orbit->end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    auto contents = std::make_shared<std::vector<MovementContents>>(*location->contents);
    contents->insert(contents->end(), placement.contents.begin(), placement.contents.end());
    std::sort(contents->begin(), contents->end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    location->orbit = std::move(orbit); location->contents = std::move(contents);
    auto& presence = location->presence; auto& production = location->production;
    presence.landed.insert(presence.landed.end(), placement.landed.begin(), placement.landed.end());
    production.landed_transports = static_cast<std::int64_t>(presence.landed.size());
    production.objects.insert(production.objects.end(), placement.structures.begin(), placement.structures.end());
    if (!placement.structures.empty()) presence.special_control = placement.special_control;
    if (placement.ground_base) { presence.ground_base = *placement.ground_base; production.ground_level = placement.ground_base->level; }
    if (placement.starbase) { presence.starbase = *placement.starbase; production.space_level = placement.starbase->level;
        production.starbase_owner = placement.starbase->owner; production.starbase_type = placement.starbase_type; }
    ++data_->work.requests; ++data_->work.copied_planets;
    data_->work.prepared_objects += placement.next_id - data_->next_id;
    data_->live_objects += static_cast<std::size_t>(placement.next_id - data_->next_id);
    data_->next_id = placement.next_id;
    if (data_->player) {
        data_->control_rows.insert_or_assign(request.planet, location->presence);
        data_->production_rows.insert_or_assign(request.planet, location->production);
        auto movement = std::make_shared<MovementSetup>(*data_->movement);
        movement->contents.insert(movement->contents.end(), placement.contents.begin(), placement.contents.end());
        std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        data_->movement = std::move(movement);
        Data::FleetOwner owner(*data_);
        for (const auto& fleet : placement.fleets) owner.insert(fleet);
        owner.finish();
        auto projected_victory = current->victory();
        const auto ordered = ordered_births(placement.originals).value();
        std::map<EntityId, const CensusObject*> originals;
        for (const auto& row : placement.originals) originals.emplace(row.id, &row);
        for (const auto& birth : ordered) {
            auto row = *originals.at(birth.actor);
            if (row.combat_role == ActorCombatRole::unavailable) {
                const auto& types = base.production()->nested_creation->types;
                const auto fact = std::lower_bound(types.begin(), types.end(), row.current_type, [](const auto& value, auto id) { return value.type < id; });
                if (fact != types.end() && fact->type == row.current_type && fact->combat_role != ActorCombatRole::unavailable) {
                    row.combat_role = fact->combat_role; row.abilities = fact->abilities; row.garrison = false;
                }
            }
            if (projected_victory) session_detail::update_native_victory_actor(*projected_victory, row);
            data_->actor_rows.insert_or_assign(row.id, std::move(row)); ++data_->observer_generation;
        }
        if (projected_victory && !data_->immediate_batch)
            data_->victory = std::make_shared<const VictoryState>(std::move(*projected_victory));
        if (base.observer() && !placement.landed.empty()) {
            const auto* shown = session_detail::native_find_row(base.observer()->planets, request.planet, [](const auto& row) { return row.planet; });
            if (shown) {
                auto row = *shown;
                for (const auto& unit : placement.landed) row.landed_transports.push_back({unit.id, unit.owner, false, false});
                data_->observer_rows.insert_or_assign(request.planet, std::move(row));
            }
        }
        ++data_->observer_generation; data_->population_stale = true; data_->publication.reset();
        if (base.production()->fleet_blockers) {
            const auto projected = data_->view();
            for (const auto& fleet : placement.fleets) {
                auto skip = fleet_combat_skip(*data_->fleet(fleet.id), projected->actor_records(), data_->movement.get());
                if (!skip) return Result::failure(skip.error());
                data_->fleet_blockers[fleet.id] = skip.value(); data_->created_blockers.push_back(fleet.id);
            }
        }
        data_->publication.reset();
    }
    data_->locations[request.planet] = std::move(location);
    const auto births = ordered_births(placement.originals).value();
    data_->actor_changes.insert(data_->actor_changes.end(), births.begin(), births.end());
    data_->forces.push_back(std::make_shared<const Data::Force>(Data::Force{request, std::move(made).value()}));
    if (data_->player && !data_->immediate_batch) data_->action_requests.emplace_back(Data::ForceRequest{data_->forces.back(), actors.value()});
    data_->publication.reset();
    return actors;
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
                const auto pending_before = impl_->pending_actor_changes.size();
                std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
                if (!data.forces.empty()) {
                    auto movement = std::make_shared<MovementSetup>(*impl_->movement_inputs);
                    for (const auto& force : data.forces)
                        if (auto applied = impl_->apply_native_force(force->request, force->placement, *movement); !applied) return applied;
                    std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& x, const auto& y) { return x.container < y.container; });
                    impl_->movement_inputs = std::move(movement);
                }
                if (actor_changes_prepared) impl_->pending_actor_changes.resize(pending_before);
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
    const auto pending_before = impl_->pending_actor_changes.size();
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
        if (actor_changes_prepared) impl_->pending_actor_changes.resize(pending_before);
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
