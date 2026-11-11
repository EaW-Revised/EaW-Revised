#include "session_combat_phases.hpp"
#include "session_land_formation.hpp"
#include "session_land_garrison.hpp"
#include "session_land_combat_internal.hpp"
#include "tactical_internal.hpp"
#include "combat_algorithms.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <array>

namespace eawr::sim::tactical::session_detail {
namespace {
using math::Fixed;

// LC-27/LC-103: a limbo object (an occupant inside a garrison structure) is never a target.
bool target_alive(const detail::CombatWorld& world, const detail::CombatUnit& source,
    const detail::CombatUnit* target) {
    return target && target->profile && !target->in_limbo && target->durability && target->durability->hull > Fixed{}
        && target->profile->valid_target && target->profile->living_projectile_collision
        && world.hostile(source.owner, target->owner);
}

}

core::Result<void> Tick::land_victory_census(const std::vector<LiveUnit>& live) {
    if (impl_->victory.condition != VictoryCondition::all_enemy_units_destroyed) return core::Result<void>::success();
    std::array<std::array<std::uint64_t, max_players>, tick_partition_count> parts{};
    const auto counted = executor.execute_phase("land-victory-census", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, live.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& unit = live[i].state;
            if (!std::binary_search(impl_->victory.relevant_types.begin(), impl_->victory.relevant_types.end(), unit.type_id)) continue;
            const auto owner = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(), unit.owner,
                [](const Player& row, PlayerId id) { return row.player_id < id; });
            if (owner != impl_->setup.players.end() && owner->player_id == unit.owner)
                ++parts[partition][static_cast<std::size_t>(owner - impl_->setup.players.begin())];
        }
    });
    if (!counted) return counted;
    for (const auto& part : parts) for (std::size_t i = 0; i < impl_->setup.players.size(); ++i)
        victory_counts_[i] += part[i];
    // G-05: occupants kept off the live roster inside a host still belong to their player's force.
    for (const auto& entry : impl_->land->garrison_entries) {
        if (entry.second.phase != LandGarrisonPhase::inside && entry.second.phase != LandGarrisonPhase::waiting_exit) continue;
        for (const auto& hidden : entry.second.hidden) {
            const auto& unit = hidden.state;
            if (!std::binary_search(impl_->victory.relevant_types.begin(), impl_->victory.relevant_types.end(), unit.type_id)) continue;
            const auto owner = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(), unit.owner,
                [](const Player& row, PlayerId id) { return row.player_id < id; });
            if (owner != impl_->setup.players.end() && owner->player_id == unit.owner)
                ++victory_counts_[static_cast<std::size_t>(owner - impl_->setup.players.begin())];
        }
    }
    return core::Result<void>::success();
}

core::Result<void> Tick::land_fire_flashes(const SensorField& sensing, const std::vector<CombatEvent>& combat_events,
    std::vector<FogFlash>& flashes) const {
    if (!targets_ || !targets_->world || !*targets_->world) return core::Result<void>::success();
    const auto& view = targets_->world->value();
    for (const auto& event : combat_events) {
        if (event.kind != CombatEventKind::weapon_fired) continue;
        const auto* shooter = view.find(event.shooter);
        const auto* target = view.find(event.target);
        if (shooter == nullptr || target == nullptr || !sensing.reveal_range(shooter->type_id)) continue;
        const auto* sensor = sensing.profile(shooter->type_id);
        const auto x = math::add(shooter->position.x, sensor->flash_offset.x);
        const auto y = math::add(shooter->position.y, sensor->flash_offset.y);
        if (!x || !y) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "fire reveal centre exceeds the coordinate range"));
        flashes.push_back(FogFlash{target->owner, {x.value(), y.value(), shooter->position.z}, sensor->flash_radius});
    }
    return core::Result<void>::success();
}

void Tick::land_commit_victory(std::vector<Event>& events) {
    impl_->starbases = std::move(pending_starbases_);
    impl_->outcome = pending_outcome_;
    if (pending_victory_event_) events.push_back(*pending_victory_event_);
}

namespace {
using Judge = detail::combat_detail::UnitCombat;

struct LandScan {
    const detail::CombatUnit* found{};
    std::optional<Fixed> priority{};
};

// LC-90 (R6/R7): the fog test of the target-suitability order, for the firing object's owner with the force flag
// (the AI is tested too). It reads the published mask of the previous tick, the same view the shared weapon gates
// read (visible_to_me), so a target no revealer of the owner covers is neither found nor kept nor fired at.
bool land_fogged(const detail::CombatUnit& source, const detail::CombatUnit& target) noexcept {
    return target.team != source.team && (target.visible_to & (std::uint64_t{1} << source.player_index)) == 0U;
}

// LC-27 (the slice this step already ran): alive, hostile, valid and projectile-collidable, not fogged for the
// scanner's owner (LC-90, in the retail order: after the never-target kinds, before the category and weapon
// tests), not in the scanner's Attack_Category_Restrictions, and some weapon of the scanner may fire at its
// category (retail: a target no weapon can fire at is unsuitable). The set's own exclusions (LC-28) come from
// target_priority.
bool land_target_admitted(const detail::CombatWorld& world, const detail::CombatUnit& source,
    const detail::CombatUnit* target) {
    if (!target_alive(world, source, target) || land_fogged(source, *target)
        || (source.profile->attack_category_restrictions & target->profile->category_bits) != 0) return false;
    // A leader with no weapon at all keeps the earlier slice's behaviour (its squad's armed members still fire at the
    // shared target), so the weapon test applies only to a leader that has weapons.
    const auto& weapons = source.profile->weapons;
    return weapons.empty() || std::any_of(weapons.begin(), weapons.end(),
        [&](const WeaponProfile& weapon) { return (weapon.category_restrictions & target->profile->category_bits) == 0; });
}

// LC-102: the scanner's authored attack distance. A limbo shooter (an occupant) reads the reach the combat world armed it
// with, which carries the garrisoned range multiplier; every other object keeps its profile's distance.
std::optional<Fixed> land_attack_range(const detail::CombatUnit& source) {
    if (!source.profile) return std::nullopt;
    return source.in_limbo ? source.attack_distance() : source.profile->max_attack_distance;
}

// LC-102: occupants of a structure shoot farther by the data's multiplier, for the object weapon (the reach the shared fire
// gates read) and for hardpoint weapons (their fire range factor). Both are set once, before any scan reads them.
void arm_occupants(detail::CombatWorld& world, const LandOccupantFire& fire, const std::optional<Fixed>& multiplier) {
    for (const auto& shooter : fire.ids) {
        const auto unit = std::lower_bound(world.units.begin(), world.units.end(), shooter.id,
            [](const detail::CombatUnit& row, const EntityId id) { return row.id < id; });
        if (unit == world.units.end() || unit->id != shooter.id) continue;
        unit->in_limbo = true;
        if (!multiplier || multiplier->raw() == Fixed::scale) continue;
        Fixed grown;
        if (const auto base = unit->attack_distance(); base && math::try_multiply(*base, *multiplier, grown))
            unit->ability_attack_range = grown;
        if (math::try_multiply(unit->fire_range, *multiplier, grown)) unit->fire_range = grown;
    }
}

// WWP-20: the target lies within the scanner's planar attack reach (its range plus the target's loaded geometry).
// This is the scan's narrow phase and, until a chase is hosted, also the test that keeps a held target (LC-29 note:
// range stands in for the chase; the fog clear is land_target_admitted).
bool land_in_reach(const detail::CombatWorld& world, const detail::CombatUnit& source, const detail::CombatUnit& target) {
    const auto authored = land_attack_range(source);
    if (!authored) return false;
    const auto range = *authored;
    math::Vec2 delta;
    if (!math::try_subtract(target.position.x, source.position.x, delta.x)
        || !math::try_subtract(target.position.y, source.position.y, delta.y)) return false;
    auto distance = math::length(delta);
    if (!distance) return false;
    auto admitted = detail::target_attack_distance(world.motion, target, source.position, range, false);
    return static_cast<bool>(admitted) && distance.value() <= admitted.value();
}

// LC-29/LC-30: Scan_For_New_Target_In_Attack_Range over this step's candidate gatherer. Players are visited
// from one keyed random start; each hostile player's best candidate is chosen by Is_Better_Target (Judge::better)
// against the best so far, stopping early on the held target at priority exactly 1.0. A later player's best
// overwrites an earlier one's, as in the shared space scan (combat_targeting.cpp).
LandScan land_scan(const detail::CombatWorld& world, const detail::CombatUnit& source, Judge& judge,
    const EntityId held, const Fixed radius, CombatRandom& random) {
    LandScan result;
    const auto authored = land_attack_range(source);
    if (!authored || world.players.empty()) return result;
    const auto range = *authored;
    // Expand the broad phase by loaded target geometry; the narrow phase retains WWP-20's planar range.
    Fixed reach;
    if (!math::try_add(range, radius, reach)) return result;
    const auto count = world.players.size();
    auto index = static_cast<std::size_t>(random.uniform(0, static_cast<std::uint32_t>(count - 1U)));
    const detail::CombatUnit* found = nullptr;
    std::optional<Fixed> carried;
    for (std::size_t visit = 0; visit < count; ++visit) {
        const auto& player = world.players[index];
        index = (index + 1U) % count;
        if (!world.hostile(source.owner, player.player_id)) continue;
        const detail::CombatUnit* best = nullptr;
        bool stop = false;
        // Candidates are judged in ascending ID, so an exact tie keeps the lowest ID (LC-29: equal distance keeps
        // the first); the cull-tree order of the original has no counterpart here.
        auto ids = world.index.range(source.position, reach, RangeMetric::planar, player.player_id);
        std::sort(ids.begin(), ids.end());
        for (const auto id : ids) {
            const auto* candidate = world.find(id);
            if (!land_target_admitted(world, source, candidate)) continue;
            if (!land_in_reach(world, source, *candidate)) continue;
            // LC-28: a set exclusion leaves the candidate with no priority, so it is not a target.
            const auto priority = judge.priority(*candidate);
            if (!priority || !judge.better(*candidate, priority, best, carried)) continue;
            best = candidate;
            carried = priority;
            if (candidate->id == held && priority->raw() == Fixed::scale) { stop = true; break; }
        }
        if (best != nullptr) found = best;
        if (stop) break;
    }
    result.found = found;
    result.priority = carried;
    return result;
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
    // LC-101: the occupants of structures stay armed; they join the combat world as limbo shooters at their host.
    const auto occupants = land_garrison_splice_occupants(land, moving);
    targets_.emplace();
    auto& built = targets_->world.emplace(impl_->combat_world(moving, starts, tick, executor));
    if (!built) return core::Result<void>::failure(built.error());
    auto& world = built.value();
    arm_occupants(world, occupants, land.content.garrisoned_range_multiplier);
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
                if (profile == nullptr || world.units[i].in_limbo || !profile->living_projectile_collision
                    || !profile->collision) continue;
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
    // LC-101: the contained squads scan after the squads in movement, with the same rules; they are not teams anybody can target.
    const auto groups = squads.size() + occupants.squads.size();
    std::vector<std::vector<EntityId>> rosters(groups);
    for (const auto& member : members) rosters[member.squad].push_back(member.object);
    for (std::size_t i = 0; i < occupants.squads.size(); ++i) rosters[squads.size() + i] = occupants.squads[i].roster;
    for (std::size_t i = 0; i < squads.size(); ++i) world.teams.emplace_back(squads[i].object, rosters[i]);
    std::sort(world.teams.begin(), world.teams.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<EntityId, std::uint64_t>> selected(groups);
    {
        core::tick_profile::Scope phase("land-targets");
        const auto scanned = executor.execute_phase("land-targets", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, groups);
            for (auto i = range.begin; i < range.end; ++i) {
                const bool contained = i >= squads.size();
                const auto* leader = rosters[i].empty() ? nullptr : world.find(rosters[i].front());
                // A contained squad has no root object in the roster and no order: only its members are armed.
                const auto* root = contained ? nullptr : live_unit(moving, squads[i].object);
                auto held = contained ? land.garrison_entries.at(occupants.squads[i - squads.size()].entrant).target
                                      : land.combat_state.targets.at(squads[i].object);
                if (!leader || !leader->profile || (!root && !contained)) { selected[i] = held; continue; }
                if (root && root->state.order.kind == OrderKind::attack) {
                    // LC-90: an ordered target that is fogged for the owner is dropped like a held one (the retail
                    // service clears the held target on the fog test whether it was ordered or found); the order
                    // itself stays, so the squad scans as for a squad with no target.
                    const auto* ordered = world.resolve_target(world.find(root->state.order.target), leader->position);
                    if (target_alive(world, *leader, ordered) && !land_fogged(*leader, *ordered)) {
                        selected[i] = {ordered->id, held.second}; continue;
                    }
                }
                // A leader without combat state still scans from its profile (retail scans from the type alone): the
                // comparator reads no weapon state, so an empty one stands in.
                std::optional<CombatState> no_state;
                std::optional<detail::CombatUnit> stateless;
                const detail::CombatUnit* scanner = leader;
                if (leader->combat == nullptr) {
                    no_state.emplace();
                    stateless.emplace(*leader);
                    stateless->combat = &*no_state;
                    scanner = &*stateless;
                }
                // LC-29: the shared Is_Better_Target chain, built once per leader and only when it is read.
                std::optional<Judge> judge;
                const auto judged = [&]() -> Judge& {
                    if (!judge) { judge.emplace(world, *scanner); judge->land_release_direct(); }
                    return *judge;
                };
                const auto* current = world.find(held.first);
                std::optional<Fixed> current_priority;
                // Deviation from retail, until a chase is hosted: a held target outside planar attack reach is
                // unsuitable (retail keeps it and walks to it). A held target that is fogged for the owner is
                // dropped by land_target_admitted (LC-90).
                if (land_target_admitted(world, *scanner, current) && land_in_reach(world, *scanner, *current))
                    current_priority = detail::target_priority(world, *scanner->profile, *current);
                // An absent, dead, excluded or otherwise unsuitable held target is dropped; the scan below
                // then runs as for a squad with no target (LC-29: a valid candidate beats an unsuitable one).
                if (!current_priority) { held.first = invalid_entity_id; current = nullptr; }
                const bool due = tick >= held.second;
                // LC-29/WCC-14: a suitable target at priority exactly 1.0, or one that is sticky (WCC-15, read
                // only when a scan is due), is kept without a scan.
                const bool keep = current != nullptr
                    && (current_priority->raw() == Fixed::scale || (due && judged().sticky(*current)));
                if (due && !keep) {
                    CombatRandom random(world.seed, tick,
                        contained ? occupants.squads[i - squads.size()].entrant : squads[i].object, ship_slot);
                    held.second = tick + land_opportunity_scan_frames + random.uniform(0, land_opportunity_scan_jitter);
                    const auto scan = land_scan(world, *scanner, judged(), held.first, land.target_query_margin, random);
                    if (current == nullptr) held.first = scan.found ? scan.found->id : invalid_entity_id;
                    else if (scan.found != nullptr && scan.found->id != current->id
                        && judged().better(*scan.found, scan.priority, current, current_priority))
                        held.first = scan.found->id;
                }
                selected[i] = held;
            }
        });
        if (!scanned) return scanned;
    }
    for (std::size_t i = 0; i < squads.size(); ++i) land.combat_state.targets[squads[i].object] = selected[i];
    for (std::size_t i = 0; i < occupants.squads.size(); ++i)
        land.garrison_entries.at(occupants.squads[i].entrant).target = selected[squads.size() + i];
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
                const auto contained = std::lower_bound(occupants.ids.begin(), occupants.ids.end(), unit.id,
                    [](const auto& row, EntityId id) { return row.id < id; });
                const bool shares = member != members.end() && member->object == unit.id;
                const bool shares_inside = contained != occupants.ids.end() && contained->id == unit.id;
                if ((shares || shares_inside) && land_share_leader_target) {
                    const auto choice = selected[shares ? member->squad : squads.size() + contained->squad];
                    input.attack_target = choice.first; input.next_scan_frame = choice.second;
                    input.attack_hardpoint = no_hardpoint;
                    const auto* ordered = source.state.order.kind == OrderKind::attack
                        ? world.resolve_target(world.find(source.state.order.target), unit.position) : nullptr;
                    input.direct = target_alive(world, unit, ordered) && !land_fogged(unit, *ordered);
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
        // LC-55: a killed built child (site or turret) is no longer an economy-created object.
        std::erase(land.dynamic_children, killed.entity_id);
    }
    auto& staged = *tracked_->staged;
    if (removed) for (auto request = land.requests.begin(); request != land.requests.end();) {
        if (std::any_of(land.movement.squads().begin(), land.movement.squads().end(),
            [&](const auto& row) { return row.object == request->first; })) { ++request; continue; }
        const auto root = staged.find(request->first);
        if (root != staged.end()) {
            track_victory_change(&root->second.state, nullptr, true);
            impacts_->events->push_back(Event{tick, EventKind::unit_destroyed, root->second.state.owner, 0, root->first});
            impl_->adjust_owned(root->second.state, false);
            staged.erase(root);
        }
        release_land_formation_member(land, request->first); // LMF-01: a dead squad leaves its pending formation order
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
    land_garrison_restore_occupants(land, live, occupants);
    // G-09: a destroyed host releases or loses its occupants in the same death phase.
    if (auto released = land_garrison_deaths(live); !released) return released;
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
