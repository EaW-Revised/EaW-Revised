#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>
#include <limits>
#include <set>

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
    if (native_actor_changes_handler) pending_actor_changes.push_back({NativeActorChange::Kind::lost, id, 0, {}, {}, {}});
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
core::Result<GalacticSession::NativeProductionReceipt> GalacticSession::admit_native_production(const NativeProductionRequest& request, const PartitionExecutor& executor) {
    using Result = core::Result<NativeProductionReceipt>;
    auto& impl = *impl_;
    if (impl.failed || !impl.production || !impl.economy || !impl.planets.contains(request.planet))
        return invalid<NativeProductionReceipt>("native production requires a live producer");
    if (auto refreshed = impl.refresh_population(executor); !refreshed) return Result::failure(refreshed.error());
    auto& state = *impl.production;
    auto planet = std::lower_bound(state.planets.begin(), state.planets.end(), request.planet,
        [](const auto& row, auto id) { return row.planet < id; });
    auto player = std::lower_bound(state.players.begin(), state.players.end(), request.player,
        [](const auto& row, auto id) { return row.player < id; });
    if (planet == state.planets.end() || planet->planet != request.planet || player == state.players.end()
        || player->player != request.player || player->human)
        return invalid<NativeProductionReceipt>("native production requires its AI player and planet");
    const auto index = static_cast<std::size_t>(player - state.players.begin());
    auto original = std::lower_bound(planet->offers.begin(), planet->offers.end(), request.type,
        [](const auto& row, auto id) { return row.type < id; });
    if (original == planet->offers.end() || original->type != request.type) return Result::success({});
    const auto sources = impl.income ? std::optional(impl.live_income_inputs()) : std::nullopt;
    auto prepared = prepared_production_offer(*original, sources ? &*sources : nullptr,
        impl.sorted_planets(), request.planet, request.player);
    if (!prepared) return Result::failure(prepared.error());
    const auto& offer = prepared.value();
    // The native request has its own ledger payment route. Reuse content,
    // ownership and population gates without pretending it is a human order.
    auto admission_player = *player; admission_player.human = true; admission_player.pay_as_you_go = true;
    const auto owner = impl.registry.get<session_detail::PlanetData>(impl.planets.at(request.planet)).owner;
    const auto admission = production_admission({state, *planet, admission_player, impl.economy->players[index],
        impl.sorted_fleets(), owner, impl.setup.census ? &impl.population[index] : nullptr,
        impl.next_id, impl.live_object_count, nullptr, impl.corruption_local_unlocks(request.planet, request.player)}, &offer, offer.domain);
    if (admission != ProductionAdmission::allowed) return Result::success({admission, 0, offer.price});
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
    impl.snapshot = impl.make_snapshot(std::move(events));
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
    auto staged = builder(*impl.snapshot, impl.next_id);
    if (!staged) return Result::failure(staged.error());
    const auto& placement = staged.value();
    auto actors = validate_native_force(request, placement, impl.next_id, impl.live_object_count,
        *impl.production->nested_creation, [&](EntityId id) { return bool(impl.snapshot->current_type(id)); });
    if (!actors) return actors;
    auto movement = std::make_shared<MovementSetup>(*impl.movement_inputs);
    if (auto applied = impl.apply_native_force(request, placement, *movement); !applied)
        return Result::failure(applied.error());
    std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    impl.movement_inputs = std::move(movement);
    const auto old = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({old.begin(), old.end()});
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
    const void* owner{};
    std::shared_ptr<const GalacticSnapshot> base;
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
void GalacticSession::NativeActionShadow::detach() {
    if (data_.use_count() != 1) data_ = std::make_shared<Data>(*data_);
}
EntityId GalacticSession::NativeActionShadow::next_entity_id() const noexcept { return data_->next_id; }
const GalacticSession::NativeActionShadow::Work& GalacticSession::NativeActionShadow::work() const noexcept { return data_->work; }
std::span<const GalacticSession::NativeActorChange> GalacticSession::NativeActionShadow::actor_changes() const noexcept { return data_->actor_changes; }
const Planet* GalacticSession::NativeActionShadow::planet(PlanetId id) const {
    const auto rows = data_->base->planets();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
std::optional<math::Fixed> GalacticSession::NativeActionShadow::credits(PlayerId id) const {
    const auto changed = data_->accounts.find(id);
    if (changed != data_->accounts.end()) return changed->second;
    if (!data_->base->player_economy()) return {};
    const auto& rows = data_->base->player_economy()->players;
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.player < key; });
    return at != rows.end() && at->player == id ? std::optional(at->credits) : std::nullopt;
}
const CensusObject* GalacticSession::NativeActionShadow::created_object(EntityId id) const {
    for (const auto& force : data_->forces) {
        const auto& rows = force->placement.originals;
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        if (at != rows.end()) return &*at;
    }
    return nullptr;
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::begin_native_actions(std::size_t max_requests) const {
    if (impl_->failed || !max_requests || max_requests > galactic_max_objects)
        return invalid<NativeActionShadow>("native shadow requires a live campaign and bounded requests");
    auto state = std::make_shared<NativeActionShadow::Data>();
    state->owner = impl_.get(); state->base = impl_->snapshot;
    state->first_id = state->next_id = impl_->next_id;
    state->live_objects = impl_->live_object_count; state->max_requests = max_requests;
    state->can_create = impl_->setup.census.has_value();
    if (state->base->victory()) state->victory = std::shared_ptr<const VictoryState>(state->base, &*state->base->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(state)));
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
    bool victory_pending = false;
    // WGEP-16 / WGS-18: same scalar credit-condition admission as add_credits.
    if (data_->victory && (!data_->victory->pending || data_->victory->waits)) {
        auto victory = std::make_shared<VictoryState>(*data_->victory);
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
    return actual;
}
core::Result<std::vector<EntityId>> GalacticSession::NativeActionShadow::create_force(const NativeProductionRequest& request,
    const NativeShadowForceBuilder& builder, const PartitionExecutor& executor) {
    using Result = core::Result<std::vector<EntityId>>;
    const auto& base = *data_->base;
    if (!builder || !data_->can_create || !request.type || !base.production() || !base.production()->nested_creation || !base.control() || !base.movement()
        || !planet(request.planet) || data_->work.requests >= data_->max_requests
        || std::none_of(base.players().begin(), base.players().end(), [&](const auto& row) { return row.player_id == request.player; }))
        return invalid<std::vector<EntityId>>("native placement needs a qualified local provider and request budget");
    detach();
    std::shared_ptr<Data::Location> location;
    const auto existing = data_->locations.find(request.planet);
    if (existing != data_->locations.end()) location = std::make_shared<Data::Location>(*existing->second);
    else {
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
            const auto* actor = created_object(id);
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
    data_->locations[request.planet] = std::move(location);
    const auto births = ordered_births(placement.originals).value();
    data_->actor_changes.insert(data_->actor_changes.end(), births.begin(), births.end());
    data_->forces.push_back(std::make_shared<const Data::Force>(Data::Force{request, std::move(made).value()}));
    return actors;
}
core::Result<void> GalacticSession::commit_native_actions(const NativeActionShadow& shadow, bool actor_changes_prepared) {
    const auto& data = *shadow.data_;
    if (impl_->failed || data.owner != impl_.get() || data.base != impl_->snapshot || data.first_id != impl_->next_id)
        return invalid<void>("native shadow is stale or belongs to another campaign");
    // Validation/placement and notification staging are already complete.
    // This ordered commit applies only new output entities, never a scene copy.
    const auto pending_before = impl_->pending_actor_changes.size();
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
    std::vector<GalacticEvent> events(data.base->events().begin(), data.base->events().end());
    events.insert(events.end(), data.events.begin(), data.events.end());
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
