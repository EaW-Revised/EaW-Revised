#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_impl.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace eawr::sim::tactical {

core::Result<void> detail::prepare_projectile_defences(CombatWorld& world,
    const std::span<const ProjectileDefenceAbilityInput> abilities, const PartitionExecutor& executor,
    const std::span<const EntityId> registration_order) {
    if (abilities.size() != world.units.size()) return core::Result<void>::failure(
        diagnostic(diagnostic_codes::invalid_setup, "projectile defence inputs must match combat units"));
    const auto live = [](const CombatUnit& unit) {
        return unit.profile && (!unit.durability || unit.durability->hull.raw() > 0);
    };
    std::vector<SpaceBody> emitters;
    std::vector<std::optional<SpaceBody>> emitter_inputs(world.units.size());
    std::vector<math::Fixed> radii(world.units.size());
    const auto copied = executor.execute_phase("projectile-jamming-inputs", tick_partition_count,
        [&](const std::size_t partition) {
        const auto range = partition_range(partition, world.units.size());
        for (auto slot = range.begin; slot < range.end; ++slot) {
            const auto& unit = world.units[slot];
            if (!abilities[slot].active_jamming || !live(unit) || unit.in_nebula) continue;
            emitter_inputs[slot] = SpaceBody{slot + 1, unit.owner, unit.position};
            radii[slot] = abilities[slot].jamming_radius.value_or(unit.profile->passive_missile_shield_radius);
        }
    });
    if (!copied) return copied;
    math::Fixed reach{};
    for (std::size_t slot = 0; slot < abilities.size(); ++slot) {
        if (!emitter_inputs[slot]) continue;
        emitters.push_back(*emitter_inputs[slot]);
        reach = std::max(reach, radii[slot]);
    }
    const auto built = SpaceIndex::build(emitters);
    if (!built) return core::Result<void>::failure(built.error());
    const auto& index = built.value();
    std::vector<std::optional<ProjectileDefenceSource>> sources(world.units.size());
    std::vector<std::uint8_t> static_sources(world.units.size());
    std::vector<std::optional<core::Diagnostic>> errors(world.units.size());
    const auto prepared = executor.execute_phase("projectile-defence-recipients", tick_partition_count,
        [&](const std::size_t partition) {
        const auto range = partition_range(partition, world.units.size());
        for (auto slot = range.begin; slot < range.end; ++slot) {
            auto& unit = world.units[slot];
            const auto& ability = abilities[slot];
            unit.sensor_jammed = false; // WHE-32: a stamp belongs only to this frame.
            if (!live(unit)) continue;
            // Death clones and deleted objects are absent from the live-unit copy;
            // projectiles have their own storage. Combat data and a locomotor are required.
            if (unit.combat && world.motion && (world.motion->find(unit.type_id)
                || world.motion->squadrons.find_craft(unit.type_id))) {
                for (const auto id : index.range(unit.position, reach, RangeMetric::spatial)) {
                    const auto source_slot = static_cast<std::size_t>(id - 1);
                    const auto& emitter = world.units[source_slot];
                    const auto radius = abilities[source_slot].jamming_radius.value_or(
                        emitter.profile->passive_missile_shield_radius);
                    if (!world.hostile(emitter.owner, unit.owner)
                        && within_range(emitter.position, unit.position, radius, RangeMetric::spatial)) {
                        unit.sensor_jammed = true;
                        break;
                    }
                }
            }
            const auto passive = unit.profile->passive_missile_shield_radius;
            static_sources[slot] = passive.raw() > 0 || ability.shield_radius ? 1U : 0U;
            if (!ability.active_shield && passive.raw() <= 0 && !ability.active_jamming
                && !(ability.shield_radius && unit.sensor_jammed)) continue;
            auto height = math::add(unit.position.z, unit.profile->ranged_target_z_adjust);
            if (!height) { errors[slot] = height.error(); continue; }
            ProjectileDefenceSource source;
            source.id = unit.id; source.owner = unit.owner; source.position = unit.position;
            source.adjusted_position = {unit.position.x, unit.position.y, height.value()};
            source.active_shield = ability.active_shield;
            source.passive_shield = passive.raw() > 0;
            source.sensor_jammed = unit.sensor_jammed;
            source.passive_radius = passive;
            source.shield_radius = ability.shield_radius;
            source.jamming_radius = ability.jamming_radius;
            sources[slot] = source;
        }
    });
    if (!prepared) return prepared;
    std::vector<ProjectileDefenceSource> registered;
    world.static_projectile_defences.clear();
    for (std::size_t slot = 0; slot < sources.size(); ++slot) {
        if (errors[slot]) return core::Result<void>::failure(*errors[slot]);
        if (static_sources[slot]) world.static_projectile_defences.push_back(world.units[slot].id);
    }
    for (const auto id : registration_order) {
        const auto unit = std::lower_bound(world.units.begin(), world.units.end(), id,
            [](const CombatUnit& candidate, const EntityId value) { return candidate.id < value; });
        if (unit == world.units.end() || unit->id != id) continue;
        auto& source = sources[static_cast<std::size_t>(unit - world.units.begin())];
        if (source) { registered.push_back(*source); source.reset(); }
    }
    for (const auto& source : sources) {
        if (source) registered.push_back(*source);
    }
    return world.projectile_defences.rebuild(std::move(registered));
}

core::Result<detail::CombatWorld> TacticalSession::Impl::combat_world(std::vector<LiveUnit>& units,
    const std::vector<std::pair<EntityId, math::Vec3>>& starts, const std::uint64_t frame,
    const PartitionExecutor& executor) const {
        detail::CombatWorld world;
        world.players = setup.players;
        world.relationships = snapshot_players;
        world.table = &combat;
        world.motion = &motion;
        world.rules = &durability.rules;
        world.damage = durability.damage ? &*durability.damage : nullptr;
        world.seed = setup.seed;
        world.frame = frame;
        if (combat.profiles.empty()) {
            return core::Result<detail::CombatWorld>::success(std::move(world)); // no unit fights
        }
        world.units.resize(units.size());
        std::vector<SpaceBody> bodies(units.size());
        std::vector<std::optional<core::Diagnostic>> errors(units.size());
        std::vector<detail::ProjectileDefenceAbilityInput> defence_inputs(units.size());
        const auto instances = current_snapshot->instances();
        const auto filled = executor.execute_phase("combat-world", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, units.size());
            if (range.begin == range.end) return;
            auto published = std::lower_bound(instances.begin(), instances.end(), units[range.begin].state.entity_id,
                [](const TacticalInstance& instance, const EntityId id) { return instance.entity_id < id; });
            auto start = std::lower_bound(starts.begin(), starts.end(), units[range.begin].state.entity_id,
                [](const std::pair<EntityId, math::Vec3>& item, const EntityId id) { return item.first < id; });
            for (auto slot = range.begin; slot < range.end; ++slot) {
                auto& unit = units[slot];
                const auto& state = unit.state;
                auto& entry = world.units[slot];
                entry.id = state.entity_id;
                entry.type_id = state.type_id;
                entry.owner = state.owner;
                const auto player = std::lower_bound(setup.players.begin(), setup.players.end(), state.owner,
                    [](const Player& candidate, const PlayerId id) { return candidate.player_id < id; });
                entry.team = player->team_id;
                entry.player_index = static_cast<std::size_t>(player - setup.players.begin());
                entry.position = state.position;
                // W-10: where the unit stood before this frame's movement (a unit new this frame: here).
                while (start != starts.end() && start->first < state.entity_id) ++start;
                entry.previous_position = start != starts.end() && start->first == state.entity_id ? start->second : state.position;
                auto transform = level_transform(unit);
                if (!transform) {
                    errors[slot] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(frame) + " unit " + std::to_string(state.entity_id) + ": "
                            + transform.error().message);
                    continue;
                }
                entry.transform = transform.value();
                while (published != instances.end() && published->entity_id < state.entity_id) {
                    ++published;
                }
                if (published != instances.end() && published->entity_id == state.entity_id) {
                    entry.visible_to = published->visible_to;
                }
                entry.profile = combat.find(state.type_id);
                entry.durability_profile = health_profile(unit);
                entry.durability = unit.durability ? &*unit.durability : nullptr;
                entry.combat = unit.combat ? &*unit.combat : nullptr;
                // A-04: only a unit at rest turns toward its target.
                entry.can_turn = unit.motion && unit.motion->kind == MotionKind::none && !unit.formation
                    && motion.find(state.type_id) != nullptr;
                // WMV-20: unit-destination facing is independent of the scanned combat target.
                // A position order contributes no destination; moving ships keep their path yaw.
                if (entry.can_turn && state.order.target != invalid_entity_id
                    && (state.order.kind == OrderKind::attack_move || state.order.kind == OrderKind::guard)) {
                    const auto destination = std::lower_bound(units.cbegin(), units.cend(), state.order.target,
                        [](const LiveUnit& candidate, const EntityId id) { return candidate.state.entity_id < id; });
                    const auto sight = std::lower_bound(instances.begin(), instances.end(), state.order.target,
                        [](const TacticalInstance& candidate, const EntityId id) { return candidate.entity_id < id; });
                    if (destination != units.cend() && destination->state.entity_id == state.order.target
                        && sight != instances.end() && sight->entity_id == state.order.target
                        && (sight->visible_to & (std::uint64_t{1} << entry.player_index)) != 0)
                        entry.facing_destination = destination->state.position;
                }
                entry.weapon_delay = ability_factor(unit, AbilityModifier::weapon_delay);
                entry.fire_rate = ion_fire_rate(unit.ion_stun, world.frame); // IS-06
                entry.scatter_radius = ability_factor(unit, AbilityModifier::scatter_radius);
                entry.in_nebula = unit.nebula && unit.nebula->present;
                entry.object_fire_rate = ability_factor(unit, AbilityModifier::fire_rate);
                if (unit.abilities) {
                    const auto& profile = *abilities.find(state.type_id);
                    auto& defence = defence_inputs[slot];
                    if (const auto shield = ability_slot(profile, AbilityKind::missile_shield)) {
                        defence.shield_radius = profile.abilities[*shield].effective_radius;
                        defence.active_shield = unit.abilities->slots[*shield].active;
                    }
                    if (const auto jammer = ability_slot(profile, AbilityKind::sensor_jamming)) {
                        defence.jamming_radius = profile.abilities[*jammer].effective_radius;
                        defence.active_jamming = unit.abilities->slots[*jammer].active;
                    }
                    const auto index = ability_slot(profile, AbilityKind::barrage);
                    if (index && unit.abilities->slots[*index].active) {
                        entry.barrage_target = unit.abilities->slots[*index].target;
                        entry.fixed_inaccuracy = profile.abilities[*index].fixed_inaccuracy;
                    }
                }
                bodies[slot] = SpaceBody{state.entity_id, state.owner, state.position};
            }
        });
        if (!filled) return core::Result<detail::CombatWorld>::failure(filled.error());
        for (std::size_t slot = 0; slot < errors.size(); ++slot) {
            if (errors[slot]) return core::Result<detail::CombatWorld>::failure(std::move(*errors[slot]));
        }
        auto defences = detail::prepare_projectile_defences(world, defence_inputs, executor, projectile_defence_order);
        if (!defences) return core::Result<detail::CombatWorld>::failure(defences.error());
        auto index = SpaceIndex::build(bodies);
        if (!index) {
            return core::Result<detail::CombatWorld>::failure(index.error());
        }
        world.index = std::move(index).value();
        // #424: a player's attack on a squadron targets its team container, which its attackers
        // resolve to one of its live craft (FO-04).
        for (const auto& squadron : squadrons) {
            std::vector<EntityId> members;
            for (const auto member : squadron.members) {
                if (world.find(member) != nullptr) members.push_back(member);
            }
            world.teams.emplace_back(squadron.container, std::move(members));
        }
        // AB-66 (#561): the craft of a squadron whose ION_CANNON_SHOT is on hold every other shot.
        const auto live = [&](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(units.begin(), units.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != units.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        for (const auto& [container, members] : world.teams) {
            const auto* owner = live(container);
            const auto* slot = owner != nullptr ? ion_slot(*owner) : nullptr;
            if (slot == nullptr || !slot->active) continue;
            for (const auto member : members) {
                const auto* craft = live(member);
                auto entry = std::lower_bound(world.units.begin(), world.units.end(), member,
                    [](const detail::CombatUnit& unit, const EntityId value) { return unit.id < value; });
                if (craft == nullptr || entry == world.units.end() || entry->id != member) continue;
                const auto* own = ion_slot(*craft);
                entry->ion_held = true;
                const auto* target = live(slot->target);
                entry->ion_armed = own != nullptr && own->active && !(owner->nebula && owner->nebula->present)
                    && !(target != nullptr && target->nebula && target->nebula->present);
                entry->ion_target = slot->target;
                entry->ion_target_hardpoint = slot->target_hardpoint;
            }
        }
        return core::Result<detail::CombatWorld>::success(std::move(world));
    }

core::Result<std::optional<detail::CollectionTrees::Member>> TacticalSession::Impl::collection_member(
    LiveUnit& unit, const bool projectile) const {
        using Member = std::optional<detail::CollectionTrees::Member>;
        const auto& state = unit.state;
        const auto* profile = combat.find(state.type_id);
        if (profile == nullptr) return core::Result<Member>::success(std::nullopt);
        detail::CullBox bounds;
        if (profile->collision) {
            auto transform = level_transform(unit);
            if (!transform) return core::Result<Member>::failure(transform.error());
            auto box = *profile->collision;
            // DG-36b: attached geometry can extend outside the parent model's box.
            // Retain those candidates; the narrow phase still decides mesh eligibility.
            if (projectile && profile->mesh_bounds) {
                const auto& meshes = *profile->mesh_bounds;
                box.min = {std::min(box.min.x, meshes.min.x), std::min(box.min.y, meshes.min.y),
                    std::min(box.min.z, meshes.min.z)};
                box.max = {std::max(box.max.x, meshes.max.x), std::max(box.max.y, meshes.max.y),
                    std::max(box.max.z, meshes.max.z)};
            }
            for (int corner = 0; corner < 8; ++corner) {
                const math::Vec3 local{(corner & 1) != 0 ? box.max.x : box.min.x,
                    (corner & 2) != 0 ? box.max.y : box.min.y, (corner & 4) != 0 ? box.max.z : box.min.z};
                auto point = math::transform_point(transform.value(), local);
                if (!point) return core::Result<Member>::failure(point.error());
                const auto& p = point.value();
                if (corner == 0) {
                    bounds = detail::CullBox{p, p};
                    continue;
                }
                bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
                bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
            }
        } else {
            const auto tenth = math::Fixed::scale / 10;
            bounds = detail::CullBox{state.position, {math::Fixed::from_raw(state.position.x.raw() + tenth),
                math::Fixed::from_raw(state.position.y.raw() + tenth), math::Fixed::from_raw(state.position.z.raw() + tenth)}};
        }
        return core::Result<Member>::success(detail::CollectionTrees::Member{state.entity_id, state.owner, bounds});
    }

} // namespace eawr::sim::tactical
