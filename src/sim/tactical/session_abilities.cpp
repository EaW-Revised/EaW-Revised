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
#include "eawr/sim/tactical/combat_modifiers.hpp"
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

const SpecialAbilityProfile* TacticalSession::Impl::beam_profile(const LiveUnit& unit, const AbilityKind kind) const {
    const auto* profile = abilities.find(unit.state.type_id);
    const auto slot = profile ? ability_slot(*profile, kind) : std::nullopt;
    if (!slot) return nullptr;
    const auto expected = kind == AbilityKind::energy_weapon ? SpecialAbilityKind::energy_weapon : SpecialAbilityKind::tractor_beam;
    const auto& name = profile->abilities[*slot].gui_activated_ability_name;
    const auto found = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& nested) {
        return nested.kind == expected && nested.name == name;
    });
    return found == profile->special.end() ? nullptr : &*found;
}

bool TacticalSession::Impl::beam_target_valid(const LiveUnit& source, const LiveUnit& target,
    const SpecialAbilityProfile& nested) const {
    const auto* profile = combat.find(target.state.type_id);
    return target.durability && target.durability->hull.raw() > 0 && profile != nullptr
        && players_hostile(snapshot_players, source.state.owner, target.state.owner)
        && !(target.nebula && target.nebula->present)
        && special_type_matches(nested.filter, target.state.type_id, profile->category_bits)
        && (nested.kind != SpecialAbilityKind::energy_weapon || profile->living_projectile_collision)
        // U-09: only the supported space-ship locomotor is admitted until the virtual exclusion is identified.
        && (nested.kind != SpecialAbilityKind::tractor_beam || motion.find(target.state.type_id) != nullptr);
}

bool TacticalSession::Impl::beam_in_range(const LiveUnit& source, const LiveUnit& target,
    const SpecialAbilityProfile& nested) const {
    const auto* ordinary = combat.find(source.state.type_id);
    const auto maximum = nested.beam_max_range.raw() > 0 ? nested.beam_max_range
        : ordinary ? ordinary->max_attack_distance.value_or(math::Fixed{}) : math::Fixed{};
    const auto minimum = nested.beam_min_range.raw() > 0 ? nested.beam_min_range
        : ordinary ? ordinary->min_attack_distance : math::Fixed{};
    const math::Vec2 delta{math::Fixed::from_raw(target.state.position.x.raw() - source.state.position.x.raw()),
        math::Fixed::from_raw(target.state.position.y.raw() - source.state.position.y.raw())};
    math::Fixed distance{};
    if (!math::try_length(delta, distance)) return false;
    math::Fixed extent{};
    if (const auto* footprint = motion.footprint(target.state.type_id)) {
        const auto facing = math::to_matrix(target.state.rotation, {});
        if (!facing) return false;
        math::Fixed alignment{};
        if (!math::try_dot(delta, {facing.value().rows[0][0], facing.value().rows[1][0]}, alignment)) return false;
        const auto threshold = math::multiply(distance, math::Fixed::from_raw(7071 * math::Fixed::scale / 10000));
        if (!threshold) return false;
        extent = std::abs(alignment.raw()) > threshold.value().raw() ? footprint->x_extent : footprint->y_extent;
    }
    return maximum.raw() > 0 && distance.raw() <= maximum.raw() + extent.raw()
        && (minimum.raw() <= 0 || distance.raw() > minimum.raw() + extent.raw());
}

void TacticalSession::Impl::release_beam(LiveUnit& unit, const AbilityKind kind, const std::uint64_t tick) const {
    const auto* nested = beam_profile(unit, kind);
    if (!nested || !unit.abilities) return;
    const auto& profile = *abilities.find(unit.state.type_id);
    const auto slot = *ability_slot(profile, kind);
    auto& state = unit.abilities->slots[slot];
    const auto switched = deactivate_ability(profile.abilities[slot], state, tick);
    unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
    if (switched.changed && kind == AbilityKind::tractor_beam)
        state.ready_tick = tick + profile.abilities[slot].recharge_frames; // U-09 conservative full recharge
    state.target = invalid_entity_id;
    state.target_hardpoint = no_hardpoint;
    auto& special = unit.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
    special.targets.clear();
    special.context.reset();
    unit.formation.reset();
    refresh_damage_modes(unit);
}

void TacticalSession::Impl::register_tractor(const LiveUnit& source, const LiveUnit* target) {
    for (const auto& effect : concentrate_effects)
        if (effect.source == source.state.entity_id && effect.kind == AbilityKind::tractor_beam)
            concentrate_changed_targets.insert(effect.target);
    std::erase_if(concentrate_effects, [&](const auto& effect) {
        return effect.source == source.state.entity_id && effect.kind == AbilityKind::tractor_beam;
    });
    const auto* nested = beam_profile(source, AbilityKind::tractor_beam);
    if (nested && source.abilities && target) {
        const auto slot = *ability_slot(*abilities.find(source.state.type_id), AbilityKind::tractor_beam);
        if (source.abilities->slots[slot].active) {
            const auto* target_profile = combat.find(target->state.type_id);
            const auto doubling = target_profile && !target_profile->hero
                && (target_profile->category_bits & nested->doubled_speed_categories) != 0;
            concentrate_changed_targets.insert(target->state.entity_id);
            concentrate_effects.push_back({target->state.entity_id, source.state.entity_id,
                nested->concentrate_stacking_category, {}, math::Fixed::from_raw(-nested->target_speed_decrease.raw()
                    * (doubling ? 2 : 1)), AbilityKind::tractor_beam});
        }
    }
    std::sort(concentrate_effects.begin(), concentrate_effects.end(), [](const auto& a, const auto& b) {
        return std::tie(a.target, a.category, a.source) < std::tie(b.target, b.category, b.source);
    });
}

core::Result<void> TacticalSession::Impl::service_beams(std::vector<LiveUnit>& units,
    const std::uint64_t tick, const PartitionExecutor& executor) {
    const bool present = std::any_of(abilities.profiles.begin(), abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::energy_weapon) || ability_slot(profile, AbilityKind::tractor_beam);
    });
    if (!present) return core::Result<void>::success();
    std::array<std::vector<ConcentrateEffect>, tick_partition_count> effects;
    std::array<std::vector<EntityId>, tick_partition_count> replans;
    const auto serviced = executor.execute_phase("hero-beams", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& source = units[i];
            if (!source.abilities) continue;
            for (const auto kind : {AbilityKind::energy_weapon, AbilityKind::tractor_beam}) {
                const auto* nested = beam_profile(source, kind);
                if (!nested) continue;
                const auto& profile = *abilities.find(source.state.type_id);
                const auto slot = *ability_slot(profile, kind);
                const auto& state = source.abilities->slots[slot];
                const auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
                const auto target = std::lower_bound(units.begin(), units.end(), state.target,
                    [](const auto& unit, const EntityId id) { return unit.state.entity_id < id; });
                if (!state.active || source.abilities->special.service_cancelled || !special.enabled || special.cancelled
                    || target == units.end() || target->state.entity_id != state.target
                    || !beam_target_valid(source, *target, *nested) || !beam_in_range(source, *target, *nested)
                    || (state.target_hardpoint != no_hardpoint && !target_hardpoint_standing(*target, state.target_hardpoint)))
                    release_beam(source, kind, tick);
                if (source.abilities->replan_due && source.motion) replans[partition].push_back(source.state.entity_id);
                if (kind == AbilityKind::tractor_beam && state.active && target != units.end()
                    && target->state.entity_id == state.target) {
                    const auto* target_profile = combat.find(target->state.type_id);
                    const auto doubling = target_profile && !target_profile->hero
                        && (target_profile->category_bits & nested->doubled_speed_categories) != 0;
                    effects[partition].push_back({state.target, source.state.entity_id, nested->concentrate_stacking_category,
                        {}, math::Fixed::from_raw(-nested->target_speed_decrease.raw() * (doubling ? 2 : 1)), kind});
                }
            }
        }
    });
    if (!serviced) return serviced;
    // Sparse ordered commit; all source work and target reads ran in partitions.
    std::vector<ConcentrateEffect> next;
    for (const auto& effect : concentrate_effects) if (effect.kind != AbilityKind::tractor_beam) next.push_back(effect);
    for (const auto& part : effects) next.insert(next.end(), part.begin(), part.end());
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return std::tie(a.target, a.category, a.source) < std::tie(b.target, b.category, b.source);
    });
    if (next != concentrate_effects) {
        for (const auto& effect : concentrate_effects) concentrate_changed_targets.insert(effect.target);
        for (const auto& effect : next) concentrate_changed_targets.insert(effect.target);
        concentrate_effects = std::move(next);
    }
    beam_replans.clear();
    for (const auto& part : replans) beam_replans.insert(beam_replans.end(), part.begin(), part.end());
    std::sort(beam_replans.begin(), beam_replans.end());
    beam_replans.erase(std::unique(beam_replans.begin(), beam_replans.end()), beam_replans.end());
    return core::Result<void>::success();
}

const SpecialAbilityProfile* TacticalSession::Impl::concentrate_profile(const LiveUnit& unit) const {
    const auto* profile = abilities.find(unit.state.type_id);
    if (profile == nullptr) return nullptr;
    const auto slot = ability_slot(*profile, AbilityKind::concentrate_fire);
    if (!slot) return nullptr;
    const auto& name = profile->abilities[*slot].gui_activated_ability_name;
    const auto found = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& special) {
        return special.kind == SpecialAbilityKind::concentrate_fire && special.name == name;
    });
    return found == profile->special.end() ? nullptr : &*found;
}

void TacticalSession::Impl::register_concentrate(const LiveUnit& unit) {
    for (const auto& effect : concentrate_effects) if (effect.source == unit.state.entity_id && effect.kind == AbilityKind::concentrate_fire)
        concentrate_changed_targets.insert(effect.target);
    std::erase_if(concentrate_effects, [&](const auto& effect) {
        return effect.source == unit.state.entity_id && effect.kind == AbilityKind::concentrate_fire;
    });
    const auto* nested = concentrate_profile(unit);
    if (nested == nullptr || !unit.abilities) return;
    const auto slot = ability_slot(*abilities.find(unit.state.type_id), AbilityKind::concentrate_fire);
    const auto& state = unit.abilities->slots[*slot];
    if (state.active && state.target != invalid_entity_id) {
        concentrate_changed_targets.insert(state.target);
        concentrate_effects.push_back({state.target, unit.state.entity_id, nested->concentrate_stacking_category,
            math::Fixed::from_raw(-nested->target_damage_increase.raw())});
        std::sort(concentrate_effects.begin(), concentrate_effects.end(), [](const auto& a, const auto& b) {
            return std::tie(a.target, a.category, a.source) < std::tie(b.target, b.category, b.source);
        });
    }
}

core::Result<void> TacticalSession::Impl::service_concentrate(std::vector<LiveUnit>& units,
    const std::uint64_t tick, const PartitionExecutor& executor) {
    if (std::none_of(abilities.profiles.begin(), abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::concentrate_fire).has_value();
    })) return core::Result<void>::success();
    concentrate_service_census = units.size();
    concentrate_service_last_id = units.empty() ? invalid_entity_id : units.back().state.entity_id;
    std::array<std::vector<ConcentrateEffect>, tick_partition_count> effects;
    // WHE-25: immutable entity identities and disjoint source-slot outputs.
    const auto result = executor.execute_phase("concentrate-service", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& unit = units[i];
            if (!unit.abilities) continue;
            const auto* nested = concentrate_profile(unit);
            if (nested == nullptr || !unit.abilities) continue;
            auto& state = *unit.abilities;
            const auto& profile = *abilities.find(unit.state.type_id);
            const auto slot = *ability_slot(profile, AbilityKind::concentrate_fire);
            auto& active = state.slots[slot];
            auto& special = state.special.slots[static_cast<std::size_t>(nested - profile.special.data())];
            const auto target = std::lower_bound(units.begin(), units.end(), active.target,
                [](const LiveUnit& input, const EntityId id) { return input.state.entity_id < id; });
            const bool target_live = target != units.end() && target->state.entity_id == active.target;
            if (!active.active || !target_live
                || state.special.service_cancelled || special.cancelled || (active.target != 0 && !special.enabled)) {
                if (active.active) static_cast<void>(deactivate_ability(profile.abilities[slot], active, tick));
                active.target = invalid_entity_id;
                active.target_hardpoint = no_hardpoint;
                state.concentrate_recruits.clear();
                special.targets.clear();
                special.context.reset();
                continue;
            }
            special.next_service_frame = tick + nested->service_interval;
            effects[partition].push_back({active.target, unit.state.entity_id, nested->concentrate_stacking_category,
                math::Fixed::from_raw(-nested->target_damage_increase.raw())});
        }
    });
    if (!result) return result;
    std::vector<ConcentrateEffect> next;
    for (const auto& effect : concentrate_effects) if (effect.kind != AbilityKind::concentrate_fire) next.push_back(effect);
    for (const auto& part : effects) next.insert(next.end(), part.begin(), part.end());
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return std::tie(a.target, a.category, a.source) < std::tie(b.target, b.category, b.source);
    });
    if (next != concentrate_effects) {
        for (const auto& effect : concentrate_effects) concentrate_changed_targets.insert(effect.target);
        for (const auto& effect : next) concentrate_changed_targets.insert(effect.target);
        concentrate_effects = std::move(next);
    }
    return core::Result<void>::success();
}

const SpawnedAbilityProfile* TacticalSession::Impl::spawn_profile(const TypeId type) const {
    for (const auto& profile : abilities.profiles) for (const auto& ability : profile.abilities)
        if (ability.spawned && ability.spawned->type == type) return &*ability.spawned;
    return nullptr;
}

core::Result<void> TacticalSession::Impl::service_weaken(std::vector<LiveUnit>& units,
    const std::uint64_t tick, const PartitionExecutor& executor) {
    if (ability_spawns.empty()) return core::Result<void>::success();
    std::array<std::vector<ConcentrateEffect>, tick_partition_count> effects;
    const auto serviced = executor.execute_phase("weaken-status", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, ability_spawns.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& spawn = ability_spawns[i];
            const auto* profile = spawn_profile(spawn.type);
            if (!profile || !profile->weaken.on_detonation) continue;
            std::erase_if(spawn.recipients, [&](const auto& recipient) {
                const auto found = std::lower_bound(units.begin(), units.end(), recipient.target,
                    [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
                return tick >= recipient.expires || found == units.end() || found->state.entity_id != recipient.target;
            });
            for (const auto& recipient : spawn.recipients) effects[partition].push_back({recipient.target, spawn.id, 0,
                math::Fixed::from_raw(-profile->weaken.take_damage_increase.raw()), {}, AbilityKind::weaken_enemy,
                math::Fixed::from_raw(-profile->weaken.cause_damage_reduction.raw())});
        }
    });
    if (!serviced) return serviced;
    std::erase_if(ability_spawns, [](const auto& spawn) { return spawn.detonated && spawn.recipients.empty(); });
    std::vector<ConcentrateEffect> next;
    for (const auto& effect : concentrate_effects) if (effect.kind != AbilityKind::weaken_enemy) next.push_back(effect);
    for (const auto& part : effects) next.insert(next.end(), part.begin(), part.end());
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return std::tie(a.target, a.category, a.source) < std::tie(b.target, b.category, b.source);
    });
    if (next != concentrate_effects) {
        for (const auto& effect : concentrate_effects) concentrate_changed_targets.insert(effect.target);
        for (const auto& effect : next) concentrate_changed_targets.insert(effect.target);
        concentrate_effects = std::move(next);
    }
    return core::Result<void>::success();
}

bool TacticalSession::Impl::accumulate_concentrate_bonus(const EntityId target,
    const std::span<CombatBonuses> categories, const UnitStage* units) const {
    auto first = std::lower_bound(concentrate_effects.begin(), concentrate_effects.end(), target,
        [](const auto& effect, const EntityId id) { return effect.target < id; });
    bool applied = false;
    for (; first != concentrate_effects.end() && first->target == target; ++first) {
        if (units != nullptr && first->kind != AbilityKind::weaken_enemy) {
            const auto source = units->find(first->source);
            if (source == units->end() || !source->second.abilities) continue;
            const auto slot = ability_slot(*abilities.find(source->second.state.type_id), first->kind);
            if (!slot || !source->second.abilities->slots[*slot].active
                || source->second.abilities->slots[*slot].target != target) continue;
        }
        const auto category = std::lower_bound(bonus_categories.begin(), bonus_categories.end(), first->category);
        if (category == bonus_categories.end() || *category != first->category) continue;
        CombatBonuses contribution{};
        contribution[1] = first->cause;
        contribution[4] = first->defense;
        contribution[5] = first->speed;
        accumulate_combat_bonus(categories[static_cast<std::size_t>(category - bonus_categories.begin())], contribution);
        applied = true;
    }
    return applied;
}

math::Fixed TacticalSession::Impl::concentrate_defense(const LiveUnit& target, const UnitStage& units,
    const std::vector<PlayerEconomy>& accounts) const {
    const auto effect = std::lower_bound(concentrate_effects.begin(), concentrate_effects.end(), target.state.entity_id,
        [](const auto& value, const EntityId id) { return value.target < id; });
    if ((effect == concentrate_effects.end() || effect->target != target.state.entity_id)
        && !concentrate_changed_targets.contains(target.state.entity_id)) return target.upgrade_bonuses[4];
    if (!impact_bonus_containers_ready) {
        for (const auto& account : accounts) for (const auto& held : account.completed) {
            const auto found = units.find(held.station);
            if (found != units.end()) impact_bonus_containers.emplace(held.station, found->second.state.type_id);
        }
        impact_bonus_containers_ready = true;
    }
    auto [base, inserted] = impact_bonus_bases.try_emplace(target.state.entity_id, bonus_categories.size());
    if (inserted) static_cast<void>(command_bonuses_for(target, command_ledger, accounts,
        impact_bonus_containers, base->second, nullptr, false));
    auto categories = base->second;
    // Cached base categories; only sparse live source references are checked on later hits.
    const bool targeted = accumulate_concentrate_bonus(target.state.entity_id, categories, &units);
    return sum_combat_bonus_categories(categories, targeted || command_ledger.recipients.contains(target.state.entity_id))[4];
}

core::Result<void> TacticalSession::Impl::refresh_concentrate_bonuses(std::vector<LiveUnit>& units,
    const CommandLedger& ledger, const std::vector<PlayerEconomy>& accounts, const PartitionExecutor& executor) const {
    std::vector<EntityId> target_ids(concentrate_changed_targets.begin(), concentrate_changed_targets.end());
    for (const auto& effect : concentrate_effects) if (effect.kind == AbilityKind::tractor_beam) target_ids.push_back(effect.target);
    std::sort(target_ids.begin(), target_ids.end());
    target_ids.erase(std::unique(target_ids.begin(), target_ids.end()), target_ids.end());
    if (target_ids.empty()) return core::Result<void>::success();
    std::vector<LiveUnit*> targets;
    for (const auto id : target_ids) {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        if (found != units.end() && found->state.entity_id == id) targets.push_back(&*found);
    }
    std::map<EntityId, TypeId> containers;
    for (const auto& account : accounts) for (const auto& held : account.completed) {
        const auto found = std::lower_bound(units.begin(), units.end(), held.station,
            [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
        if (found != units.end() && found->state.entity_id == held.station) containers.emplace(held.station, found->state.type_id);
    }
    std::array<std::optional<core::Diagnostic>, tick_partition_count> errors{};
    const auto applied = executor.execute_phase("concentrate-target-modifiers", tick_partition_count, [&](const std::size_t partition) {
        std::vector<CombatBonuses> categories(bonus_categories.size());
        const auto range = partition_range(partition, targets.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& unit = *targets[i];
            unit.in_tractor_beam = false;
            auto effect = std::lower_bound(concentrate_effects.begin(), concentrate_effects.end(), unit.state.entity_id,
                [](const auto& value, const EntityId id) { return value.target < id; });
            for (; effect != concentrate_effects.end() && effect->target == unit.state.entity_id; ++effect) {
                if (effect->kind != AbilityKind::tractor_beam) continue;
                const auto source = std::lower_bound(units.begin(), units.end(), effect->source,
                    [](const auto& value, const EntityId id) { return value.state.entity_id < id; });
                if (source != units.end() && source->state.entity_id == effect->source) unit.in_tractor_beam = true;
            }
            const auto result = apply_bonuses(unit, command_bonuses_for(unit, ledger, accounts, containers, categories), BonusAdjustment::loss);
            if (!result) { errors[partition] = result.error(); break; }
        }
    });
    if (!applied) return applied;
    for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
    return core::Result<void>::success();
}

std::optional<AbilityState> TacticalSession::Impl::entering_abilities(const TypeId type, const PlayerId owner) const {
        const auto* profile = abilities.find(type);
        return profile != nullptr ? std::optional(initial_abilities(*profile, abilities.autofire_default(owner))) : std::nullopt;
    }

AbilityGate TacticalSession::Impl::ability_gate(const LiveUnit& unit, const std::uint64_t tick) const {
        AbilityGate gate;
        gate.in_nebula = unit.nebula && unit.nebula->frame && unit.nebula->present;
        if (!unit.durability) return gate;
        const auto& profile = *health_profile(unit);
        gate.engines_online = engines_online(profile, *unit.durability);
        gate.ion_stunned = ion_stunned(unit.ion_stun, tick);
        if (!durability.damage) return gate;
        gate.shielded = profile.max_shields.raw() > 0;
        gate.shields_online = shields_online(profile, *unit.durability);
        gate.shield_depleted = shield_depleted(*durability.damage, *unit.durability, tick);
        gate.in_ion_storm = in_ion_storm(*durability.damage, *unit.durability, tick);
        return gate;
    }

const AbilitySlot* TacticalSession::Impl::ion_slot(const LiveUnit& unit) const {
        if (!unit.abilities) return nullptr;
        const auto slot = ability_slot(*abilities.find(unit.state.type_id), AbilityKind::ion_cannon_shot);
        return slot ? &unit.abilities->slots[*slot] : nullptr;
    }

AbilitySlot* TacticalSession::Impl::ion_slot(LiveUnit& unit) const {
        return const_cast<AbilitySlot*>(ion_slot(static_cast<const LiveUnit&>(unit)));
    }

bool TacticalSession::Impl::ion_target_valid(const PlayerId owner, const LiveUnit* target) const {
        return target != nullptr && players_hostile(snapshot_players, owner, target->state.owner)
            && combat.find(target->state.type_id) != nullptr && !(target->nebula && target->nebula->present);
    }

void TacticalSession::Impl::finish_ion_shot(AbilitySlot& team, const std::vector<AbilitySlot*>& craft, const TypeId container_type,
    const std::uint64_t tick) const {
        std::size_t due = 0;
        for (auto* slot : craft) {
            due += slot->active ? 1 : 0;
            slot->active = false;
        }
        const auto& profile = *abilities.find(container_type);
        const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::ion_cannon_shot)];
        team.active = false;
        team.target = invalid_entity_id;
        team.target_hardpoint = no_hardpoint;
        if (due < craft.size()) team.ready_tick = tick + ability.recharge_frames;
    }

math::Fixed TacticalSession::Impl::ability_factor(const LiveUnit& unit, const AbilityModifier modifier) const {
        if (!unit.abilities) return math::Fixed::from_raw(math::Fixed::scale);
        return ability_multiplier(*abilities.find(unit.state.type_id), *unit.abilities, modifier);
    }

void TacticalSession::Impl::refresh_damage_modes(LiveUnit& unit) const {
    unit.cause_damage_mode = ability_factor(unit, AbilityModifier::cause_damage);
    unit.take_damage_mode = ability_factor(unit, AbilityModifier::take_damage);
}

void TacticalSession::Impl::defend_switched_on(LiveUnit& unit, const std::uint64_t tick) const {
        if (!unit.durability || !durability.damage) return;
        unit.durability->next_shield_frame = tick + 1;
        if (has_energy_pool(*health_profile(unit), *durability.damage)) {
            unit.durability->next_energy_frame = tick + 1;
        }
    }

void TacticalSession::Impl::track_damage(LiveUnit& unit, const math::Fixed hull_before, const math::Fixed shields_before) const {
        if (!unit.abilities || !unit.durability || !abilities.find(unit.state.type_id)->defend_script) return;
        const auto before = hull_before.raw() + shields_before.raw();
        const auto after = unit.durability->hull.raw() + unit.durability->shields.raw();
        if (before > after) {
            unit.abilities->window_damage = math::Fixed::from_raw(unit.abilities->window_damage.raw() + (before - after));
        }
    }

void TacticalSession::Impl::ion_stun_unit(LiveUnit& unit, const IonStunShot& shot, const std::uint64_t tick) const {
        unit.ion_stun = ion_stun(unit.ion_stun, shot, tick);
        if (!unit.abilities) return;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto slot = ability_slot(profile, AbilityKind::defend);
        if (!slot) return;
        const auto switched = deactivate_ability(profile.abilities[*slot], unit.abilities->slots[*slot], tick);
        unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
        if (switched.changed) refresh_damage_modes(unit);
    }

void TacticalSession::Impl::service_ion_stun(LiveUnit& unit, const std::uint64_t tick) {
        if (tick >= unit.ion_stun->end_frame) unit.ion_stun.reset();
    }

void TacticalSession::Impl::end_depleted_defend(LiveUnit& unit, const std::uint64_t tick, const bool storm_branch) const {
        if (!unit.abilities || !unit.durability || (!storm_branch && unit.durability->shields.raw() > 0)) return;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto slot = ability_slot(profile, AbilityKind::defend);
        if (!slot) return;
        const auto switched = deactivate_ability(profile.abilities[*slot], unit.abilities->slots[*slot], tick);
        unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
        if (switched.changed) refresh_damage_modes(unit);
    }

bool TacticalSession::Impl::service_abilities(LiveUnit& unit, const std::uint64_t tick) const {
        auto& state = *unit.abilities;
        const auto& profile = *abilities.find(unit.state.type_id);
        bool speed = state.replan_due;
        state.replan_due = false;
        speed = expire_abilities(profile, state, tick).speed || speed;
        if (!unit.motion) {
            if (const auto index = ability_slot(profile, AbilityKind::barrage)) {
                speed = deactivate_ability(profile.abilities[*index], state.slots[*index], tick).speed || speed;
            }
        }
        // AB-16: lost engines end TURBO and SPOILER_LOCK (HD-11).
        if (unit.durability) {
            if (const auto* hull = health_profile(unit); hull != nullptr && !engines_online(*hull, *unit.durability)) {
                for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
                    const auto kind = profile.abilities[index].kind;
                    if (kind != AbilityKind::turbo && kind != AbilityKind::spoiler_lock) continue;
                    speed = deactivate_ability(profile.abilities[index], state.slots[index], tick).speed || speed;
                }
            }
        }
        // AB-41, AB-42: the object script's check runs when the damage-rate window closes.
        if (profile.defend_script && rate_window_closes(tick)) {
            close_rate_window(state);
            const auto slot = ability_slot(profile, AbilityKind::defend);
            const bool armed = slot && (!abilities.human(unit.state.owner) || state.slots[*slot].autofire);
            if (armed && state.damage_rate.raw() > defend_rate_threshold * math::Fixed::scale) {
                const auto switched =
                    activate_ability(profile.abilities[*slot], state.slots[*slot], ability_gate(unit, tick), tick);
                if (switched.changed) defend_switched_on(unit, tick);
                speed = switched.speed || speed;
            }
        }
        refresh_damage_modes(unit);
        return speed;
    }

std::vector<AbilityStatus> TacticalSession::Impl::ability_statuses(const LiveUnit& unit, const std::uint64_t tick) const {
        std::vector<AbilityStatus> result;
        if (!unit.abilities) return result;
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto gate = ability_gate(unit, tick);
        for (std::size_t index = 0; index < profile.abilities.size(); ++index) {
            const auto& ability = profile.abilities[index];
            const auto& slot = unit.abilities->slots[index];
            AbilityStatus status;
            status.kind = ability.kind;
            status.started_tick = slot.started_tick;
            status.active = slot.active;
            status.ready = ability_ready(ability, slot, gate, tick);
            status.autofire = slot.autofire;
            status.supports_autofire = ability.supports_autofire;
            if (slot.active && (ability.kind == AbilityKind::energy_weapon || ability.kind == AbilityKind::tractor_beam)) {
                status.target = slot.target;
                status.target_hardpoint = slot.target_hardpoint;
            }
            // AB-05: publication is one tick ahead of timer servicing. The final active
            // snapshot still has a timed dial, even when its remaining duration is zero.
            if (slot.active && slot.expires_tick != 0) {
                status.remaining_frames = slot.expires_tick > tick
                    ? static_cast<std::uint32_t>(slot.expires_tick - tick) : 0U;
                status.total_frames = ability.expiration_frames;
            } else if (!slot.active && slot.ready_tick > tick) {
                status.remaining_frames = static_cast<std::uint32_t>(slot.ready_tick - tick);
                status.total_frames = ability.recharge_frames;
            }
            result.push_back(status);
        }
        return result;
    }

} // namespace eawr::sim::tactical
