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
        return LiveUnit{
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
        std::fill(committed_ownership_counts.begin(), committed_ownership_counts.end(), 0);
        const auto insert = [this](const LiveUnit& unit) {
            const auto& state = unit.state;
            const auto owned = ownership_slot(state.owner, purchase_identity(state));
            if (owned != ownership_keys.size()) ++committed_ownership_counts[owned];
            const auto handle = registry.create();
            emplace_component<StableId>(handle, state.entity_id);
            emplace_component<Identity>(handle, state.type_id, state.owner, state.purchase_type,
                state.purchase_token, state.barrage_source, state.garrison_enabled);
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

void TacticalSession::Impl::commit_units(std::vector<LiveUnit>& units, std::uint64_t& writes) {
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
            const auto& state = unit.state;
            const auto found = handles.find(state.entity_id);
            const auto handle = found != handles.end() ? found->second : registry.create();
            if (found == handles.end()) {
                const auto slot = ownership_slot(state.owner, purchase_identity(state));
                if (slot != ownership_keys.size()) ++committed_ownership_counts[slot];
                handles.emplace(state.entity_id, handle);
                emplace_component<StableId>(handle, state.entity_id);
                emplace_component<Identity>(handle, state.type_id, state.owner, state.purchase_type,
                    state.purchase_token, state.barrage_source, state.garrison_enabled);
                emplace_component<Placement>(handle, state.position, state.rotation);
                emplace_component<CurrentOrder>(handle, state.order);
                writes += 4;
            } else {
                auto& identity = registry.get<Identity>(handle);
                if (identity.type_id != state.type_id || identity.owner != state.owner || identity.purchase_type != state.purchase_type
                    || identity.purchase_token != state.purchase_token || identity.barrage_source != state.barrage_source
                    || identity.garrison_enabled != state.garrison_enabled) {
                    const auto before = ownership_slot(identity.owner, identity.purchase_type != 0 ? identity.purchase_type : identity.type_id);
                    const auto after = ownership_slot(state.owner, purchase_identity(state));
                    if (before != ownership_keys.size()) --committed_ownership_counts[before];
                    if (after != ownership_keys.size()) ++committed_ownership_counts[after];
                    identity = {state.type_id, state.owner, state.purchase_type, state.purchase_token,
                        state.barrage_source, state.garrison_enabled}; ++writes;
                }
                auto& placement = registry.get<Placement>(handle);
                if (placement.position != state.position || placement.rotation != state.rotation) {
                    placement = {state.position, state.rotation}; ++writes;
                }
                auto& order = registry.get<CurrentOrder>(handle);
                if (order.value != state.order) { order.value = state.order; ++writes; }
            }
            auto carried = state.contained.empty() ? std::optional<std::vector<CarriedObject>>{} : std::optional(state.contained);
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
            commit_optional<IonStunned>(handle, unit.ion_stun, writes);
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
