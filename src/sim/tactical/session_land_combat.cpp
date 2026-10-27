#include "session_combat_phases.hpp"
#include "session_land_combat_internal.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <array>

namespace eawr::sim::tactical::session_detail {
namespace {
using math::Fixed;

bool target_alive(const detail::CombatWorld& world, const detail::CombatUnit& source,
    const detail::CombatUnit* target) {
    return target && target->profile && target->durability && target->durability->hull > Fixed{}
        && target->profile->valid_target && target->profile->living_projectile_collision
        && world.hostile(source.owner, target->owner);
}

const detail::CombatUnit* nearest_land_target(const detail::CombatWorld& world, const detail::CombatUnit& source,
    const Fixed radius) {
    if (!source.profile || !source.profile->max_attack_distance) return nullptr;
    const auto range = *source.profile->max_attack_distance;
    // Expand the broad phase by loaded target geometry; the narrow phase retains WWP-20's planar range.
    Fixed reach;
    if (!math::try_add(range, radius, reach)) return nullptr;
    const detail::CombatUnit* best = nullptr;
    Fixed best_distance;
    for (const auto& player : world.players) {
        if (!world.hostile(source.owner, player.player_id)) continue;
        for (const auto id : world.index.range(source.position, reach, RangeMetric::planar, player.player_id)) {
            const auto* candidate = world.find(id);
            if (!target_alive(world, source, candidate)
                || (source.profile->attack_category_restrictions & candidate->profile->category_bits) != 0) continue;
            math::Vec2 delta;
            if (!math::try_subtract(candidate->position.x, source.position.x, delta.x)
                || !math::try_subtract(candidate->position.y, source.position.y, delta.y)) continue;
            auto distance = math::length(delta);
            if (!distance) continue;
            auto admitted = detail::target_attack_distance(world.motion, *candidate, source.position, range, false);
            if (!admitted || distance.value() > admitted.value()) continue;
            if (!best || distance.value() < best_distance || (distance.value() == best_distance && candidate->id < best->id)) {
                best = candidate; best_distance = distance.value();
            }
        }
    }
    return best;
}
}

core::Result<detail::ProjectileStep> land_projectile_contact(const LandTerrain& terrain, detail::ProjectileStep flight,
    std::vector<Fixed>* const crossings) {
    // Query only as far as the first object contact. An exact endpoint contact belongs to the object.
    const auto end = flight.hit ? flight.contact : flight.projectile.position;
    const auto ground = terrain.segment_intersection(flight.from, end, {}, crossings);
    if (ground.status == LandSegmentStatus::overflow)
        return core::Result<detail::ProjectileStep>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "land projectile terrain intersection overflow"));
    if (ground.status == LandSegmentStatus::hit && ground.point
        && (!flight.hit || (ground.fraction < Fixed::from_raw(Fixed::scale) && *ground.point != flight.contact))) {
        flight.hit.reset(); flight.expired = true;
        flight.contact = *ground.point; flight.projectile.position = *ground.point;
        flight.expiry_reason = ProjectileExpiryReason::ground_contact;
    }
    return core::Result<detail::ProjectileStep>::success(std::move(flight));
}

// LC-26/27/30, LC-33/34: authored Tactical_Health, Armor_Type, Shield_Armor_Type,
// Shield_Points, Damage_Type, Projectile_Types, Projectile_Damage,
// Projectile_Fire_Recharge_Seconds, Projectile_Fire_Pulse_Count,
// Projectile_Fire_Pulse_Delay_Seconds, Targeting_Max_Attack_Distance,
// Targeting_Min_Attack_Distance, Ranged_Target_Z_Adjust, Targeting_Fire_Inaccuracy,
// Collidable_By_Projectile_Living, Is_Valid_Target, Attack_Category_Restrictions,
// CategoryMask flow through the copied combat world, shared weapon and shared damage driver.
core::Result<void> Tick::land_combat(std::vector<LiveUnit>& live,
    const std::vector<std::pair<EntityId, math::Vec3>>& starts,
    std::vector<Event>& events, std::vector<CombatEvent>& combat_events) {
    if (impl_->combat.profiles.empty() && impl_->projectiles.empty()) return core::Result<void>::success();
    auto& land = *impl_->land;
    gather_.emplace(); gather_->tick_work.emplace();
    auto& moving = gather_->moving.emplace(std::move(live));
    targets_.emplace();
    auto& built = targets_->world.emplace(impl_->combat_world(moving, starts, tick, executor));
    if (!built) return core::Result<void>::failure(built.error());
    auto& world = built.value();
    // DG-30: projectile flight reads the persistent per-player collection, committed serially in ascending
    // ID from boxes the workers fill into their own slots. Land target scans use the spatial index
    // directly, so only the projectile collection (and its checkpointed history) is kept.
    auto& projectile_collection = targets_->projectile_collection.emplace(impl_->projectile_collection);
    if (impl_->durability.damage) {
        std::vector<std::optional<detail::CollectionTrees::Member>> boxes(moving.size());
        std::vector<std::optional<core::Diagnostic>> box_errors(moving.size());
        core::tick_profile::Scope phase("land-collection-boxes");
        const auto boxed = executor.execute_phase("land-collection-boxes", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto* profile = world.units[i].profile;
                if (profile == nullptr || !profile->living_projectile_collision || !profile->collision) continue;
                auto member = impl_->collection_member(moving[i], true);
                if (!member) {
                    box_errors[i] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(moving[i].state.entity_id) + ": "
                            + member.error().message);
                    continue;
                }
                boxes[i] = member.value();
            }
        });
        if (!boxed) return boxed;
        for (auto& error : box_errors) if (error) return core::Result<void>::failure(std::move(*error));
        std::vector<detail::CollectionTrees::Member> collidables;
        collidables.reserve(moving.size());
        for (const auto& box : boxes) if (box) collidables.push_back(*box);
        projectile_collection.update(collidables, tick);
    }
    world.projectile_collection = &projectile_collection;
    const auto squads = land.movement.squads();
    const auto members = land.movement.members();
    // Q1: one shared target from the first living member; a target squad resolves through its live roster.
    std::vector<std::vector<EntityId>> rosters(squads.size());
    for (const auto& member : members) rosters[member.squad].push_back(member.object);
    for (std::size_t i = 0; i < squads.size(); ++i) world.teams.emplace_back(squads[i].object, rosters[i]);
    std::sort(world.teams.begin(), world.teams.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<EntityId, std::uint64_t>> selected(squads.size());
    {
        core::tick_profile::Scope phase("land-targets");
        const auto scanned = executor.execute_phase("land-targets", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, squads.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto* leader = rosters[i].empty() ? nullptr : world.find(rosters[i].front());
                const auto* root = live_unit(moving, squads[i].object);
                auto held = land.combat_state.targets.at(squads[i].object);
                if (!leader || !leader->profile || !root) { selected[i] = held; continue; }
                if (root->state.order.kind == OrderKind::attack) {
                    const auto* ordered = world.resolve_target(world.find(root->state.order.target), leader->position);
                    if (target_alive(world, *leader, ordered)) {
                        selected[i] = {ordered->id, held.second}; continue;
                    }
                }
                const auto* current = world.find(held.first);
                if (!target_alive(world, *leader, current)) held.first = invalid_entity_id;
                if (tick >= held.second) {
                    CombatRandom random(world.seed, tick, squads[i].object, ship_slot);
                    held.second = tick + land_opportunity_scan_frames + random.uniform(0, land_opportunity_scan_jitter);
                    const auto* target = nearest_land_target(world, *leader, land.target_query_margin);
                    held.first = target ? target->id : invalid_entity_id;
                }
                selected[i] = held;
            }
        });
        if (!scanned) return scanned;
    }
    for (std::size_t i = 0; i < squads.size(); ++i) land.combat_state.targets[squads[i].object] = selected[i];
    std::vector<std::optional<detail::CombatStep>> shots(moving.size());
    std::vector<std::optional<core::Diagnostic>> errors(moving.size());
    {
        core::tick_profile::Scope phase("land-fire");
        const auto fired = executor.execute_phase("land-fire", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& source = moving[i];
                if (!source.combat) continue;
                auto unit = world.units[i];
                auto input = *source.combat;
                const auto member = std::lower_bound(members.begin(), members.end(), unit.id,
                    [](const auto& row, EntityId id) { return row.object < id; });
                if (member != members.end() && member->object == unit.id && land_share_leader_target) {
                    const auto choice = selected[member->squad];
                    input.attack_target = choice.first; input.next_scan_frame = choice.second;
                    input.attack_hardpoint = no_hardpoint;
                    const auto* ordered = source.state.order.kind == OrderKind::attack
                        ? world.resolve_target(world.find(source.state.order.target), unit.position) : nullptr;
                    input.direct = target_alive(world, unit, ordered);
                    unit.target_prepared = true;
                }
                unit.combat = &input;
                auto step = detail::step_combat(world, unit);
                if (!step) errors[i] = step.error();
                else shots[i] = std::move(step).value();
            }
        });
        if (!fired) return fired;
    }
    auto& fired_events = targets_->combat_events.emplace();
    auto& offsets = targets_->aim_offsets.emplace();
    for (std::size_t i = 0; i < moving.size(); ++i) {
        if (errors[i]) return core::Result<void>::failure(*errors[i]);
        if (!shots[i]) continue;
        moving[i].combat = std::move(shots[i]->state);
        fired_events.insert(fired_events.end(), shots[i]->events.begin(), shots[i]->events.end());
        offsets.insert(offsets.end(), shots[i]->aim_offsets.begin(), shots[i]->aim_offsets.end());
        if (moving[i].durability && shots[i]->energy_spent > Fixed{})
            moving[i].durability->energy = Fixed::from_raw(moving[i].durability->energy.raw() - shots[i]->energy_spent.raw());
    }
    CombatPhases driver(*this);
    {
        core::tick_profile::Scope phase("land-projectiles");
        if (auto flown = projectile_inputs(); !flown) return flown;
        if (auto launched = driver.launch_shots(); !launched) return launched;
    }
    staging_.emplace(); staging_->arrivals.emplace(); staging_->unloaded.emplace(); staging_->ledgers.emplace(impl_->ledgers);
    moved_.emplace(); moved_->craft_defense.emplace();
    craft_prep_.emplace(); craft_prep_->craft_squadron.emplace(); craft_prep_->minds.emplace(impl_->minds);
    tracked_.emplace(); tracked_->replans.emplace();
    tracked_->staged.emplace(std::move(moving));
    {
        core::tick_profile::Scope phase("land-impacts");
        if (auto hit = driver.impacts(); !hit) return hit;
    }
    // The shared journal already removed killed children. Land additionally owns their physical roster.
    bool removed = false;
    for (const auto& killed : *impacts_->killed) {
        const auto member = std::lower_bound(land.movement.members().begin(), land.movement.members().end(), killed.entity_id,
            [](const auto& row, EntityId id) { return row.object < id; });
        if (member != land.movement.members().end() && member->object == killed.entity_id) {
            if (!land.movement.remove_member(killed.entity_id)) return core::Result<void>::failure(
                detail::diagnostic(diagnostic_codes::worker_failure, "land death roster removal failed"));
            removed = true;
        }
        if (land.static_roots.erase(killed.entity_id)) removed = true;
    }
    auto& staged = *tracked_->staged;
    if (removed) for (auto request = land.requests.begin(); request != land.requests.end();) {
        if (std::any_of(land.movement.squads().begin(), land.movement.squads().end(),
            [&](const auto& row) { return row.object == request->first; })) { ++request; continue; }
        const auto root = staged.find(request->first);
        if (root != staged.end()) {
            impacts_->events->push_back(Event{tick, EventKind::unit_destroyed, root->second.state.owner, 0, root->first});
            impl_->adjust_owned(root->second.state, false);
            staged.erase(root);
        }
        land.searches.erase(request->first); land.combat_state.targets.erase(request->first);
        request = land.requests.erase(request);
    }
    if (removed && land_free_occupancy_at_death) {
        std::vector<LandOccupation> occupations;
        for (const auto& occupation : land.occupancy.occupations())
            if (staged.find(occupation.object) != staged.end()) occupations.push_back(occupation);
        if (auto committed = land.occupancy.commit(occupations, executor); !committed) return committed;
    }
    // Land has no capture/system instance stage; the shared driver commits its damage journal directly.
    if (auto committed = driver.commit_survivors(); !committed) return committed;
    mark("survivors", false);
    live = std::move(*surviving_->survivors);
    impl_->projectiles = std::move(*impacts_->projectiles);
    impl_->next_projectile = *flights_->next_projectile;
    impl_->pending_blast_damage = std::move(*impacts_->pending_blast_damage);
    if (!pending_losses_.empty()) {
        // Loss history is immutable snapshot storage; copy only on a sparse death commit.
        auto losses = std::make_shared<std::vector<BattleLoss>>(*impl_->losses);
        losses->insert(losses->end(), pending_losses_.begin(), pending_losses_.end());
        impl_->losses = std::move(losses);
    }
    impl_->projectile_collection = std::move(projectile_collection);
    events = std::move(*impacts_->events); combat_events = std::move(fired_events);
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::session_detail
