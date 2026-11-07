#include "eawr/core/load_profile.hpp"
#include "eawr/core/tick_profile.hpp"
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

LiveUnit TacticalSession::Impl::live_at(const EntityId id, const entt::entity handle) const {
        const auto& identity = registry.get<Identity>(handle);
        const auto& placement = registry.get<Placement>(handle);
        const auto* health = registry.try_get<Health>(handle);
        const auto* moving = registry.try_get<Motion>(handle);
        const auto* fighting = registry.try_get<Combat>(handle);
        const auto* waiting = registry.try_get<Waiting>(handle);
        const auto* able = registry.try_get<Abilities>(handle);
        const auto* carried = registry.try_get<CarriedHeroes>(handle);
        const auto* approaching = registry.try_get<Approaching>(handle);
        const auto* stunned = registry.try_get<IonStunned>(handle);
        const auto* vulnerable = registry.try_get<ArrivalVulnerability>(handle);
        const auto* upgraded = registry.try_get<UpgradeModifiers>(handle);
        const auto* asteroid = registry.try_get<AsteroidContact>(handle);
        const auto* nebula = registry.try_get<NebulaState>(handle);
        const auto* corrupt = registry.try_get<CorruptedSystems>(handle);
        const auto* sighted = registry.try_get<Sighted>(handle);
        auto result = LiveUnit{
            UnitState{
                id,
                identity.type_id,
                identity.owner,
                placement.position,
                placement.rotation,
                registry.get<CurrentOrder>(handle).value,
                identity.purchase_type,
                carried != nullptr ? carried->value : std::vector<CarriedObject>{},
                identity.purchase_token,
                identity.barrage_source,
                identity.garrison_enabled,
                identity.controller_seat,
                identity.control,
            },
            health != nullptr ? std::optional(health->value) : std::nullopt,
            moving != nullptr ? std::optional(moving->value) : std::nullopt,
            moving != nullptr ? moving->speed : math::Fixed{},
            moving != nullptr ? moving->roll : math::Fixed{},
            fighting != nullptr ? std::optional(fighting->value) : std::nullopt,
            waiting != nullptr ? std::optional(waiting->value) : std::nullopt,
            able != nullptr ? std::optional(able->value) : std::nullopt,
            approaching != nullptr ? std::optional(approaching->value) : std::nullopt,
            stunned != nullptr ? std::optional(stunned->value) : std::nullopt,
            vulnerable != nullptr ? std::optional(vulnerable->until) : std::nullopt,
            upgraded != nullptr ? upgraded->bonuses : CombatBonuses{},
            upgraded != nullptr ? upgraded->health : std::nullopt,
            {}, 0, 0,
            asteroid != nullptr ? std::optional(asteroid->value) : std::nullopt,
            false, // engine-recovery scratch starts clear on each gathered tick
            nebula != nullptr ? std::optional(nebula->value) : std::nullopt,
        };
        result.corrupt = corrupt != nullptr ? std::optional(corrupt->value) : std::nullopt;
        result.sighting = sighted != nullptr ? std::optional(sighted->value) : std::nullopt;
        result.garrison_unit = registry.all_of<GarrisonUnit>(handle);
        result.script_invulnerable = registry.all_of<ScriptInvulnerable>(handle);
        if (const auto* route = registry.try_get<Waypoints>(handle)) result.waypoints = route->value;
        if (const auto* history = registry.try_get<PositionHistoryComponent>(handle)) result.position_history = history->value;
        return result;
    }

bool TacticalSession::Impl::garrison_marked(const EntityId id) const {
        const auto found = handles.find(id);
        return found != handles.end() && registry.all_of<GarrisonUnit>(found->second);
    }

std::vector<LiveUnit> TacticalSession::Impl::sorted_live() const {
        std::vector<LiveUnit> result;
        result.reserve(handles.size());
        for (const auto& [id, handle] : handles) result.push_back(live_at(id, handle));
        return result;
    }

std::vector<UnitState> TacticalSession::Impl::sorted_units() const {
        std::vector<UnitState> result;
        for (auto& unit : sorted_live()) {
            result.push_back(std::move(unit.state));
        }
        return result;
    }

void TacticalSession::Impl::rebuild(const std::vector<LiveUnit>& units, const bool reverse) {
        registry.clear();
        handles.clear();
        radar_roster.clear();
        waypoint_queues_present = false;
        std::fill(committed_ownership_counts.begin(), committed_ownership_counts.end(), 0);
        const auto insert = [this](const LiveUnit& unit) {
            const auto& state = unit.state;
            auto radar = radar_slots(state.type_id);
            if (!radar.empty()) radar_roster.emplace(state.entity_id, std::move(radar));
            const auto owned = ownership_slot(state.owner, purchase_identity(state));
            if (owned != ownership_keys.size()) ++committed_ownership_counts[owned];
            const auto handle = registry.create();
            emplace_component<StableId>(handle, state.entity_id);
            if (unit.garrison_unit) emplace_component<GarrisonUnit>(handle);
            if (unit.script_invulnerable) emplace_component<ScriptInvulnerable>(handle);
            emplace_component<Identity>(handle, state.type_id, state.owner, state.purchase_type,
                state.purchase_token, state.barrage_source, state.garrison_enabled, state.controller_seat, state.control);
            emplace_component<Placement>(handle, state.position, state.rotation);
            emplace_component<CurrentOrder>(handle, state.order);
            if (!state.contained.empty()) emplace_component<CarriedHeroes>(handle, state.contained);
            if (unit.durability) {
                emplace_component<Health>(handle, *unit.durability);
            }
            if (unit.motion) {
                emplace_component<Motion>(handle, *unit.motion, unit.speed, unit.roll);
            }
            if (unit.combat) {
                emplace_component<Combat>(handle, *unit.combat);
            }
            if (unit.formation) {
                emplace_component<Waiting>(handle, *unit.formation);
            }
            if (unit.abilities) {
                emplace_component<Abilities>(handle, *unit.abilities);
            }
            if (unit.approach) {
                emplace_component<Approaching>(handle, *unit.approach);
            }
            if (unit.sighting) {
                emplace_component<Sighted>(handle, *unit.sighting);
            }
            if (unit.ion_stun) {
                emplace_component<IonStunned>(handle, *unit.ion_stun);
            }
            if (unit.arrival_vulnerable_until) {
                emplace_component<ArrivalVulnerability>(handle, *unit.arrival_vulnerable_until);
            }
            if (unit.upgrade_bonuses != CombatBonuses{} || unit.upgraded_durability) {
                emplace_component<UpgradeModifiers>(handle, unit.upgrade_bonuses, unit.upgraded_durability);
            }
            if (unit.asteroid_contact) emplace_component<AsteroidContact>(handle, *unit.asteroid_contact);
            if (unit.nebula) emplace_component<NebulaState>(handle, *unit.nebula);
            if (unit.corrupt) emplace_component<CorruptedSystems>(handle, *unit.corrupt);
            if (unit.waypoints) {
                waypoint_queues_present |= !unit.waypoints->points.empty();
                emplace_component<Waypoints>(handle, *unit.waypoints);
            }
            if (unit.position_history) emplace_component<PositionHistoryComponent>(handle, *unit.position_history);
            handles.emplace(state.entity_id, handle);
        };
        if (reverse) {
            for (auto iterator = units.rbegin(); iterator != units.rend(); ++iterator) {
                insert(*iterator);
            }
        } else {
            for (const auto& unit : units) {
                insert(unit);
            }
        }
    }

template <typename Component, typename... Args>
Component& TacticalSession::Impl::emplace_component(const entt::entity handle, Args&&... args) {
        auto& value = registry.emplace<Component>(handle, std::forward<Args>(args)...);
        ++registry_emplacement_count;
        return value;
    }

template <typename Component, typename Value>
void TacticalSession::Impl::commit_optional(const entt::entity handle, std::optional<Value>& value,
    std::uint64_t& writes) {
        auto* current = registry.try_get<Component>(handle);
        if (!value) {
            if (current != nullptr) { registry.remove<Component>(handle); ++writes; }
        } else if (current == nullptr) {
            emplace_component<Component>(handle, std::move(*value));
            ++writes;
        } else if (current->value != *value) {
            current->value = std::move(*value);
            ++writes;
        }
    }

namespace {

// Components whose presence differs from the unit's value. The registry-commit phase only flags them; the ordered
// tail emplaces or removes them (a structural change of a shared pool), together with a new unit's entity.
enum CommitBit : std::uint32_t {
    commit_garrison = 1U << 0, commit_carried = 1U << 1, commit_health = 1U << 2, commit_asteroid = 1U << 3,
    commit_nebula = 1U << 4, commit_motion = 1U << 5, commit_combat = 1U << 6, commit_waiting = 1U << 7,
    commit_abilities = 1U << 8, commit_approaching = 1U << 9, commit_sighted = 1U << 10, commit_ion = 1U << 11,
    commit_corrupt = 1U << 12, commit_waypoints = 1U << 13, commit_history = 1U << 14,
    commit_vulnerable = 1U << 15, commit_upgraded = 1U << 16, commit_script_invulnerable = 1U << 17,
    commit_every_component = (1U << 18) - 1U,
    commit_new_entity = 1U << 18,
};

// One optional component in the phase: assign a changed value in place (the pool is not restructured), or flag
// the component when only the serial tail may add or remove it. Moves exactly what commit_optional moves.
template <typename Storage, typename Value>
void settle_component(Storage& storage, const entt::entity handle, std::optional<Value>& value,
    std::uint64_t& writes, std::uint32_t& mask, const std::uint32_t bit) {
    if (!storage.contains(handle)) {
        if (value) mask |= bit;
        return;
    }
    if (!value) {
        mask |= bit;
        return;
    }
    auto& current = storage.get(handle);
    if (current.value != *value) {
        current.value = std::move(*value);
        ++writes;
    }
}

} // namespace

core::Result<void> TacticalSession::Impl::commit_units(std::vector<LiveUnit>& units, std::uint64_t& writes,
    const PartitionExecutor& executor) {
        waypoint_queues_present = false;
        {
            core::tick_profile::Scope sweep_scope("serial.tactical.commit.destroy");
            auto next = units.begin();
            for (auto it = handles.begin(); it != handles.end();) {
                while (next != units.end() && next->state.entity_id < it->first) ++next;
                if (next == units.end() || next->state.entity_id != it->first) {
                    const auto& identity = registry.get<Identity>(it->second);
                    const auto slot = ownership_slot(identity.owner, identity.purchase_type != 0 ? identity.purchase_type : identity.type_id);
                    if (slot != ownership_keys.size()) --committed_ownership_counts[slot];
                    registry.destroy(it->second);
                    it = handles.erase(it);
                } else ++it;
            }
        }
        // Every handle left belongs to a unit. Workers use only these storages (taken here, so a missing pool is
        // created on this thread: the non-const registry accessors assure it) and never call the registry.
        auto& identity_storage = registry.storage<Identity>();
        auto& placement_storage = registry.storage<Placement>();
        auto& order_storage = registry.storage<CurrentOrder>();
        auto& garrison_storage = registry.storage<GarrisonUnit>();
        auto& script_invulnerable_storage = registry.storage<ScriptInvulnerable>();
        auto& carried_storage = registry.storage<CarriedHeroes>();
        auto& health_storage = registry.storage<Health>();
        auto& asteroid_storage = registry.storage<AsteroidContact>();
        auto& nebula_storage = registry.storage<NebulaState>();
        auto& motion_storage = registry.storage<Motion>();
        auto& combat_storage = registry.storage<Combat>();
        auto& waiting_storage = registry.storage<Waiting>();
        auto& abilities_storage = registry.storage<Abilities>();
        auto& approaching_storage = registry.storage<Approaching>();
        auto& sighted_storage = registry.storage<Sighted>();
        auto& ion_storage = registry.storage<IonStunned>();
        auto& corrupt_storage = registry.storage<CorruptedSystems>();
        auto& waypoints_storage = registry.storage<Waypoints>();
        auto& history_storage = registry.storage<PositionHistoryComponent>();
        auto& vulnerable_storage = registry.storage<ArrivalVulnerability>();
        auto& upgraded_storage = registry.storage<UpgradeModifiers>();
        const auto& roster = handles;
        const auto settle = [&](CommitPartition& part, const std::size_t begin, const std::size_t end) {
            auto cursor = roster.end();
            for (auto index = begin; index < end; ++index) {
                auto& unit = units[index];
                std::optional<core::tick_profile::JobScope> profile_job;
                if (core::tick_profile::active_jobs)
                    profile_job.emplace("job.registry-commit", unit.state.entity_id, nullptr, unit.state.type_id);
                const auto& state = unit.state;
                // Handles and units ascend together, so follow the map instead of searching it for every unit.
                if (cursor == roster.end() || cursor->first != state.entity_id) cursor = roster.find(state.entity_id);
                // Derive the next tick's readiness gate in this existing stable commit, before the route is moved.
                part.waypoints |= unit.waypoints && !unit.waypoints->points.empty();
                CommitPartition::Op op;
                op.index = static_cast<std::uint32_t>(index);
                if (cursor == roster.end()) {
                    op.mask = commit_new_entity | commit_every_component;
                    part.ops.push_back(op);
                    continue;
                }
                const auto handle = cursor->second;
                ++cursor;
                auto& identity = identity_storage.get(handle);
                if (identity.type_id != state.type_id || identity.owner != state.owner || identity.purchase_type != state.purchase_type
                    || identity.purchase_token != state.purchase_token || identity.barrage_source != state.barrage_source
                    || identity.garrison_enabled != state.garrison_enabled
                    || identity.controller_seat != state.controller_seat || identity.control != state.control) {
                    part.ownership.emplace_back(
                        ownership_slot(identity.owner, identity.purchase_type != 0 ? identity.purchase_type : identity.type_id),
                        ownership_slot(state.owner, purchase_identity(state)));
                    identity = {state.type_id, state.owner, state.purchase_type, state.purchase_token,
                        state.barrage_source, state.garrison_enabled, state.controller_seat, state.control}; ++part.writes;
                }
                auto& placement = placement_storage.get(handle);
                if (placement.position != state.position || placement.rotation != state.rotation) {
                    placement = {state.position, state.rotation}; ++part.writes;
                }
                auto& order = order_storage.get(handle);
                if (order.value != state.order) { order.value = state.order; ++part.writes; }
                std::uint32_t mask = 0;
                if (unit.garrison_unit != garrison_storage.contains(handle)) mask |= commit_garrison;
                if (unit.script_invulnerable != script_invulnerable_storage.contains(handle)) mask |= commit_script_invulnerable;
                if (carried_storage.contains(handle)) {
                    if (state.contained.empty()) mask |= commit_carried;
                    else if (auto& current = carried_storage.get(handle); current.value != state.contained) {
                        current.value = state.contained; ++part.writes;
                    }
                } else if (!state.contained.empty()) mask |= commit_carried;
                settle_component(health_storage, handle, unit.durability, part.writes, mask, commit_health);
                settle_component(asteroid_storage, handle, unit.asteroid_contact, part.writes, mask, commit_asteroid);
                settle_component(nebula_storage, handle, unit.nebula, part.writes, mask, commit_nebula);
                if (!unit.motion) {
                    if (motion_storage.contains(handle)) mask |= commit_motion;
                } else if (!motion_storage.contains(handle)) {
                    mask |= commit_motion;
                } else if (auto& current = motion_storage.get(handle);
                    current.value != *unit.motion || current.speed != unit.speed || current.roll != unit.roll) {
                    current = {std::move(*unit.motion), unit.speed, unit.roll}; ++part.writes;
                }
                settle_component(combat_storage, handle, unit.combat, part.writes, mask, commit_combat);
                settle_component(waiting_storage, handle, unit.formation, part.writes, mask, commit_waiting);
                settle_component(abilities_storage, handle, unit.abilities, part.writes, mask, commit_abilities);
                settle_component(approaching_storage, handle, unit.approach, part.writes, mask, commit_approaching);
                settle_component(sighted_storage, handle, unit.sighting, part.writes, mask, commit_sighted);
                settle_component(ion_storage, handle, unit.ion_stun, part.writes, mask, commit_ion);
                settle_component(corrupt_storage, handle, unit.corrupt, part.writes, mask, commit_corrupt);
                settle_component(waypoints_storage, handle, unit.waypoints, part.writes, mask, commit_waypoints);
                settle_component(history_storage, handle, unit.position_history, part.writes, mask, commit_history);
                if (!unit.arrival_vulnerable_until) {
                    if (vulnerable_storage.contains(handle)) mask |= commit_vulnerable;
                } else if (!vulnerable_storage.contains(handle)) {
                    mask |= commit_vulnerable;
                } else if (auto& current = vulnerable_storage.get(handle); current.until != *unit.arrival_vulnerable_until) {
                    current.until = *unit.arrival_vulnerable_until; ++part.writes;
                }
                // WPR-55: preserve cached upgrade profiles across the sparse component commit.
                if (unit.upgrade_bonuses == CombatBonuses{} && !unit.upgraded_durability) {
                    if (upgraded_storage.contains(handle)) mask |= commit_upgraded;
                } else if (!upgraded_storage.contains(handle)) {
                    mask |= commit_upgraded;
                } else if (auto& current = upgraded_storage.get(handle);
                    current.bonuses != unit.upgrade_bonuses || current.health != unit.upgraded_durability) {
                    current.bonuses = unit.upgrade_bonuses;
                    current.health = std::move(unit.upgraded_durability);
                    ++part.writes;
                }
                if (mask != 0U) {
                    op.handle = handle;
                    op.mask = mask;
                    part.ops.push_back(op);
                }
            }
        };
        // Below 256 units the partition dispatch costs more than the commit (the gather cutoff): one inline
        // partition. The cutoff schedules work only; the 64 ranges and the tail order never depend on workers.
        const auto partitions = units.size() < 256 ? std::size_t{1} : tick_partition_count;
        for (std::size_t slot = 0; slot < partitions; ++slot) {
            auto& part = commit_scratch[slot];
            part.ops.clear();
            part.ownership.clear();
            part.writes = 0;
            part.waypoints = false;
        }
        if (partitions == 1) {
            settle(commit_scratch[0], 0, units.size());
        } else {
            const auto committed = executor.execute_phase("registry-commit", partitions, [&](const std::size_t partition) {
                const auto range = partition_range(partition, units.size());
                settle(commit_scratch[partition], range.begin, range.end);
            });
            if (!committed) return committed;
        }
        // Ordered tail: partitions 0..63 are ascending unit ranges, so replaying their ops in order reproduces the
        // serial loop's per-pool emplace/remove order and entity recycling exactly.
        core::tick_profile::Scope tail_scope("serial.tactical.commit.registry");
        for (std::size_t slot = 0; slot < partitions; ++slot) {
            auto& part = commit_scratch[slot];
            writes += part.writes;
            waypoint_queues_present |= part.waypoints;
            for (const auto& [before, after] : part.ownership) {
                if (before != ownership_keys.size()) --committed_ownership_counts[before];
                if (after != ownership_keys.size()) ++committed_ownership_counts[after];
            }
            for (const auto& op : part.ops) {
                auto& unit = units[op.index];
                const auto& state = unit.state;
                auto handle = op.handle;
                if ((op.mask & commit_new_entity) != 0U) {
                    handle = registry.create();
                    const auto owned = ownership_slot(state.owner, purchase_identity(state));
                    if (owned != ownership_keys.size()) ++committed_ownership_counts[owned];
                    handles.emplace(state.entity_id, handle);
                    emplace_component<StableId>(handle, state.entity_id);
                    emplace_component<Identity>(handle, state.type_id, state.owner, state.purchase_type,
                        state.purchase_token, state.barrage_source, state.garrison_enabled, state.controller_seat, state.control);
                    emplace_component<Placement>(handle, state.position, state.rotation);
                    emplace_component<CurrentOrder>(handle, state.order);
                    writes += 4;
                }
                if ((op.mask & commit_garrison) != 0U) {
                    if (unit.garrison_unit != registry.all_of<GarrisonUnit>(handle)) {
                        if (unit.garrison_unit) emplace_component<GarrisonUnit>(handle);
                        else registry.remove<GarrisonUnit>(handle);
                        ++writes;
                    }
                }
                if ((op.mask & commit_script_invulnerable) != 0U) {
                    if (unit.script_invulnerable != registry.all_of<ScriptInvulnerable>(handle)) {
                        if (unit.script_invulnerable) emplace_component<ScriptInvulnerable>(handle);
                        else registry.remove<ScriptInvulnerable>(handle);
                        ++writes;
                    }
                }
                if ((op.mask & commit_carried) != 0U) {
                    auto carried = state.contained.empty() ? std::optional<std::vector<CarriedObject>>{} : std::optional(state.contained);
                    commit_optional<CarriedHeroes>(handle, carried, writes);
                }
                if ((op.mask & commit_health) != 0U) commit_optional<Health>(handle, unit.durability, writes);
                if ((op.mask & commit_asteroid) != 0U) commit_optional<AsteroidContact>(handle, unit.asteroid_contact, writes);
                if ((op.mask & commit_nebula) != 0U) commit_optional<NebulaState>(handle, unit.nebula, writes);
                if ((op.mask & commit_motion) != 0U) {
                    const bool present = registry.all_of<Motion>(handle);
                    if (!unit.motion) {
                        if (present) { registry.remove<Motion>(handle); ++writes; }
                    } else if (!present) {
                        emplace_component<Motion>(handle, std::move(*unit.motion), unit.speed, unit.roll);
                        ++writes;
                    }
                }
                if ((op.mask & commit_combat) != 0U) commit_optional<Combat>(handle, unit.combat, writes);
                if ((op.mask & commit_waiting) != 0U) commit_optional<Waiting>(handle, unit.formation, writes);
                if ((op.mask & commit_abilities) != 0U) commit_optional<Abilities>(handle, unit.abilities, writes);
                if ((op.mask & commit_approaching) != 0U) commit_optional<Approaching>(handle, unit.approach, writes);
                if ((op.mask & commit_sighted) != 0U) commit_optional<Sighted>(handle, unit.sighting, writes);
                if ((op.mask & commit_ion) != 0U) commit_optional<IonStunned>(handle, unit.ion_stun, writes);
                if ((op.mask & commit_corrupt) != 0U) commit_optional<CorruptedSystems>(handle, unit.corrupt, writes);
                if ((op.mask & commit_waypoints) != 0U) commit_optional<Waypoints>(handle, unit.waypoints, writes);
                if ((op.mask & commit_history) != 0U) commit_optional<PositionHistoryComponent>(handle, unit.position_history, writes);
                if ((op.mask & commit_vulnerable) != 0U) {
                    const bool present = registry.all_of<ArrivalVulnerability>(handle);
                    if (!unit.arrival_vulnerable_until) {
                        if (present) { registry.remove<ArrivalVulnerability>(handle); ++writes; }
                    } else if (!present) {
                        emplace_component<ArrivalVulnerability>(handle, *unit.arrival_vulnerable_until);
                        ++writes;
                    }
                }
                if ((op.mask & commit_upgraded) != 0U) {
                    const bool present = registry.all_of<UpgradeModifiers>(handle);
                    if (unit.upgrade_bonuses == CombatBonuses{} && !unit.upgraded_durability) {
                        if (present) { registry.remove<UpgradeModifiers>(handle); ++writes; }
                    } else if (!present) {
                        emplace_component<UpgradeModifiers>(handle, unit.upgrade_bonuses, std::move(unit.upgraded_durability));
                        ++writes;
                    }
                }
            }
        }
        return core::Result<void>::success();
    }

// The serial commit this file used before the registry-commit phase. Test-only oracle: commit_units must leave
// the same registry, handles, counters and unit values for any worker count.
void TacticalSession::Impl::commit_units_reference(std::vector<LiveUnit>& units, std::uint64_t& writes) {
        waypoint_queues_present = false;
        auto next = units.begin();
        for (auto it = handles.begin(); it != handles.end();) {
            while (next != units.end() && next->state.entity_id < it->first) ++next;
            if (next == units.end() || next->state.entity_id != it->first) {
                const auto& identity = registry.get<Identity>(it->second);
                const auto slot = ownership_slot(identity.owner, identity.purchase_type != 0 ? identity.purchase_type : identity.type_id);
                if (slot != ownership_keys.size()) --committed_ownership_counts[slot];
                registry.destroy(it->second);
                it = handles.erase(it);
            } else ++it;
        }
        for (auto& unit : units) {
            std::optional<core::tick_profile::JobScope> profile_job;
            if (core::tick_profile::active_jobs)
                profile_job.emplace("job.registry-commit", unit.state.entity_id, nullptr, unit.state.type_id);
            const auto& state = unit.state;
            const auto found = handles.find(state.entity_id);
            const auto handle = found != handles.end() ? found->second : registry.create();
            if (found == handles.end()) {
                const auto slot = ownership_slot(state.owner, purchase_identity(state));
                if (slot != ownership_keys.size()) ++committed_ownership_counts[slot];
                handles.emplace(state.entity_id, handle);
                emplace_component<StableId>(handle, state.entity_id);
                emplace_component<Identity>(handle, state.type_id, state.owner, state.purchase_type,
                    state.purchase_token, state.barrage_source, state.garrison_enabled, state.controller_seat, state.control);
                emplace_component<Placement>(handle, state.position, state.rotation);
                emplace_component<CurrentOrder>(handle, state.order);
                writes += 4;
            } else {
                auto& identity = registry.get<Identity>(handle);
                if (identity.type_id != state.type_id || identity.owner != state.owner || identity.purchase_type != state.purchase_type
                    || identity.purchase_token != state.purchase_token || identity.barrage_source != state.barrage_source
                    || identity.garrison_enabled != state.garrison_enabled
                    || identity.controller_seat != state.controller_seat || identity.control != state.control) {
                    const auto before = ownership_slot(identity.owner, identity.purchase_type != 0 ? identity.purchase_type : identity.type_id);
                    const auto after = ownership_slot(state.owner, purchase_identity(state));
                    if (before != ownership_keys.size()) --committed_ownership_counts[before];
                    if (after != ownership_keys.size()) ++committed_ownership_counts[after];
                    identity = {state.type_id, state.owner, state.purchase_type, state.purchase_token,
                        state.barrage_source, state.garrison_enabled, state.controller_seat, state.control}; ++writes;
                }
                auto& placement = registry.get<Placement>(handle);
                if (placement.position != state.position || placement.rotation != state.rotation) {
                    placement = {state.position, state.rotation}; ++writes;
                }
                auto& order = registry.get<CurrentOrder>(handle);
                if (order.value != state.order) { order.value = state.order; ++writes; }
            }
            auto carried = state.contained.empty() ? std::optional<std::vector<CarriedObject>>{} : std::optional(state.contained);
            const bool marked = registry.all_of<GarrisonUnit>(handle);
            if (unit.garrison_unit != marked) {
                if (unit.garrison_unit) emplace_component<GarrisonUnit>(handle);
                else registry.remove<GarrisonUnit>(handle);
                ++writes;
            }
            if (unit.script_invulnerable != registry.all_of<ScriptInvulnerable>(handle)) {
                if (unit.script_invulnerable) emplace_component<ScriptInvulnerable>(handle);
                else registry.remove<ScriptInvulnerable>(handle);
                ++writes;
            }
            commit_optional<CarriedHeroes>(handle, carried, writes);
            commit_optional<Health>(handle, unit.durability, writes);
            commit_optional<AsteroidContact>(handle, unit.asteroid_contact, writes);
            commit_optional<NebulaState>(handle, unit.nebula, writes);
            auto* current_motion = registry.try_get<Motion>(handle);
            if (!unit.motion) {
                if (current_motion != nullptr) { registry.remove<Motion>(handle); ++writes; }
            } else if (current_motion == nullptr) {
                emplace_component<Motion>(handle, std::move(*unit.motion), unit.speed, unit.roll);
                ++writes;
            } else if (current_motion->value != *unit.motion || current_motion->speed != unit.speed || current_motion->roll != unit.roll) {
                *current_motion = {std::move(*unit.motion), unit.speed, unit.roll}; ++writes;
            }
            commit_optional<Combat>(handle, unit.combat, writes);
            commit_optional<Waiting>(handle, unit.formation, writes);
            commit_optional<Abilities>(handle, unit.abilities, writes);
            commit_optional<Approaching>(handle, unit.approach, writes);
            commit_optional<Sighted>(handle, unit.sighting, writes);
            commit_optional<IonStunned>(handle, unit.ion_stun, writes);
            commit_optional<CorruptedSystems>(handle, unit.corrupt, writes);
            // Derive the next tick's readiness gate in this existing stable commit, before the route is moved.
            waypoint_queues_present |= unit.waypoints && !unit.waypoints->points.empty();
            commit_optional<Waypoints>(handle, unit.waypoints, writes);
            commit_optional<PositionHistoryComponent>(handle, unit.position_history, writes);
            auto* vulnerable = registry.try_get<ArrivalVulnerability>(handle);
            if (!unit.arrival_vulnerable_until) {
                if (vulnerable != nullptr) { registry.remove<ArrivalVulnerability>(handle); ++writes; }
            } else if (vulnerable == nullptr) {
                emplace_component<ArrivalVulnerability>(handle, *unit.arrival_vulnerable_until);
                ++writes;
            } else if (vulnerable->until != *unit.arrival_vulnerable_until) {
                vulnerable->until = *unit.arrival_vulnerable_until; ++writes;
            }
            // WPR-55: preserve cached upgrade profiles across the sparse component commit.
            auto* upgraded = registry.try_get<UpgradeModifiers>(handle);
            if (unit.upgrade_bonuses == CombatBonuses{} && !unit.upgraded_durability) {
                if (upgraded != nullptr) { registry.remove<UpgradeModifiers>(handle); ++writes; }
            } else if (upgraded == nullptr) {
                emplace_component<UpgradeModifiers>(handle, unit.upgrade_bonuses,
                    std::move(unit.upgraded_durability));
                ++writes;
            } else if (upgraded->bonuses != unit.upgrade_bonuses ||
                upgraded->health != unit.upgraded_durability) {
                upgraded->bonuses = unit.upgrade_bonuses;
                upgraded->health = std::move(unit.upgraded_durability);
                ++writes;
            }
        }
    }

} // namespace eawr::sim::tactical
